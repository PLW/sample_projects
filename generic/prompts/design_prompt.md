
You are my C++ system architecture and design expert consultant.
We are designing a distributed XXX system.
We need a high-level design document that describes the system
but does not contain actual C++ implementations.  The document
should include tradeoff analysis.  It should include:
* ADR (Architecture Decision Records). 
* NFR (Non-Functional Requirements),
* SLO (Service Level Objectives), and
* SRE (Site Reliability Engineering).

---

Following are details of the XXX system objectives, requirements,
 and basic design elements.

---

**Basic system elements and NFRs**

* The system distributes storage units (indexes, trees, etc.) into
   per-host `shards`, and
* within each shard partitions the storage units into `segments`.  
* Sharding is based on object, (e.g.) URL, hash. 
* New objects are dipatched to the appropriate shard, where
   they are processed into an in-memory segment.
* The in-memory segment is periodically flushed to persistent storage
   as an immutable persistent segment.
* Immutable segments are periodically merged.
* The set of `in-play` segments is maintained by a metadata service that
   contains the (hash -> host) mapping, and the (shard -> active segments) list.
* Queries are evaluated by scatter-gather broadcast to all shards (scatter),
   followed by merge results and re-rank (gather).

The system consists of 
* some number of independent `eval nodes` and
* some number of `data nodes`.
* Eval nodes maintain a copy of the (object -> shard) hash map and
    dispatch requests to the appropriate shard.
* Data nodes process query/insert/delete requests independently.
* Eval nodes process queries by broadcasting to one replica in every
  shard, then gathering and re-ranking the results.

---

**MVP (minimum viable product) functions**
  * `load(obj) => obj_id`
  * `delete(obj_id)`
  * `extract_keys(obj, obj_id) => key_stream`
       (e.g.) terms, tokens, features, etc.
  * `process(key_stream, obj_id) => processed_key_stream`
       (e.g.) generate new index structure items: postings, embedding vectors
  * `update_in_memory_segment(processed_key_stream, obj_id)`
       (e.g.) insert new keys into memory segment index
  * `flush_memory_segment() => persistent_segment`
  * `multi_way_merge` of persistent segments for query optimization
  * `result_iterator` composable iterator interface for query results
  * `filters` as instances of `result_iterator`
  * `and_iterator : public result_iterator`
  * `or_iterator : public result_iterator`
  * `near_iterator : public result_iterator`
  * `query(q) => result_iterator`
  * `query(q1 AND q2) => and_iterator(it1(q1),it2(q2))`
  * `query(q1 OR q2) => or_iterator(it1(q1),it2(q2))`
  * `obj(obj_id) => obj`
  * `metadata(obj_id) => obj_metadata`
  * `metadata() => shard_status`

---

**Replication**

Each shard runs at least three replicas with one primary and at least two
secondaries.  Each shard:
  * Maintains a Write-Ahead Log (WAL)
  * Maintains replicated commit logs
  * Supports checkpoint + replay
  * Maintains an immutable file manifest + atomic swap (on merge)

---

**Invariants**
1. Access-Object Mapping Invariants
 The core integrity of the access structure depends on the bidirectional
 relationship between objects and the access keys they contain.

* Reflexive Consistency: For every object $B$ containing access key $K$, the
  access structure $A_K$ must contain a reference to $B$. Conversely, if $A_K$
  contains $K$, the key $K$ must actually exist in the current version of object 
  $B$.

* Unique Object Identity: An object $B$ must be mapped to exactly one
  primary shard at any given time (usually via $hash(objID) \pmod N$) to prevent
  duplicate entries in global result sets.

* Idempotency of Updates: Re-indexing the same object version $V_1$ must
  result in a state identical to a single indexing event. This is crucial for
  handling network retries.

2. Distributed State and Partitioning
Because the access structure is distributed, you must maintain invariants
regarding how data is sliced and moved.

* Shard Completeness: The union of all shards must equal the complete set of
  stored objects. $\bigcup_{i=1}^{n} S_i = \mathcal{D}_{total}$.

* Key Locality (for Key-Partitioned): If using key-partitioning, all
  occurrences of a key $K$ across the entire corpus must reside on the same
  logical shard.

* Routing Stability: During a "rebalance" or "resharding" event, an object
  must be searchable in either the old shard or the new shard, but never
  "missing" or "double-counted" in a final merged result set.

3. Concurrency and Ordering
In a high-velocity system, the order of operations determines the "truth" of the
index.

* Happens-Before Consistency: If Update A (Delete $D$) is acknowledged before
  Update B (Re-add $D$), the system must not reflect $D$ in a stale state.

* Monotonic Versioning: Every object must have an associated version number
  or timestamp. The system must reject any write where $Version_{incoming} \le
  Version_{stored}$ to prevent "out-of-order" synchronization from overwriting
  newer data.

* Visibility Latency Bound: The "Refresh" invariant. Once a write is
  acknowledged as "durable," it must become "searchable" within a strictly
  defined time bound ($T_{refresh}$), ensuring eventual consistency across all
  replicas.

4. Durability and Replication
These invariants ensure the system survives node failures.

* Write-Ahead Integrity: No update is considered "committed" until it is
  persisted to a non-volatile Write-Ahead Log (WAL) or a quorum of replicas.

* Replica Convergence: In the absence of new updates, all replicas of Shard
  $S_i$ must eventually reach a bit-identical state (or functionally identical
  state regarding query results).

* The "Split-Brain" Guard: The system must never allow two nodes to believe
  they are the "Primary" for the same shard simultaneously. This is usually
  enforced via a consensus protocol (Raft/Paxos) or a lease mechanism.
      
---

**Design document structure**

We construct the distributed system XXX by decomposing the problem into
small, manageable, testable steps:

* Part 1: define the data structures
* Part 2: outline the system at a high level: modules, classes, methods
* Part 3: outline the data and control flow among the components
* Part 4: catalog the invariants which must hold for the system state
* Part 5: define the unit-testable (gtest) components which allow
  incremental testing for correctness with respect to invariants and
  gold-standard expected behavior
* Part 6: define a general integration testing strategy using gtest:
  worker simulations, crash testing, high concurrency, gold-standard
  behavioral checking
* Part 7: define a scale-testing strategy, using the "default mode is failure,
  correctness must be engineered" model
* Part 8: Durability guarantees that address
  * Partial failure
  * Split-brain
  * Node loss
  * Rebalancing events

---
