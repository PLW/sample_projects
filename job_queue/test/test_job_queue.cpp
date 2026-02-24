// Gtest suite for leaseq::LeaseQueue invariants.
// Assumes headers are available at:
//   - LeaseQueue.h
//   - FakeClock.h
//
// Build tip (CMake):
//   add_executable(test_job_queue tests/test_job_queue.cpp)
//   target_link_libraries(test_job_queue PRIVATE gtest_main)
//   target_include_directories(test_job_queue PRIVATE ${CMAKE_SOURCE_DIR}/src)
//   add_test(NAME test_job_queue COMMAND test_job_queue)

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "LeaseQueue.h"
#include "FakeClock.h"

using namespace std::chrono_literals;

namespace leaseq::test {

static LeaseQueue make_queue(std::shared_ptr<FakeClock> const& clk) {
  return LeaseQueue(clk);
}

TEST(LeaseQueue, EnqueueRejectsDuplicateJobId) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  EXPECT_TRUE(q.enqueue("job1", {"a"}));
  EXPECT_FALSE(q.enqueue("job1", {"b"})); // MVP: reject duplicates
  EXPECT_EQ(q.jobs_size(), 1u);
  EXPECT_EQ(q.ready_size(), 1u);
  EXPECT_EQ(q.leased_size(), 0u);
}

TEST(LeaseQueue, LeaseFromEmptyReturnsNullopt) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  auto lr = q.lease("w1", 100ms);
  EXPECT_FALSE(lr.has_value());
  EXPECT_EQ(q.jobs_size(), 0u);
  EXPECT_EQ(q.ready_size(), 0u);
  EXPECT_EQ(q.leased_size(), 0u);
}

TEST(LeaseQueue, FIFOLeaseOrder) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"p1"}));
  ASSERT_TRUE(q.enqueue("job2", {"p2"}));
  ASSERT_TRUE(q.enqueue("job3", {"p3"}));

  auto a = q.lease("wA", 1s);
  auto b = q.lease("wB", 1s);
  auto c = q.lease("wC", 1s);

  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_TRUE(c.has_value());

  EXPECT_EQ(a->job.id, "job1");
  EXPECT_EQ(b->job.id, "job2");
  EXPECT_EQ(c->job.id, "job3");

  EXPECT_EQ(q.ready_size(), 0u);
  EXPECT_EQ(q.leased_size(), 3u);
  EXPECT_EQ(q.jobs_size(), 3u);
}

TEST(LeaseQueue, LeasingMovesJobReadyToLeasedAndSetsExpiry) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));
  EXPECT_EQ(q.ready_size(), 1u);
  EXPECT_EQ(q.leased_size(), 0u);

  clk->t = TimePoint{} + 100ms;
  auto lr = q.lease("w1", 250ms);
  ASSERT_TRUE(lr.has_value());

  EXPECT_EQ(q.ready_size(), 0u);
  EXPECT_EQ(q.leased_size(), 1u);
  EXPECT_EQ(q.jobs_size(), 1u);

  EXPECT_EQ(lr->lease.id, "job1");
  EXPECT_EQ(lr->lease.worker, "w1");
  EXPECT_EQ(lr->lease.expiry, clk->now() + 250ms);
  EXPECT_NE(lr->lease.token, 0u);
}

TEST(LeaseQueue, FencingTokenRequiredForComplete) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));
  auto lr = q.lease("w1", 1s);
  ASSERT_TRUE(lr.has_value());

  // Wrong token fails
  EXPECT_FALSE(q.complete("job1", "w1", lr->lease.token + 1));
  EXPECT_EQ(q.jobs_size(), 1u);
  EXPECT_EQ(q.leased_size(), 1u);

  // Wrong worker fails
  EXPECT_FALSE(q.complete("job1", "w2", lr->lease.token));
  EXPECT_EQ(q.jobs_size(), 1u);
  EXPECT_EQ(q.leased_size(), 1u);

  // Correct worker+token succeeds
  EXPECT_TRUE(q.complete("job1", "w1", lr->lease.token));
  EXPECT_EQ(q.jobs_size(), 0u);
  EXPECT_EQ(q.leased_size(), 0u);
  EXPECT_EQ(q.ready_size(), 0u);

  // Completing again fails (already removed)
  EXPECT_FALSE(q.complete("job1", "w1", lr->lease.token));
}

TEST(LeaseQueue, FencingTokenRequiredForExtend) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));
  auto lr = q.lease("w1", 200ms);
  ASSERT_TRUE(lr.has_value());

  // Wrong token fails
  EXPECT_FALSE(q.extend("job1", "w1", lr->lease.token + 1, 100ms));

  // Wrong worker fails
  EXPECT_FALSE(q.extend("job1", "w2", lr->lease.token, 100ms));

  // Correct worker+token succeeds
  EXPECT_TRUE(q.extend("job1", "w1", lr->lease.token, 100ms));
}

TEST(LeaseQueue, RequeueOnExpiry) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));
  auto lr = q.lease("w1", 100ms);
  ASSERT_TRUE(lr.has_value());

  EXPECT_EQ(q.ready_size(), 0u);
  EXPECT_EQ(q.leased_size(), 1u);

  // Not yet expired: reap does nothing
  clk->advance(99ms);
  EXPECT_EQ(q.reap_expired(), 0u);
  EXPECT_EQ(q.ready_size(), 0u);
  EXPECT_EQ(q.leased_size(), 1u);

  // Expired: should requeue
  clk->advance(2ms);
  EXPECT_EQ(q.reap_expired(), 1u);
  EXPECT_EQ(q.ready_size(), 1u);
  EXPECT_EQ(q.leased_size(), 0u);
  EXPECT_EQ(q.jobs_size(), 1u);

  // Can be leased again by another worker
  auto lr2 = q.lease("w2", 100ms);
  ASSERT_TRUE(lr2.has_value());
  EXPECT_EQ(lr2->job.id, "job1");
  EXPECT_EQ(lr2->lease.worker, "w2");
  EXPECT_NE(lr2->lease.token, lr->lease.token); // new lease instance
}

TEST(LeaseQueue, ExtendSemanticsUsesMaxNowAndCurrentExpiry) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));

  // Lease at t=0 for 100ms => expiry=100ms
  clk->t = TimePoint{}; 
  auto lr = q.lease("w1", 100ms);
  ASSERT_TRUE(lr.has_value());
  EXPECT_EQ(lr->lease.expiry, TimePoint{} + 100ms);

  // Advance to t=50ms, extend by 80ms using base=max(now=50, expiry=100)=100 => expiry=180
  clk->t = TimePoint{} + 50ms;
  ASSERT_TRUE(q.extend("job1", "w1", lr->lease.token, 80ms));

  // Move to t=150ms (still before expiry=180), extend by 40ms using base=max(150,180)=180 => expiry=220
  clk->t = TimePoint{} + 150ms;
  ASSERT_TRUE(q.extend("job1", "w1", lr->lease.token, 40ms));

  // Confirm it does not requeue at t=200ms (not yet expired), but does at t=221ms.
  clk->t = TimePoint{} + 200ms;
  EXPECT_EQ(q.reap_expired(), 0u);
  EXPECT_EQ(q.leased_size(), 1u);

  clk->t = TimePoint{} + 221ms;
  EXPECT_EQ(q.reap_expired(), 1u);
  EXPECT_EQ(q.leased_size(), 0u);
  EXPECT_EQ(q.ready_size(), 1u);
}

TEST(LeaseQueue, StaleExpiryEntriesAreIgnoredAfterExtend) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));

  // Lease at t=0, dur=100ms => expiry=100ms (heap entry A)
  clk->t = TimePoint{};
  auto lr = q.lease("w1", 100ms);
  ASSERT_TRUE(lr.has_value());

  // Extend at t=10ms by 200ms => base=max(10,100)=100 => expiry=300ms (heap entry B)
  clk->t = TimePoint{} + 10ms;
  ASSERT_TRUE(q.extend("job1", "w1", lr->lease.token, 200ms));

  // Now advance to t=150ms. The stale heap entry A (100ms) is expired, but current lease expiry is 300ms,
  // so reap_expired must NOT requeue.
  clk->t = TimePoint{} + 150ms;
  EXPECT_EQ(q.reap_expired(), 0u);
  EXPECT_EQ(q.leased_size(), 1u);
  EXPECT_EQ(q.ready_size(), 0u);

  // At t=301ms it should requeue.
  clk->t = TimePoint{} + 301ms;
  EXPECT_EQ(q.reap_expired(), 1u);
  EXPECT_EQ(q.leased_size(), 0u);
  EXPECT_EQ(q.ready_size(), 1u);
}

TEST(LeaseQueue, StaleExpiryEntriesAreIgnoredAfterReLeaseWithNewToken) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));

  // First lease t=0 dur=50 => expiry=50 (heap entry token1)
  clk->t = TimePoint{};
  auto lr1 = q.lease("w1", 50ms);
  ASSERT_TRUE(lr1.has_value());
  const auto token1 = lr1->lease.token;

  // Let it expire and requeue.
  clk->t = TimePoint{} + 60ms;
  ASSERT_EQ(q.reap_expired(), 1u);
  ASSERT_EQ(q.ready_size(), 1u);

  // Second lease t=60 dur=200 => expiry=260 (heap entry token2)
  auto lr2 = q.lease("w2", 200ms);
  ASSERT_TRUE(lr2.has_value());
  const auto token2 = lr2->lease.token;
  ASSERT_NE(token1, token2);

  // There may still be stale entries for token1; reaping at t=80 should not affect current lease.
  clk->t = TimePoint{} + 80ms;
  EXPECT_EQ(q.reap_expired(), 0u);
  EXPECT_EQ(q.leased_size(), 1u);

  // Expire current lease at t=261 => should requeue once.
  clk->t = TimePoint{} + 261ms;
  EXPECT_EQ(q.reap_expired(), 1u);
  EXPECT_EQ(q.leased_size(), 0u);
  EXPECT_EQ(q.ready_size(), 1u);
}

TEST(LeaseQueue, CompletePreventsFutureRequeueEvenIfExpiryHeapHasEntries) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));

  clk->t = TimePoint{};
  auto lr = q.lease("w1", 100ms);
  ASSERT_TRUE(lr.has_value());

  // Extend to create extra heap entries.
  clk->t = TimePoint{} + 10ms;
  ASSERT_TRUE(q.extend("job1", "w1", lr->lease.token, 200ms));

  // Complete at t=20ms.
  clk->t = TimePoint{} + 20ms;
  ASSERT_TRUE(q.complete("job1", "w1", lr->lease.token));
  EXPECT_EQ(q.jobs_size(), 0u);
  EXPECT_EQ(q.leased_size(), 0u);
  EXPECT_EQ(q.ready_size(), 0u);

  // Advance beyond any previous expiries; reap must not resurrect the job.
  clk->t = TimePoint{} + 1000ms;
  EXPECT_EQ(q.reap_expired(), 0u);
  EXPECT_EQ(q.jobs_size(), 0u);
  EXPECT_EQ(q.ready_size(), 0u);
  EXPECT_EQ(q.leased_size(), 0u);
}

TEST(LeaseQueue, WrongWorkerOrTokenFailsCases) {
  auto clk = std::make_shared<FakeClock>();
  LeaseQueue q(clk);

  ASSERT_TRUE(q.enqueue("job1", {"x"}));
  auto lr = q.lease("w1", 100ms);
  ASSERT_TRUE(lr.has_value());

  // Extend failures
  EXPECT_FALSE(q.extend("job1", "wX", lr->lease.token, 10ms));
  EXPECT_FALSE(q.extend("job1", "w1", lr->lease.token + 999, 10ms));

  // Complete failures
  EXPECT_FALSE(q.complete("job1", "wX", lr->lease.token));
  EXPECT_FALSE(q.complete("job1", "w1", lr->lease.token + 999));

  // Still leased
  EXPECT_EQ(q.leased_size(), 1u);
  EXPECT_EQ(q.jobs_size(), 1u);
}

} // namespace leaseq::test
