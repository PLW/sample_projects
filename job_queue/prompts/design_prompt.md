You are my expert C++ coding assistant.  We are building job queue
where workers "lease" jobs for a visibility timeout; jobs are
re-queued if not acknowledged or if the lease expires before completion
(stalled or crashed).

The minimum viable product requirements are: 
1) Enqueuing: A producer application adds a job to the main work queue.
The job includes a `job_id` and data to be processed.
2) Leasing: A worker requests a job from the queue and specifies a lease
duration. The job is moved to a separate processing or leased tracking
area and associated with the `worker_id` and `expiry_time`.
3) Processing: The worker works on the task. The lease can be extended
if the task takes longer than expected.
4) Completion: Upon successful completion, the worker notifies the queue
system (via a `complete(job_id)` or `delete(job_id)` command), and the job
is permanently removed from the system.
5) Cleaning/Re-queuing: A separate cleaning process (or the queue system
itself) monitors for expired leases. If a lease expires without the job
being marked as complete, the job is automatically moved back to the
main queue to be processed by another available worker.
6) Fault Tolerance: If the worker fails or crashes before completing the
job and the lease expires, the job is automatically returned to the main
queue for another worker to pick up and process. This ensures the job is
not lost and will eventually be completed.
7) Idempotency: Because a job might be processed multiple times if a
worker fails to call complete() before the lease expires, it is
important to design workers and jobs to be idempotent (able to run
multiple times without causing issues).

We address the task of constructing the software system X
by decomposing the problem into small, manageable steps:

* Step 1: define the data structures;
* Step 2: outline the system at a high level: modules, classes, methods;
  and the data and control flow among the components;
* Step 3: define the invariants which must hold for the system state;
* Step 4: implement small, unit-testable (gtest) components which allow
  incremental testing for correctness with respect to invariants and
  gold-standard expected behavior;
* Step 5: general integration testing strategy using gtest: worker simulations,
  crash testing, high concurrency, gold-standard behavioral checking.

