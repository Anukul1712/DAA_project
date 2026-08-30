# DAG Task Modeling & Scheduling Core

**Owner:** Member 1

## What this module owns

- Representing an application as a DAG of tasks (nodes = tasks, edges =
  data/precedence dependencies)
- Tracking which tasks are "ready" (all predecessors completed)
- Producing an execution order / priority for ready tasks, under a
  swappable scheduling policy (e.g. HEFT-style rank, earliest-deadline,
  greedy critical-path)

## Interface contract

`ITaskScheduler` (see `task_scheduler.h`) is the Strategy interface every
scheduling policy implements. `offloading/` and `resource_mgmt/` only ever
talk to this interface — never to a concrete policy — so policies can be
swapped for experiments without touching the rest of the pipeline.

```
DAGModel  --ready tasks-->  ITaskScheduler  --ordered tasks-->  (Offloading Engine)
```

## Status

- [x] DAG data structure implemented (`Task` in `task_scheduler.h`)
- [x] Dependency resolution / ready-queue logic implemented (in `main.cpp`'s
      drive loop for now — will move into this module as it grows)
- [x] Concrete policy implemented: `HeftLiteScheduler` (`heft_lite_scheduler.h`)
      — upward-rank priority, a simplified HEFT rule
- [ ] Unit tests for a small hand-built DAG
- [ ] Second policy to compare against (e.g. plain FIFO)

## What to present at mid-eval

- The `Task` struct and the sample 5-task fork-join DAG in `main.cpp`
- `HeftLiteScheduler`: rank(task) = workload + max(rank(child)), so tasks
  on the longest remaining dependency chain get priority
- How `ITaskScheduler` keeps this swappable — `main.cpp` only calls the
  interface, never `HeftLiteScheduler` directly
