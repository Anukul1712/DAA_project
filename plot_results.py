#!/usr/bin/env python3
"""Make the result figures and tables from a sweep (python run_experiment.py --sweep).

Reads:  results/summary.csv            one row per run (always needed)
        results/sweep/*_dags.csv       per-DAG data (used if present)
        results/sweep/*_ticks.csv      per-tick data for seed 1 (used if present)
        sumo_scenario/city/servers.csv server capacities (for utilisation by class)
        ml/training_report.json, ml/data/test_predictions.csv   (ML accuracy, if present)
Writes: results/aggregate.csv          mean and 95% CI over seeds per configuration
        results/paired_vs_qajs.csv     every configuration vs QAJS, paired by seed
        results/plots/*.png
"""
import argparse
import glob
import json
import math
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))

# Validated colour-blind-safe categorical palette, fixed order.
BLUE, ORANGE, AQUA, YELLOW, MAGENTA, GREEN, VIOLET, RED = (
    "#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948")
INK, INK2, GRID, BG = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"

QAJS = "srpt_eft_priority"
EFT = "fifo_eft_fifo"
# label -> (display name, colour, marker, line style)
CONFIGS = {
    "fifo_nearest_fifo": ("Nearest", AQUA, "^", "-"),
    "fifo_least_loaded_fifo": ("Least-loaded", ORANGE, "s", "-"),
    "fifo_ml_fifo": ("ML (neural net)", VIOLET, "D", "-"),
    "fifo_eft_fifo": ("EFT", BLUE, "o", "-"),
    "heft_eft_fifo": ("HEFT-lite + EFT", YELLOW, "v", "--"),
    "heft_eft_priority": ("HEFT-lite + EFT + prio. queues", MAGENTA, "P", "--"),
    "srpt_eft_fifo": ("DAG-SRPT + EFT", GREEN, "X", "--"),
    "srpt_eft_priority_ahead": ("QAJS, optimistic backlog", INK2, "h", ":"),
    "srpt_eft_priority": ("QAJS (proposed)", RED, "*", "-"),
    "srpt_ml_priority": ("QAJS with ML", VIOLET, "d", ":"),
}
OFFLOADING_SET = ["fifo_nearest_fifo", "fifo_least_loaded_fifo", "fifo_ml_fifo", EFT, QAJS]
ABLATION_SET = ["heft_eft_fifo", "heft_eft_priority", "srpt_eft_fifo", "srpt_eft_priority_ahead", QAJS,
                "srpt_ml_priority"]
WORKLOAD_TITLE = {"synthetic": "Synthetic DAGs (3–6 tasks)", "wfcommons": "WfCommons DAGs (22–104 tasks)"}
CLASS_COLOR = {2.0: BLUE, 3.0: ORANGE, 4.0: AQUA, 6.0: VIOLET}

plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": BG, "savefig.facecolor": BG,
    "axes.edgecolor": INK2, "axes.labelcolor": INK, "text.color": INK,
    "xtick.color": INK2, "ytick.color": INK2, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 10, "axes.titlesize": 10.5, "axes.titleweight": "bold", "legend.frameon": False,
    "lines.linewidth": 2, "lines.markersize": 6,
})

T95 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179,
       2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064,
       2.060, 2.056, 2.052, 2.048, 2.045, 2.042]
KEYS = ["workload", "label", "load"]
METRICS = ["mean_makespan_s", "median_makespan_s", "p95_makespan_s", "p99_makespan_s", "mean_slowdown",
           "p95_slowdown", "mean_wait_ms", "mean_upload_ms", "mean_queue_ms", "mean_exec_ms", "util_mean",
           "util_max", "util_jain", "tasks_completed", "dags_completed", "mean_vehicles"]


def ci95(s):
    n = s.count()
    return 0.0 if n < 2 else T95[min(n - 1, 30) - 1] * s.std(ddof=1) / math.sqrt(n)


def aggregate(summary):
    rows = []
    for key, g in summary.groupby(KEYS):
        row = dict(zip(KEYS, key), runs=len(g), dags_unfinished=int(g.dags_unfinished.sum()))
        for m in METRICS:
            row[m] = g[m].mean()
            row[m + "_ci95"] = ci95(g[m])
        rows.append(row)
    return pd.DataFrame(rows)


def paired_vs(summary, ref=QAJS, metric="mean_makespan_s"):
    """Percent difference of every configuration vs `ref`, paired by seed
    (same traffic, same DAG arrivals). Positive = slower than ref."""
    rows = []
    for (wl, load), g in summary.groupby(["workload", "load"]):
        base = g[g.label == ref].set_index("seed")[metric]
        for lab in CONFIGS:
            if lab == ref:
                continue
            other = g[g.label == lab].set_index("seed")[metric]
            pct = ((other / base) - 1.0).dropna() * 100.0
            if pct.empty:
                continue
            rows.append({"workload": wl, "load": load, "config": lab, "metric": metric, "seeds": int(pct.count()),
                         "diff_pct": pct.mean(), "ci95_pct": ci95(pct), "seeds_ref_better": int((pct > 0).sum())})
    return pd.DataFrame(rows)


def pick(agg, **kw):
    d = agg
    for k, v in kw.items():
        d = d[d[k] == v]
    return d.sort_values("load")


def load_axis(ax, loads):
    ax.set_xticks(loads)
    ax.set_xticklabels([f"{l:.0%}" for l in loads])
    ax.set_xlabel("offered load ρ")


def workloads_of(agg):
    return [w for w in WORKLOAD_TITLE if w in set(agg.workload)]


def save(fig, out, name):
    fig.savefig(os.path.join(out, name), dpi=150)
    plt.close(fig)


# ------------------------------------------------------------------ figures

def fig_offloading(agg, out):
    wls = workloads_of(agg)
    fig, axes = plt.subplots(len(wls), 2, figsize=(11, 3.9 * len(wls)), squeeze=False)
    for r, wl in enumerate(wls):
        for c, (metric, name) in enumerate([("mean_makespan_s", "mean"), ("p95_makespan_s", "P95")]):
            ax = axes[r][c]
            for lab in OFFLOADING_SET:
                disp, color, marker, ls = CONFIGS[lab]
                d = pick(agg, workload=wl, label=lab)
                ax.errorbar(d.load, d[metric], yerr=d[metric + "_ci95"], color=color, marker=marker,
                            linestyle=ls, capsize=3, label=disp, markersize=8 if marker == "*" else 6)
            ax.set_yscale("log")
            ax.set_title(f"{WORKLOAD_TITLE[wl]}: {name} makespan", loc="left")
            ax.set_ylabel("seconds (log scale)")
            load_axis(ax, sorted(agg.load.unique()))
    h, l = axes[0][0].get_legend_handles_labels()
    fig.legend(h, l, ncol=5, loc="upper center", bbox_to_anchor=(0.5, 0.965), fontsize=9.5)
    fig.suptitle("Server-selection baselines vs QAJS (16 servers, mean ± 95% CI over 10 seeds)",
                 fontsize=10, color=INK2)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out, "offloading_vs_load.png")


def fig_ablation(paired_eft, out):
    """Change in mean and P95 makespan vs the EFT baseline, paired by seed."""
    wls = [w for w in WORKLOAD_TITLE if w in set(paired_eft.workload)]
    fig, axes = plt.subplots(len(wls), 2, figsize=(11, 3.9 * len(wls)), squeeze=False)
    for r, wl in enumerate(wls):
        for c, metric in enumerate(["mean_makespan_s", "p95_makespan_s"]):
            ax = axes[r][c]
            d = paired_eft[(paired_eft.workload == wl) & (paired_eft.metric == metric)]
            for lab in ABLATION_SET:
                g = d[d.config == lab].sort_values("load")
                if g.empty:
                    continue
                disp, color, marker, ls = CONFIGS[lab]
                ax.errorbar(g.load, g.diff_pct, yerr=g.ci95_pct, color=color, marker=marker, linestyle=ls,
                            capsize=3, label=disp, markersize=8 if marker == "*" else 6)
            ax.axhline(0, color=INK, linewidth=1)
            ax.set_yscale("symlog", linthresh=10)
            ax.set_title(f"{WORKLOAD_TITLE[wl]}: {'mean' if c == 0 else 'P95'} makespan", loc="left")
            ax.set_ylabel("change vs EFT baseline (%)")
            load_axis(ax, sorted(paired_eft.load.unique()))
    h, l = axes[0][0].get_legend_handles_labels()
    fig.legend(h, l, ncol=3, loc="upper center", bbox_to_anchor=(0.5, 0.975), fontsize=9)
    fig.suptitle("Scheduling and resource-allocation ablation (all use EFT-type server choice). "
                 "Below 0 = better than EFT with FIFO order and FIFO queues", fontsize=10, color=INK2)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    save(fig, out, "ablation_vs_eft.png")


def fig_ranking(agg, out, load):
    """All configurations at one load, sorted."""
    wls = workloads_of(agg)
    fig, axes = plt.subplots(1, len(wls), figsize=(6.2 * len(wls), 4.6), squeeze=False)
    for ax, wl in zip(axes[0], wls):
        d = pick(agg, workload=wl, load=load).set_index("label").reindex(list(CONFIGS)).dropna(subset=["mean_makespan_s"])
        d = d.sort_values("mean_makespan_s", ascending=False)
        ys = np.arange(len(d))
        colors = [CONFIGS[l][1] for l in d.index]
        ax.barh(ys, d.mean_makespan_s, xerr=d.mean_makespan_s_ci95, color=colors, height=0.65,
                error_kw={"ecolor": INK2, "elinewidth": 1})
        ax.set_yticks(ys)
        ax.set_yticklabels([CONFIGS[l][0] for l in d.index])
        ax.set_xscale("log")
        for y, v in zip(ys, d.mean_makespan_s):
            ax.text(v * 1.07, y, f"{v:.2f} s", va="center", fontsize=8, color=INK)
        ax.set_xlim(right=d.mean_makespan_s.max() * 3)
        ax.set_xlabel("mean DAG makespan (s, log scale)")
        ax.grid(axis="y", visible=False)
        ax.set_title(WORKLOAD_TITLE[wl], loc="left")
    fig.suptitle(f"All ten configurations at {load:.0%} load (mean ± 95% CI, 10 seeds)", fontsize=10, color=INK2)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    save(fig, out, "ranking.png")


def fig_breakdown(agg, out, load):
    parts = [("mean_wait_ms", "wait for decision tick", BLUE), ("mean_upload_ms", "upload", ORANGE),
             ("mean_queue_ms", "queue at server", AQUA), ("mean_exec_ms", "execute", YELLOW)]
    labs = OFFLOADING_SET
    wls = workloads_of(agg)
    fig, axes = plt.subplots(1, len(wls), figsize=(6.2 * len(wls), 3.6), squeeze=False)
    for ax, wl in zip(axes[0], wls):
        for i, lab in enumerate(labs):
            d = pick(agg, workload=wl, label=lab, load=load).iloc[0]
            left = 0.0
            for col, name, color in parts:
                ax.barh(i, d[col], left=left, color=color, edgecolor=BG, linewidth=2, height=0.62,
                        label=name if i == 0 else None)
                left += d[col]
            ax.text(left * 1.01 + 5, i, f"{left:.0f} ms", va="center", fontsize=8.5, color=INK)
        ax.set_yticks(range(len(labs)))
        ax.set_yticklabels([CONFIGS[l][0] for l in labs])
        ax.invert_yaxis()
        ax.set_xscale("symlog", linthresh=500)
        ax.set_xlabel("mean time per task (ms; linear up to 500, log above)")
        ax.grid(axis="y", visible=False)
        ax.set_title(WORKLOAD_TITLE[wl], loc="left")
    h, l = axes[0][0].get_legend_handles_labels()
    fig.legend(h, l, ncol=4, loc="upper center", bbox_to_anchor=(0.5, 0.93), fontsize=9)
    fig.suptitle(f"Where each task's time goes at {load:.0%} load", fontsize=10, color=INK2, y=0.99)
    fig.tight_layout(rect=(0, 0, 1, 0.84))
    save(fig, out, "time_breakdown.png")


def fig_utilisation(summary, out, load, servers_csv):
    if not os.path.exists(servers_csv):
        return
    caps = pd.read_csv(servers_csv).set_index("name").cpu_capacity
    classes = sorted(caps.unique())
    labs = OFFLOADING_SET
    d = summary[(summary.workload == "synthetic") & (summary.load == load)]
    fig, ax = plt.subplots(figsize=(10, 3.8))
    w = 0.19
    for j, cap in enumerate(classes):
        cols = [f"{n}_util" for n, c in caps.items() if c == cap]
        for i, lab in enumerate(labs):
            v = d[d.label == lab][cols].mean(axis=1).mean() * 100
            x = i + (j - (len(classes) - 1) / 2) * (w + 0.02)
            ax.bar(x, v, width=w, color=CLASS_COLOR.get(cap, INK2), label=f"speed {cap:g}" if i == 0 else None)
            ax.text(x, v + 1.5, f"{v:.0f}", ha="center", fontsize=7.5, color=INK)
    ax.set_xticks(range(len(labs)))
    ax.set_xticklabels([CONFIGS[l][0] for l in labs])
    ax.set_ylabel("mean CPU busy (% of time)")
    ax.set_ylim(0, 105)
    ax.grid(axis="x", visible=False)
    ax.set_title(f"Server utilisation by server speed class (Mcycles/ms), synthetic DAGs at {load:.0%} load",
                 loc="left", pad=28)
    ax.legend(ncol=4, loc="lower left", bbox_to_anchor=(0.0, 1.0), fontsize=9)
    ax.set_ylim(0, 108)
    fig.tight_layout()
    save(fig, out, "server_utilisation.png")


def fig_fairness(dags, out, load):
    """Mean makespan per DAG size, EFT vs QAJS (shows who gains and who pays)."""
    if dags is None or dags.empty:
        return
    wls = [w for w in WORKLOAD_TITLE if w in set(dags.workload)]
    fig, axes = plt.subplots(1, len(wls), figsize=(6.2 * len(wls), 3.9), squeeze=False)
    for ax, wl in zip(axes[0], wls):
        d = dags[(dags.workload == wl) & (dags.load == load)]
        g = d.groupby(["label", "n_tasks"]).makespan_s.mean().unstack(0)
        x = np.arange(len(g.index))
        w = 0.38
        for k, lab in enumerate([EFT, QAJS]):
            if lab not in g:
                continue
            ax.bar(x + (k - 0.5) * (w + 0.02), g[lab], width=w, color=CONFIGS[lab][1], label=CONFIGS[lab][0])
        ax.set_xticks(x)
        ax.set_xticklabels([str(n) for n in g.index], rotation=0 if len(x) < 8 else 45)
        ax.set_xlabel("tasks in the DAG" + (" (one bar pair per WfCommons workflow)" if wl == "wfcommons" else ""))
        ax.set_ylabel("mean makespan (s)")
        ax.grid(axis="x", visible=False)
        ax.set_title(WORKLOAD_TITLE[wl], loc="left")
        ax.legend(loc="upper left")
    fig.suptitle(f"Who gains? Mean makespan by DAG size at {load:.0%} load (all 10 seeds)", fontsize=10, color=INK2)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    save(fig, out, "fairness_by_dag_size.png")


def fig_ml_accuracy(out):
    pred_path = os.path.join(REPO_ROOT, "ml", "data", "test_predictions.csv")
    rep_path = os.path.join(REPO_ROOT, "ml", "training_report.json")
    if not (os.path.exists(pred_path) and os.path.exists(rep_path)):
        return
    d = pd.read_csv(pred_path)
    rep = json.load(open(rep_path))
    queues = [q for q in ["fifo", "priority"] if "queue" in d and q in set(d["queue"])] or [None]
    fig, axes = plt.subplots(len(queues), 2, figsize=(10, 4.0 * len(queues)), sharex=True, sharey=True,
                             squeeze=False)
    rows = []
    for r, q in enumerate(queues):
        g = d if q is None else d[d["queue"] == q]
        for c, (col, color, name) in enumerate([("ml_pred_ms", VIOLET, "ML (neural net)"),
                                                ("eft_estimate_ms", BLUE, "EFT formula")]):
            ax = axes[r][c]
            err = (g[col] - g.actual_ms).abs()
            r2 = 1 - ((g[col] - g.actual_ms) ** 2).sum() / ((g.actual_ms - g.actual_ms.mean()) ** 2).sum()
            rows.append({"queue": q, "predictor": name, "tasks": len(g), "median_abs_error_ms": err.median(),
                         "mean_abs_error_ms": err.mean(), "r2": r2})
            ax.scatter(g.actual_ms, g[col].clip(lower=1), s=4, alpha=0.25, color=color, linewidths=0)
            lo, hi = 5, max(d.actual_ms.max(), d[col].max()) * 1.2
            ax.plot([lo, hi], [lo, hi], color=INK2, linewidth=1, linestyle=":")
            ax.set_xscale("log")
            ax.set_yscale("log")
            qname = "" if q is None else f"{q.upper()} queues — "
            ax.set_title(f"{qname}{name}: median err. {err.median():.0f} ms, R² {r2:.2f}", loc="left", fontsize=9.5)
            if r == len(queues) - 1:
                ax.set_xlabel("actual time, dispatch → finish (ms)")
        axes[r][0].set_ylabel("predicted time (ms)")
    pd.DataFrame(rows).round(3).to_csv(os.path.join(REPO_ROOT, "results", "ml_accuracy.csv"), index=False)
    fig.suptitle(f"Predicting task completion time on held-out runs (sample of {len(d):,} test tasks; "
                 "dotted line = perfect)", fontsize=10, color=INK2)
    fig.tight_layout()
    save(fig, out, "ml_prediction_accuracy.png")


def fig_timeseries(sweep_dir, out, load):
    paths = {lab: os.path.join(sweep_dir, f"synthetic_{lab}_u{load:g}_s1_ticks.csv") for lab in [EFT, QAJS]}
    if not all(os.path.exists(p) for p in paths.values()):
        return
    dfs = {lab: pd.read_csv(p) for lab, p in paths.items()}
    fig, axes = plt.subplots(2, 1, figsize=(9, 5.8), sharex=True)
    d0 = dfs[QAJS]
    axes[0].plot(d0.time_s, d0.num_vehicles, color=BLUE, linewidth=1.5)
    axes[0].set_ylabel("vehicles in SUMO")
    axes[0].axvspan(0, 100, color=GRID, alpha=0.6, linewidth=0)
    axes[0].text(50, axes[0].get_ylim()[1] * 0.85, "warm-up\n(no tasks)", ha="center", va="top", fontsize=8, color=INK2)
    axes[0].set_title(f"One run (synthetic DAGs, {load:.0%} load): SUMO traffic and total server backlog", loc="left")
    roll = 50
    for lab, df in dfs.items():
        axes[1].plot(df.time_s, df.total_backlog_ms.rolling(roll, min_periods=1).mean() / 1000.0,
                     color=CONFIGS[lab][1], label=CONFIGS[lab][0], linewidth=1.8)
    axes[1].set_ylabel("total backlog over 16 servers\n(s of work, 5-s average)")
    axes[1].set_xlabel("simulation time (s)")
    axes[1].legend(loc="upper left")
    fig.tight_layout()
    save(fig, out, "run_timeseries.png")


def load_dags(sweep_dir, labels, load):
    frames = []
    for lab in labels:
        for p in glob.glob(os.path.join(sweep_dir, f"*_{lab}_u{load:g}_s*_dags.csv")):
            wl = os.path.basename(p).split("_", 1)[0]
            df = pd.read_csv(p, usecols=["n_tasks", "makespan_s"])
            df["workload"], df["label"], df["load"] = wl, lab, load
            frames.append(df)
    return pd.concat(frames, ignore_index=True) if frames else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--summary", default=os.path.join(REPO_ROOT, "results", "summary.csv"))
    ap.add_argument("--sweep-dir", default=os.path.join(REPO_ROOT, "results", "sweep"))
    ap.add_argument("--out-dir", default=os.path.join(REPO_ROOT, "results", "plots"))
    ap.add_argument("--servers", default=os.path.join(REPO_ROOT, "sumo_scenario", "city", "servers.csv"))
    ap.add_argument("--focus-load", type=float, default=0.9, help="load used for bar charts")
    a = ap.parse_args()

    if not os.path.exists(a.summary):
        raise SystemExit(f"{a.summary} not found -- run `python run_experiment.py --sweep` first.")
    os.makedirs(a.out_dir, exist_ok=True)
    summary = pd.read_csv(a.summary)
    agg = aggregate(summary)
    agg.round(4).to_csv(os.path.join(REPO_ROOT, "results", "aggregate.csv"), index=False)
    paired_q = pd.concat([paired_vs(summary, QAJS, m) for m in ["mean_makespan_s", "p95_makespan_s"]])
    paired_q.round(2).to_csv(os.path.join(REPO_ROOT, "results", "paired_vs_qajs.csv"), index=False)
    paired_e = pd.concat([paired_vs(summary, EFT, m) for m in ["mean_makespan_s", "p95_makespan_s"]])
    paired_e.round(2).to_csv(os.path.join(REPO_ROOT, "results", "paired_vs_eft.csv"), index=False)

    fig_offloading(agg, a.out_dir)
    fig_ablation(paired_e, a.out_dir)
    fig_ranking(agg, a.out_dir, a.focus_load)
    fig_breakdown(agg, a.out_dir, a.focus_load)
    fig_utilisation(summary, a.out_dir, a.focus_load, a.servers)
    if os.path.isdir(a.sweep_dir):
        fig_fairness(load_dags(a.sweep_dir, [EFT, QAJS], a.focus_load), a.out_dir, a.focus_load)
        fig_timeseries(a.sweep_dir, a.out_dir, a.focus_load)
    fig_ml_accuracy(a.out_dir)
    print("Wrote results/aggregate.csv, paired_vs_qajs.csv, paired_vs_eft.csv and figures in "
          f"{os.path.relpath(a.out_dir, REPO_ROOT)}/")


if __name__ == "__main__":
    main()
