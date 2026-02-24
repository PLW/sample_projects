
## Unit test targets (small, gold-standard behaviors)

1. **Enqueue + lease FIFO**
2. **Lease moves job from ready→leased**
3. **Complete removes job permanently**
4. **Expired lease gets requeued**
5. **Extend prevents requeue**
6. **Wrong worker/token can’t extend or complete**
7. **Stale heap entries are ignored** (extend twice then reap)

Example gtest:

```cpp
#include <gtest/gtest.h>

TEST(LeaseQueue, ExpiredLeaseRequeues) {
  auto clock = std::make_shared<FakeClock>();
  LeaseQueue q(clock);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));
  auto lr = q.lease("w1", std::chrono::milliseconds(100));
  ASSERT_TRUE(lr.has_value());
  EXPECT_EQ(q.ready_size(), 0u);
  EXPECT_EQ(q.leased_size(), 1u);

  clock->advance(std::chrono::milliseconds(150));
  EXPECT_EQ(q.reap_expired(), 1u);
  EXPECT_EQ(q.ready_size(), 1u);
  EXPECT_EQ(q.leased_size(), 0u);

  auto lr2 = q.lease("w2", std::chrono::milliseconds(100));
  ASSERT_TRUE(lr2.has_value());
  EXPECT_EQ(lr2->job.id, "job1");
  EXPECT_EQ(lr2->lease.worker, "w2");
}
```

## Integration testing strategy (gtest)

### Worker simulations

* Build a `WorkerSim` that loops:
  * `lease(worker_id, dur)`
  * “process” by advancing fake time or sleeping (if real clock)
  * sometimes `extend` mid-flight
  * sometimes `complete`

* Track “side effects” in a shared map keyed by `job_id` to assert idempotency patterns (e.g., processed count).

### Crash testing

* Simulate crash = lease a job but never call `complete`.
* Advance time beyond lease duration; `reap_expired`; ensure the job is leasable again.
* Add randomized crash points: crash before extend, after extend, etc.

### High concurrency

Even for an in-memory MVP, you can:
* Use real threads, each as a worker repeatedly leasing/completing.
* Use a barrier to start together.
* Invariants: total completed == initial enqueued (if all workers eventually complete), no duplicates in `jobs`.
* Add sanitizer runs (TSan/ASan/UBSan) in CI.

### Gold-standard behavioral checking

Maintain a simple “model” reference in tests:
* A single-threaded spec simulator (same fake time) that applies operations deterministically.
* In randomized test sequences (property-style), compare:
  * ready set, leased set, completed set
  * per-job attempts, and that completion is terminal

