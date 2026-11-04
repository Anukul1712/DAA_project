#!/usr/bin/env python3
"""Draw the SUMO road network, the edge servers and the vehicles.

Needs a run made with --log-positions, e.g.
    python run_experiment.py --log-positions --out-prefix results/viz
    python visualize.py --run results/viz

Writes:
    results/plots/network_snapshot.png   one moment of the run
    results/plots/network_animation.gif  a short animation (needs Pillow)

Vehicles are coloured by the server their DAG's latest task was sent to;
grey vehicles currently have no tasks in the system.
"""
import argparse
import os
import xml.etree.ElementTree as ET

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from matplotlib.animation import FuncAnimation, PillowWriter
from matplotlib.patches import Circle

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
SERVER_COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#6250d6"]
IDLE = "#b5b3ad"
ROAD = "#8f8d87"
BG = "#fcfcfb"


def road_shapes(net_path):
    shapes = []
    for edge in ET.parse(net_path).getroot().iter("edge"):
        if edge.get("function") == "internal":
            continue
        for lane in edge.iter("lane"):
            pts = [tuple(map(float, p.split(","))) for p in lane.get("shape").split()]
            shapes.append(pts)
    return shapes


def draw_base(ax, roads, servers):
    ax.set_facecolor(BG)
    for pts in roads:
        xs, ys = zip(*pts)
        ax.plot(xs, ys, color=ROAD, linewidth=2.2, solid_capstyle="round", zorder=1)
    for _, s in servers.iterrows():
        color = SERVER_COLORS[int(s.server_id) % len(SERVER_COLORS)]
        ax.add_patch(Circle((s.x, s.y), 28, color=color, alpha=0.25, zorder=2, linewidth=0))
        ax.add_patch(Circle((s.x, s.y), 9, color=color, zorder=4))
        ax.annotate(f"{s['name']}  {s.cpu_capacity:g} Mcycles/ms", (s.x, s.y), xytext=(0, -34),
                    textcoords="offset points", ha="center", fontsize=8, color="#0b0b0b", zorder=5,
                    bbox=dict(boxstyle="round,pad=0.2", fc=BG, ec="none", alpha=0.9))
    ax.set_xlim(-40, 640)
    ax.set_ylim(-40, 640)
    ax.set_aspect("equal")
    ax.set_xticks([])
    ax.set_yticks([])
    for sp in ax.spines.values():
        sp.set_visible(False)


def colors_for(frame):
    return [IDLE if sid < 0 else SERVER_COLORS[int(sid) % len(SERVER_COLORS)] for sid in frame.server_id]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", default=os.path.join("results", "viz"), help="out-prefix of the run")
    ap.add_argument("--net", default=os.path.join("sumo_scenario", "grid.net.xml"))
    ap.add_argument("--snapshot-time", type=float, default=200.0)
    ap.add_argument("--gif-start", type=float, default=150.0)
    ap.add_argument("--gif-seconds", type=float, default=30.0)
    ap.add_argument("--out-dir", default=os.path.join("results", "plots"))
    a = ap.parse_args()

    pos_path = os.path.join(REPO_ROOT, a.run + "_positions.csv")
    if not os.path.exists(pos_path):
        raise SystemExit(f"{pos_path} not found. Make a run with --log-positions first:\n"
                         f"    python run_experiment.py --log-positions --out-prefix {a.run}")
    pos = pd.read_csv(pos_path)
    servers = pd.read_csv(os.path.join(REPO_ROOT, a.run + "_servers.csv"))
    roads = road_shapes(os.path.join(REPO_ROOT, a.net))
    out_dir = os.path.join(REPO_ROOT, a.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    times = sorted(pos.time_s.unique())

    def nearest_time(t):
        return min(times, key=lambda x: abs(x - t))

    # ---- snapshot
    t = nearest_time(a.snapshot_time)
    f = pos[pos.time_s == t]
    fig, ax = plt.subplots(figsize=(6.4, 6.8))
    fig.patch.set_facecolor(BG)
    draw_base(ax, roads, servers)
    ax.scatter(f.x, f.y, s=34, c=colors_for(f), edgecolors=BG, linewidths=1.2, zorder=3)
    busy = int((f.server_id >= 0).sum())
    ax.set_title(f"SUMO grid at t = {t:.0f} s: {len(f)} vehicles, {busy} with tasks offloaded\n"
                 "vehicle colour = server running its tasks, grey = no tasks", fontsize=10, loc="left")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "network_snapshot.png"), dpi=150)
    plt.close(fig)

    # ---- animation
    frames = [x for x in times if a.gif_start <= x <= a.gif_start + a.gif_seconds]
    if frames:
        fig, ax = plt.subplots(figsize=(5.2, 5.6))
        fig.patch.set_facecolor(BG)
        draw_base(ax, roads, servers)
        dots = ax.scatter([], [], s=30, edgecolors=BG, linewidths=1.0, zorder=3)
        title = ax.set_title("", fontsize=10, loc="left")

        def update(tt):
            g = pos[pos.time_s == tt]
            dots.set_offsets(g[["x", "y"]].values)
            dots.set_facecolors(colors_for(g))
            title.set_text(f"t = {tt:5.1f} s   vehicles: {len(g)}   with tasks: {int((g.server_id >= 0).sum())}")
            return dots, title

        anim = FuncAnimation(fig, update, frames=frames, blit=False)
        try:
            anim.save(os.path.join(out_dir, "network_animation.gif"), writer=PillowWriter(fps=8), dpi=80)
        except Exception as e:  # Pillow missing
            print(f"Skipping GIF ({e}). Install Pillow: python -m pip install pillow")
        plt.close(fig)
    print(f"Wrote network_snapshot.png and network_animation.gif to {a.out_dir}/")


if __name__ == "__main__":
    main()
