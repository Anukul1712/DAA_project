# Queue & Resource Allocation Manager

**Owner:** Member 3

## What this module owns

- Per-edge-server queue state (what's waiting, what's executing)
- Heterogeneous resource modeling (servers differ in CPU/bandwidth
  capacity)
- Admission/execution of assigned tasks, and reporting live queue-length
  and load state back to `offloading/` so it can make congestion-aware
  decisions
- Swappable allocation policy via `IResourceAllocator` (e.g. FIFO,
  priority queue, capacity-weighted)

## Interface contract

```
(Offloading Engine) --assignment--> IResourceAllocator --admits/executes--> Server queues
                                            |
                                            +--> queue/load state exposed back to Offloading Engine
```

## Status

- [x] Per-server queue data structure implemented (`std::deque<QueueEntry>`
      per server in `FifoResourceAllocator`)
- [x] Heterogeneous capacity modeling — each `ServerSpec` has its own
      `cpu_capacity`
- [x] `IResourceAllocator` interface + concrete `FifoResourceAllocator`
      (`fifo_resource_allocator.h`)
- [x] State-reporting API (`currentState()`) that `offloading/` queries
      each tick
- [ ] Priority-queue variant (currently FIFO only)
- [ ] Queue-length-over-time logging for a congestion plot

## What to present at mid-eval

- `tick(dt_ms)`: head-of-line task on each server loses
  `cpu_capacity * dt` of remaining workload per tick — this is the
  "dynamic queue congestion" and "heterogeneous resources" requirement
  from the objective, made concrete
- `currentState()` as the live feedback loop into the offloading decision
