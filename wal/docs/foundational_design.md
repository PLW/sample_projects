## WAL for KV mutations with torn-write tolerance

### Summary

We want a per-node, append-only write-ahead log (WAL) that persists `PUT k v` and `DEL k` mutations, supports crash recovery even if the last write is partially torn, and rebuilds the in-memory KV state by replaying records until the first invalid/partial record (then **stop safely**). Stretch goals add log rotation + snapshots and an explicit durability/throughput knob (fsync policy). The design also states what durability means in a distributed setting (split-brain, node loss, rebalancing), even though the WAL itself is a local primitive.

---

# Part 0: Scope, assumptions, and explicit non-goals

### In scope (this document)

* Local durable log format and recovery rules.
* Append path, checksum policy, and torn-write handling.
* Snapshot + rotation mechanics (stretch).
* Tests including simulated torn writes.
* Operational guidance (SRE) and SLOs.

### Assumptions

* Single writer per WAL file (or a single logical writer with serialization).
* WAL is stored on a local filesystem that supports `fsync`/`fdatasync`.
* Keys/values are byte strings; we don’t assume ordering.
* Recovery builds an in-memory map (or hands mutations to a state machine).

### Non-goals

* Replication protocol, consensus, and cross-node commit correctness (we describe interfaces/expectations, but not implement Raft/Paxos here).
* Encryption/compression (can be layered later).
* Online compaction of the KV state (beyond snapshotting).

---

# Part 1: Data structures

## 1.1 Record model (logical)

Two mutation types:

* `PUT(key, value)`
* `DEL(key)`

Recovered state:

* `KVState`: map-like structure that can apply `PUT/DEL` in order.

## 1.2 On-disk record framing (physical)

**MVP constraint:** `length + payload + checksum`

We’ll still define a *minimal header* to make “length” unambiguous and support versioning.

**Record layout (recommended MVP):**

* `u32 len` — number of bytes in `payload` (not including checksum)
* `payload` — encoded mutation (type + key length + key + [value length + value])
* `u32 crc32c` — checksum over (`len` + `payload`) *or* over `payload` (see ADR)

Notes:

* Fixed-width integers little-endian.
* `len` includes everything in payload, enabling a single forward scan.
* Checksum validates corruption and helps detect partial writes.

Optional but strongly recommended “near-MVP” fields:

* `u32 magic` at record start to reduce false positives
* `u8 version` and `u8 type` in header
* `u32 header_crc` if header grows

Even if the MVP says “length+payload+checksum”, adding `magic+version` early prevents long-term format traps and makes resync possible later (even if MVP recovery stops at first corruption).

## 1.3 File structure

* One active WAL file at a time: `wal-<sequence>.log`
* Monotonic sequence in filename (or stored in manifest).
* Append-only. No in-place updates.

---

# Part 2: Modules, classes, methods (high level)

### `WalWriter`

Responsibilities:

* Serialize mutations to bytes.
* Append record atomically *as a sequence of writes*.
* Enforce fsync policy.
  Key methods:
* `append_put(key, value)`
* `append_del(key)`
* `flush()` (optional external trigger)
* `rotate()` (stretch)

### `WalReader`

Responsibilities:

* Sequentially scan a WAL file.
* Validate `len` bounds and checksum.
* Return decoded mutations until first invalid/partial record.
  Key methods:
* `scan_until_corruption()` → stream of mutations + stop reason + last_good_offset

### `RecoveryManager`

Responsibilities:

* On startup, identify WAL(s) and snapshot.
* Load snapshot (if present), then replay WAL segments in order.
* Stop safely at first corrupt record in the last segment (or earlier if segments are individually corrupt).
  Key methods:
* `recover()` → `KVState` + recovery report

### `SnapshotManager` (stretch)

Responsibilities:

* Create point-in-time snapshot of KV state.
* Coordinate “snapshot barrier” with writer so WAL position is known.
  Key methods:
* `create_snapshot()`
* `load_snapshot()`

### `WalManifest` (stretch, recommended)

Responsibilities:

* Track active WAL sequence, durable rotation points, snapshot metadata.
  Key methods:
* `load()`, `persist()`

---

# Part 3: Data and control flow

## 3.1 Write path (PUT/DEL)

1. Client calls `KVStore.put(k,v)` (or `del(k)`).
2. `WalWriter` encodes mutation → `(len, payload, checksum)`.
3. Append to file descriptor (buffered or direct).
4. Apply fsync policy:

   * `ALWAYS`: fsync per mutation (slowest, strongest local durability).
   * `PER_BATCH`: fsync every N records or every T milliseconds.
   * `NEVER`: rely on OS flush (fastest, weakest).
5. Only after “durable enough” per policy do we ACK to caller (ADR-defined).

## 3.2 Startup recovery

1. `RecoveryManager` loads manifest (if used).
2. Load snapshot (if any) → base state.
3. Enumerate WAL segments in sequence order.
4. For each WAL segment:

   * Scan records sequentially:

     * Read `len`, verify it’s within bounds.
     * Read payload of length `len`.
     * Read checksum and verify.
   * Apply mutation to `KVState` for each valid record.
   * **Stop safely** at first invalid/partial record (MVP).
5. Report: last good offset, number of applied records, stop reason.

---

# Part 4: Invariants

## 4.1 Safety invariants (must always hold)

* **I1: Prefix correctness** — Recovery applies a prefix of the successfully written records, never a “made-up” record.
* **I2: Stop-on-corruption** — On encountering any invalid framing/checksum/partial read, recovery stops without applying that record or anything after it in that file.
* **I3: Deterministic replay** — Given the same bytes on disk, recovery produces the same KV state.
* **I4: Monotonic append** — Writer only appends; it never overwrites earlier bytes.
* **I5: Bounded length** — `len` must be ≤ configured max record size; otherwise treat as corruption.
* **I6: Snapshot + WAL ordering (stretch)** — A snapshot corresponds to a precise WAL position; replay begins strictly after that position.

## 4.2 Liveness / operational invariants

* **I7: Writer progress** — Under healthy disk, appends eventually succeed; under disk-full, system fails fast with explicit error.
* **I8: Rotation bounds (stretch)** — WAL disk usage remains under configured limits via rotation + snapshot.

---

# Part 5: Unit-testable components (gtest)

### Encoding/decoding

* `RecordCodec`: encode/decode `PUT/DEL`, verify round-trips.
* `Checksum`: compute/verify checksum on known vectors.

### Framing and scanning

* `WalScanner` given a byte buffer:

  * Stops at partial header.
  * Stops if payload shorter than `len`.
  * Stops if checksum mismatch.
  * Rejects oversize `len`.
  * Accepts multiple valid records.

### Recovery behavior

* `RecoveryApplier`:

  * Applies a mutation stream to a model map.
  * Verifies idempotence constraints if needed (generally replay is not idempotent unless you define it so; we treat log as the source of truth).

### Fsync policy logic (stretch)

* Test that “ack rules” match configured policy (e.g., `PER_BATCH` acks after flush boundary).

---

# Part 6: Integration testing strategy (gtest)

## 6.1 Gold-standard model checking

* Maintain a pure in-memory reference model that applies the same operations.
* Randomized sequence of ops:

  * Run operations, append to WAL, update model.
  * Crash simulation (below).
  * Recover from WAL and compare recovered state to *expected prefix* behavior.

## 6.2 Crash + torn-write simulation

Core idea: after some writes, create a “damaged WAL image” that mimics real crashes:

* **Truncation at arbitrary byte** (most important).
* **Torn sector** simulation: overwrite a middle region with zeros or old bytes.
* **Bit flips**: mutate random bytes to force checksum mismatch.
* **Partial checksum**: truncate inside checksum field.

Assertions:

* Recovery stops at first invalid/partial record.
* All records before that point match the reference model state.
* No records after corruption are applied.

## 6.3 Concurrency tests

Even if single-writer is assumed:

* Multi-threaded callers that enqueue ops to a single writer thread.
* Ensure serialization order is the order of application and replay.

## 6.4 Restart storms

* Repeated cycles: write N ops → crash → recover → continue writing.
* Ensures recovery is stable and doesn’t regress with long histories.

---

# Part 7: Scale testing (“default mode is failure”)

### Soak tests

* Run for hours with high write rate, periodic crashes, periodic snapshots/rotations.
* Track recovery time and WAL growth.

### Fault injection matrix

* Disk full / quota exceeded.
* fsync latency spikes.
* File descriptor errors (EIO).
* Slow storage and intermittent failures.

### Large-record stress

* Near max record size.
* Many small records (metadata overhead).
* Mixed workloads.

### Observability-driven validation

* Emit metrics for:

  * append rate, bytes/sec
  * fsync rate + latency histograms
  * corruption stop events
  * recovery duration
  * last_good_offset per segment

---

# Part 8: Durability guarantees in distributed realities

The WAL is a **local durability primitive**. Distributed guarantees require higher layers, but the WAL must expose the right “commit points”.

## 8.1 Partial failure (single node crash mid-write)

Guarantee (MVP):

* After crash, recovered state equals the application of some prefix of fully validated records.
* Any partially written last record is discarded.

## 8.2 Split-brain

WAL alone cannot prevent two leaders from accepting writes. To make WAL compatible with split-brain prevention:

* Include a **term/epoch** (from consensus or lease) in each record or in segment metadata.
* Recovery should expose the maximum observed term and last applied index (if provided).
* Higher layer must enforce **fencing**: only the current leader term can append.

## 8.3 Node loss

* WAL ensures node-local durability if storage survives.
* If node is lost, durability depends on replication factor and quorum policy, not WAL.
  Design hook:
* Provide “durable_lsn” (log sequence number) that can be used by replication to know what is safely persisted.

## 8.4 Rebalancing events

During shard movement:

* Snapshot + WAL shipping can be used:

  * Copy snapshot at known WAL position.
  * Stream subsequent WAL records from that position.
* Requires stable segment naming/LSN and consistent snapshot barrier semantics.

---

# ADRs (Architecture Decision Records)

## ADR-001: Stop-at-first-corruption vs resynchronization

**Decision:** MVP stops at first corrupt/partial record.
**Why:** Simple, provably safe, matches requirement, and keeps recovery logic tight.
**Tradeoff:** You may lose valid records that appear after a corrupted region (rare but possible).
**Future:** Add optional resync scanning using `magic` + bounds checks.

## ADR-002: Checksum algorithm

**Decision:** Use a fast, well-supported checksum (CRC32C preferred).
**Why:** CRC32C is widely used in storage systems, fast with hardware acceleration on many platforms, and good at detecting common corruptions.
**Tradeoff:** Not cryptographic (fine for integrity, not authenticity).
**Alternative:** xxHash (faster but less “standard” as an integrity checksum), or SHA (too slow).

## ADR-003: Checksum coverage

**Decision:** Checksum covers `len + payload`.
**Why:** Detects corruption in length (which otherwise can cause wild reads) and payload.
**Tradeoff:** Slightly less flexible if you later change header fields; mitigated by versioning/magic.

## ADR-004: Record length limits

**Decision:** Enforce `max_record_bytes` and treat violations as corruption.
**Why:** Prevents OOM, pathological seeks/reads, and exploitation by corrupt length.
**Tradeoff:** Large values must be chunked or stored differently.

## ADR-005: ACK point and fsync policy

**Decision:** Make durability explicit via a knob; define what “acknowledged” means per mode:

* `ALWAYS`: ack after fsync completes for that record.
* `PER_BATCH`: ack after batch fsync boundary.
* `NEVER`: ack after write() to OS buffers.
  **Why:** Lets deployments choose correctness/perf tradeoff intentionally.
  **Risk:** `NEVER` can lose acknowledged writes on power loss.

## ADR-006: Rotation + snapshot (stretch)

**Decision:** Use periodic snapshots to bound recovery time, and rotate WAL segments based on size/time.
**Why:** Without snapshotting, recovery time grows unbounded with log length.
**Tradeoff:** Snapshot creation cost; must coordinate consistent snapshot point.

## ADR-007: Single-writer serialization

**Decision:** One logical append stream per WAL file.
**Why:** Simplifies torn-write reasoning and record boundaries; avoids interleaving hazards.
**Alternative:** Multi-writer with locking or per-thread logs + merge (more complex).

---

# NFRs (Non-Functional Requirements)

### Correctness & safety

* Must never apply a record that fails checksum or is partially written.
* Recovery must be deterministic and safe under arbitrary truncations.

### Performance

* Append throughput scalable to high write rates (subject to fsync mode).
* Recovery throughput linear in WAL size, with bounded overhead per record.

### Operability

* Clear metrics, logs, and recovery reports.
* Simple on-disk format with versioning.

### Maintainability

* Versioned record schema and forward-compatible framing strategy.
* Clear separation: codec vs IO vs recovery logic.

### Portability

* Works across common filesystems and platforms; avoid relying on undefined atomicity beyond what `fsync` provides.

---

# SLOs (Service Level Objectives)

These are reasonable starting targets for an MVP; tune to your environment.

### Recovery

* **R1 (MVP):** Recover from last clean shutdown or crash with WAL ≤ 1 GB in **p95 < 2s** on SSD-class storage.
* **R2:** Detect corruption and stop with an explicit reason and offset in **< 100ms** after encountering the first bad record (bounded by IO).

### Durability semantics

* **D1:** In `ALWAYS` mode, acknowledged writes survive process crash; on power loss, survival depends on storage honoring `fsync` semantics.
* **D2:** In `PER_BATCH`, at most one batch of acknowledged writes may be lost on power failure (batch defined by config).

### Availability

* **A1:** WAL append errors surface within **< 1s** to callers (fail fast on disk full / IO error).

---

# SRE: Observability, runbooks, and failure handling

## Key metrics

* `wal_append_records_total`, `wal_append_bytes_total`
* `wal_fsync_total`, `wal_fsync_latency_ms` (histogram)
* `wal_corruption_events_total` with labels: `checksum_mismatch`, `truncation`, `len_oversize`, `io_error`
* `recovery_time_ms`, `recovered_records_total`, `recovery_last_good_offset`
* `wal_segment_size_bytes`, `wal_disk_used_bytes`
* Snapshot metrics (stretch): `snapshot_create_time_ms`, `snapshot_size_bytes`

## Logging (structured)

* On recovery: segment name, start/end offsets, stop reason, last_good_offset.
* On corruption: include offset and the failing check (never dump raw key/value by default).

## Alerts

* Any corruption event.
* Recovery time exceeds SLO.
* Disk used exceeds threshold (rotation/snapshot not keeping up).
* fsync latency elevated (predicts tail latency).

## Runbooks (MVP)

1. **Corruption detected on startup**

   * Confirm last_good_offset.
   * Option A (safe): start from prefix; accept data loss after last_good_offset.
   * Option B (manual salvage): copy WAL image, attempt offline tooling (future resync feature).
2. **Disk full**

   * Stop accepting writes; emit clear error.
   * Trigger rotation/snapshot if enabled; otherwise require operator action.
3. **Excessive recovery time**

   * Increase snapshot frequency and/or reduce rotation thresholds.

---

# Concrete design choices you can lock in immediately (to de-risk the MVP)

* Add `magic + version` even if MVP says “length+payload+checksum” (it will save you later).
* Enforce a strict maximum record size and treat violation as corruption.
* Make the fsync policy define *ack semantics* explicitly (no ambiguity).
* Build the test harness around truncation-at-random-byte; it catches most real-world crash edge cases.

