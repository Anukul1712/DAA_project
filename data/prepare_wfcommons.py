#!/usr/bin/env python3
"""Build the real-DAG workload from the public WfCommons WfInstances dataset.

Downloads 9 real workflow executions (one small instance from each of 9
applications, e.g. Montage, Epigenomics, 1000Genome, BLAST) from
https://github.com/wfcommons/WfInstances at a pinned commit, and converts
them into data/wfcommons_dags.csv, which iov_sim reads with
--workload wfcommons.

What is kept from the data:  the real DAG structure (dependencies) and the
relative runtime and input size of each task within its workflow.
What is rescaled:            each workflow is scaled so its mean task work is
300 Mcycles and its mean input is 225 KB -- the same means as the synthetic
workload -- because the originals run for seconds to hours on cluster
nodes, which does not fit a vehicular edge setting.

The dataset is licensed LGPL-3.0 by the WfCommons project; it is
downloaded on demand and not redistributed in this repository.

Usage:  python data/prepare_wfcommons.py
"""
import csv
import json
import os
import sys
import urllib.request
from collections import deque

COMMIT = "c532951e4c9c375c33e0b8c64e6da1b1a09eb440"  # WfInstances main, Nov 2026
BASE = f"https://raw.githubusercontent.com/wfcommons/WfInstances/{COMMIT}/"
INSTANCES = [
    "pegasus/1000genome/1000genome-chameleon-2ch-100k-001.json",
    "pegasus/cycles/cycles-chameleon-1l-1c-9p-001.json",
    "pegasus/epigenomics/epigenomics-chameleon-hep-1seq-100k-001.json",
    "pegasus/montage/montage-chameleon-dss-05d-001.json",
    "pegasus/seismology/seismology-chameleon-100p-001.json",
    "pegasus/soykb/soykb-chameleon-10fastq-10ch-001.json",
    "pegasus/srasearch/srasearch-chameleon-10a-001.json",
    "makeflow/blast/blast-chameleon-small-001.json",
    "makeflow/bwa/bwa-chameleon-small-001.json",
]
MEAN_WORK_MCYCLES = 300.0
MEAN_DATA_KB = 225.0
MIN_WORK_MCYCLES = 5.0
MIN_DATA_KB = 1.0

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "wfinstances")
OUT = os.path.join(HERE, "wfcommons_dags.csv")


def fetch(path):
    local = os.path.join(CACHE, os.path.basename(path))
    if not os.path.exists(local):
        os.makedirs(CACHE, exist_ok=True)
        print(f"  downloading {path}")
        with urllib.request.urlopen(BASE + path, timeout=60) as r, open(local, "wb") as f:
            f.write(r.read())
    with open(local, encoding="utf-8") as f:
        return json.load(f)


def topo_order(tasks):
    """Kahn's algorithm; ties broken by task id so the order is stable."""
    parents = {t["id"]: list(t.get("parents", [])) for t in tasks}
    children = {t["id"]: [] for t in tasks}
    for tid, ps in parents.items():
        for p in ps:
            children[p].append(tid)
    indeg = {tid: len(ps) for tid, ps in parents.items()}
    ready = deque(sorted(t for t, d in indeg.items() if d == 0))
    order = []
    while ready:
        t = ready.popleft()
        order.append(t)
        newly = []
        for c in children[t]:
            indeg[c] -= 1
            if indeg[c] == 0:
                newly.append(c)
        ready.extend(sorted(newly))
    if len(order) != len(tasks):
        raise ValueError("workflow has a cycle")
    return order, parents


def main():
    rows = []
    for path in INSTANCES:
        wf = fetch(path)["workflow"]
        spec, ex = wf["specification"], wf["execution"]
        runtime = {t["id"]: float(t.get("runtimeInSeconds", 0.0)) for t in ex["tasks"]}
        size = {f["id"]: int(f.get("sizeInBytes", 0)) for f in spec["files"]}
        inputs = {t["id"]: sum(size.get(f, 0) for f in t.get("inputFiles", [])) for t in spec["tasks"]}
        order, parents = topo_order(spec["tasks"])
        index = {tid: i for i, tid in enumerate(order)}

        mean_rt = sum(runtime[t] for t in order) / len(order)
        mean_in = sum(inputs[t] for t in order) / len(order)
        name = os.path.basename(path).replace(".json", "")
        for tid in order:
            work = max(MIN_WORK_MCYCLES, runtime[tid] / mean_rt * MEAN_WORK_MCYCLES) if mean_rt > 0 \
                else MEAN_WORK_MCYCLES
            data = max(MIN_DATA_KB, inputs[tid] / mean_in * MEAN_DATA_KB) if mean_in > 0 else MEAN_DATA_KB
            rows.append({
                "workflow": name,
                "task": index[tid],
                "parents": ";".join(str(index[p]) for p in sorted(parents[tid], key=index.get)),
                "workload_mcycles": round(work, 3),
                "data_kb": round(data, 3),
                "runtime_s": runtime[tid],
                "input_bytes": inputs[tid],
            })
        print(f"  {name}: {len(order)} tasks")

    with open(OUT, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"Wrote {os.path.relpath(OUT)} ({len(rows)} tasks from {len(INSTANCES)} workflows)")


if __name__ == "__main__":
    try:
        main()
    except OSError as e:
        sys.exit(f"Download failed ({e}). Check your internet connection and try again.")
