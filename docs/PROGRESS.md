# Progress Tracker — Mid Evaluation

| Component | Owner | Status | What's done | What's next |
|---|---|---|---|---|
| DAG Scheduling Core | Member 1 | 🟡 Working v1 | `ITaskScheduler` interface + `HeftLiteScheduler` (upward-rank priority) implemented and running in the end-to-end demo | Add a second policy for comparison (e.g. plain FIFO) to show the Strategy pattern actually matters; unit tests on a hand-built DAG |
| Offloading Engine | Member 2 | 🟡 Working v1 | `IOffloadingPolicy` interface + `GreedyLatencyLoadPolicy` (load + latency scoring) implemented and running | Tune/justify `latency_weight`; add a naive baseline (nearest-server-only) to compare against |
| Resource & Queue Manager | Member 3 | 🟡 Working v1 | `IResourceAllocator` interface + `FifoResourceAllocator` (per-server FIFO queue, heterogeneous capacity) implemented and running | Add a priority-queue variant; expose queue-length-over-time for a congestion plot |
| SUMO/TraCI Integration | Member 4 | 🟡 Mocked, real integration pending | `IMobilityProvider` interface + `MockMobilityProvider` (synthetic vehicle motion, distance-based latency model) implemented and feeding the demo | Swap in real `SumoBridge` (libsumo) — interface is already in place, this is a drop-in replacement, not a redesign |

**Overall: core pipeline is implemented and runs end-to-end** (`main.cpp` ->
`./demo`), producing real per-task completion times and a makespan — see
`results/demo_run.csv`. The one deliberately-deferred piece is swapping
the mock mobility feed for real libsumo; everything downstream of it
already works against that interface, so it's a scoped, well-isolated
remaining task rather than a blocker.

## Integration status

- [x] Scheduling -> Offloading interface wired up
- [x] Offloading -> Resource Manager interface wired up
- [ ] SUMO feeding live vehicle/latency data into Offloading Engine (currently mocked)
- [x] First end-to-end run completed
- [x] Baseline completion-time metric captured (see `results/demo_run.csv`)

## Mid-eval talking points

1. **Problem restated in one line:** Given a DAG of tasks, decide execution
   order, which edge server each task runs on, and how it's queued —
   jointly, under congestion and heterogeneous capacity — to minimize
   total completion time.
2. **What's runnable today (demo-able):** `./demo` builds a 5-task
   fork-join DAG, schedules it via upward-rank priority, offloads each
   task to one of two heterogeneous servers using a load+latency score,
   executes it through per-server FIFO queues, and reports per-task
   completion times and total makespan.
3. **Biggest technical risk / open question:** Real libsumo integration —
   build/link setup and getting realistic latency numbers from actual
   vehicle trajectories instead of the placeholder distance model.
4. **Next milestone (post mid-eval):** Replace `MockMobilityProvider`
   with `SumoBridge`; add a second policy per module so we can compare
   against a baseline and show the joint-optimization actually helps.
