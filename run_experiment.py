#!/usr/bin/env python3
"""Run the IoV edge-scheduling simulator against SUMO.

This script starts SUMO (or sumo-gui) on a free TCP port, runs the C++
simulator (build/iov_sim) connected to it, and shuts SUMO down afterwards.

Examples
--------
  # one run, headless
  python run_experiment.py --offloading eft --load 0.6

  # real workflow DAGs (run `python data/prepare_wfcommons.py` once first)
  python run_experiment.py --workload wfcommons

  # the ML policy (uses ml/offload_mlp.txt)
  python run_experiment.py --offloading ml

  # watch it in the SUMO GUI (vehicles are coloured by the server they use)
  python run_experiment.py --gui

  # no SUMO at all (synthetic vehicles)
  python run_experiment.py --mock

  # every experiment in the README
  python run_experiment.py --sweep
"""
import argparse
import csv
import itertools
import os
import platform
import shutil
import socket
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
IS_WINDOWS = platform.system() == "Windows"
EXE = ".exe" if IS_WINDOWS else ""

SCHEDULERS = ["fifo", "heft", "srpt"]
QUEUES = ["fifo", "priority"]
SCENARIOS = ["city", "small"]
WARMUP = {"city": 100.0, "small": 60.0}  # seconds before the first DAG
POLICIES = ["eft", "least_loaded", "nearest", "ml"]
WORKLOADS = ["synthetic", "wfcommons"]
DAG_FILE = os.path.join(REPO_ROOT, "data", "wfcommons_dags.csv")
ML_MODEL = os.path.join(REPO_ROOT, "ml", "offload_mlp.txt")


# ------------------------------------------------------------------ discovery

def find_driver(explicit=None):
    candidates = [explicit] if explicit else [
        os.path.join(REPO_ROOT, "build", "iov_sim" + EXE),
        os.path.join(REPO_ROOT, "build", "Release", "iov_sim" + EXE),  # Visual Studio
        os.path.join(REPO_ROOT, "iov_sim" + EXE),
    ]
    for c in candidates:
        for path in (c, c + EXE):
            if path and os.path.isfile(path):
                return path
    sys.exit("Could not find the iov_sim executable. Build it first:\n"
             "    cmake -S . -B build && cmake --build build\n"
             "or pass --driver-bin PATH.")


def find_sumo(gui=False):
    name = ("sumo-gui" if gui else "sumo") + EXE
    places = []
    if os.environ.get("SUMO_HOME"):
        places.append(os.path.join(os.environ["SUMO_HOME"], "bin", name))
    try:  # installed with `pip install eclipse-sumo`
        import sumo as sumo_pkg
        places.append(os.path.join(os.path.dirname(sumo_pkg.__file__), "bin", name))
    except ImportError:
        pass
    for p in places:
        if os.path.isfile(p):
            return p
    on_path = shutil.which(name)
    if on_path:
        return on_path
    sys.exit(f"Could not find '{name}'. Install SUMO with:\n"
             f"    {os.path.basename(sys.executable)} -m pip install eclipse-sumo\n"
             "(using the same Python you run this script with), or set SUMO_HOME.")


def check_inputs(workloads, policies):
    if "wfcommons" in workloads and not os.path.exists(DAG_FILE):
        sys.exit("The wfcommons workload needs data/wfcommons_dags.csv. Create it with:\n"
                 "    python data/prepare_wfcommons.py")
    if "ml" in policies and not os.path.exists(ML_MODEL):
        sys.exit("The ml policy needs ml/offload_mlp.txt. Train it with:\n"
                 "    python ml/train_offload_model.py")


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# ------------------------------------------------------------------ one run

def run_one(driver, sumo, *, out_prefix, label, scenario="city", scheduler="fifo", offloading="eft",
            server_queue="fifo", workload="synthetic", load=0.4, arrival_rate=None, seed=1, slots=0,
            warmup=100.0, duration=300.0, drain=120.0, mock=False, gui=False, gui_delay=50,
            extra=(), quiet=False):
    """Run iov_sim once (starting and stopping SUMO around it)."""
    os.makedirs(os.path.dirname(os.path.join(REPO_ROOT, out_prefix)), exist_ok=True)
    args = ["--servers", os.path.join("sumo_scenario", scenario, "servers.csv"),
            "--scheduler", scheduler, "--offloading", offloading, "--server-queue", server_queue,
            "--workload", workload,
            "--seed", str(seed), "--server-slots", str(slots), "--warmup", str(warmup),
            "--duration", str(duration), "--drain", str(drain),
            "--out-prefix", out_prefix, "--label", label]
    args += ["--arrival-rate", str(arrival_rate)] if arrival_rate is not None else ["--load", str(load)]
    args += list(extra)
    out = subprocess.DEVNULL if quiet else None

    if mock:
        subprocess.run([driver, "--mock"] + args, check=True, cwd=REPO_ROOT, stdout=out)
        return

    for attempt in range(3):  # retry if another process grabbed the port
        port = free_port()
        sumo_cmd = [sumo, "-c", os.path.join("sumo_scenario", scenario, "grid.sumocfg"),
                    "--remote-port", str(port), "--step-length", "0.1"]
        if gui:
            sumo_cmd += ["--gui-settings-file", os.path.join("sumo_scenario", "gui-settings.xml"),
                         "--start", "--quit-on-end", "--delay", str(gui_delay)]
        sumo_proc = subprocess.Popen(sumo_cmd, cwd=REPO_ROOT,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            # No port probe here: SUMO accepts exactly one TraCI client, so a
            # probe would steal it. iov_sim retries its own connection.
            time.sleep(0.2)
            cmd = [driver, "--port", str(port)] + args + (["--color-vehicles"] if gui else [])
            result = subprocess.run(cmd, cwd=REPO_ROOT, stdout=out)
            if result.returncode == 0:
                return
            if sumo_proc.poll() is None or attempt == 2:
                raise RuntimeError(f"iov_sim failed for {out_prefix} (exit {result.returncode})")
        finally:
            if sumo_proc.poll() is None:
                sumo_proc.terminate()
                try:
                    sumo_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    sumo_proc.kill()


def run_many(driver, sumo, jobs, n_parallel, progress_every=20):
    """jobs: list of dicts of run_one keyword arguments. Returns their out_prefixes."""
    t0, done, prefixes = time.time(), 0, []
    with ThreadPoolExecutor(max_workers=n_parallel) as ex:
        futures = [ex.submit(run_one, driver, sumo, quiet=True, **j) for j in jobs]
        for f in as_completed(futures):
            f.result()  # re-raises if a run failed
            done += 1
            if done % progress_every == 0 or done == len(jobs):
                print(f"  {done}/{len(jobs)} runs done ({time.time() - t0:.0f} s)", flush=True)
    return [j["out_prefix"] for j in jobs]


def merge_csvs(paths, dest):
    rows, header = [], None
    for p in paths:
        with open(p, newline="") as f:
            r = list(csv.reader(f))
        header = header or r[0]
        rows.extend(r[1:])
    with open(dest, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


# ------------------------------------------------------------------ sweep

# Benchmark configurations: (scheduler, offloading, server queue, backlog view).
#   A. Offloading: FIFO order + FIFO servers; only the server choice changes.
#   B. Framework ablation: EFT server choice; scheduling and resource allocation change.
#   C. The ML model inside the proposed framework.
# QAJS (the proposed framework) = srpt + eft + priority + "all".
BENCHMARK = [
    ("fifo", "nearest", "fifo", "all"),
    ("fifo", "least_loaded", "fifo", "all"),
    ("fifo", "ml", "fifo", "all"),
    ("fifo", "eft", "fifo", "all"),
    ("heft", "eft", "fifo", "all"),
    ("heft", "eft", "priority", "all"),
    ("srpt", "eft", "fifo", "all"),
    ("srpt", "eft", "priority", "ahead"),
    ("srpt", "eft", "priority", "all"),
    ("srpt", "ml", "priority", "all"),
]


def config_label(sch, pol, q, view):
    return f"{sch}_{pol}_{q}" + ("_ahead" if view == "ahead" else "")


def sweep_jobs(loads, seeds, mock, scenario):
    jobs = []
    for (sch, pol, q, view), wl, load, seed in itertools.product(BENCHMARK, WORKLOADS, loads, seeds):
        label = config_label(sch, pol, q, view)
        prefix = os.path.join("results", "sweep", f"{wl}_{label}_u{load:g}_s{seed}")
        extra = ["--backlog-view", view] + ([] if seed == 1 else ["--light"])
        jobs.append(dict(out_prefix=prefix, label=label, scenario=scenario, scheduler=sch,
                         offloading=pol, server_queue=q, workload=wl, load=load, seed=seed, mock=mock,
                         warmup=WARMUP[scenario], extra=extra))
    return jobs


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", default="city", choices=SCENARIOS,
                    help="city: 8x8 grid, 16 servers (default); small: 4x4 grid, 4 servers")
    ap.add_argument("--scheduler", default="srpt", choices=SCHEDULERS)
    ap.add_argument("--server-queue", default="priority", choices=QUEUES,
                    help="how a server picks its next task (default priority)")
    ap.add_argument("--offloading", default="eft", choices=POLICIES + ["random"])
    ap.add_argument("--workload", default="synthetic", choices=WORKLOADS)
    ap.add_argument("--load", type=float, default=0.4,
                    help="offered load: arriving work / total server capacity (default 0.4)")
    ap.add_argument("--arrival-rate", type=float, default=None, help="DAGs per second instead of --load")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--server-slots", type=int, default=0,
                    help="max tasks a server holds at once (uploading+queued+running); 0 = unlimited")
    ap.add_argument("--warmup", type=float, default=None, help="default: 100 s (city), 60 s (small)")
    ap.add_argument("--duration", type=float, default=300.0)
    ap.add_argument("--drain", type=float, default=120.0)
    ap.add_argument("--out-prefix", default=os.path.join("results", "run"))
    ap.add_argument("--log-positions", action="store_true", help="write vehicle positions (for visualize.py)")
    ap.add_argument("--mock", action="store_true", help="synthetic vehicles, no SUMO needed")
    ap.add_argument("--gui", action="store_true", help="show the run live in sumo-gui")
    ap.add_argument("--gui-delay", type=int, default=50, help="ms between GUI steps (default 50)")
    ap.add_argument("--driver-bin", default=None, help="path to iov_sim (auto-detected)")
    ap.add_argument("--sweep", action="store_true", help="run every experiment in the README")
    ap.add_argument("--sweep-loads", default="0.2,0.4,0.6,0.8,0.9,0.95")
    ap.add_argument("--sweep-seeds", type=int, default=10, help="number of seeds (1..N)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                    help="parallel runs during --sweep")
    a = ap.parse_args()

    driver = find_driver(a.driver_bin)
    sumo = None if a.mock else find_sumo(a.gui)

    if not a.sweep:
        check_inputs([a.workload], [a.offloading])
        label = f"{a.scheduler}_{a.offloading}_{a.server_queue}"
        load_txt = f"{a.arrival_rate} DAG/s" if a.arrival_rate is not None else f"load {a.load}"
        print(f"Running {label} on the {a.scenario} scenario, {a.workload} workload, {load_txt}"
              + (" (mock mobility)" if a.mock else f" with {os.path.basename(sumo)}"), flush=True)
        run_one(driver, sumo, out_prefix=a.out_prefix, label=label, scenario=a.scenario, scheduler=a.scheduler,
                offloading=a.offloading, server_queue=a.server_queue, workload=a.workload, load=a.load,
                arrival_rate=a.arrival_rate, seed=a.seed, slots=a.server_slots,
                warmup=a.warmup if a.warmup is not None else WARMUP[a.scenario], duration=a.duration, drain=a.drain,
                mock=a.mock, gui=a.gui, gui_delay=a.gui_delay,
                extra=["--log-positions"] if a.log_positions else [])
        print(f"Outputs: {a.out_prefix}_*.csv", flush=True)
        return

    check_inputs(WORKLOADS, [c[1] for c in BENCHMARK])
    loads = [float(x) for x in a.sweep_loads.split(",")]
    jobs = sweep_jobs(loads, range(1, a.sweep_seeds + 1), a.mock, a.scenario)
    sweep_dir = os.path.join(REPO_ROOT, "results", "sweep")
    if os.path.isdir(sweep_dir):
        shutil.rmtree(sweep_dir)
    print(f"Sweep: {len(jobs)} runs, {a.jobs} in parallel...", flush=True)
    prefixes = run_many(driver, sumo, jobs, a.jobs)
    merge_csvs(sorted(os.path.join(REPO_ROOT, p + "_summary.csv") for p in prefixes),
               os.path.join(REPO_ROOT, "results", "summary.csv"))
    print("Wrote results/summary.csv. Next: python plot_results.py", flush=True)


if __name__ == "__main__":
    main()
