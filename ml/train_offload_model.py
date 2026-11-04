#!/usr/bin/env python3
"""Train the ML offloading model (a small neural network).

1. Collect data: run the simulator against SUMO with a *random* offloading
   policy (so every server is tried in every kind of situation), logging,
   for each task, six raw features of the server it was sent to and the
   real time from dispatch to finish:
       distance_m, data_kb, workload_mcycles, cpu_capacity,
       pending_work_mcycles, tasks_in_system   ->  actual_ms
   Training runs use seeds 1001-1004, which are never used in the
   evaluation (seeds 1-10), on both workloads and at 4 load levels.
2. Train an MLP regressor (2 hidden layers of 32 ReLU units, scikit-learn)
   to predict log(1 + actual_ms). The model is never shown the link
   latency/rate formula or the EFT estimate.
3. Test it on the held-out seed-1004 runs and compare its prediction error
   with that of EFT's analytic formula on the same tasks.
4. Export it to ml/offload_mlp.txt, which iov_sim loads for --offloading ml.

Usage:  python ml/train_offload_model.py [--jobs N]
"""
import argparse
import json
import os
import sys

import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
sys.path.insert(0, REPO_ROOT)
import run_experiment as rx  # noqa: E402

FEATURES = ["distance_m", "data_kb", "workload_mcycles", "cpu_capacity",
            "pending_work_mcycles", "tasks_in_system"]
TRAIN_SEEDS = [1001, 1002, 1003]
TEST_SEEDS = [1004]
LOADS = [0.2, 0.4, 0.6, 0.8]
DATA_DIR = os.path.join(HERE, "data")
MODEL_PATH = os.path.join(HERE, "offload_mlp.txt")
REPORT_PATH = os.path.join(HERE, "training_report.json")


def collect(jobs_parallel):
    rx.check_inputs(["wfcommons"], [])
    driver, sumo = rx.find_driver(), rx.find_sumo()
    jobs = []
    for wl in rx.WORKLOADS:
        for load in LOADS:
            for seed in TRAIN_SEEDS + TEST_SEEDS:
                prefix = os.path.relpath(os.path.join(DATA_DIR, f"{wl}_u{load:g}_s{seed}"), REPO_ROOT)
                jobs.append(dict(out_prefix=prefix, label="random", offloading="random", workload=wl,
                                 load=load, seed=seed, extra=["--light", "--log-decisions"]))
    print(f"Collecting training data: {len(jobs)} runs with random offloading...", flush=True)
    rx.run_many(driver, sumo, jobs, jobs_parallel, progress_every=8)
    frames = []
    for j in jobs:
        df = pd.read_csv(os.path.join(REPO_ROOT, j["out_prefix"] + "_decisions.csv"))
        df["workload"], df["load"], df["seed"] = j["workload"], j["load"], j["seed"]
        frames.append(df)
    return pd.concat(frames, ignore_index=True)


def metrics(actual, predicted):
    err = predicted - actual
    ss_res = float(np.sum(err ** 2))
    ss_tot = float(np.sum((actual - actual.mean()) ** 2))
    return {"mae_ms": float(np.mean(np.abs(err))),
            "median_abs_error_ms": float(np.median(np.abs(err))),
            "r2": 1.0 - ss_res / ss_tot}


def export(scaler, mlp, path):
    lines = ["iov-mlp-v1", f"inputs {len(FEATURES)}",
             "mean " + " ".join(f"{v:.10g}" for v in scaler.mean_),
             "scale " + " ".join(f"{v:.10g}" for v in scaler.scale_),
             "target log1p_ms", f"layers {len(mlp.coefs_)}"]
    for i, (W, b) in enumerate(zip(mlp.coefs_, mlp.intercepts_)):
        act = "relu" if i < len(mlp.coefs_) - 1 else "identity"
        n_in, n_out = W.shape
        lines.append(f"layer {n_in} {n_out} {act}")
        for o in range(n_out):  # row-major [out][in]
            lines.append(" ".join(f"{v:.10g}" for v in W[:, o]))
        lines.append(" ".join(f"{v:.10g}" for v in b))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--reuse-data", action="store_true", help="skip collection if data exists")
    a = ap.parse_args()

    from sklearn.neural_network import MLPRegressor
    from sklearn.preprocessing import StandardScaler

    cache = os.path.join(DATA_DIR, "all_decisions.csv")
    if a.reuse_data and os.path.exists(cache):
        data = pd.read_csv(cache)
    else:
        data = collect(a.jobs)
        data.to_csv(cache, index=False)

    train = data[data.seed.isin(TRAIN_SEEDS)]
    test = data[data.seed.isin(TEST_SEEDS)]
    print(f"Training rows: {len(train)}, test rows: {len(test)}", flush=True)

    scaler = StandardScaler().fit(train[FEATURES].values)
    mlp = MLPRegressor(hidden_layer_sizes=(32, 32), activation="relu", alpha=1e-4,
                       learning_rate_init=1e-3, max_iter=200, early_stopping=True,
                       n_iter_no_change=10, random_state=0)
    mlp.fit(scaler.transform(train[FEATURES].values), np.log1p(train.actual_ms.values))

    pred = np.expm1(mlp.predict(scaler.transform(test[FEATURES].values)))
    report = {
        "features": FEATURES,
        "model": "MLPRegressor(hidden_layer_sizes=(32, 32), relu), target log1p(ms)",
        "train_rows": int(len(train)), "test_rows": int(len(test)),
        "train_seeds": TRAIN_SEEDS, "test_seeds": TEST_SEEDS, "loads": LOADS,
        "epochs": int(mlp.n_iter_),
        "test_ml": metrics(test.actual_ms.values, pred),
        "test_eft_formula": metrics(test.actual_ms.values, test.eft_estimate_ms.values),
        "test_per_workload": {},
    }
    for wl, g in test.assign(pred=pred).groupby("workload"):
        report["test_per_workload"][wl] = {
            "ml": metrics(g.actual_ms.values, g.pred.values),
            "eft_formula": metrics(g.actual_ms.values, g.eft_estimate_ms.values),
        }
    export(scaler, mlp, MODEL_PATH)
    with open(REPORT_PATH, "w") as f:
        json.dump(report, f, indent=2)
    test.assign(ml_pred_ms=pred).sample(min(20000, len(test)), random_state=0).to_csv(
        os.path.join(DATA_DIR, "test_predictions.csv"), index=False)

    m, e = report["test_ml"], report["test_eft_formula"]
    print(f"Held-out test: ML  MAE {m['mae_ms']:.1f} ms, R2 {m['r2']:.3f}")
    print(f"               EFT formula MAE {e['mae_ms']:.1f} ms, R2 {e['r2']:.3f}")
    print(f"Wrote {os.path.relpath(MODEL_PATH, REPO_ROOT)} and {os.path.relpath(REPORT_PATH, REPO_ROOT)}")


if __name__ == "__main__":
    main()
