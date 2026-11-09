# How to run

You need three things: **Python 3**, a **C++17 compiler with CMake**, and
**SUMO**. Everything is run from the project folder.

> On Linux/macOS, type `python3` wherever this guide says `python`.

---

## 1. Install the tools (once)

### Windows

1. **Python 3.9 or newer** from [python.org](https://www.python.org/downloads/).
   Tick *"Add python.exe to PATH"* during setup.
2. **MSYS2** from [msys2.org](https://www.msys2.org/). Open the
   **"MSYS2 UCRT64"** shell and run:
   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
   ```
   Then add `C:\msys64\ucrt64\bin` to your Windows `PATH`, or just run every
   command below from the MSYS2 UCRT64 shell.

### Ubuntu / Debian

```bash
sudo apt install build-essential cmake python3 python3-pip
```

### macOS

```bash
xcode-select --install
brew install cmake python
```

---

## 2. Install the Python packages (once)

```bash
python -m pip install -r requirements.txt
```

This installs SUMO itself (`eclipse-sumo`), `pandas`, `matplotlib` and
`pillow` for the plots, and `scikit-learn` for the ML model. If you already
have SUMO installed and `SUMO_HOME` set, that copy is used instead.

> **Use the same `python` for everything.** If you have several Pythons,
> install with the one you will run the scripts with. Check with
> `python -c "import sumo, pandas, matplotlib, sklearn; print('ok')"`.

Then download the public real-workflow dataset (about 1.5 MB, needs
internet once):

```bash
python data/prepare_wfcommons.py
```

---

## 3. Build

```bash
cmake -S . -B build
cmake --build build
```

This makes two programs in `build/`: `iov_sim` (the simulator) and
`iov_tests` (unit tests). Check the build:

```bash
ctest --test-dir build --output-on-failure
```

You should see `100% tests passed`.

---

## 4. Run

### One run

```bash
python run_experiment.py
```

This runs the proposed framework (QAJS) on the large **city** scenario:
an 8×8-block SUMO city with several hundred vehicles and 16 edge servers.
It starts SUMO in the background, then runs a 100 s warm-up, 300 s of
application arrivals at 40% load, and up to 120 s for the last DAGs to
finish. It takes about 20–60 seconds and ends with a line like:

```
done at t=401.4 s: 5398/5398 DAGs, 24324 tasks completed (0 DAGs unfinished), mean makespan 0.841036 s, p95 1.29835 s
```

The results go to `results/run_*.csv`:

| File | One row per | Main columns |
|---|---|---|
| `run_dags.csv` | DAG | `makespan_s`, `slowdown`, `n_tasks` |
| `run_tasks.csv` | task | `server_id`, `wait_ms`, `upload_ms`, `queue_ms`, `exec_ms` |
| `run_ticks.csv` | 0.1 s tick | vehicles, active DAGs, total and max backlog |
| `run_summary.csv` | run | averages, percentiles, utilisation per server |

Change the setup with flags:

```bash
python run_experiment.py --workload wfcommons --load 0.9 --scheduler fifo --server-queue fifo --offloading ml
```

| Flag | Values | Default |
|---|---|---|
| `--scenario` | `city` (16 servers), `small` (4×4 grid, 4 servers) | `city` |
| `--workload` | `synthetic` (random DAGs), `wfcommons` (real workflow DAGs) | `synthetic` |
| `--scheduler` | `srpt` (DAG-SRPT), `heft`, `fifo` | `srpt` |
| `--offloading` | `eft`, `least_loaded`, `nearest`, `ml` | `eft` |
| `--server-queue` | `priority`, `fifo`: how a server picks its next task | `priority` |
| `--load` | offered load: arriving work ÷ total server capacity (0.9 = 90%) | `0.4` |
| `--seed` | any integer | `1` |
| `--mock` | use synthetic vehicles instead of SUMO | off |

The defaults (`srpt` + `eft` + `priority`) are QAJS. `python run_experiment.py --help` lists the rest.

### Watch it in the SUMO GUI

```bash
python run_experiment.py --gui
```

The sumo-gui window opens and plays the run. The edge servers are the
coloured circles (colour = server speed: blue 2, orange 3, green 4,
violet 6 Mcycles/ms). While a vehicle has tasks in the
system it takes the colour of the server running them, and it goes back to
yellow when its tasks finish. With the default 50 ms delay per step, a
full run takes several minutes to play (`--scenario small` is quicker to watch). Use the GUI's *Delay* box, or
`--gui-delay 0`, to speed it up. Close the window to stop.

### Make the network pictures (no GUI needed)

```bash
python run_experiment.py --log-positions --out-prefix results/viz
python visualize.py --run results/viz
```

This writes `results/plots/network_snapshot.png` and `network_animation.gif`.

### Retrain the ML model (optional)

A trained model (`ml/offload_mlp.txt`) is included. To retrain it from scratch:

```bash
python ml/train_offload_model.py     # 96 SUMO runs + training; about 15 minutes
```

See [ml/README.md](ml/README.md) for what it does.

### Reproduce all results in the README

```bash
python run_experiment.py --sweep --jobs 14   # 1200 runs on the city scenario; ~1.5-2 hours on 14 cores
python plot_results.py                       # figures + results/aggregate.csv + paired tables
```

The sweep runs 10 configurations (4 server-selection baselines, 5
scheduling/allocation variants, and ML inside QAJS) × 2 workloads × 6 loads
(20–95%) × 10 seeds. Set `--jobs` to about half your CPU cores. Use
`--sweep-seeds 3` or `--sweep-loads 0.4,0.9` for a quicker, rougher version.
The raw per-run files in `results/sweep/` can be deleted after plotting.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Could not find the iov_sim executable` | Build first (step 3). If you built somewhere else, pass `--driver-bin path/to/iov_sim`. |
| `Could not find 'sumo'` | `python -m pip install eclipse-sumo` with the **same** Python you run the script with. |
| `ModuleNotFoundError: pandas` / `matplotlib` / `sklearn` | Same cause: install `requirements.txt` with the Python you are using. |
| `The wfcommons workload needs data/wfcommons_dags.csv` | Run `python data/prepare_wfcommons.py` (needs internet once). |
| CMake says the compiler *"is not able to compile a simple test program"* (Windows) | Another MinGW is first on your `PATH` (Git for Windows ships one). Use the MSYS2 UCRT64 shell, or put `C:\msys64\ucrt64\bin` first in `PATH`. |
| `--gui` does nothing on Linux | sumo-gui needs a desktop session. On a server, use the pictures from `visualize.py` instead. |
| Visual Studio instead of MSYS2 | `cmake -S . -B build` then `cmake --build build --config Release`. The runner also looks in `build/Release/`. This setup has not been tested. |

Run the simulator directly, without the Python wrapper: start
`sumo -c sumo_scenario/city/grid.sumocfg --remote-port 8813`, then run
`build/iov_sim --port 8813 --scheduler srpt --server-queue priority`
(`build/iov_sim --help` lists all options). Or run `build/iov_sim --mock`
without SUMO at all.

The SUMO scenarios are included. To rebuild them from scratch (fixed seeds,
identical result): `python sumo_scenario/make_scenario.py`.
