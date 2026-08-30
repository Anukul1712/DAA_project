# SUMO / TraCI Simulation & Evaluation Layer

**Owner:** Member 4

## What this module owns

- Driving the SUMO mobility simulation via **libsumo** (in-process C++ API
  — faster than the socket-based TraCI client, worth calling out at the
  mid-eval as a deliberate design choice)
- Reading live vehicle positions each simulation step and computing
  vehicle-to-edge-server proximity / link latency
- Feeding that latency data into `offloading/`'s `ServerState`
- Collecting evaluation metrics (end-to-end completion time, per-task
  latency, offload ratio) into `results/`

## Interface contract

```
libsumo step --> vehicle positions --> latency model --> ServerState.link_latency_ms
                                                                  |
                                                                  v
                                                          (Offloading Engine)
```

This module is the one place that talks to SUMO directly — nothing else
in the repo should `#include <libsumo/...>`.

## Status

- [x] `IMobilityProvider` interface defined — shared by the mock feed and
      the real SUMO bridge, so downstream code doesn't care which is
      plugged in
- [x] `MockMobilityProvider` implemented — synthetic vehicles on circular
      paths, distance-based latency model. **This is what the current
      demo runs against**, clearly labeled as a placeholder
- [x] Vehicle position → edge-server latency mapping implemented (in the
      mock; same signature the real bridge will use)
- [x] Metrics collection — `main.cpp` writes `results/demo_run.csv`
      (per-task completion time + makespan)
- [ ] SUMO network/route files set up (`.net.xml`, `.rou.xml`)
- [ ] libsumo linked and `SumoBridge` actually implemented (currently a
      stub — this is the real remaining work for this module)

## What to present at mid-eval

- **Be upfront about this one:** the pipeline runs today against a mock
  mobility feed, not real SUMO yet. Show `IMobilityProvider` and explain
  that `SumoBridge` is a drop-in replacement behind the same interface —
  this is a scoped remaining task, not a redesign
- Walk through `MockMobilityProvider`'s latency model as a stand-in for
  what real vehicle-to-edge distances will produce
- `results/demo_run.csv` as proof the metrics-collection path works
  end-to-end already

## Setup notes

Requires `SUMO_HOME` set and libsumo built/available
(`pip install libsumo` gives you headers/libs location, or build SUMO
from source with `--enable-libsumo`). Link against `libsumocpp` in
CMake.
