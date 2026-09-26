#!/usr/bin/env python3
"""Plots for kslat runs.
  kslat_plot.py RESULTS_DIR   -> RESULTS_DIR/summary.png + <run>/tail.png for each run
"""
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]   # fixed categorical order
MUTED = "#b8b7b1"                                     # "unexplained" is not a category
GROUPS = ["Scheduler (off-CPU)", "Interrupts & timer", "Memory (faults, TLB)", "Syscalls"]


def group_of(cause):
    if cause.startswith(("preempted", "blocked")):
        return 0
    if cause.startswith(("page fault", "TLB", "direct")):
        return 2
    if cause.startswith("syscall"):
        return 3
    return 1


def style(ax):
    ax.set_facecolor(SURFACE)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9, length=0)
    ax.xaxis.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def summary(R, runs):
    fig, ax = plt.subplots(figsize=(9, 0.55 * len(runs) + 1.6), facecolor=SURFACE)
    style(ax)
    names = [r["run"] for r in runs][::-1]
    for i, r in enumerate(runs[::-1]):
        t = r["tail"]
        ex = max(t["excess_ns"], 1)
        shares = [0.0] * 4
        for k, v in t["causes_ns"].items():
            shares[group_of(k)] += v / ex * 100
        left = 0
        for g, sh in enumerate(shares):
            if sh > 0:
                ax.barh(i, sh, left=left, height=0.62, color=SERIES[g],
                        edgecolor=SURFACE, linewidth=2)
                left += sh
        ax.barh(i, 100 - left, left=left, height=0.62, color=MUTED, edgecolor=SURFACE, linewidth=2)
        ax.text(101, i, f"{left:.0f}% kernel", va="center", fontsize=9, color=INK)
    ax.set_yticks(range(len(names)), names, color=INK)
    ax.set_xlim(0, 115)
    ax.set_xticks([0, 25, 50, 75, 100], ["0%", "25%", "50%", "75%", "100%"])
    fig.suptitle("Where the >p99.9 excess latency went, per scenario", x=0.02, ha="left",
                 fontsize=12, color=INK)
    handles = [plt.Rectangle((0, 0), 1, 1, color=c) for c in SERIES] + \
              [plt.Rectangle((0, 0), 1, 1, color=MUTED)]
    ax.legend(handles, GROUPS + ["Unexplained (user-space / hypervisor steal)"], ncol=3,
              loc="lower left", bbox_to_anchor=(0, 1.0), frameon=False, fontsize=8.5,
              labelcolor=INK2, handlelength=1, handleheight=1)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(os.path.join(R, "summary.png"), dpi=150)
    plt.close(fig)


def tail(R, r):
    t = r["tail"]
    items = list(t["causes_ns"].items())[:7]
    un = t["excess_ns"] - t["attributed_ns"]
    items.append(("unexplained", un))
    items = sorted(items, key=lambda kv: kv[1])
    fig, ax = plt.subplots(figsize=(8, 0.42 * len(items) + 1.3), facecolor=SURFACE)
    style(ax)
    vals = [v / 1e3 for _, v in items]
    cols = [MUTED if k == "unexplained" else SERIES[0] for k, _ in items]
    ax.barh(range(len(items)), vals, height=0.6, color=cols)
    for i, v in enumerate(vals):
        ax.text(v, i, f"  {v:,.0f} µs", va="center", fontsize=8.5, color=INK2)
    ax.set_yticks(range(len(items)), [k for k, _ in items], color=INK)
    ax.set_xlim(0, max(vals) * 1.2)
    ax.set_xlabel("time inside >p99.9 messages (µs)", color=INK2, fontsize=9)
    ax.set_title(f"{r['run']}: tail excess by cause", loc="left", fontsize=11, color=INK)
    fig.tight_layout()
    fig.savefig(os.path.join(R, r["run"], "tail.png"), dpi=150)
    plt.close(fig)


if __name__ == "__main__":
    R = sys.argv[1] if len(sys.argv) > 1 else "results"
    runs = []
    for d in sorted(os.listdir(R)):
        p = os.path.join(R, d, "attribution.json")
        if os.path.exists(p):
            runs.append(json.load(open(p)))
    for r in runs:
        tail(R, r)
    summary(R, runs)
    print(f"wrote {R}/summary.png and {len(runs)} tail.png files")
