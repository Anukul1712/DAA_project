# Progress

| Component | Owner | Status |
|---|---|---|
| DAG scheduling (`src/scheduling/`) | Member 1 | Done: HEFT-lite (upward rank) and FIFO, random DAG generator, unit tests |
| Offloading (`src/offloading/`) | Member 2 | Done: EFT, Least-loaded, Nearest and an ML (neural-network) baseline, unit tests |
| Server queues (`src/resource_mgmt/`) | Member 3 | Done: exact (event-accurate) FIFO servers with upload phase, unit tests |
| SUMO / TraCI (`simulation/sumo_integration/`) | Member 4 | Done: TraCI client, SUMO bridge, mock feed, channel model, sumo-gui colouring |

## Integration

- [x] Scheduling → offloading → server queues wired together in `main.cpp`
- [x] Real SUMO vehicle positions drive every offloading decision and every upload time
- [x] Experiment runner (`run_experiment.py`) with single runs, `--gui`, `--mock` and `--sweep`
- [x] Figures (`plot_results.py`) and network visualisation (`visualize.py`)
- [x] Unit tests (`build/iov_tests`, run with `ctest`)
- [x] Real-DAG workload from the public WfCommons dataset (`data/`)
- [x] ML benchmark: neural-network offloading policy trained on simulator data (`ml/`)

## Changes after the first review

The first version had modelling problems that made its results unreliable.
They are fixed, and all results were regenerated:

| Problem | Fix |
|---|---|
| Every task finished in exactly one tick (work per tick > largest task, leftover capacity thrown away), so server speed made no difference | Servers are simulated exactly; leftover time goes to the next task; workloads now take tens to hundreds of ms |
| Link latency only influenced the choice and was never added to task time; `data_size` was unused | Every task now pays an upload time (latency + data / distance-dependent rate) |
| All ready tasks were dispatched immediately, so the scheduler could not matter | Global waiting list across DAGs, plus an optional per-server admission limit (`--server-slots`) |
| Finish times were stamped one tick early (some tasks took 0 s) | Exact finish times from the server simulation |
| "Task latency" was measured from DAG arrival | Main metric is now DAG makespan, plus a per-task time breakdown |
| Greedy score added `queue / capacity` to `0.3 × ms` (mismatched units) | Replaced by EFT, which estimates everything in ms |
| Completion ratio was always 1 by construction | Unfinished DAGs are counted and reported |
| 3 seeds, no error bars | 10 seeds, 95% confidence intervals, paired comparisons || TraCI extended-length field was off by one byte | Fixed and unit-tested |
| Docs and build names out of date (`iov_demo` vs `experiment_driver`, libsumo, "mocked") | One program (`iov_sim`), docs rewritten |

## Possible next steps

- Executing tasks locally on the vehicle as an extra option
- A multi-DAG-aware scheduler (e.g. smallest remaining DAG first), since
  HEFT-lite's critical-path order is not designed for many competing DAGs
- A richer channel model (shared bandwidth, interference, handover)
- TraCI subscriptions instead of polling, for larger networks
