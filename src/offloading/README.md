# Edge Server Selection / Offloading Engine

**Owner:** Member 2

## What this module owns

- For each ready task (handed off by `scheduling/`), decide: execute
  locally, or offload — and if offloading, to which edge server
- Uses live server state (queue length, load) from `resource_mgmt/` and,
  once wired up, live proximity/latency data from `simulation/` (SUMO)
- Swappable offloading policy via the `IOffloadingPolicy` Strategy
  interface (e.g. nearest-server, least-loaded, latency-aware greedy)

## Interface contract

```
(Scheduling Core) --ordered tasks--> IOffloadingPolicy --assignment--> (Resource Manager)
                                            ^
                                            | queue/load state, vehicle position
                                            (Resource Manager, SUMO bridge)
```

## Status

- [x] `IOffloadingPolicy` interface defined
- [x] Concrete policy implemented: `GreedyLatencyLoadPolicy`
      (`greedy_latency_load_policy.h`) — scores each server by
      `queue_length / cpu_capacity + latency_weight * link_latency_ms`,
      picks the minimum
- [x] Wired to receive tasks from `scheduling/` (via `main.cpp`)
- [x] Wired to query `resource_mgmt/` for live server state
- [ ] Baseline policy (e.g. nearest-server-only) to compare against
- [ ] `latency_weight` tuned/justified rather than a placeholder default

## What to present at mid-eval

- The scoring rule and why it captures "joint" congestion + proximity
  optimization from the objective
- That it's already consuming live queue state from `resource_mgmt/` and
  live (currently mocked) latency from `simulation/`
