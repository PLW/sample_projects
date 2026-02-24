// lease_queue.hpp
// C++20, header-only MVP implementation of a lease-based job queue.
//
// Design goals (MVP):
// - In-memory, single-process.
// - Thread-safe via a single mutex.
// - Deterministic testing via IClock (FakeClock in tests).
// - Correctness under lease extensions via (job_id, token) fencing.
// - Efficient-ish expiration via min-heap with stale-entry tolerance.
//
// Not included (by design for MVP):
// - Persistence/WAL, multi-process coordination, distributed leasing, dedupe logs,
//   per-worker heartbeats, priority scheduling, dead-letter queues, visibility jitter, etc.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace leaseq {

// ------------------------------
// Time / Clock abstraction
// ------------------------------

using Duration  = std::chrono::milliseconds;
using TimePoint = std::chrono::steady_clock::time_point;

/// Clock interface for deterministic tests (inject FakeClock) and production (SteadyClock).
struct IClock {
  virtual ~IClock() = default;
  virtual TimePoint now() const = 0;
};

/// Production clock (monotonic).
struct SteadyClock final : IClock {
  TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

// ------------------------------
// Domain types
// ------------------------------

using JobId    = std::string;
using WorkerId = std::string;

/// Minimal worker descriptor (extend as needed).
struct Worker {
  WorkerId id;
};

/// Opaque job payload (swap for JSON/proto/etc).
struct JobPayload {
  std::string data;
};

/// Authoritative job data stored by the system.
/// - attempts increments each time a job is leased.
/// - enqueue_seq provides FIFO stability/observability if needed.
struct Job {
  JobId id;
  JobPayload payload;

  std::uint64_t enqueue_seq = 0;
  std::uint32_t attempts    = 0;
};

/// Lease record: “job is owned by worker until expiry”.
/// token fences extend/complete to the *current* lease instance.
struct Lease {
  JobId id;
  WorkerId worker;
  TimePoint expiry{};
  std::uint64_t token = 0;
};

/// A single lease operation yields both the job and its lease metadata.
struct LeaseResult {
  Job job;
  Lease lease;
};

// ------------------------------
// Internal scheduling structures
// ------------------------------

/// Heap item for expirations. Stale entries are allowed and safely ignored.
struct ExpiryItem {
  TimePoint expiry{};
  JobId id;
  std::uint64_t token = 0;
};

struct ExpiryItemGreater {
  bool operator()(ExpiryItem const& a, ExpiryItem const& b) const noexcept {
    // priority_queue is max-heap by default; invert to behave like min-heap.
    return a.expiry > b.expiry;
  }
};

/// In-memory queue state.
/// Invariant intent (informal):
/// - If id in leased => id in jobs.
/// - If id in jobs => id is either in ready (at least once) XOR leased.
/// - After complete => id removed from jobs and leased; stale heap entries ok.
struct QueueState {
  std::uint64_t next_enqueue_seq = 1;
  std::uint64_t next_lease_token = 1;

  std::unordered_map<JobId, Job> jobs;    // authoritative job store
  std::deque<JobId> ready;                // FIFO queue of job ids
  std::unordered_map<JobId, Lease> leased; // in-flight leases

  std::priority_queue<ExpiryItem, std::vector<ExpiryItem>, ExpiryItemGreater> expiries;
};

// ------------------------------
// LeaseQueue
// ------------------------------

/// Lease-based work queue with visibility timeout semantics.
class LeaseQueue {
public:
  explicit LeaseQueue(std::shared_ptr<IClock> clock)
      : clock_(std::move(clock)) {
    if (!clock_) throw std::invalid_argument("LeaseQueue requires a non-null clock");
  }

  /// Enqueue a new job.
  /// Returns false if job_id already exists (MVP: reject duplicates).
  bool enqueue(JobId job_id, JobPayload payload) {
    std::lock_guard<std::mutex> lk(mu_);

    if (st_.jobs.find(job_id) != st_.jobs.end()) return false;

    Job j;
    j.id          = std::move(job_id);
    j.payload     = std::move(payload);
    j.enqueue_seq = st_.next_enqueue_seq++;

    const JobId id_copy = j.id; // for ready queue key
    st_.jobs.emplace(id_copy, std::move(j));
    st_.ready.push_back(id_copy);
    return true;
  }

  /// Lease the next available job (FIFO) for a given worker for duration `dur`.
  /// Returns nullopt if no job is available.
  ///
  /// Notes:
  /// - Leasing increments Job::attempts.
  /// - The returned Lease includes a fencing token required for extend/complete.
  std::optional<LeaseResult> lease(WorkerId worker_id, Duration dur) {
    std::lock_guard<std::mutex> lk(mu_);

    if (st_.ready.empty()) return std::nullopt;

    JobId id = std::move(st_.ready.front());
    st_.ready.pop_front();

    auto job_it = st_.jobs.find(id);
    if (job_it == st_.jobs.end()) {
      // Should not happen if invariants hold; tolerate by skipping.
      return std::nullopt;
    }

    job_it->second.attempts++;

    const TimePoint now = clock_->now();
    Lease l;
    l.id     = id;
    l.worker = std::move(worker_id);
    l.expiry = now + dur;
    l.token  = st_.next_lease_token++;

    st_.leased.emplace(id, l);
    push_expiry_(id, l.expiry, l.token);

    return LeaseResult{job_it->second, l};
  }

  /// Extend an active lease.
  /// Fails unless (job_id, worker_id, token) matches the current lease.
  ///
  /// Semantics:
  /// - New expiry is computed from base = max(now, current_expiry), then + extra.
  ///   This makes late extensions still useful and avoids “losing time” if worker
  ///   extends after expiry (but before reaper runs).
  bool extend(JobId const& job_id, WorkerId const& worker_id, std::uint64_t token,
              Duration extra) {
    std::lock_guard<std::mutex> lk(mu_);

    auto it = st_.leased.find(job_id);
    if (it == st_.leased.end()) return false;

    Lease& l = it->second;
    if (l.worker != worker_id || l.token != token) return false;

    const TimePoint now  = clock_->now();
    const TimePoint base = (l.expiry > now) ? l.expiry : now;
    l.expiry             = base + extra;

    push_expiry_(job_id, l.expiry, l.token);
    return true;
  }

  /// Complete a leased job, permanently removing it from the system.
  /// Fails unless (job_id, worker_id, token) matches the current lease.
  bool complete(JobId const& job_id, WorkerId const& worker_id,
                std::uint64_t token) {
    std::lock_guard<std::mutex> lk(mu_);

    auto it = st_.leased.find(job_id);
    if (it == st_.leased.end()) return false;

    Lease const& l = it->second;
    if (l.worker != worker_id || l.token != token) return false;

    st_.leased.erase(it);
    st_.jobs.erase(job_id);
    // Any expiry heap entries for this job become stale and will be ignored.
    return true;
  }

  /// Reap expired leases and re-queue those jobs to be leased again.
  ///
  /// Stale heap entries are ignored by verifying:
  /// - job still leased
  /// - lease token matches
  /// - current lease expiry <= now
  ///
  /// Returns the number of jobs requeued.
  std::size_t reap_expired() {
    std::lock_guard<std::mutex> lk(mu_);

    const TimePoint now = clock_->now();
    std::size_t requeued = 0;

    while (!st_.expiries.empty() && st_.expiries.top().expiry <= now) {
      ExpiryItem top = st_.expiries.top();
      st_.expiries.pop();

      auto it = st_.leased.find(top.id);
      if (it == st_.leased.end()) continue;           // already completed/requeued
      Lease const& l = it->second;
      if (l.token != top.token) continue;             // stale heap entry
      if (l.expiry > now) continue;                   // extended after this heap push

      // Current lease expired: requeue.
      st_.leased.erase(it);
      st_.ready.push_back(top.id);
      ++requeued;
    }

    return requeued;
  }

  // ------------------------------
  // Introspection (useful for tests/metrics)
  // ------------------------------

  std::size_t ready_size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_.ready.size();
  }

  std::size_t leased_size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_.leased.size();
  }

  std::size_t jobs_size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_.jobs.size();
  }

private:
  void push_expiry_(JobId const& id, TimePoint expiry, std::uint64_t token) {
    st_.expiries.push(ExpiryItem{expiry, id, token});
  }

  mutable std::mutex mu_;
  QueueState st_;
  std::shared_ptr<IClock> clock_;
};

} // namespace leaseq
