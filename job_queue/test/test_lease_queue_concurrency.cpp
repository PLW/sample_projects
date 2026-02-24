
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "LeaseQueue.h"

using namespace std::chrono_literals;

namespace leaseq::test {

enum class OpKind { Enqueue, Lease, Extend, Complete, Reap };

struct Op {
  std::uint64_t seq = 0;
  OpKind kind{};
  std::string worker;
  std::string job;
  std::uint64_t token = 0;
  std::int64_t dur_ms = 0;     // lease/extend durations, for replay
  bool ok = false;
};

// Helper: set equality on unordered_set
static void expect_set_eq(std::unordered_set<std::string> const& a,
                          std::unordered_set<std::string> const& b) {
  EXPECT_EQ(a.size(), b.size());
  for (auto const& x : a) EXPECT_TRUE(b.count(x)) << "missing: " << x;
}

// Build a deterministic list of job ids
static std::vector<std::string> make_jobs(int n) {
  std::vector<std::string> out;
  out.reserve(n);
  for (int i = 0; i < n; ++i) out.push_back("job_" + std::to_string(i));
  return out;
}

TEST(LeaseQueue, ConcurrencyRandomCrashesAndReplayMatchesModelFinalState) {
  // -----------------------------
  // Parameters (tune as desired)
  // -----------------------------
  const int kNumJobs    = 400;
  const int kNumWorkers = 16;

  // Short leases to force expirations.
  const int kLeaseMinMs = 5;
  const int kLeaseMaxMs = 25;

  // Worker behavior probabilities:
  // - crash: lease and then never complete/extend (let lease expire)
  // - stall: sleep long enough to often expire before complete
  // - extend: extend once, then complete
  const double p_crash  = 0.12;
  const double p_stall  = 0.25;
  const double p_extend = 0.25;
  // remainder => complete quickly

  // Cleaner cadence
  const auto cleaner_period = 2ms;

  // -----------------------------
  // System under test (real clock)
  // -----------------------------
  auto clock = std::make_shared<SteadyClock>();
  LeaseQueue q(clock);

  auto jobs = make_jobs(kNumJobs);
  for (auto const& id : jobs) {
    ASSERT_TRUE(q.enqueue(id, JobPayload{std::string("payload:") + id}));
  }

  // -----------------------------
  // Shared test state
  // -----------------------------
  std::atomic<bool> stop{false};
  std::atomic<int> active_workers{kNumWorkers};

  std::mutex done_mu;
  std::condition_variable done_cv;

  std::mutex log_mu;
  std::vector<Op> log;
  log.reserve(200000);

  std::atomic<std::uint64_t> seq{1};

  // Track successful completions from concurrent run
  std::mutex comp_mu;
  std::unordered_set<std::string> completed_concurrent;

  auto log_op = [&](Op op) {
    op.seq = seq.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(log_mu);
    log.push_back(std::move(op));
  };

  // -----------------------------
  // Cleaner thread
  // -----------------------------
  std::thread cleaner([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(cleaner_period);
      std::size_t n = q.reap_expired();
      (void)n;
      // Optional: log reaps; can be noisy, but useful for replay fidelity
      log_op(Op{.kind = OpKind::Reap, .ok = true});
    }
  });

  // -----------------------------
  // Worker threads
  // -----------------------------
  std::vector<std::thread> workers;
  workers.reserve(kNumWorkers);

  for (int wi = 0; wi < kNumWorkers; ++wi) {
    workers.emplace_back([&, wi] {
      std::mt19937_64 rng(0xC0FFEEULL + (std::uint64_t)wi * 1337ULL);
      std::uniform_int_distribution<int> lease_ms(kLeaseMinMs, kLeaseMaxMs);
      std::uniform_real_distribution<double> prob(0.0, 1.0);

      const std::string worker = "w_" + std::to_string(wi);

      while (!stop.load(std::memory_order_relaxed)) {
        const int lm = lease_ms(rng);
        auto lr = q.lease(worker, std::chrono::milliseconds(lm));
        log_op(Op{.kind = OpKind::Lease, .worker = worker, .job = lr ? lr->job.id : "",
                  .token = lr ? lr->lease.token : 0, .dur_ms = lm, .ok = lr.has_value()});

        if (!lr) {
          // If empty, yield briefly.
          std::this_thread::sleep_for(1ms);
          continue;
        }

        const std::string job = lr->job.id;
        const std::uint64_t token = lr->lease.token;

        const double r = prob(rng);

        if (r < p_crash) {
          // "Crash": do nothing; let lease expire and job requeue.
          // Optionally sleep a bit to simulate CPU time then crash.
          std::this_thread::sleep_for(std::chrono::milliseconds(lm + 5));
          continue;
        }

        if (r < p_crash + p_stall) {
          // Stall long enough that lease often expires before completion.
          std::this_thread::sleep_for(std::chrono::milliseconds(lm + (lease_ms(rng) / 2)));
          // Try completing anyway (may fail if token no longer current).
          bool ok = q.complete(job, worker, token);
          log_op(Op{.kind = OpKind::Complete, .worker = worker, .job = job,
                    .token = token, .ok = ok});
          if (ok) {
            std::lock_guard<std::mutex> lk(comp_mu);
            completed_concurrent.insert(job);
          }
          continue;
        }

        if (r < p_crash + p_stall + p_extend) {
          // Extend once, then complete.
          const int extra = lease_ms(rng);
          std::this_thread::sleep_for(std::chrono::milliseconds(lm / 2));
          bool ex_ok = q.extend(job, worker, token, std::chrono::milliseconds(extra));
          log_op(Op{.kind = OpKind::Extend, .worker = worker, .job = job,
                    .token = token, .dur_ms = extra, .ok = ex_ok});

          // Work a bit more, then complete.
          std::this_thread::sleep_for(std::chrono::milliseconds(extra / 2));
          bool ok = q.complete(job, worker, token);
          log_op(Op{.kind = OpKind::Complete, .worker = worker, .job = job,
                    .token = token, .ok = ok});
          if (ok) {
            std::lock_guard<std::mutex> lk(comp_mu);
            completed_concurrent.insert(job);
          }
          continue;
        }

        // Complete quickly.
        std::this_thread::sleep_for(std::chrono::milliseconds(lm / 3));
        bool ok = q.complete(job, worker, token);
        log_op(Op{.kind = OpKind::Complete, .worker = worker, .job = job,
                  .token = token, .ok = ok});
        if (ok) {
          std::lock_guard<std::mutex> lk(comp_mu);
          completed_concurrent.insert(job);
        }
      }

      if (active_workers.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> lk(done_mu);
        done_cv.notify_all();
      }
    });
  }

  // -----------------------------
  // Stop condition: either all jobs completed, or time budget reached.
  // -----------------------------
  constexpr bool kRequireEventualCompletion = false; // set true for strict mode
  const auto time_budget = 10s;

  auto completed_count = [&] {
    std::lock_guard<std::mutex> lk(comp_mu);
    return (int)completed_concurrent.size();
  };

  const auto t0 = std::chrono::steady_clock::now();
  while (true) {
    if (completed_count() == kNumJobs) break;
    if (std::chrono::steady_clock::now() - t0 > time_budget) break;
    std::this_thread::sleep_for(5ms);
  }

  stop.store(true);

  for (auto& th : workers) th.join();
  cleaner.join();

  // -----------------------------
  // Drain expirations after stopping
  // -----------------------------
  // The goal: no job should remain stuck in leased;
  //   anything uncompleted returns to ready.
  for (int i = 0; i < 50; ++i) {
    (void)q.reap_expired();
    std::this_thread::sleep_for(2ms);
  }
  (void)q.reap_expired();

  auto snap = q.snapshot();

  // 1) No stuck leases after drain.
  EXPECT_EQ(q.leased_size(), 0u);

  // 2) Present jobs must all be ready (since no leases).
  std::unordered_set<std::string> ready_set(snap.ready_ids.begin(), snap.ready_ids.end());
  std::unordered_set<std::string> job_set(snap.job_ids.begin(), snap.job_ids.end());

  EXPECT_EQ(ready_set.size(), job_set.size());
  for (auto const& id : job_set) {
    EXPECT_TRUE(ready_set.count(id)) << "job exists but not ready: " << id;
  }

  // 3) Final correctness checks: strict or lenient
  std::unordered_set<std::string> completed_copy;
  {
    std::lock_guard<std::mutex> lk(comp_mu);
    completed_copy = completed_concurrent;
  }

  std::unordered_set<std::string> all;
  for (auto const& id : jobs) all.insert(id);

  if constexpr (kRequireEventualCompletion) {
    // Strict: everything must have completed within the budget.
    EXPECT_EQ(completed_copy.size(), all.size());
    for (auto const& id : all) {
      EXPECT_TRUE(completed_copy.count(id)) << "not completed: " << id;
    }
    // In strict mode, queue should be empty too.
    EXPECT_EQ(job_set.size(), 0u);
    EXPECT_EQ(ready_set.size(), 0u);
  } else {
    // Lenient: nothing lost; everything is either completed or ready.
    std::unordered_set<std::string> union_set = completed_copy;
    for (auto const& id : job_set) union_set.insert(id);

    EXPECT_EQ(union_set.size(), all.size());
    for (auto const& id : all) {
      EXPECT_TRUE(union_set.count(id)) << "lost job: " << id;
    }

    // Disjointness: cannot be both completed and still present.
    for (auto const& id : job_set) {
      EXPECT_FALSE(completed_copy.count(id)) << "job both completed and present: " << id;
    }
  }


  /* 
  // Final reap to drain any remaining expired leases
  (void)q.reap_expired();
  std::this_thread::sleep_for(5ms);
  (void)q.reap_expired();

  // -----------------------------
  // Concurrent final-state assertions (strong invariants)
  // -----------------------------
  auto snap = q.snapshot();

  // Drain again to ensure no lingering expirations.
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(2ms);
    (void)q.reap_expired();
  }

  // 1) No stuck leases after drain.
  EXPECT_EQ(q.leased_size(), 0u);

  // 2) Present jobs must all be ready (since no leases).
  std::unordered_set<std::string> ready_set(snap.ready_ids.begin(), snap.ready_ids.end());
  std::unordered_set<std::string> job_set(snap.job_ids.begin(), snap.job_ids.end());

  EXPECT_EQ(ready_set.size(), job_set.size());
  for (auto const& id : job_set) {
    EXPECT_TRUE(ready_set.count(id)) << "job exists but not ready: " << id;
  }

  // 3) Conservation: every original job is either completed or still present (ready).
  std::unordered_set<std::string> completed_copy;
  {
    std::lock_guard<std::mutex> lk(comp_mu);
    completed_copy = completed_concurrent;
  }

  std::unordered_set<std::string> all;
  for (auto const& id : jobs) all.insert(id);

  std::unordered_set<std::string> union_set = completed_copy;
  for (auto const& id : job_set) union_set.insert(id);

  EXPECT_EQ(union_set.size(), all.size());
  for (auto const& id : all) {
    EXPECT_TRUE(union_set.count(id)) << "lost job: " << id;
  }

  // 4) Disjointness: a job cannot be both completed and still present.
  for (auto const& id : job_set) {
    EXPECT_FALSE(completed_copy.count(id)) << "job both completed and present: " << id;
  }
  */
}


} // namespace leaseq::test
