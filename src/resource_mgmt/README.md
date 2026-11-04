# Resource management (server queues)

Simulates the edge servers. `FifoResourceAllocator` (`fifo_resource_allocator.h`)
keeps, for each server:

1. **Uploading** tasks: admitted, but their input data is still on the way.
2. **Queue**: data has arrived; tasks run one at a time, in arrival order.

A server does `cpu_capacity` mega-cycles of work per millisecond. The
simulation is exact rather than tick-rounded: if a task finishes halfway
through a 100 ms tick, the next task starts at that moment, and the finish
time is reported precisely. So a fast server really is faster.

`state()` reports, per server, how many tasks it holds and how much work is
still pending (uploading + queued + the rest of the running task). The
offloading policies use this. `busyMs()` gives CPU busy time, for utilisation.

| File | What it is |
|---|---|
| `resource_allocator.h` | `ServerSpec`, `Job`, `Completion` and the `IResourceAllocator` interface |
| `fifo_resource_allocator.h` | The FIFO implementation described above |

Tests: `tests/run_tests.cpp` checks exact finish times, work spanning
several ticks, waiting for uploads, and queue order.
