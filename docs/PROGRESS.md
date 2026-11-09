# Progress

| Component | Owner | Status |
|---|---|---|
| DAG scheduling (`src/scheduling/`) | Member 1 | Done: FIFO, HEFT-lite (upward rank) and DAG-SRPT priorities, synthetic and WfCommons workloads, unit tests |
| Offloading (`src/offloading/`) | Member 2 | Done: EFT, Least-loaded, Nearest and an ML (neural-network) baseline, unit tests |
| Server queues (`src/resource_mgmt/`) | Member 3 | Done: exact (event-accurate) servers with upload phase, FIFO and priority disciplines, unit tests |
| SUMO / TraCI (`simulation/sumo_integration/`) | Member 4 | Done: TraCI client, SUMO bridge, mock feed, channel model, two scenarios, sumo-gui colouring |

## Integration

- [x] Scheduling → server selection → server queues wired together in `main.cpp`
- [x] Real SUMO vehicle positions drive every server choice and every upload time
- [x] QAJS: DAG-SRPT priorities + EFT + priority queues, working as one framework
- [x] Large city scenario: 8×8 grid, 2,000 trips, 16 heterogeneous servers
- [x] Experiment runner (`run_experiment.py`) with single runs, `--gui`, `--mock` and `--sweep`
- [x] Figures (`plot_results.py`) and network visualisation (`visualize.py`)
- [x] Unit tests (`build/iov_tests`, run with `ctest`)
- [x] Real-DAG workload from the public WfCommons dataset (`data/`)
- [x] ML baseline: neural-network server selection trained on simulator data (`ml/`)
- [x] Benchmark: 1,200 runs, 45.9 million tasks

## Changes after the first review

The first version had modelling problems that made its results unreliable.
They are fixed, and all results were regenerated:

| Problem | Fix |
|---|---|
| Every task finished in exactly one tick (work per tick > largest task, leftover capacity thrown away), so server speed made no difference | Servers are simulated exactly; leftover time goes to the next task; workloads take tens to hundreds of ms |
| Link latency only influenced the choice and was never added to task time; `data_size` was unused | Every task pays an upload time (latency + data / distance-dependent rate) |
| All ready tasks were dispatched immediately, so the scheduler could not matter | Global waiting list across DAGs; the scheduler's priority is also used by the server queues |
| Finish times were stamped one tick early (some tasks took 0 s) | Exact finish times from the server simulation |
| "Task latency" was measured from DAG arrival | Main metric is DAG makespan, plus a per-task time breakdown |
| Greedy score added `queue / capacity` to `0.3 × ms` (mismatched units) | Replaced by EFT, which estimates everything in ms |
| Completion ratio was always 1 by construction | Unfinished DAGs are counted and reported |
| 3 seeds, no error bars | 10 seeds, 95% confidence intervals, paired comparisons |
| TraCI extended-length field was off by one byte | Fixed and unit-tested |
| Docs and build names out of date (`iov_demo` vs `experiment_driver`, libsumo, "mocked") | One program (`iov_sim`), docs rewritten |

## Scale-up (second round)

| Before | After |
|---|---|
| 4×4 grid, 4 servers, 55–85 vehicles | 8×8 city, 16 servers (4 speed classes), 300–760 vehicles |
| No resource-allocation decision (FIFO servers only) | Priority queues driven by the scheduler's priority |
| HEFT-lite / FIFO order only | DAG-SRPT (shortest remaining application first) added |
| Loads up to 80% | Loads up to 95% |
| 560 runs | 1,200 runs, 11,000–61,000 tasks each |

## Possible next steps

- Priority ageing, so that large applications are not delayed too much (tail latency)
- Executing tasks locally on the vehicle as an extra option
- A richer channel model (shared bandwidth, interference, handover)
- Data transfer between servers for dependent tasks
- TraCI subscriptions instead of polling, for larger networks
