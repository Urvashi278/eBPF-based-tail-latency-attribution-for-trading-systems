#!/usr/bin/env python3
"""One-line-per-scenario summary across run directories -> results/SUMMARY.md"""
import json, os, sys
R = sys.argv[1] if len(sys.argv) > 1 else "results"
rows = []
for d in sorted(os.listdir(R)):
    p = os.path.join(R, d, "attribution.json")
    if not os.path.exists(p):
        continue
    o = json.load(open(p)); t = o["tail"]; L = o["latency_ns"]
    top = list(t["causes_ns"].items())[:2]
    tops = ", ".join(f"{k} ({v / t['excess_ns'] * 100:.0f}%)" for k, v in top) or "—"
    val = "; ".join(f"{k}: recall {v['recall']*100:.0f}% / precision {v['precision']*100:.0f}%"
                    for k, v in o["validation"].items()) or "—"
    rows.append(f"| {d} | {L['p99']/1e3:.2f} | {L['p99.9']/1e3:.2f} | {L['p99.99']/1e3:.1f} | {L['max']/1e3:.0f} | "
                f"{t['attributed_frac']*100:.0f}% | {tops} | {val} |")
out = ["| scenario | p99 µs | p99.9 µs | p99.99 µs | max µs | tail explained | top causes (share of >p99.9 excess) | ground-truth check |",
       "|---|---|---|---|---|---|---|---|"] + rows
open(os.path.join(R, "SUMMARY.md"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
