# Scheduling (task order)

Decides **in which order waiting tasks are dispatched**. All tasks whose
dependencies are finished, from all DAGs, sit in one waiting list; each tick
the scheduler sorts that list and the driver offers tasks to servers in that
order.

| File | What it is |
|---|---|
| `task_scheduler.h` | `Task`, `Dag`, `ReadyTask` types and the `ITaskScheduler` interface |
| `heft_lite_scheduler.h` | **HEFT-lite**: highest *upward rank* first (the prioritisation phase of HEFT) |
| `fifo_scheduler.h` | **FIFO** baseline: the task that became ready first goes first |
| `dag_generator.h` | Random DAG generator (synthetic workload) |
| `workload.h` | `IDagSource`: synthetic DAGs, or real workflow DAGs read from `data/wfcommons_dags.csv` |

**Upward rank** of a task = its mean execution time + mean upload time + the
largest rank among its children. It estimates how long the rest of the DAG
takes from this task onwards, so the highest rank is on the critical path.
"Lite" because only HEFT's ordering phase is implemented; picking the server
is the offloading policy's job (`eft` there is the closest to HEFT's
server-selection phase).

**When does the order matter?** Mostly when tasks have to wait for a
server. With unlimited admission every waiting task is dispatched in the same
tick, and the order only sets queue positions; HEFT-lite is then slightly
faster than FIFO (about 0.3–1%). With an admission limit (`--server-slots K`)
tasks queue centrally and the order decides who goes first. Under heavy load
HEFT-lite is then clearly *worse*, because new DAGs keep jumping ahead of
older ones. See the main README for the numbers.

**Random DAGs** (`dag_generator.h`): 3–6 tasks; every task after the first
depends on 1–2 earlier tasks (so the graph is acyclic and topologically
ordered); workload U(100, 500) Mcycles; input data U(50, 400) KB.

Tests: `tests/run_tests.cpp` (rank values, ordering, generator validity).
