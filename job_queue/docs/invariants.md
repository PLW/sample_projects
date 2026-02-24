## System invariants

These are the “must always be true” facts your unit tests should hammer.

### State partitioning invariants

1. **A job is either ready, leased, or absent** (completed), never in two places at once.
   * `id ∈ st.jobs` implies exactly one of:
     * `id` appears in `st.ready` **xor**
     * `id ∈ st.leased`

2. **Leased jobs must exist in job store**:
   * `id ∈ st.leased  ⇒  id ∈ st.jobs`

3. **Completed jobs do not exist anywhere**:
   * after `complete(id)`: `id ∉ st.jobs`, `id ∉ st.leased`, `id ∉ ready`

### Lease correctness invariants

4. Lease ownership is exclusive: one lease per job.
5. `complete/extend` must only succeed if `(worker_id, token)` matches the current lease record.
6. Requeue only if lease is expired *at reap time* and still current (token matches).

### Time/expiry invariants

7. `leased[id].expiry` is monotonic non-decreasing under successful `extend`.
8. Expiry heap may contain stale entries, but they must be harmless:
   * A heap entry `(id, token, expiry)` is actionable iff
     * `id ∈ st.leased` and `st.leased[id].token == token` and `st.leased[id].expiry == expiry` (or `<= now` check uses current lease expiry).

