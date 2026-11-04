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

### One run (about 3 seconds)

```bash
python run_experiment.py
```

This starts SUMO in the background and runs the simulator against it: a
60 s warm-up, then 300 s of task arrivals, then up to 120 s for the last
DAGs to finish. Then it stops SUMO. It prints:

```
done at t=360.3 s: 1205/1205 DAGs completed (0 unfinished), mean makespan 0.845358 s, p95 1.2993 s
```

(Same numbers on the same compiler; other compilers may give slightly
different random workloads.)

The results go to `results/run_*.csv`:

| File | One row per | Main columns |
|---|---|---|
| `run_dags.csv` | DAG | `makespan_s`, `slowdown`, `n_tasks` |
| `run_tasks.csv` | task | `server_id`, `wait_ms`, `upload_ms`, `queue_ms`, `exec_ms` |
| `run_ticks.csv` | 0.1 s tick | vehicles, per-server tasks and backlog |
| `run_summary.csv` | run | averages, percentiles, utilisation |

Change the setup with flags:

```bash
python run_experiment.py --workload wfcommons --offloading ml --load 0.6 --seed 3
```

| Flag | Values | Default |
|---|---|---|
| `--workload` | `synthetic` (random DAGs), `wfcommons` (real workflow DAGs) | `synthetic` |
| `--offloading` | `eft`, `least_loaded`, `nearest`, `ml` | `eft` |
| `--scheduler` | `heft`, `fifo` | `heft` |
| `--load` | offered load: arriving work ÷ total server capacity (0.6 = 60%) | `0.4` |
| `--arrival-rate` | DAGs per second, instead of `--load` | – |
| `--server-slots` | max tasks a server holds at once, `0` = no limit | `0` |
| `--seed` | any integer | `1` |
| `--mock` | use synthetic vehicles instead of SUMO | off |

`python run_experiment.py --help` lists the rest.

### Watch it in the SUMO GUI

```bash
python run_experiment.py --gui
```

The sumo-gui window opens and plays the run. The four edge servers are
the coloured circles near the corners. While a vehicle has tasks in the
system it takes the colour of the server running them, and it goes back to
yellow when its tasks finish. With the default 50 ms delay per step, a
full run takes about 3 minutes to play. Use the GUI's *Delay* box, or
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
python ml/train_offload_model.py     # 32 SUMO runs + training; about 1 minute
```

See [ml/README.md](ml/README.md) for what it does.

### Reproduce all results in the README

```bash
python run_experiment.py --sweep     # 560 runs; about 6 minutes with 6 parallel jobs
python plot_results.py               # figures + results/aggregate.csv
```

The sweep covers two experiments on both workloads, at 4 loads with 10
seeds each:

- **Offloading benchmark:** EFT, Least-loaded, Nearest and ML, with HEFT-lite order.
- **Scheduler comparison:** HEFT-lite vs FIFO with EFT offloading, with and
  without an admission limit of 5 tasks per server.

Use `--jobs N` to set the parallelism, and `--sweep-seeds 3` for a quicker,
rougher version. The raw per-run files in `results/sweep/` take about
75 MB and can be deleted after plotting.

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
`sumo -c sumo_scenario/grid.sumocfg --remote-port 8813`, then run
`build/iov_sim --port 8813` (`build/iov_sim --help` lists all options).
Or run `build/iov_sim --mock` without SUMO at all.
