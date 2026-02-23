## 1) Minimal architecture

**Core objects**

* **Memtable** (in-memory, mutable): `key -> (value | tombstone)` kept in sorted order.
* **WAL** (optional but usually necessary for durability): append-only log of writes before memtable apply.
* **SSTables** (immutable, on-disk): sorted key/value runs produced by flush.
* **Manifest / Version**: authoritative list of current SSTables (atomic publish) for snapshot reads.
* **Compaction (optional for “mini”, but you’ll want at least level-0 compaction soon):** merges SSTables to bound read amplification.

**Write path**

1. Append record to WAL (fsync policy configurable).
2. Apply to memtable.
3. If memtable exceeds threshold → **freeze** it and **flush** to a new SSTable.

**Read path (as you specified)**

1. Lookup in mutable memtable.
2. Lookup in immutable “frozen memtables” (if a flush is in progress).
3. Search SSTables newest → oldest, stop at first hit (tombstone counts as hit).

---

## 2) Write amplification tradeoffs (what knobs matter)

Even in a mini-engine, these are the key tradeoffs:

### A) No compaction (flush-only)

* **Write amp:** ~1× (memtable → SSTable once).
* **Read amp:** grows with number of SSTables (worst-case many disk probes + bloom checks).
* **Space amp:** tombstones and overwritten keys persist forever.

Good for a teaching engine; not good for sustained workloads.

### B) Level-0 + compaction into Level-1 (recommended “mini+”)

* **Write amp:** higher (keys rewritten during compaction).
* **Read amp:** bounded (few levels).
* **Space amp:** bounded (old versions and tombstones eventually dropped).

Knobs:

* Memtable size (flush frequency)
* L0 file count trigger
* Target file size
* Compaction fan-in and level size ratio

---

## 3) SSTable file format (simple but real)

A practical SSTable layout:

```
[Data Blocks...]
[Index Block]
[Meta Block]          (optional: bloom filter + properties)
[Footer]
```

### Data block (sorted entries)

* Prefix-compressed keys (restart array every R entries)
* Values stored verbatim (or optional compression later)

Example entry encoding:

* `shared_prefix_len` (varint)
* `unshared_key_len` (varint)
* `value_len` (varint; 0 means tombstone if you prefer)
* `unshared_key_bytes`
* `value_bytes`

Block trailer:

* restart offsets array
* `num_restarts`

### Index block

One entry per data block:

* `last_key_in_block` (or separator key)
* `block_offset` + `block_size`

### Meta block

* Bloom filter bits + k
* Properties: min/max key, num entries, sequence range, etc.

### Footer

* fixed-size pointers to index/meta blocks + magic number (for validation)
* e.g. 48–64 bytes, easy mmap read

This gives you fast:

* binary search over index → single data block read → within-block search using restart points.

---

## 4) Memtable design

Pick one:

* **std::map / std::vector + sort-on-freeze** (simplest)
* **Skiplist** (classic LSM)
* **B-tree** (cache friendly)

For a mini-engine: `std::vector<Entry>` append + periodic sort is surprisingly good if you only need point lookups and can tolerate sort cost at flush.

**Internal key**
To resolve overwrites across sources, store an internal key:

* `(user_key, sequence, kind)`
  where `kind` is {PUT, DEL}. Higher `sequence` wins.
  This avoids needing “newest → oldest” scans inside an SSTable: each SSTable can contain multiple versions, but reads pick the latest by sequence.

---

## 5) Iterator merging (the heart of LSM)

You want these iterators:

* `MemIter`: over memtable internal keys
* `TableIter`: over one SSTable (seeks + next)
* `MergingIter`: merges many sorted child iterators by internal key

### For point lookup

A fast path:

1. Check memtable hash index (optional) or ordered lookup.
2. For SSTables: use bloom → if maybe, do index lookup → data block lookup.

### For range scan

Use a **k-way merge**:

* Maintain a min-heap keyed by `internal_key`.
* Pop smallest, advance that iterator, push back.
* While emitting, **collapse duplicates** for the same user_key by taking highest-seq visible entry and skipping older ones.
* Stop emitting if latest entry is a tombstone.

### De-dup rule

When heap top changes user_key:

* emit exactly one record for that user_key (or none if tombstone)
* discard all other entries with same user_key from other iterators until next key

This requires comparing by `(user_key asc, sequence desc)` so that newest versions come first for equal user_key.

---

## 6) Minimal compaction (if you add one thing)

Implement **L0→L1 compaction**:

* Pick a set of L0 files (usually overlapping key ranges) and overlapping L1 files.
* Merge them with iterators into new L1 SSTables of target size.
* Drop overwritten/hidden entries and apply tombstone elision when safe.

Even a single compaction step dramatically improves read performance and bounds file count.

---

## 7) Suggested C++ module split

* `wal/` append + recovery
* `memtable/` (mutable + frozen)
* `sstable/`

  * builder (write blocks, index, meta, footer)
  * reader (seek, iterate)
  * bloom
* `version/manifest` atomic publish of current file set
* `iter/` merging iterator + internal key comparator
* `compaction/` (optional L0→L1)

