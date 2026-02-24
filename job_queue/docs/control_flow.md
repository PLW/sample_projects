
**Control/data flow**

* Producer: enqueue(id, payload) →
  * inserts jobs[id],
  * appends id to ready.

* Worker: lease(worker_id, duration) →
  * pops ready,
  * creates Lease{id, worker, now+dur, token},
  * moves id to leased,
  * pushes expiry heap entry.

* Worker: extend(id, worker, token, extra) →
  * verifies active lease matches (worker, token),
  * updates expiry to max(current_expiry, now)+extra (define semantics),
  * pushes new heap entry;
  * stale heap entries are ignored later.

* Worker: complete(id, worker, token) →
  * verifies active lease matches (worker, token),
  * erases from leased,
  * erases jobs[id]. (Job is gone forever.)

* Cleaner: reap_expired() →
  * while heap top expiry ≤ now,
      check if lease still matches token and is expired;
      if yes, remove from leased and push back to ready (increment attempts on job).


