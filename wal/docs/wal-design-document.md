# Write-Ahead Log (WAL) for Key-Value Mutations: High-Level Design Document

**Status:** Draft  
**Authors:** Architecture Review  
**Last Updated:** 2026-02-25  
**Classification:** System Design — Storage Infrastructure

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Non-Functional Requirements (NFR)](#2-non-functional-requirements)
3. [Service Level Objectives (SLO)](#3-service-level-objectives)
4. [Architecture Decision Records (ADR)](#4-architecture-decision-records)
5. [Part 1 — Data Structures](#5-part-1--data-structures)
6. [Part 2 — System Outline: Modules, Classes, Methods](#6-part-2--system-outline)
7. [Part 3 — Data and Control Flow](#7-part-3--data-and-control-flow)
8. [Part 4 — System Invariants](#8-part-4--system-invariants)
9. [Part 5 — Unit Testing Strategy](#9-part-5--unit-testing-strategy)
10. [Part 6 — Integration Testing Strategy](#10-part-6--integration-testing-strategy)
11. [Part 7 — Scale Testing Strategy](#11-part-7--scale-testing-strategy)
12. [Part 8 — Durability Guarantees](#12-part-8--durability-guarantees)
13. [SRE Operational Concerns](#13-sre-operational-concerns)
14. [Tradeoff Analysis Summary](#14-tradeoff-analysis-summary)

---

## 1. Executive Summary

This document describes a write-ahead log (WAL) for a key-value store that tolerates crashes mid-write. The WAL provides the foundational durability layer: every mutation (PUT or DEL) is serialized to a binary log with length-framing and checksums before being applied to in-memory state. On recovery, the log is replayed forward, halting at the first corrupt or partial record, thereby guaranteeing that recovered state reflects a valid prefix of committed operations.

The MVP scope covers the core append/replay/recovery loop with a `get/put/del` API. Stretch goals introduce log rotation, periodic snapshots, and a configurable fsync policy knob to let operators trade durability for throughput.

---

## 2. Non-Functional Requirements (NFR)

### NFR-1: Crash Safety

The system must tolerate abrupt process termination (kill -9, power loss, kernel panic) at any point during a write and recover to a consistent state. "Consistent" means the recovered key-value map reflects a contiguous prefix of the operations that were acknowledged to the caller.

### NFR-2: Data Integrity

Every persisted record must be protected by a checksum. Bit-rot, partial writes, and filesystem corruption must be detected, not silently ingested. The system must never serve data that fails integrity verification.

### NFR-3: Append-Only Sequential I/O

All WAL writes are sequential appends. No in-place updates to the log file. This is the single most important performance characteristic: sequential writes on modern storage (NVMe, SSD, even spinning disk) are orders of magnitude faster than random writes, and it eliminates the class of bugs where a mid-update crash leaves a record in a neither-old-nor-new state.

### NFR-4: Deterministic Recovery

Recovery must be deterministic: given the same log file byte-for-byte, replay must produce the same key-value state. No reliance on wall-clock time, randomness, or external coordination during recovery.

### NFR-5: Bounded Recovery Time (Stretch)

With log rotation and snapshots enabled, recovery time must be bounded. Specifically, recovery should require loading at most one snapshot plus replaying at most one active log segment.

### NFR-6: Observability

The system must expose metrics for: records written, bytes written, fsync latency, recovery duration, records replayed, corrupt records encountered, and current log segment size.

### NFR-7: Testability

Every component must be testable in isolation with deterministic, injectable I/O. The system must support fault-injection interfaces for simulating partial writes, fsync failures, and storage corruption.

---

## 3. Service Level Objectives (SLO)

| SLO ID | Objective | Target | Measurement |
|--------|-----------|--------|-------------|
| SLO-1 | **Write latency (p99)** | ≤ 1 ms without fsync; ≤ 10 ms with fsync per-op | Histogram of `put`/`del` call duration |
| SLO-2 | **Recovery correctness** | 100% — zero silent data corruption across all crash scenarios | Verified by crash-recovery test suite against oracle |
| SLO-3 | **Recovery time** | ≤ 5 s for 1 GB active log (MVP); ≤ 2 s with snapshots (stretch) | Wall-clock from process start to serving reads |
| SLO-4 | **Data loss window** | ≤ 1 s of operations with default fsync policy (batched); 0 with per-op fsync | Gap between last durable op and crash point |
| SLO-5 | **Throughput** | ≥ 100 K ops/s for small KV pairs (≤ 256 B) with batched fsync | Sustained write benchmark, single thread |
| SLO-6 | **Detection rate** | 100% of single-bit errors, ≥ 99.999% of multi-bit corruption | CRC-32C collision properties; verified by fault-injection |

### SLO Tradeoff Notes

SLO-1 and SLO-4 are in direct tension. The fsync policy knob (stretch) is the mechanism by which operators resolve this tension for their deployment:

- **`fsync=every`**: Meets SLO-4 (zero loss) at the cost of SLO-1 (write latency dominated by device flush, typically 50 µs–2 ms on NVMe, 5–15 ms on SATA SSD).
- **`fsync=batched`** (e.g., every 100 ms or N ops): Amortizes fsync cost across many operations, meeting SLO-1 and SLO-5, but the data loss window equals the batch interval.
- **`fsync=none`**: Maximum throughput, data loss window extends to OS page cache writeback interval (often 5–30 s). Only appropriate for reconstructible or non-critical data.

---

## 4. Architecture Decision Records (ADR)

### ADR-1: Record Framing — Length-Prefixed with Trailing Checksum

**Context:** We need a self-describing record format that allows sequential reading, identifies record boundaries, and detects corruption or truncation.

**Alternatives Considered:**

| Approach | Pros | Cons |
|----------|------|------|
| Delimiter-based (e.g., newline) | Simple, human-readable | Requires escaping; can't handle binary payloads; delimiter in data creates ambiguity |
| Length-prefixed, checksum trailing | O(1) to find next record; binary-safe; checksum covers full record; truncation detected by short read | Requires reading length before payload; not human-readable |
| Length-prefixed, checksum in header | Can verify header before reading payload | Checksum doesn't cover payload — defeats the purpose |
| Page-aligned blocks (LevelDB/RocksDB style) | Excellent alignment with filesystem pages; supports partial page recovery | Significantly more complex; internal fragmentation; premature for MVP |

**Decision:** Length-prefixed with trailing checksum. The format is `[uint32 length][payload bytes][uint32 checksum]`. This is the simplest format that is binary-safe, self-framing, and fully integrity-checked.

**Consequences:** We accept that this format does not align records to page boundaries, which means a single record can straddle a filesystem page and a partial page write can corrupt the length prefix itself. This is acceptable because our recovery strategy (halt at first corruption) tolerates this: the torn record and everything after it are discarded. The tradeoff is simplicity now vs. potentially adopting a block-based format later if we need mid-log recovery (skipping past a corrupt record to recover subsequent valid records).

---

### ADR-2: Checksum Algorithm — CRC-32C

**Context:** We need a fast, well-understood checksum with hardware acceleration on modern x86 and ARM.

**Alternatives Considered:**

| Algorithm | Speed (GB/s, HW-accel) | Collision Resistance | Notes |
|-----------|------------------------|---------------------|-------|
| CRC-32C | ~20+ GB/s (SSE4.2) | Excellent for error detection; not cryptographic | Used by RocksDB, LevelDB, ext4, iSCSI |
| CRC-32 (ISO) | ~5 GB/s (no HW accel on x86) | Good | Legacy; no hardware assist on modern CPUs |
| xxHash32/64 | ~15 GB/s | Good | Fast but no standardized HW instruction |
| SHA-256 | ~0.5 GB/s | Cryptographic | Massive overkill; 50x slower than CRC-32C |

**Decision:** CRC-32C. It is the industry standard for storage integrity checks, has hardware acceleration via `_mm_crc32_*` intrinsics (SSE4.2, universally available on x86 since 2009; ARMv8 CRC extension since 2013), and provides excellent error detection properties for the corruption patterns we care about (bit flips, partial writes, zero-fill).

**Consequences:** We are explicitly choosing error detection, not tamper resistance. An adversary who can write to the log file can forge valid checksums. This is acceptable: the WAL's threat model is hardware/OS faults, not malicious modification.

---

### ADR-3: Recovery Strategy — Halt at First Corruption

**Context:** When recovery encounters a record that fails validation (bad checksum, short read, invalid length), it must decide what to do.

**Alternatives Considered:**

| Strategy | Pros | Cons |
|----------|------|------|
| Halt at first bad record | Simple; guarantees a valid prefix; no risk of applying out-of-order ops | Loses all records after a single torn write, even if subsequent records are valid |
| Skip bad record, continue scanning | Recovers more data | Requires a secondary framing mechanism to resynchronize; risks applying ops out of order if length field is corrupt; substantially more complex |
| Redundant log (double-write) | Can recover past a single corruption | 2x write amplification; complexity |

**Decision:** Halt at first corruption. This is the standard approach used by LevelDB, SQLite (in WAL mode), and most single-node WALs.

**Rationale:** The "skip and continue" approach is fundamentally unsound with our framing format. If the length field of record N is corrupt, we cannot reliably find the start of record N+1 — we'd be interpreting payload bytes as a length prefix. Block-based formats (LevelDB) solve this by aligning to fixed-size pages, providing known-good resynchronization points. But block-based formats are ADR-1's "premature complexity" option. If we later need skip-past-corruption, we adopt a block-based format as a breaking change, which is the correct point to take on that complexity.

**Consequences:** A single torn write at any point in the log discards all subsequent records. In practice, torn writes only occur at the tail of the log (the most recently appended record at crash time), so the data loss is typically zero or one operation. This is consistent with SLO-4.

---

### ADR-4: In-Memory Index — `std::unordered_map`

**Context:** After replaying the WAL, we need an in-memory data structure to serve `get` queries.

**Alternatives Considered:**

| Structure | Lookup | Insertion | Memory | Notes |
|-----------|--------|-----------|--------|-------|
| `std::unordered_map<std::string, std::string>` | O(1) amortized | O(1) amortized | Higher per-entry overhead (~64–96 B/entry) | Standard, well-understood |
| `std::map` (red-black tree) | O(log n) | O(log n) | Lower overhead per entry | Ordered iteration; slower point lookups |
| Flat hash map (Abseil, Robin Hood) | O(1) amortized, better constants | O(1) amortized | Lower overhead, better cache behavior | External dependency |
| Skip list | O(log n) | O(log n) | Moderate | Concurrent-friendly; overkill for single-threaded MVP |

**Decision:** `std::unordered_map` for MVP. It is in the standard library, requires no external dependencies, and its performance characteristics are adequate for the MVP scope.

**Consequences:** We accept higher per-entry memory overhead relative to open-addressing hash maps. If profiling shows this matters (e.g., for datasets exceeding available RAM), we can swap to Abseil `flat_hash_map` as a drop-in replacement — the API surface is nearly identical.

---

### ADR-5: Fsync Policy — Configurable Knob (Stretch)

**Context:** `fsync()` / `fdatasync()` is the only mechanism to guarantee data has reached durable storage. But it is the dominant cost in write-path latency.

**Options:**

| Policy | Durability | Throughput | Use Case |
|--------|-----------|------------|----------|
| Per-operation fsync | fsync after every append | Lowest (bounded by device flush latency) | Financial transactions, metadata stores |
| Batched fsync | fsync every N ops or T milliseconds | Moderate | General-purpose; default |
| No fsync | OS decides when to flush | Highest | Caches, reconstructible data |

**Decision:** Make this a runtime-configurable enum (`FsyncPolicy::kEveryOp`, `kBatched`, `kNone`). Default to `kBatched` with a 100 ms interval.

**Consequences:** The `kBatched` default means callers must understand they are not getting per-operation durability. The API should document this clearly. The `kEveryOp` mode must use `fdatasync()` (not `fsync()`) to avoid unnecessary metadata flushes — we only need data durability, and the file's metadata (mtime, size) can lag.

---

### ADR-6: Log Rotation and Snapshots (Stretch)

**Context:** Without rotation, the WAL grows without bound. Recovery time is proportional to log size.

**Decision:** Implement a two-phase rotation:

1. **Snapshot**: Serialize the current in-memory map to a binary snapshot file (sorted key-value pairs with a header checksum).
2. **Rotate**: Close the current log segment, open a new one. Snapshot and old segments can be archived/deleted once the snapshot is confirmed durable.

**Trigger:** Rotation triggers when the active log exceeds a configurable size threshold (default: 64 MB).

**Recovery with snapshots:** Load the most recent valid snapshot, then replay only the active log segment. This bounds recovery time per NFR-5.

**Consequences:** Snapshot creation is a stop-the-world operation in the MVP (no concurrent writes during snapshot). A production system would want a copy-on-write snapshot or a background compaction thread, but that is out of scope.

---

## 5. Part 1 — Data Structures

### 5.1 On-Disk Record Format

```
┌──────────────┬─────────────────────────────┬──────────────┐
│ payload_len  │         payload             │   checksum   │
│  (4 bytes)   │   (payload_len bytes)       │  (4 bytes)   │
│  uint32 LE   │                             │  uint32 LE   │
└──────────────┴─────────────────────────────┴──────────────┘
```

All multi-byte integers are little-endian. The checksum is CRC-32C computed over the concatenation of `[payload_len bytes || payload bytes]` — i.e., the checksum covers the length field as well as the payload. This is critical: if a partial write corrupts the length field, the checksum catches it.

### 5.2 Payload Encoding

```
PUT record:  [uint8 op=0x01] [uint32 key_len] [key bytes] [uint32 val_len] [val bytes]
DEL record:  [uint8 op=0x02] [uint32 key_len] [key bytes]
```

Keys and values are arbitrary byte strings. No null-termination assumptions.

### 5.3 Snapshot Format (Stretch)

```
┌─────────────┬───────────────┬──────────────────────────┬──────────────┐
│ magic       │ entry_count   │  entries (sorted)        │ checksum     │
│ (8 bytes)   │ (uint64 LE)   │  [key_len|key|val_len|val] × N │ (4 bytes) │
└─────────────┴───────────────┴──────────────────────────┴──────────────┘
```

The snapshot is a single contiguous write. Sorted order is not required for correctness but enables binary search if we later want to memory-map the snapshot instead of loading it into a hash map.

### 5.4 In-Memory Structures

- **`KeyValueMap`**: `std::unordered_map<std::string, std::string>` — the recovered/current state.
- **`WalRecord`**: A discriminated union (or struct with op-type tag) representing a parsed mutation. Used only transiently during encoding/decoding; never stored in bulk.
- **`LogSegment`**: File descriptor, current write offset, path. One active segment at a time.

---

## 6. Part 2 — System Outline

### 6.1 Module Decomposition

```
┌─────────────────────────────────────────────────────────┐
│                     KVStore (API Layer)                  │
│         get(key) → optional<string>                     │
│         put(key, value) → Status                        │
│         del(key) → Status                               │
├─────────────────────────────────────────────────────────┤
│                     WalWriter                           │
│         append(WalRecord) → Status                     │
│         sync() → Status                                │
│         rotate() → Status            [stretch]         │
├─────────────────────────────────────────────────────────┤
│                     WalReader                           │
│         open(path) → Status                            │
│         next_record() → optional<WalRecord>            │
│         (iterator interface over valid record prefix)  │
├─────────────────────────────────────────────────────────┤
│                     RecordCodec                         │
│         encode(WalRecord) → bytes                      │
│         decode(bytes) → WalRecord                      │
│         compute_checksum(bytes) → uint32               │
│         verify_checksum(bytes, uint32) → bool          │
├─────────────────────────────────────────────────────────┤
│                     Recovery                            │
│         recover(log_path) → KeyValueMap                │
│         recover(snapshot_path, log_path) → KeyValueMap │
├─────────────────────────────────────────────────────────┤
│                     SnapshotWriter / SnapshotReader     │
│         write(KeyValueMap, path) → Status   [stretch]  │
│         read(path) → KeyValueMap            [stretch]  │
├─────────────────────────────────────────────────────────┤
│                  FileInterface (abstraction)            │
│         write(bytes) → Status                          │
│         read(offset, len) → bytes                      │
│         sync() → Status                                │
│         size() → uint64                                │
│     (Injectable: RealFile, FaultInjectingFile)         │
└─────────────────────────────────────────────────────────┘
```

### 6.2 Key Design Principles

**Separation of codec from I/O.** `RecordCodec` is pure computation: bytes in, bytes out. It has no file handles, no system calls, no side effects. This makes it trivially unit-testable and fuzz-testable. `WalWriter` and `WalReader` handle I/O but delegate all serialization to the codec.

**Injectable file interface.** All file I/O goes through `FileInterface`. Production uses `RealFile` (thin wrapper around POSIX `open/write/read/fdatasync`). Tests use `FaultInjectingFile`, which can simulate partial writes (write only the first N bytes of a record), fsync failures, and latency injection.

**Reader as iterator.** `WalReader::next_record()` returns `std::optional<WalRecord>` — present if a valid record was read, empty if the log is exhausted or the first corruption was encountered. The caller (Recovery) consumes records in a simple while loop. The reader internally tracks its file offset.

---

## 7. Part 3 — Data and Control Flow

### 7.1 Write Path

```
Caller
  │
  ▼
KVStore::put(k, v)
  │
  ├──▶ RecordCodec::encode({PUT, k, v})
  │         │
  │         ▼
  │    raw bytes: [len][op|key_len|key|val_len|val][crc32c]
  │
  ├──▶ WalWriter::append(raw_bytes)
  │         │
  │         ▼
  │    FileInterface::write(raw_bytes)   ← single write() call
  │         │
  │         ▼
  │    (if fsync_policy requires) FileInterface::sync()
  │
  ├──▶ memtable_.insert_or_assign(k, v)   ← update in-memory state
  │
  └──▶ return Status::kOk
```

**Critical ordering:** The WAL append **must** precede the in-memory update. If the process crashes between the WAL write and the memtable update, recovery replays the WAL and the operation is not lost. If the ordering were reversed, a crash after the memtable update but before the WAL write would lose the operation — the in-memory state would have reflected a mutation that has no durable record.

**Single write() call:** The entire record (length + payload + checksum) is assembled in a contiguous buffer and written in one `write()` system call. This does not guarantee atomicity (the kernel may perform a partial write), but it minimizes the window and avoids interleaving if we later add concurrent writers.

### 7.2 Recovery Path

```
Process Start
  │
  ▼
Recovery::recover(log_path)
  │
  ├──▶ WalReader::open(log_path)
  │
  ├──▶ loop:
  │       │
  │       ▼
  │    WalReader::next_record()
  │       │
  │       ├── Some(record) ──▶ apply to KeyValueMap
  │       │                        PUT → map[k] = v
  │       │                        DEL → map.erase(k)
  │       │
  │       └── None ──▶ break (clean end or first corruption)
  │
  ├──▶ truncate log to last valid record offset
  │       (removes the partial/corrupt tail so new appends
  │        start from a clean boundary)
  │
  └──▶ return KeyValueMap
```

**Truncation after recovery:** After replaying the valid prefix, the log file is truncated to the byte offset immediately after the last valid record. This serves two purposes: (1) new appends don't follow corrupt data, and (2) the next recovery of the same file won't re-encounter the same corruption.

### 7.3 Snapshot + Rotation Flow (Stretch)

```
Trigger: active_log.size() >= rotation_threshold
  │
  ▼
KVStore::rotate()
  │
  ├──▶ SnapshotWriter::write(memtable_, snapshot_path)
  │         └── single file write + fsync
  │
  ├──▶ fsync snapshot file
  │
  ├──▶ WalWriter::rotate()
  │         ├── close current log segment
  │         └── open new log segment
  │
  └──▶ delete old log segments + old snapshots
           (only after new snapshot is confirmed durable)
```

**Ordering discipline:** The snapshot must be durable before the old log is deleted. Otherwise, a crash between log deletion and snapshot write loses data. The sequence is: write snapshot → fsync snapshot → open new log → delete old log. The old log is the safety net until the snapshot is confirmed.

---

## 8. Part 4 — System Invariants

These invariants must hold at all times. Every test — unit, integration, scale — is ultimately verifying one or more of these.

### INV-1: WAL-Memtable Consistency

> The in-memory key-value map is always equal to the result of replaying the valid prefix of the WAL from the beginning.

Formally: `memtable == replay(wal[0..last_valid_record])`. This is the master invariant. All other invariants serve it.

### INV-2: Record Integrity

> Every record that passes checksum verification has the exact bytes that were originally written. No bit has been flipped, no byte truncated, no field corrupted.

The contrapositive is equally important: every corrupt record **must** fail checksum verification. This is the checksum's job.

### INV-3: Prefix Validity

> The set of records recovered after a crash is always a prefix of the records that were successfully appended before the crash. No suffix records are recovered while an earlier record is missing or corrupt.

This follows directly from the halt-at-first-corruption strategy (ADR-3).

### INV-4: Write-Before-Apply

> A mutation is present in the WAL before it is reflected in the memtable. Equivalently: there is no state observable via `get()` that would be lost if the process crashed immediately.

Note: this invariant holds per-operation only under `fsync=every`. Under batched or no fsync, the WAL write may be in the OS page cache but not on durable storage, so a power loss (not just a process crash) can lose recent operations. This is expected and documented in SLO-4.

### INV-5: Monotonic Log Growth

> The WAL file size never decreases during normal operation. It only decreases during recovery truncation (removing a corrupt tail).

This is a consequence of append-only writes and provides a useful sanity check: if the file size is less than the last known size without a recovery event, something is wrong.

### INV-6: Snapshot-Log Continuity (Stretch)

> After a rotation, the snapshot reflects exactly the state as of the last record in the previous log segment. The new log segment's first record is the operation immediately following the snapshot.

Formally: `replay(snapshot) == replay(old_log[0..end])`, and `replay(snapshot, new_log) == replay(old_log, new_log)`.

---

## 9. Part 5 — Unit Testing Strategy (gtest)

Each component is tested in isolation. Tests are deterministic (no real I/O, no threads, no timing dependencies) and fast (milliseconds each).

### 9.1 RecordCodec Tests

**What we're verifying:** INV-2 (record integrity), plus basic correctness of encode/decode.

| Test Case | Description | Invariant |
|-----------|-------------|-----------|
| `RoundTrip_Put` | Encode a PUT record, decode it, verify fields match | INV-2 |
| `RoundTrip_Del` | Encode a DEL record, decode it, verify fields match | INV-2 |
| `RoundTrip_EmptyKey` | Key = "", Value = "" — edge case for zero-length fields | INV-2 |
| `RoundTrip_LargePayload` | 1 MB key, 10 MB value — test at size boundaries | INV-2 |
| `RoundTrip_BinaryData` | Key/value containing null bytes, 0xFF, etc. | INV-2 |
| `Checksum_DetectsBitFlip` | Encode record, flip one bit in payload, verify checksum fails | INV-2 |
| `Checksum_DetectsLengthCorruption` | Encode record, corrupt length field, verify checksum fails | INV-2 |
| `Checksum_DetectsTruncation` | Encode record, truncate last N bytes, verify detection | INV-2 |
| `Checksum_DetectsZeroFill` | Overwrite tail with zeros (simulating sparse-file behavior) | INV-2 |
| `Decode_RejectsGarbage` | Random bytes should fail validation, not crash | INV-2 |

### 9.2 WalWriter Tests (with `FaultInjectingFile`)

| Test Case | Description | Invariant |
|-----------|-------------|-----------|
| `Append_SingleRecord` | Write one record, read back bytes, verify structure | INV-5 |
| `Append_MultipleRecords` | Write N records, verify file contains N contiguous valid records | INV-5 |
| `Append_WriteFails` | Inject write() failure; verify writer returns error, file not extended | INV-5 |
| `Sync_CalledPerPolicy` | Verify fsync is called at correct intervals per policy setting | — |

### 9.3 WalReader Tests (with synthetic log files)

| Test Case | Description | Invariant |
|-----------|-------------|-----------|
| `ReadAll_ValidLog` | Pre-built log with N records; reader yields all N | INV-3 |
| `ReadAll_EmptyLog` | Zero-byte file; reader yields nothing, no error | INV-3 |
| `StopAtCorruption_MidPayload` | Log with record whose payload is truncated mid-way | INV-3 |
| `StopAtCorruption_MidLength` | Log truncated in the middle of a length prefix | INV-3 |
| `StopAtCorruption_BadChecksum` | Valid length and payload but wrong checksum | INV-2, INV-3 |
| `StopAtCorruption_ZeroLength` | Length field = 0 (degenerate case) | INV-3 |
| `ValidPrefix_BeforeCorruption` | 5 valid records, then corruption; verify exactly 5 yielded | INV-3 |

### 9.4 Recovery Tests

| Test Case | Description | Invariant |
|-----------|-------------|-----------|
| `Recover_EmptyLog` | Empty log → empty map | INV-1 |
| `Recover_PutsOnly` | N PUT records → map has N entries | INV-1 |
| `Recover_PutThenDel` | PUT k, DEL k → map does not contain k | INV-1 |
| `Recover_OverwriteKey` | PUT k v1, PUT k v2 → map[k] == v2 | INV-1 |
| `Recover_TruncatedTail` | Valid prefix + torn final record → map reflects prefix only | INV-1, INV-3 |
| `Recover_TruncatesFile` | After recovery, file size equals last valid record offset | — |

---

## 10. Part 6 — Integration Testing Strategy

Integration tests exercise the full system (KVStore) end-to-end, including real file I/O, and verify behavioral correctness against an oracle.

### 10.1 Oracle Model

Maintain a parallel `std::map<std::string, std::optional<std::string>>` as the gold-standard reference. Every `put` and `del` applied to the KVStore is also applied to the oracle. After every operation (or batch of operations), assert that the KVStore's state matches the oracle.

### 10.2 Crash Simulation Tests

**Approach:** Use `FaultInjectingFile` to simulate a crash at every possible byte offset within a write.

```
For each record R in a sequence of N records:
    For each byte offset B in record R:
        1. Write records 0..R-1 fully
        2. Write the first B bytes of record R, then "crash" (stop writing)
        3. Create a new KVStore, recover from the log
        4. Assert: KVStore state == oracle state after records 0..R-1
```

This is O(N × avg_record_size) test cases, but each is fast (in-memory file). It exhaustively verifies INV-3.

### 10.3 Randomized Workload Tests

Generate random sequences of operations (PUT with random keys/values, DEL of random existing keys) of length 10,000+. After each operation, optionally simulate a crash (with configurable probability). Verify oracle consistency after every recovery.

### 10.4 Concurrent Access Tests (Stretch)

If the system is extended to support concurrent readers during writes, test with multiple reader threads issuing `get()` while a writer thread issues `put()/del()`. Verify that readers never observe a state that violates INV-1 — specifically, no reader should see a DEL-ed key that was DEL-ed before a PUT they've already observed.

### 10.5 Fsync Policy Integration Tests (Stretch)

For each fsync policy, run a workload and then simulate a power loss (as opposed to a process crash). Verify that the recovered state is consistent (INV-1) and that the data loss is within the bounds declared by SLO-4 for that policy.

---

## 11. Part 7 — Scale Testing Strategy

The operating principle: **failure is the default; correctness must be engineered.**

### 11.1 Philosophy

Scale testing is not about proving the system works at scale. It is about finding the scale at which the system breaks, characterizing the failure modes, and driving design changes. Every scale test should be expected to find a bug or a performance cliff. If your scale tests always pass, they are not aggressive enough.

### 11.2 Stress Dimensions

| Dimension | What It Stresses | What Breaks |
|-----------|-----------------|-------------|
| **Record count** (10⁶–10⁹) | Log file size, recovery time, memory during replay | Recovery OOM, file offset overflow (if using 32-bit), slow sequential scan |
| **Record size** (1 B – 100 MB) | Single-record write latency, buffer allocation, checksum computation time | Memory allocation failure, write() partial return, checksum timeout |
| **Key cardinality** | In-memory map size after recovery | OOM, hash map rehash stalls, bucket collision chains |
| **Write rate** (sustained 100 K+ ops/s) | I/O subsystem, fsync queueing, CPU for checksums | Throughput cliff when fsync batches overlap, write amplification from OS page cache pressure |
| **Crash frequency** (crash every N ops) | Recovery logic, log truncation, re-open latency | File descriptor leaks, truncation races, accumulation of partial tails |

### 11.3 Specific Scale Tests

**Test: Recovery at 1M, 10M, 100M records.** Measure wall-clock recovery time. Plot the curve. It should be linear in record count. If it's superlinear, there's a bug (likely in the hash map or the I/O pattern).

**Test: Sustained write throughput.** Write 10M small records (64 B payload) with each fsync policy. Measure: ops/sec over time (should be flat, not degrading), p50/p99/p999 latency, and total bytes written to device (write amplification). If throughput degrades over time, suspect OS page cache eviction or log file fragmentation.

**Test: Crash-recovery cycle.** In a loop: write 10K records → kill -9 → recover → verify oracle → repeat. Run for 1,000 iterations. This finds resource leaks (file descriptors, memory not freed on recovery path), state accumulation bugs, and truncation logic errors.

**Test: Maximum record size.** Write a single record with a 1 GB value. Verify encode, write, read, decode, and checksum all work. This tests that no internal buffer has a 32-bit size assumption and that `write()` correctly handles partial returns for large buffers.

### 11.4 Observability During Scale Tests

All scale tests should emit structured metrics (JSON or Prometheus format):

- Throughput (ops/s) sampled every second
- Latency histogram (p50, p90, p99, p999)
- Recovery time per iteration
- Memory RSS before and after recovery
- File sizes (log, snapshot)

These metrics are the primary output of a scale test, not just pass/fail. A "passing" scale test with a latency p999 that doubled from the previous run is a regression.

---

## 12. Part 8 — Durability Guarantees

This section addresses the four canonical durability failure modes and the WAL system's posture toward each.

### 12.1 Partial Failure (Torn Writes)

**Threat:** The process or OS crashes in the middle of a `write()` call. The file now contains a prefix of the record's bytes but not all of them.

**Mitigation:** The record format (length + payload + checksum) is designed precisely for this. There are three subcases:

1. **Crash within the length field (first 4 bytes):** The length field is garbage. On recovery, the reader attempts to read `payload_len` bytes, either gets fewer bytes than requested (short read → corruption detected) or reads the wrong number of bytes and the checksum fails.

2. **Crash within the payload:** The reader reads `payload_len` bytes, either gets fewer (short read → detected) or reads bytes that include data from a previous write cycle (stale data), and the checksum fails.

3. **Crash within the checksum:** The reader reads the full payload successfully but the checksum bytes are truncated or corrupt. Checksum verification fails.

In all three cases, recovery halts and the torn record (plus anything after it) is discarded. Post-recovery truncation cleans the file.

**Residual risk:** Filesystem-level corruption that alters multiple records in a way that preserves CRC-32C consistency. The probability of an accidental CRC-32C collision is 1 in 2³², approximately 2.3 × 10⁻¹⁰ per corrupt record. For a billion records, the expected number of undetected corruptions is ~0.23. This is acceptable for non-safety-critical storage. If it's not, the mitigation is to use a 64-bit or 128-bit checksum (xxHash128), which we can adopt in a v2 record format.

### 12.2 Split-Brain

**Threat:** Two processes believe they are the active writer to the same log file, producing interleaved or conflicting records.

**Posture:** The MVP is a single-process, single-writer system. Split-brain is prevented by process-level exclusion, not by the WAL format.

**Mitigation (production hardening):** Acquire an `flock()` (advisory file lock) on the WAL file at startup. If the lock cannot be acquired, refuse to open the WAL. This is not Byzantine-fault-tolerant — advisory locks can be bypassed — but it catches the common case (two instances of the same binary started by accident).

**Mitigation (distributed, out of scope):** In a distributed system, split-brain prevention is the responsibility of the consensus layer (Raft, Paxos, etc.), not the WAL. The WAL is a local durability mechanism. The consensus log and the local WAL are separate abstractions: the consensus log determines the total order of committed operations across replicas, and each replica's WAL ensures local durability of its applied operations.

### 12.3 Node Loss

**Threat:** The entire node is permanently destroyed (disk failure, fire, decommissioning). All data on the node, including the WAL and snapshots, is lost.

**Posture:** A single-node WAL provides no protection against node loss. This is a fundamental limitation, not a bug.

**Mitigation (operational):**

- **Replication:** The WAL is replicated to at least N other nodes (typically N=2 for 3-way replication). Replication can be synchronous (commit only after N+1 nodes acknowledge) or asynchronous (commit locally, replicate in the background). Synchronous replication provides RPO=0 (zero data loss). Asynchronous replication provides RPO equal to the replication lag.

- **Backup:** Periodic snapshots are copied to a durable object store (S3, GCS). This provides disaster recovery but with a data loss window equal to the snapshot interval.

**The WAL's role in node loss recovery:** When a replacement node is provisioned, it receives a snapshot from a surviving replica or from backup, then replays the replication stream to catch up. The local WAL on the new node begins recording from that point. The WAL does not need to be shipped — only the logical operations matter.

### 12.4 Rebalancing Events

**Threat:** A key range is migrated from one node to another (due to cluster rebalancing, scaling, or topology changes). During migration, both the source and destination may have partial state for the migrating keys.

**Posture:** Rebalancing is a distributed-systems concern that sits above the WAL layer. The WAL is not aware of key ranges or ownership — it records all mutations unconditionally.

**Mitigation (integration contract):**

The rebalancing protocol must ensure:

1. **Freeze-then-transfer:** The source freezes writes to the migrating key range (returns errors or redirects to the destination), transfers the snapshot of those keys to the destination, then releases ownership. During the freeze, the source's WAL still contains the historical mutations, but no new ones are appended for those keys.

2. **Destination WAL continuity:** The destination ingests the transferred snapshot, applies it to its memtable, and begins recording new mutations for those keys in its own WAL. There is no gap: the snapshot is the bridge between the source's history and the destination's future.

3. **Source cleanup:** After the destination confirms it has taken ownership, the source can eventually compact away records for the migrated keys. This is a garbage collection concern, not a correctness one — the source's WAL still contains valid records for those keys, they're just no longer relevant.

**Failure during rebalancing:** If the rebalancing coordinator crashes, the protocol must be idempotent. The destination should be able to discard partially received state and re-request the transfer. The source should remain the owner until it receives confirmation that the destination has committed.

---

## 13. SRE Operational Concerns

### 13.1 Monitoring and Alerting

| Metric | Alert Threshold | Rationale |
|--------|----------------|-----------|
| `wal.write_latency_p99` | > 50 ms sustained for 5 min | Indicates fsync stalls, storage degradation, or I/O contention |
| `wal.recovery_duration_seconds` | > SLO-3 target | Log is too large; rotation may be failing |
| `wal.corrupt_records_on_recovery` | > 0 | Expected after a crash, but recurring occurrences without crashes indicate storage hardware issues |
| `wal.active_log_size_bytes` | > 2× rotation threshold | Rotation is failing or not triggering |
| `wal.fsync_error_count` | > 0 | Storage subsystem is returning errors; data loss risk |
| `wal.checksum_failures_in_flight` | > 0 | Should never happen outside recovery; indicates memory corruption or a bug |

### 13.2 Runbook Entries

**Scenario: Recovery takes too long.** Check `wal.active_log_size_bytes`. If the log is large, rotation may have failed. Manually trigger snapshot + rotation. If the log is reasonably sized, check I/O throughput — the disk may be degraded.

**Scenario: Repeated `corrupt_records_on_recovery` without known crashes.** Suspect storage hardware. Check `smartctl` for drive errors. Check kernel logs for I/O errors. Consider migrating the node's data and decommissioning the hardware.

**Scenario: `fsync_error_count` > 0.** The storage device is refusing fsync. This is a critical durability risk. All data written since the last successful fsync may be lost on crash. Alert on-call immediately. Consider draining the node and failing over.

### 13.3 Capacity Planning

- **Disk space:** WAL growth rate = write rate × average record size. With rotation at 64 MB and one snapshot retained, steady-state disk usage is: snapshot (~= memtable size) + active log (≤ 64 MB) + one old log (≤ 64 MB during rotation).

- **Recovery time budget:** At ~500 MB/s sequential read throughput (typical NVMe), a 64 MB log recovers in ~130 ms. A 1 GB log (no rotation) takes ~2 s. Plan rotation thresholds to stay within SLO-3.

- **Memory:** The memtable's memory usage is proportional to the number of live keys × (average key size + average value size + hash map overhead). For 1 M keys with 64 B keys and 256 B values, expect ~400 MB.

### 13.4 Incident Response: Data Loss

If a crash occurs and recovery reports discarded records:

1. The discarded records represent the operations that were accepted by the process but not yet durable at crash time.
2. Under `fsync=every`, this is at most the one in-flight operation at crash time.
3. Under `fsync=batched`, this is at most one batch interval worth of operations.
4. There is no mechanism to recover these operations from the local node. If replication is in place, the operations may exist on a replica.
5. Log the count and nature of discarded records in a post-incident report.

---

## 14. Tradeoff Analysis Summary

### 14.1 Core Tradeoffs

| Tradeoff | Option A | Option B | Our Choice | Rationale |
|----------|----------|----------|------------|-----------|
| **Format simplicity vs. mid-log recovery** | Length-prefixed (simple, halt at first corruption) | Block-based (complex, can skip past corruption) | Length-prefixed | Corruption at the tail loses ≤1 op; the complexity of block-based recovery is not justified for MVP |
| **Durability vs. throughput** | Per-op fsync (zero loss window) | Batched/no fsync (higher throughput) | Configurable knob (default batched) | Different deployments have different requirements; forcing a single policy is wrong |
| **Memory overhead vs. dependency count** | `std::unordered_map` (stdlib, higher overhead) | Abseil `flat_hash_map` (lower overhead, external dep) | `std::unordered_map` | No external dependencies in MVP; swap later if profiling warrants |
| **Recovery speed vs. implementation complexity** | Full log replay every time | Snapshots + incremental replay | Full replay for MVP; snapshots stretch | Bounded recovery is important but not critical for small-to-medium datasets |
| **Checksum strength vs. speed** | CRC-32C (fast, HW-accelerated, 32-bit) | SHA-256 (cryptographic, slow) | CRC-32C | Threat model is accidental corruption, not adversarial; 50x speed difference matters |

### 14.2 What We're Deliberately Not Doing (and Why)

- **No concurrent writers.** Concurrent WAL access requires a mutex on the write path (serializing appends) or a lock-free ring buffer. Both add complexity. Single-writer is the right starting point.

- **No compression.** Compressing individual records adds CPU cost on the write path and complicates the record format (compressed length vs. uncompressed length). Compression is more effective at the snapshot or log-segment level, which can be added as a stretch goal.

- **No encryption.** Encryption at rest is better handled at the filesystem or block-device level (dm-crypt, LUKS, EBS encryption). Application-level encryption of the WAL would prevent tools from inspecting the log for debugging.

- **No WAL shipping (replication).** Replication is a distributed-systems concern that belongs in a layer above the WAL. The WAL provides local durability. A replication layer would read committed records from the WAL and ship them to replicas, but this is a separate module with its own design document.

- **No backward compatibility versioning in the record format.** The MVP record format has no version field. If we change the format, we create a new format and migrate. For an MVP, this is the right call — adding a version field adds a byte to every record and forces us to design a migration strategy for a format that doesn't exist yet.

---

*End of document.*
