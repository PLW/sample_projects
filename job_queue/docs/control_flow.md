
**Control/data flow**

* Producer: `enqueue(job_id, payload)` →
  * inserts `jobs[job_id].,
  * appends `job_id` to ready queue.

* Worker: `lease(worker_id, duration)` →
  * pops `ready`,
  * creates `Lease{job_id, worker_id, now+dur, token}`,
  * moves `job_id` to leased,
  * pushes expiry heap entry.

* Worker: `extend(job_id, worker_id, token, extra)` →
  * verifies active lease matches (worker_id, token),
  * updates expiry to max(current_expiry, now)+extra (define semantics),
  * pushes new heap entry;
  * stale heap entries are ignored later.

* Worker: `complete(job_id, worker_id, token)` →
  * verifies active lease matches `(worker_id, token)`,
  * erases from `leased`,
  * erases `jobs[job_id]`. (Job is gone forever.)

* Cleaner: reap_expired() →
  * `while (heap.top.expiry ≤ now)`
      check if `lease` still matches token and is expired;
      if yes, remove from `leased` and push back to `ready` (++job.attempts)

