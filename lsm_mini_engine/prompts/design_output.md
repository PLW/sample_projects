## Log-Structured Merge (LSM) Trees — High-Level Introduction

A **Log-Structured Merge tree (LSM tree)** is a write-optimized data structure used in modern storage engines to support **high-throughput inserts, updates, and deletes** on large datasets stored on disk.

It is the core design behind systems such as:

* LevelDB
* RocksDB
* Apache Cassandra
* Apache HBase
* ScyllaDB

Given your ongoing interest in storage engines and compaction-based segment lifecycles, LSM trees are directly aligned with the “immutable segments + background compaction” architecture you've been designing.

---

# 1. Why LSM Trees Exist

## The Core Problem

Disk storage is:

* Slow for **random writes**
* Fast for **sequential writes**

Traditional B-trees perform in-place updates:

* Random disk I/O
* Write amplification
* Poor SSD endurance under heavy write load

LSM trees solve this by:

> Turning many small random writes into large sequential writes.

---

# 2. Big Idea

Instead of updating data in place:

1. Buffer writes in memory.
2. Periodically flush sorted data to disk as immutable files.
3. Merge files in the background.

Hence the name:

* **Log-structured** → append-only, sequential writes
* **Merge tree** → background merging of sorted runs

---

# 3. Core Components

## 3.1 Write Path

### Step 1: WAL (Write-Ahead Log)

* Every write is appended to a **WAL** for durability.
* Sequential disk write.
* Guarantees recovery after crash.

### Step 2: Memtable

* An in-memory sorted structure (usually a skip list or red-black tree).
* Holds recent writes.
* Provides fast reads of newest data.

When full → flushed to disk as an SSTable.

---

## 3.2 SSTables (Sorted String Tables)

An SSTable is:

* Immutable
* Sorted by key
* Stored on disk
* Contains an index and optional Bloom filter

Typical internal layout:

```
+-------------------+
| Data blocks       |
+-------------------+
| Block index       |
+-------------------+
| Bloom filter      |
+-------------------+
| Metadata/footer   |
+-------------------+
```

Properties:

* Immutable after creation
* Enables efficient binary search
* Supports range scans

---

# 4. Read Path

To read key K:

1. Search Memtable
2. Search immutable memtables (if flushing)
3. Search SSTables from newest to oldest

Because data may exist in multiple levels, reads can be expensive unless:

* Bloom filters reduce disk hits
* Index blocks narrow search
* Compaction reduces overlap

---

# 5. Deletes and Updates

LSM trees do not delete immediately.

They insert a **tombstone marker**:

```
Key: X
Value: <TOMBSTONE>
```

Actual removal happens during compaction.

Updates are simply:

* Insert new version of key
* Old version discarded later

---

# 6. Compaction

Compaction merges multiple sorted SSTables into fewer larger ones.

Goals:

* Remove deleted entries
* Remove overwritten versions
* Reduce read amplification
* Maintain sorted structure

Two major strategies:

---

## 6.1 Leveled Compaction (RocksDB style)

```
Level 0: small, overlapping SSTables
Level 1: larger, non-overlapping key ranges
Level 2: 10x larger
Level 3: 10x larger
...
```

Properties:

* Each level ~10× larger than previous
* Except L0, levels have non-overlapping key ranges
* Good read performance
* Higher write amplification

---

## 6.2 Size-Tiered Compaction (Cassandra style)

* Merge files of similar size
* Fewer total rewrites
* Higher read amplification

---

# 7. Amplification Tradeoffs

LSM trees balance three competing costs:

| Type                | Meaning                           |
| ------------------- | --------------------------------- |
| Write Amplification | Data rewritten during compaction  |
| Read Amplification  | Number of files examined per read |
| Space Amplification | Temporary duplicate data          |

Tuning compaction strategy shifts these tradeoffs.

---

# 8. Why LSM Trees Scale

They are ideal when:

* Write-heavy workloads
* Large datasets (>> memory)
* SSD-based storage
* Distributed systems

Because:

* Writes are sequential
* Data is immutable
* Compaction is parallelizable
* Files are naturally shardable

This matches very closely with your distributed vector + inverted index segment model.

---

# 9. Conceptual Model

An LSM tree is essentially:

> A hierarchy of immutable sorted segments + background merging.

Which is exactly the same conceptual model as:

* Hybrid vector search engines
* Log-based storage engines
* Columnar analytics systems

---

# 10. Simple Algorithm Summary

### Insert(K, V)

```
append to WAL
insert into memtable

if memtable full:
    freeze memtable
    flush to SSTable
    schedule compaction
```

---

### Get(K)

```
check memtable
check immutable memtables
for each level:
    check Bloom filter
    if match:
        search SSTable
return newest version
```

---

### Compaction(L_i, L_{i+1})

```
select overlapping SSTables
merge sorted streams
discard tombstones and old versions
write new SSTable(s)
delete old SSTables
```

---

# 11. Strengths and Weaknesses

### Strengths

* Extremely high write throughput
* Crash safe
* Excellent for time-series
* SSD-friendly
* Easy replication (immutable files)

### Weaknesses

* Compaction complexity
* Write amplification
* Read amplification without tuning
* Space overhead

---

# 12. Mental Model

Think of an LSM tree as:

> A continuously self-cleaning pile of sorted files.

New data piles up at the top.
Background processes gradually merge and clean it.

---

# 13. Why It Matters for You

Given your work on:

* Immutable segment architecture
* Tombstone-based deletes
* Background merges
* Distributed vector search

You're effectively implementing:

> A generalized LSM variant for hybrid (vector + inverted) search.

The LSM pattern extends beautifully to:

* ANN index segments
* Posting list segments
* Metadata segments
* Multi-shard coordination

