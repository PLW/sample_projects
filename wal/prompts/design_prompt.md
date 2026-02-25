
You are my C++ system architecture and design expert consultant.
We are designing a write-ahead log for key-value mutations and
recovery that tolerates crashes mid-write. 

We need a high-level design document that describes the system
but does not contain actual C++ implementations.  The document
should include tradeoff analysis.  It should include:
* ADR (Architecture Decision Records). 
* NFR (Non-Functional Requirements),
* SLO (Service Level Objectives), and
* SRE (Site Reliability Engineering).

---

Following are details of the WAL system objectives, requirements,
 and basic design elements.

---

## WAL with checksums + crash recovery (partial write handling)

**MVP** 

* Record format: length + payload + checksum
* Append `PUT k v` and `DEL k`
* On startup: replay log, **stop safely** at first corrupt/partial record
* Provide `get/put/del` API over recovered state

**Stretch**

* Log rotation and snapshot
* Fsync policy knob (durability vs throughput)

**Probes**

* Real durability edge cases
* Binary formats, checksums, recovery logic
* Tests that simulate torn writes

---

**Design document structure**

We construct the WAL system by decomposing the problem into
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
