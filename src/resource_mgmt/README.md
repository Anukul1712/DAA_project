# Resource management (server queues)

Simulates the edge servers and decides **which queued task gets a server's
CPU next**: the resource-allocation part of the framework.

`QueueResourceAllocator` (`fifo_resource_allocator.h`) keeps, for each server:

1. **Uploading** tasks: admitted, but their input data is still on the way.
2. **Queue**: data has arrived; waiting for the CPU.
3. **Running**: the one task on the CPU (non-preemptive).

When the CPU becomes free, the next task is picked by the **discipline**
(`--server-queue`):

| Discipline | Next task |
|---|---|
| `fifo` | the one whose data arrived first |
| `priority` | the smallest priority value given by the scheduler (ties: arrival order) |

A server does `cpu_capacity` mega-cycles of work per millisecond. The
simulation is exact rather than tick-rounded: if a task finishes halfway
through a 100 ms tick, the next task starts at that moment, and finish times
are reported precisely.

`state()` reports each server's number of tasks and pending work (uploading
+ queued + the rest of the running task). `stateFor(p)` reports only the work
a new task with priority `p` would wait for (the "optimistic" backlog view,
`--backlog-view ahead`). `busyMs()` gives CPU busy time for utilisation.

Tests: `tests/run_tests.cpp` checks exact finish times, work spanning
several ticks, waiting for uploads, FIFO order, priority order
(non-preemptive), and the priority-aware backlog.
