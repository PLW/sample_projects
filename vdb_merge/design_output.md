## 1) System shape (single-node, segment-based)

**Core idea:** treat the vector store like a Lucene-style immutable-segment index:

* a **mutable in-memory segment** receives new blocks, builds/updates its HNSW incrementally
* once it reaches a **vector-count threshold**, it is **flushed** into a new **immutable persistent segment**
* a background **compaction/merge** policy periodically merges smaller persistent segments into larger ones
* **queries run on a snapshot**: a stable list of “in-play” segments + their search-time state, unaffected by flush/merge until the snapshot advances

This matches the motivation behind immutable segments and merge policies discussed in Elastic’s HNSW merge work. ([Elastic][1])

---

## 2) Data model

### 2.1 Records

Each ingested “text block” becomes one vector point:

```cpp
struct PointId { uint64_t value; };        // stable external id
struct DocId   { uint64_t value; };        // optional grouping id

struct PointRecord {
  PointId id;
  DocId doc;
  uint64_t ts_us;                           // for ordering / debugging
  std::vector<uint32_t> tokens;             // optional: token ids
  std::array<float, D> embedding;           // D fixed at index creation
  // optional payload pointer / offset into a separate store
};
```

### 2.2 Embedding pipeline (your spec)

1. Parse block → tokens
2. For each token: look up its embedding vector from a configured HuggingFace embedding set
3. Aggregate token vectors → block vector (e.g., mean/max, weighted mean, attention-lite, etc.)
4. Insert vector into the active (memory) segment’s HNSW incrementally

Keep this pipeline **purely CPU** and **batch-friendly** (accumulate token vectors, then one aggregation pass).

---

## 3) Segment abstraction

### 3.1 Segment types

* **MemSegment (mutable)**: vectors + payload in RAM, HNSW mutable
* **DiskSegment (immutable)**: memory-mapped files for vectors + HNSW adjacency + metadata

### 3.2 On-disk layout (one directory per segment)

```
seg_<uuid>/
  manifest.json          // counts, dims, metric, build params, min/max id
  vectors.bin            // packed float32/float16 or quantized
  ids.bin                // PointId for each ordinal
  payload.bin            // optional (or offsets into external store)
  hnsw_levels.bin        // per-node level
  hnsw_graph_L0.bin      // adjacency for layer 0 (largest)
  hnsw_graph_Lk.bin      // adjacency for upper layers
  deletes.bin            // optional bitset or tombstone list
  checksum.crc
```

**Key invariant:** inside a segment, points are referenced by **ordinal** `0..N-1`; `PointId`→`ordinal` uses a compact map (sorted ids + binary search, or minimal perfect hash if you want).

### 3.3 HNSW parameters (stored per segment)

Store `M`, `efConstruction`, `efSearchDefault`, metric (L2 / cosine / IP), and any quantization flags. (Elastic’s newer HNSW notes around parameters are useful background, but your design can keep it simple.) ([Elastic][2])

---

## 4) Global manifest + snapshots (query isolation)

### 4.1 Global state

Maintain a single authoritative **DB manifest**:

```cpp
struct DbState {
  uint64_t generation;
  std::vector<std::shared_ptr<const DiskSegment>> disk;
  std::shared_ptr<const MemSegmentView> mem_view; // read-only view of current mem segment
};
std::atomic<std::shared_ptr<const DbState>> g_state;
```

### 4.2 Snapshot semantics

* A query does: `auto snap = g_state.load();`
* It searches exactly `snap->mem_view + snap->disk`
* Flush/merge publishes a **new DbState** (copy-on-write), then atomically swaps `g_state`
* Old snapshots remain valid until last reader releases (shared_ptr/RCU style)

This guarantees: “queries run against a snapshot … not affected by flush/merges”.

---

## 5) Ingestion & flush

### 5.1 Ingestion threads

Pipeline:

1. tokenize
2. embedding lookup + aggregate
3. assign `PointId` (monotonic counter or hash)
4. insert into MemSegment:

   * append vector to vector store
   * append id/payload
   * incremental HNSW insert (standard HNSW insert path)

### 5.2 Flush trigger

If `mem.N >= mem_flush_threshold`:

* “freeze” current MemSegment into an immutable **MemSegmentView** for readers
* start a flush job that writes a new DiskSegment on disk:

  * write packed vectors + ids
  * serialize HNSW graph
* publish a new DbState where:

  * disk += new segment
  * mem becomes a fresh empty MemSegment (new writer)
  * mem_view points to the new writer’s read-view (or nullptr if you prefer)

---

## 6) Merge policy & background compaction

### 6.1 Merge trigger

When `disk_segments.size() > segment_count_threshold`:

* choose a set of **smallest segments** to merge (tiered policy; similar spirit to Lucene’s “merge similar sizes” to keep write amplification logarithmic) ([Elastic][1])
* launch background merge job(s)

### 6.2 Merge job output

Create one new DiskSegment `S_new`, then publish:

* `disk = (disk - inputs) + S_new`
* delete input segment directories **after** no snapshot references them (or keep refcounts and delete lazily)

---

## 7) HNSW merge: “insert-a-graph-into-a-graph” using join set J

Your spec says: “HNSW indexes are merged using the algorithm in the Elastic paper/blog.”

Elastic’s approach (for merging a smaller HNSW graph `Gs` into a larger `Gl`) is:

* compute a **join set** `J ⊂ Vs` to insert “normally”
* for each remaining vertex `u ∈ Vs \ J`, seed candidate search using already-inserted neighbors from `Gs`, then run a faster search-layer variant before applying the normal neighbor-selection heuristic ([Elastic][1])

Their high-level merge pseudocode (“MERGE-HNSW”) is shown directly in the blog, including the key steps:

* `Ju ← J ∩ Ns(u)`
* `Eu ← ⋃_{v∈Ju} Nl(v)`
* `W ← FAST-SEARCH-LAYER(Ju, Eu)`
* `neighbors ← SELECT-NEIGHBORS-HEURISTIC(u, W)` ([Elastic][1])

They also describe how to size/ensure coverage of `J` (so every not-directly-inserted vertex has enough neighbors in `J`) and provide a concrete heuristic for `k_u` based on degree, plus a greedy “gain” procedure for COMPUTE-JOIN-SET. ([Elastic][1])

### 7.1 Practical way to implement merges in your segment system

To merge multiple segments `{S1..Sk}`:

1. pick the **largest** as `Gl` (or build an empty destination graph sized for total N)
2. merge others one-by-one into `Gl` using Elastic’s “merge small into large” primitive ([Elastic][1])
3. write out the resulting combined vectors/ids/graph as `S_new`

### 7.2 Alternative algorithms (optional)

There’s also a 2025 paper proposing three HNSW merge algorithms (NGM / IGTM / CGTM) and framing merging as vertex-selection → candidate-collection → neighborhood-construction → information-propagation. If you want a second reference point beyond Elastic/Lucene, it’s useful. ([arXiv][3])

---

## 8) Query execution (multi-segment)

Given snapshot `snap` and query vector `q`:

1. search `mem_view` HNSW → topK candidates
2. for each DiskSegment: search its HNSW → topK candidates
3. merge the candidate lists (k-way heap) and optionally rerank with exact distances

You can optionally implement “segment-aware” optimizations later; Elastic has discussed multi-graph search improvements in Lucene/Elasticsearch land. ([Elastic][4])

---

## 9) C++ module breakdown (clean “from scratch” skeleton)

**Core libraries**

* `libvectordb_core`

  * `Segment` interfaces, mmap readers, writers
  * vector codecs (fp32/fp16/int8…)
  * distance metrics (L2/cos/IP)
* `libvectordb_hnsw`

  * HNSW build/search/insert
  * HNSW serialization
  * HNSW merge (Elastic join-set + fast-search-layer)
* `libvectordb_runtime`

  * DbState, snapshotting, background job system
  * merge policy + scheduler
  * ingestion pipeline (tokenize → embed lookup → aggregate → insert)
* `vectordb_server` (optional)

  * RPC/HTTP interface

**Concurrency primitives**

* `std::shared_ptr` snapshots (RCU-like)
* background thread pool for flush/merge
* per-mem-segment writer lock + readers get immutable views

---

If you want to go one notch deeper next: I can spell out (1) exact binary encodings for each on-disk HNSW structure, (2) the `FAST-SEARCH-LAYER` data structures you’ll want to avoid allocations (fixed-capacity heaps, visited bitsets per segment), and (3) a merge policy that bounds both disk bytes and query fanout.

[1]: https://www.elastic.co/search-labs/blog/hnsw-graphs-speed-up-merging "HNSW graphs: Speeding up merging - Elasticsearch Labs"
[2]: https://www.elastic.co/search-labs/blog/hnsw-graph?utm_source=chatgpt.com "HNSW graph: How to improve Elasticsearch performance"
[3]: https://www.arxiv.org/pdf/2505.16064 "Three Algorithms for Merging Hierarchical Navigable Small World Graphs"
[4]: https://www.elastic.co/search-labs/blog/multi-graph-vector-search?utm_source=chatgpt.com "Speeding up multi-graph vector search - Elasticsearch Labs"
