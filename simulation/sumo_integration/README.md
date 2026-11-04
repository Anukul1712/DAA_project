# Mobility and SUMO integration

Supplies vehicle positions to the simulator and turns distance into link
quality.

| File | What it is |
|---|---|
| `mobility_provider.h` | `IMobilityProvider` interface (step, vehicle positions, optional recolouring) |
| `sumo_bridge.h/.cpp` | Real SUMO: talks to a running `sumo`/`sumo-gui` over TraCI |
| `traci_client.h` | Small TraCI client over a plain TCP socket (Windows and POSIX) |
| `mobility_mock.h` | Synthetic vehicles driving in a circle, for `--mock` runs without SUMO |
| `channel_model.h` | Distance → link latency and upload rate |
| `TRACI_PROTOCOL_NOTES.md` | Byte-level notes on the TraCI messages used |

## How it connects

`run_experiment.py` starts SUMO with `--remote-port N`, then starts `iov_sim
--port N`. Each tick `SumoBridge::step()` sends one *simulation step*
command and then fetches every vehicle's position and speed in a single
batched request. SUMO accepts exactly one TraCI client, so the runner does
not probe the port; `TraciClient::connect` retries instead.

TraCI commands implemented: simulation step, vehicle ID list, vehicle
position, vehicle speed, set vehicle colour (for the GUI view), close.

Why TraCI over a socket and not libsumo: libsumo has to be linked as a C++
library, and the `pip install eclipse-sumo` package does not ship one that
fits the MinGW toolchain. TraCI needs only the `sumo` program.

## Channel model (an assumption, not a measurement)

```
latency_ms  = 2 + 0.05 * distance_m
upload_rate = 5 KB/ms / (1 + distance_m / 200)     (5 KB/ms = 40 Mbit/s)
```

It only encodes "farther away means a slower, more delayed link". It is not
a radio or network simulator: no interference, no shadowing, no handover,
and no sharing of bandwidth between vehicles.

## Scenario (`sumo_scenario/`)

- `grid.net.xml`: 4×4 grid of junctions, 200 m blocks, 600 m × 600 m (made with `netgenerate`).
- `routes.rou.xml`: 500 random trips, one departure per second over 0–500 s
  (`randomTrips.py --seed 7 -p 1.0 --fringe-factor 5 --validate`). About 75
  vehicles are on the road at the same time once traffic has built up.
- `servers.add.xml`: the four edge servers, drawn as coloured circles in sumo-gui.
- `gui-settings.xml`: view settings used by `run_experiment.py --gui`.
