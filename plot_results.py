#!/usr/bin/env python3
"""Make the result figures from a sweep (python run_experiment.py --sweep).

Reads:  results/summary.csv               one row per run (always needed)
        results/sweep/*_dags.csv          per-DAG data (used if present)
        results/sweep/*_ticks.csv         per-tick data (used if present)
        ml/training_report.json, ml/data/test_predictions.csv   (ML accuracy, if present)
Writes: results/aggregate.csv             mean and 95% CI over seeds per configuration
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
import pandas as pd

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))

# Validated colour-blind-safe categorical colours, fixed order.
BLUE, ORANGE, AQUA, YELLOW, VIOLET = "#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#6250d6"
INK, INK2, GRID, BG = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"

POLICY_STYLE = {  # colour + marker, so identity never relies on colour alone
    "eft": (BLUE, "o", "EFT"),
    "least_loaded": (ORANGE, "s", "Least-loaded"),
    "nearest": (AQUA, "^", "Nearest"),
    "ml": (VIOLET, "D", "ML (neural net)"),
}
SCHED_STYLE = {"heft": (BLUE, "-", "HEFT-lite"), "fifo": (ORANGE, "--", "FIFO")}
SERVER_COLORS = {"S0": BLUE, "S1": ORANGE, "S2": AQUA, "S3": VIOLET}
SERVER_LABELS = {"S0": "S0 (2.0)", "S1": "S1 (3.0)", "S2": "S2 (3.0)", "S3": "S3 (6.0)"}
WORKLOAD_TITLE = {"synthetic": "Synthetic DAGs (3–6 tasks)", "wfcommons": "Real workflow DAGs (22–104 tasks)"}

plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": BG, "savefig.facecolor": BG,
    "axes.edgecolor": INK2, "axes.labelcolor": INK, "text.color": INK,
    "xtick.color": INK2, "ytick.color": INK2, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 10, "axes.titlesize": 10.5, "axes.titleweight": "bold", "legend.frameon": False,
    "lines.linewidth": 2, "lines.markersize": 6,
})

# Two-sided 95% Student-t critical values, df = 1..30.
T95 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179,
       2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064,
       2.060, 2.056, 2.052, 2.048, 2.045, 2.042]

KEYS = ["workload", "server_slots", "scheduler", "offloading", "load"]
METRICS = ["mean_makespan_s", "median_makespan_s", "p95_makespan_s", "mean_slowdown",
           "mean_wait_ms", "mean_upload_ms", "mean_queue_ms", "mean_exec_ms",
           "S0_util", "S1_util", "S2_util", "S3_util",
           "S0_task_share", "S1_task_share", "S2_task_share", "S3_task_share"]


def ci95(s):
    n = s.count()
    return 0.0 if n < 2 else T95[min(n - 1, 30) - 1] * s.std(ddof=1) / math.sqrt(n)


def aggregate(summary):
    rows = []
    for key, g in summary.groupby(KEYS):
        row = dict(zip(KEYS, key), runs=len(g), dags_completed=int(g.dags_completed.sum()),
                   dags_unfinished=int(g.dags_unfinished.sum()))
        for m in METRICS:
            row[m] = g[m].mean()
            row[m + "_ci95"] = ci95(g[m])
        rows.append(row)
    return pd.DataFrame(rows)


def paired_vs_eft(summary):
    """Extra mean makespan of each policy relative to EFT, paired by seed
    (same traffic and same DAGs in both runs), with a 95% CI."""
    base = summary[(summary.server_slots == 0) & (summary.scheduler == "heft")]
    rows = []
    for (wl, load), g in base.groupby(["workload", "load"]):
        eft = g[g.offloading == "eft"].set_index("seed").mean_makespan_s
        for pol in [p for p in POLICY_STYLE if p != "eft"]:
            other = g[g.offloading == pol].set_index("seed").mean_makespan_s
            pct = ((other / eft) - 1.0).dropna() * 100.0
            rows.append({"workload": wl, "load": load, "policy": pol, "seeds": int(pct.count()),
                         "extra_vs_eft_pct": pct.mean(), "ci95_pct": ci95(pct),
                         "seeds_where_eft_better": int((pct > 0).sum())})
    return pd.DataFrame(rows)


def pick(agg, **kw):
    d = agg
    for k, v in kw.items():
        d = d[d[k] == v]
    return d.sort_values("load")


def one(agg, **kw):
    d = pick(agg, **kw)
    if len(d) != 1:
        raise ValueError(f"expected one row for {kw}, got {len(d)}")
    return d.iloc[0]


def pct_axis(ax):
    ax.set_xticks([0.2, 0.4, 0.6, 0.8])
    ax.set_xticklabels(["20%", "40%", "60%", "80%"])
    ax.set_xlabel("offered load (arriving work ÷ total server capacity)")


# ------------------------------------------------------------------ figures

def fig_policies(agg, out):
    """Makespan vs load for the four offloading policies, both workloads."""
    workloads = [w for w in WORKLOAD_TITLE if w in set(agg.workload)]
    fig, axes = plt.subplots(len(workloads), 2, figsize=(11, 4.0 * len(workloads)), squeeze=False)
    for r, wl in enumerate(workloads):
        for c, (metric, name) in enumerate([("mean_makespan_s", "mean"), ("p95_makespan_s", "P95")]):
            ax = axes[r][c]
            for pol, (color, marker, label) in POLICY_STYLE.items():
                d = pick(agg, workload=wl, server_slots=0, scheduler="heft", offloading=pol)
                if d.empty:
                    continue
                ax.errorbar(d.load, d[metric], yerr=d[metric + "_ci95"], color=color, marker=marker,
                            capsize=3, label=label)
            ax.set_yscale("log")
            ax.set_title(f"{WORKLOAD_TITLE[wl]}: {name} makespan", loc="left")
            ax.set_ylabel("seconds (log scale)")
            pct_axis(ax)
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, ncol=4, loc="upper center", bbox_to_anchor=(0.5, 0.965), fontsize=9.5)
    fig.suptitle("Offloading policies (HEFT-lite order, unlimited admission, mean ± 95% CI over 10 seeds)",
                 fontsize=10, color=INK2)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(os.path.join(out, "policies_vs_load.png"), dpi=150)
    plt.close(fig)


def fig_paired(paired, out):
    """Extra makespan vs EFT, paired by seed (Nearest left out: 20-1000%)."""
    workloads = [w for w in WORKLOAD_TITLE if w in set(paired.workload)]
    fig, axes = plt.subplots(1, len(workloads), figsize=(5.6 * len(workloads), 3.8), squeeze=False,
                             sharey=True)
    pols = ["ml", "least_loaded"]
    w = 0.36
    for ax, wl in zip(axes[0], workloads):
        d = paired[paired.workload == wl]
        loads = sorted(d.load.unique())
        for k, pol in enumerate(pols):
            color, _, name = POLICY_STYLE[pol]
            g = d[d.policy == pol].set_index("load").reindex(loads)
            xs = [i + (k - 0.5) * (w + 0.03) for i in range(len(loads))]
            ax.bar(xs, g.extra_vs_eft_pct, width=w, yerr=g.ci95_pct, capsize=3, color=color, label=name,
                   error_kw={"ecolor": INK2, "elinewidth": 1.2})
            for x, v, e in zip(xs, g.extra_vs_eft_pct, g.ci95_pct):
                ax.text(x, v + e + 0.6, f"+{v:.1f}%", ha="center", fontsize=8, color=INK)
        ax.axhline(0, color=INK2, linewidth=1)
        ax.set_xticks(range(len(loads)))
        ax.set_xticklabels([f"{l:.0%}" for l in loads])
        ax.set_xlabel("offered load")
        ax.grid(axis="x", visible=False)
        ax.set_title(WORKLOAD_TITLE[wl], loc="left")
    axes[0][0].set_ylabel("extra mean makespan vs EFT (%)")
    axes[0][0].legend(loc="upper right")
    fig.suptitle("How much slower than EFT? Paired by seed (same traffic, same DAGs); 0% = as good as EFT",
                 fontsize=10, color=INK2)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "benchmark_vs_eft.png"), dpi=150)
    plt.close(fig)


def fig_breakdown(agg, out, load):
    parts = [("mean_wait_ms", "wait for decision tick", BLUE), ("mean_upload_ms", "upload", ORANGE),
             ("mean_queue_ms", "queue at server", AQUA), ("mean_exec_ms", "execute", YELLOW)]
    workloads = [w for w in WORKLOAD_TITLE if w in set(agg.workload)]
    fig, axes = plt.subplots(1, len(workloads), figsize=(6 * len(workloads), 3.4), squeeze=False)
    for ax, wl in zip(axes[0], workloads):
        names = []
        for i, pol in enumerate(POLICY_STYLE):
            d = one(agg, workload=wl, server_slots=0, scheduler="heft", offloading=pol, load=load)
            left = 0.0
            for col, lab, color in parts:
                v = d[col]
                ax.barh(i, v, left=left, color=color, edgecolor=BG, linewidth=2, height=0.62,
                        label=lab if i == 0 else None)
                left += v
            ax.text(left * 1.01 + 5, i, f"{left:.0f} ms", va="center", fontsize=8.5, color=INK)
            names.append(POLICY_STYLE[pol][2])
        ax.set_yticks(range(len(names)))
        ax.set_yticklabels(names)
        ax.invert_yaxis()
        ax.set_xlabel("mean time per task (ms)")
        ax.grid(axis="y", visible=False)
        ax.set_title(WORKLOAD_TITLE[wl], loc="left")
        ax.set_xlim(right=ax.get_xlim()[1] * 1.15)
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, ncol=4, loc="upper center", bbox_to_anchor=(0.5, 0.93), fontsize=9)
    fig.suptitle(f"Where each task's time goes at {load:.0%} load (mean per task)", fontsize=10,
                 color=INK2, y=0.99)
    fig.tight_layout(rect=(0, 0, 1, 0.84))
    fig.savefig(os.path.join(out, "time_breakdown.png"), dpi=150)
    plt.close(fig)


def fig_servers(agg, out, load, workload="synthetic"):
    fig, ax = plt.subplots(figsize=(9.5, 3.7))
    width = 0.19
    for i, pol in enumerate(POLICY_STYLE):
        d = one(agg, workload=workload, server_slots=0, scheduler="heft", offloading=pol, load=load)
        for j, s in enumerate(SERVER_COLORS):
            x = i + (j - 1.5) * (width + 0.02)
            v = d[f"{s}_util"] * 100
            ax.bar(x, v, width=width, color=SERVER_COLORS[s], label=SERVER_LABELS[s] if i == 0 else None)
            ax.text(x, v + 1.5, f"{v:.0f}", ha="center", fontsize=8, color=INK)
    ax.set_xticks(range(len(POLICY_STYLE)))
    ax.set_xticklabels([v[2] for v in POLICY_STYLE.values()])
    ax.set_ylabel("CPU busy (% of time)")
    ax.set_ylim(0, 105)
    ax.grid(axis="x", visible=False)
    ax.set_title(f"Server utilisation, synthetic DAGs at {load:.0%} load (capacity in Mcycles/ms)", loc="left")
    ax.legend(ncol=4, loc="upper right", fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "server_utilisation.png"), dpi=150)
    plt.close(fig)


def fig_scheduler(agg, dags, out, slots):
    workloads = [w for w in WORKLOAD_TITLE if w in set(agg.workload)]
    has_sizes = dags is not None and not dags.empty
    n = len(workloads) + (1 if has_sizes else 0)
    fig, axes = plt.subplots(1, n, figsize=(5.2 * n, 4.0), squeeze=False)
    for ax, wl in zip(axes[0], workloads):
        for sch, (color, ls, name) in SCHED_STYLE.items():
            d = pick(agg, workload=wl, server_slots=slots, scheduler=sch, offloading="eft")
            ax.errorbar(d.load, d.mean_makespan_s, yerr=d.mean_makespan_s_ci95, color=color,
                        linestyle=ls, marker="o", capsize=3, label=name)
        ax.set_ylabel("mean DAG makespan (s)")
        ax.set_title(WORKLOAD_TITLE[wl].split(" (")[0] + f", limit {slots} tasks/server", loc="left")
        pct_axis(ax)
        ax.legend(loc="upper left")
    if has_sizes:
        ax = axes[0][-1]
        d = dags[(dags.workload == "synthetic") & (dags.slots == slots) & (dags.offloading == "eft")
                 & (dags.load == dags.load.max())]
        g = d.groupby(["scheduler", "n_tasks"]).makespan_s.mean().unstack(0)
        w = 0.38
        for k, (sch, (color, _, name)) in enumerate(SCHED_STYLE.items()):
            xs = g.index.values + (k - 0.5) * (w + 0.02)
            ax.bar(xs, g[sch], width=w, color=color, label=name)
            for xi, v in zip(xs, g[sch]):
                ax.text(xi, v + 0.02, f"{v:.2f}", ha="center", fontsize=8, color=INK)
        ax.set_xticks(g.index.values)
        ax.set_xlabel("tasks in the DAG")
        ax.set_ylabel("mean makespan (s)")
        ax.grid(axis="x", visible=False)
        ax.set_title(f"Synthetic, by DAG size at {dags.load.max():.0%} load", loc="left")
        ax.legend(loc="upper left")
    fig.suptitle("HEFT-lite vs FIFO with EFT offloading when servers accept at most "
                 f"{slots} tasks (mean ± 95% CI over 10 seeds)", fontsize=10, color=INK2)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "scheduler_comparison.png"), dpi=150)
    plt.close(fig)


def fig_ml_accuracy(out):
    pred_path = os.path.join(REPO_ROOT, "ml", "data", "test_predictions.csv")
    rep_path = os.path.join(REPO_ROOT, "ml", "training_report.json")
    if not (os.path.exists(pred_path) and os.path.exists(rep_path)):
        return False
    d = pd.read_csv(pred_path)
    rep = json.load(open(rep_path))
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.4), sharex=True, sharey=True)
    for ax, col, color, name, key in [(axes[0], "ml_pred_ms", VIOLET, "ML (neural net)", "test_ml"),
                                      (axes[1], "eft_estimate_ms", BLUE, "EFT formula", "test_eft_formula")]:
        ax.scatter(d.actual_ms, d[col].clip(lower=1), s=4, alpha=0.25, color=color, linewidths=0)
        lo, hi = 10, max(d.actual_ms.max(), d[col].max()) * 1.2
        ax.plot([lo, hi], [lo, hi], color=INK2, linewidth=1, linestyle=":")
        ax.set_xscale("log")
        ax.set_yscale("log")
        m = rep[key]
        ax.set_title(f"{name}: median error {m['median_abs_error_ms']:.0f} ms, R² {m['r2']:.3f}", loc="left")
        ax.set_xlabel("actual time, dispatch → finish (ms)")
    axes[0].set_ylabel("predicted time (ms)")
    fig.suptitle(f"Predicting task completion time on held-out runs ({rep['test_rows']:,} tasks, "
                 "random server choice; dotted line = perfect)", fontsize=10, color=INK2)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "ml_prediction_accuracy.png"), dpi=150)
    plt.close(fig)
    return True


def fig_timeseries(ticks_path, out):
    df = pd.read_csv(ticks_path)
    fig, axes = plt.subplots(2, 1, figsize=(9, 5.6), sharex=True)
    axes[0].plot(df.time_s, df.num_vehicles, color=BLUE, linewidth=1.5)
    axes[0].set_ylabel("vehicles in SUMO")
    axes[0].set_title("One run (synthetic DAGs, EFT, 60% load): SUMO traffic and server backlog", loc="left")
    axes[0].axvspan(0, 60, color=GRID, alpha=0.6, linewidth=0)
    axes[0].text(30, axes[0].get_ylim()[1] * 0.9, "warm-up\n(no tasks)", ha="center", va="top",
                 fontsize=8, color=INK2)
    roll = max(1, int(round(10.0 / (df.time_s.iloc[1] - df.time_s.iloc[0]))))
    for s, color in SERVER_COLORS.items():
        col = f"{s}_backlog_ms"
        if col in df:
            axes[1].plot(df.time_s, df[col].rolling(roll, min_periods=1).mean(), color=color,
                         linewidth=2, label=SERVER_LABELS[s])
    axes[1].set_ylabel("backlog (ms of work)\n10-s moving average")
    axes[1].set_xlabel("simulation time (s)")
    axes[1].legend(ncol=4, loc="upper right", fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "run_timeseries.png"), dpi=150)
    plt.close(fig)


def load_dags(sweep_dir):
    frames = []
    for p in glob.glob(os.path.join(sweep_dir, "synthetic_*_k5_*_dags.csv")):
        base = os.path.basename(p)[: -len("_dags.csv")]  # synthetic_heft_eft_k5_u0.8_s3
        wl, sch, rest = base.split("_", 2)
        pol, k, u, _seed = rest.rsplit("_", 3)
        df = pd.read_csv(p, usecols=["n_tasks", "makespan_s"])
        df["workload"], df["scheduler"], df["offloading"] = wl, sch, pol
        df["slots"], df["load"] = int(k[1:]), float(u[1:])
        frames.append(df)
    return pd.concat(frames, ignore_index=True) if frames else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--summary", default=os.path.join(REPO_ROOT, "results", "summary.csv"))
    ap.add_argument("--sweep-dir", default=os.path.join(REPO_ROOT, "results", "sweep"))
    ap.add_argument("--out-dir", default=os.path.join(REPO_ROOT, "results", "plots"))
    ap.add_argument("--focus-load", type=float, default=0.6, help="load used for the bar charts")
    ap.add_argument("--admission-slots", type=int, default=5)
    a = ap.parse_args()

    if not os.path.exists(a.summary):
        raise SystemExit(f"{a.summary} not found -- run `python run_experiment.py --sweep` first.")
    os.makedirs(a.out_dir, exist_ok=True)
    summary = pd.read_csv(a.summary)
    agg = aggregate(summary)
    agg.round(4).to_csv(os.path.join(REPO_ROOT, "results", "aggregate.csv"), index=False)
    paired = paired_vs_eft(summary)
    paired.round(2).to_csv(os.path.join(REPO_ROOT, "results", "paired_vs_eft.csv"), index=False)
    fig_paired(paired, a.out_dir)

    fig_policies(agg, a.out_dir)
    fig_breakdown(agg, a.out_dir, a.focus_load)
    fig_servers(agg, a.out_dir, a.focus_load)
    if a.admission_slots in set(agg.server_slots):
        dags = load_dags(a.sweep_dir) if os.path.isdir(a.sweep_dir) else None
        fig_scheduler(agg, dags, a.out_dir, a.admission_slots)
    fig_ml_accuracy(a.out_dir)
    ticks = os.path.join(a.sweep_dir, f"synthetic_heft_eft_k0_u{a.focus_load:g}_s1_ticks.csv")
    if os.path.exists(ticks):
        fig_timeseries(ticks, a.out_dir)
    print("Wrote results/aggregate.csv, results/paired_vs_eft.csv and figures in "
          f"{os.path.relpath(a.out_dir, REPO_ROOT)}/")


if __name__ == "__main__":
    main()
