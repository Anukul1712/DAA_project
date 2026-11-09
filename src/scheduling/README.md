# Scheduling (task priorities and dispatch order)

All tasks whose dependencies are finished, from all DAGs, wait in one list.
Each tick the scheduler gives every waiting task a **priority** (lower =
more urgent) and the driver offers tasks to servers in that order. With
`--server-queue priority`, servers also use the same priority to choose
which queued task runs next, which joins scheduling and resource allocation.

| File | What it is |
|---|---|
| `task_scheduler.h` | `Task`, `Dag`, `ReadyTask` types and the `ITaskScheduler` interface |
| `srpt_dag_scheduler.h` | **DAG-SRPT** (ours): the DAG with the least remaining work first; critical path breaks ties |
| `heft_lite_scheduler.h` | **HEFT-lite**: highest upward rank first (the ordering phase of HEFT) |
| `fifo_scheduler.h` | **FIFO**: the task that became ready first goes first |
| `dag_generator.h` | Random DAG generator (synthetic workload) |
| `workload.h` | `IDagSource`: synthetic DAGs, or real workflow DAGs from `data/wfcommons_dags.csv` |

**Formulas**

- Upward rank: `rank(t) = w_t / f_mean + d_t / r_mean + max over children c of rank(c)`
- HEFT-lite priority: `-rank(t)`
- DAG-SRPT priority: `W_g - 1e-6 * rank(t)`, where `W_g` is the total work
  of the unfinished tasks of the task's DAG `g`
- FIFO priority: the time the task became ready

**Random DAGs** (`dag_generator.h`): 3–6 tasks; every task after the first
depends on 1–2 earlier tasks; work U(100, 500) Mcycles; input U(50, 400) KB.

Tests: `tests/run_tests.cpp` (rank values, FIFO order, DAG-SRPT order,
generator validity).
