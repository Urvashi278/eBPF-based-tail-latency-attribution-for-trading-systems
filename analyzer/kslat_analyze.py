#!/usr/bin/env python3
"""kslat analyzer: attribute every slow message to the kernel events that overlapped it.

Inputs (a run directory written by run.sh):
  spans.bin   one 32-byte record per message: seq, t0, t1, type, inject (ground truth)
  events.bin  one 64-byte record per kernel event from the BPF tracer
  meta.json   pid/tid/cpu + engine config
Outputs:
  attribution.json, slow_spans.csv, report.md (+ plots via kslat_plot.py)

Attribution model
-----------------
excess(span)  = span latency - median latency of that message type
Each nanosecond inside a span is given to at most one cause, by priority:
  off-CPU > hardirq / system vector > softirq > reclaim / compaction > syscall > page fault
Interval events (everything except faults) have exact BPF-measured durations.
Page faults only have an entry tracepoint, so a fault gets the window from its entry
to the next event on the thread (or span end), capped at a per-fault cost estimated
from spans whose *only* kernel event was a single fault.
unexplained = excess - attributed (clamped at 0): user-space causes (cache misses,
branch mispredicts, long book walks) or hypervisor steal time, which no guest
tracepoint can see.
"""
import argparse
import csv
import json
import os
import re
import sys
from collections import defaultdict

import numpy as np

SPAN_V1 = np.dtype([("seq", "u8"), ("t0", "u8"), ("t1", "u8"), ("type", "u4"), ("inj", "u4")])
SPAN_V2 = np.dtype([("seq", "u8"), ("t_rx", "u8"), ("t0", "u8"), ("t1", "u8"), ("wire", "i8"),
                    ("op", "u4"), ("inj", "u4")])
# normalized in-memory form for both versions
SPAN_DT = np.dtype([("seq", "u8"), ("t_rx", "u8"), ("t0", "u8"), ("t1", "u8"), ("wire", "i8"),
                    ("type", "u4"), ("last", "u1"), ("inj", "u4")])
EV_DT = np.dtype([("ts", "u8"), ("dur", "u8"), ("type", "u4"), ("tid", "u4"), ("cpu", "u4"),
                  ("arg0", "u4"), ("arg1", "u8"), ("arg2", "u8"), ("comm", "S16")])

EV_OFFCPU, EV_HARDIRQ, EV_SOFTIRQ, EV_VECTOR, EV_FAULT, EV_TLB, EV_SYSCALL, \
    EV_RECLAIM, EV_COMPACTION, EV_MIGRATE = range(1, 11)
MSG_TYPES = {0: "add", 1: "cancel", 2: "aggress", 3: "rest", 4: "modify", 5: "delete"}
INJ_FAULT, INJ_SYSCALL = 1, 2
SOFTIRQ_NAMES = ["HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL", "TASKLET",
                 "SCHED", "HRTIMER", "RCU"]
VEC_NAMES = {1: "local_timer", 2: "call_function", 3: "call_function_single",
             4: "reschedule", 5: "irq_work", 6: "other"}
TLB_REMOTE_SHOOTDOWN = 1

# lower number = higher priority when intervals overlap
PRIO = {"offcpu": 0, "hardirq": 1, "vector": 1, "softirq": 2, "reclaim": 3,
        "compaction": 3, "syscall": 4, "fault": 5}


def load_syscall_names():
    names = {}
    for p in ("/usr/include/x86_64-linux-gnu/asm/unistd_64.h", "/usr/include/asm/unistd_64.h"):
        if os.path.exists(p):
            for line in open(p):
                m = re.match(r"#define __NR_(\w+)\s+(\d+)", line)
                if m:
                    names[int(m.group(2))] = m.group(1)
            break
    return names


SYS_NAMES = load_syscall_names()


def load_threads(d):
    """tid -> name for the engine's own threads (written by the engine)"""
    out = {}
    p = os.path.join(d, "threads")
    if os.path.exists(p):
        for line in open(p):
            f = line.split()
            if len(f) == 2:
                out[int(f[0])] = f[1]
    return out


def load_irq_names(path):
    names = {}
    if os.path.exists(path):
        for line in open(path):
            m = re.match(r"\s*(\d+):.*\s(\S+)\s*$", line)
            if m:
                names[int(m.group(1))] = m.group(2)
    return names


def normalize_spans(raw, version):
    """v1 (32 B) or v2 (48 B) records -> SPAN_DT"""
    out = np.zeros(len(raw), dtype=SPAN_DT)
    for f in ("seq", "t0", "t1", "inj"):
        out[f] = raw[f]
    if version >= 2:
        out["t_rx"] = raw["t_rx"]; out["wire"] = raw["wire"]
        out["type"] = raw["op"] & 0xff; out["last"] = (raw["op"] >> 8) & 1
    else:
        out["t_rx"] = raw["t0"]; out["type"] = raw["type"]; out["last"] = 1
    return out


def read_spans(path, version, offset_records=0, max_records=-1):
    dt = SPAN_V2 if version >= 2 else SPAN_V1
    raw = np.fromfile(path, dtype=dt, count=max_records, offset=offset_records * dt.itemsize)
    return normalize_spans(raw, version)


class Run:
    """Spans + hot-thread kernel events for one measurement window."""

    def __init__(self, meta, spans, events, irq_names=None, t_lo=None, t_hi=None):
        self.dir = None
        self.meta = meta
        self.spans = spans
        t_lo = meta["t_start"] if t_lo is None else t_lo
        t_hi = (meta["t_end"] or np.iinfo(np.uint64).max) if t_hi is None else t_hi
        ev = events[(events["tid"] == meta["tid"]) & (events["ts"] >= t_lo) & (events["ts"] <= t_hi)]
        self.events = np.sort(ev, order="ts")          # only the measured window
        self.lat = (spans["t1"] - spans["t0"]).astype(np.int64)
        self.e2e = (spans["t1"].astype(np.int64) - spans["t_rx"].astype(np.int64))
        self.sys_names = SYS_NAMES
        self.irq_names = irq_names or {}

    @classmethod
    def from_dir(cls, d):
        meta = json.load(open(os.path.join(d, "meta.json")))
        spans = read_spans(os.path.join(d, "spans.bin"), meta.get("span_version", 1))
        events = np.fromfile(os.path.join(d, "events.bin"), dtype=EV_DT)
        r = cls(meta, spans, events, load_irq_names(os.path.join(d, "interrupts.txt")))
        r.own_threads = load_threads(d)
        r.dir = d
        return r


def label(run, e, tlb_inside=False):
    """(category, detailed cause label) for one event."""
    t = int(e["type"])
    if t == EV_OFFCPU:
        comm = e["comm"].split(b"\0")[0].decode(errors="replace")
        if int(e["arg0"]) == 0:
            own = getattr(run, "own_threads", {}).get(int(e["arg2"]))
            if own:
                return "offcpu", f"preempted by own thread {own}"
            return "offcpu", f"preempted by {comm}"
        return "offcpu", "blocked (sleep/IO)"
    if t == EV_HARDIRQ:
        return "hardirq", f"hardirq {run.irq_names.get(int(e['arg0']), int(e['arg0']))}"
    if t == EV_SOFTIRQ:
        v = int(e["arg0"])
        return "softirq", f"softirq {SOFTIRQ_NAMES[v] if v < len(SOFTIRQ_NAMES) else v}"
    if t == EV_VECTOR:
        name = VEC_NAMES.get(int(e["arg0"]), "vector")
        if tlb_inside:
            return "vector", "TLB shootdown IPI"
        return "vector", f"IPI/{name}" if name != "local_timer" else "timer tick"
    if t == EV_SYSCALL:
        return "syscall", f"syscall {run.sys_names.get(int(e['arg0']), int(e['arg0']))}"
    if t == EV_RECLAIM:
        return "reclaim", "direct reclaim"
    if t == EV_COMPACTION:
        return "compaction", "direct compaction"
    if t == EV_FAULT:
        return "fault", "page fault"
    return None, None


def attribute(run, cap_fault_ns=None):
    """Per-span exclusive attribution. Returns dict span_idx -> {cause: ns}, plus fault info."""
    sp, ev = run.spans, run.events
    t0 = sp["t0"].astype(np.int64)
    t1 = sp["t1"].astype(np.int64)

    # TLB shootdown points, used to relabel the call_function IPI that contained them
    tlb = ev[(ev["type"] == EV_TLB) & (ev["arg0"] == TLB_REMOTE_SHOOTDOWN)]["ts"].astype(np.int64)

    # Build intervals [start, end) with labels, only for events that can overlap spans
    ivs = []   # (start, end, prio, category, label, is_fault)
    evs = ev[np.isin(ev["type"], [EV_OFFCPU, EV_HARDIRQ, EV_SOFTIRQ, EV_VECTOR, EV_SYSCALL,
                                  EV_RECLAIM, EV_COMPACTION, EV_FAULT])]
    starts = evs["ts"].astype(np.int64)
    for i, e in enumerate(evs):
        s = int(starts[i])
        if int(e["type"]) == EV_FAULT:
            # window: to next event start on this thread; span end applied later
            nxt = int(starts[i + 1]) if i + 1 < len(evs) else s + 10_000_000
            end = max(nxt, s + 1)
            cat, lab = "fault", "page fault"
        else:
            end = s + int(e["dur"])
            inside = False
            if int(e["type"]) == EV_VECTOR and len(tlb):
                k = np.searchsorted(tlb, s)
                inside = k < len(tlb) and tlb[k] < end
            cat, lab = label(run, e, inside)
        ivs.append((s, end, PRIO[cat], cat, lab, cat == "fault"))

    per_span = defaultdict(list)
    for iv in ivs:
        s, e = iv[0], iv[1]
        # spans overlapping [s, e): t1 > s and t0 < e
        lo = np.searchsorted(t1, s, side="right")
        hi = np.searchsorted(t0, e, side="left")
        for j in range(lo, hi):
            per_span[j].append(iv)

    result = {}
    fault_windows = {}
    for j, lst in per_span.items():
        a, b = int(t0[j]), int(t1[j])
        # elementary segments between all boundaries, each given to the top-priority cover
        cuts = sorted({a, b} | {min(max(x[0], a), b) for x in lst} | {min(max(x[1], a), b) for x in lst})
        acc = defaultdict(int)
        fault_ns = 0
        for k in range(len(cuts) - 1):
            lo, hi = cuts[k], cuts[k + 1]
            if hi <= lo:
                continue
            cover = [x for x in lst if x[0] <= lo and x[1] >= hi]
            if not cover:
                continue
            best = min(cover, key=lambda x: x[2])
            if best[5]:
                fault_ns += hi - lo
            else:
                acc[best[4]] += hi - lo
        nfaults = sum(1 for x in lst if x[5] and a <= x[0] < b)
        if nfaults:
            fault_windows[j] = (nfaults, fault_ns)
            f = fault_ns if cap_fault_ns is None else min(fault_ns, nfaults * cap_fault_ns)
            if f:
                acc["page fault"] += f
        if acc:
            result[j] = dict(acc)
    return result, fault_windows


def analyze(d, tail_q=99.9):
    run = Run.from_dir(d)
    lat = run.lat
    types = run.spans["type"]
    base = np.zeros(len(lat), dtype=np.int64)
    baselines = {}
    for t in np.unique(types):
        m = types == t
        baselines[MSG_TYPES.get(int(t), str(t))] = int(np.median(lat[m]))
        base[m] = int(np.median(lat[m]))
    excess = np.maximum(lat - base, 0)

    # pass 1: uncapped, to estimate per-fault cost from "clean" single-fault spans
    attr0, fw = attribute(run)
    clean = [j for j, (n, _) in fw.items() if n == 1 and set(attr0[j]) <= {"page fault"}]
    fault_cost = int(np.median(excess[clean])) if len(clean) >= 5 else None
    attr, _ = attribute(run, cap_fault_ns=fault_cost)

    pct = {f"p{q}": int(np.percentile(lat, q)) for q in (50, 90, 99, 99.9, 99.99)}
    pct["max"] = int(lat.max())
    pct["mean"] = float(lat.mean())
    e2e = {f"p{q}": int(np.percentile(run.e2e, q)) for q in (50, 90, 99, 99.9, 99.99)}
    e2e["max"] = int(run.e2e.max())

    def bucket_summary(mask):
        idx = np.nonzero(mask)[0]
        tot_excess = int(excess[idx].sum())
        causes = defaultdict(int)
        explained_spans = 0
        for j in idx:
            a = attr.get(int(j))
            if a:
                explained_spans += 1
                budget = int(excess[j])
                # never attribute more than the span's excess
                s = sum(a.values())
                scale = min(1.0, budget / s) if s else 0
                for k, v in a.items():
                    causes[k] += int(v * scale)
        attributed = sum(causes.values())
        causes = dict(sorted(causes.items(), key=lambda kv: -kv[1]))
        return {
            "spans": int(len(idx)),
            "spans_with_kernel_event": explained_spans,
            "excess_ns": tot_excess,
            "attributed_ns": attributed,
            "attributed_frac": attributed / tot_excess if tot_excess else 0.0,
            "causes_ns": causes,
        }

    thr = {q: np.percentile(lat, q) for q in (99, 99.9, 99.99)}
    buckets = {
        "p99-p99.9": bucket_summary((lat > thr[99]) & (lat <= thr[99.9])),
        "p99.9-p99.99": bucket_summary((lat > thr[99.9]) & (lat <= thr[99.99])),
        ">p99.99": bucket_summary(lat > thr[99.99]),
    }
    tail = bucket_summary(lat > np.percentile(lat, tail_q))

    # top slowest spans with full breakdown
    top = np.argsort(lat)[::-1][:25]
    slow = []
    for j in top:
        a = attr.get(int(j), {})
        slow.append({
            "seq": int(run.spans["seq"][j]), "type": MSG_TYPES.get(int(types[j]), "?"),
            "latency_ns": int(lat[j]), "excess_ns": int(excess[j]),
            "causes_ns": dict(sorted(a.items(), key=lambda kv: -kv[1])),
            "unexplained_ns": max(0, int(excess[j]) - sum(a.values())),
            "t_offset_ms": (int(run.spans["t0"][j]) - run.meta["t_start"]) / 1e6,
        })

    # ground-truth validation (only when faults/syscalls were injected)
    validation = {}
    inj = run.spans["inj"]
    for bit, name, key in ((INJ_FAULT, "injected page fault", "page fault"),
                           (INJ_SYSCALL, "injected syscall", "syscall")):
        truth = (inj & bit) != 0
        if not truth.any():
            continue
        found = np.array([any(k.startswith(key) for k in attr.get(int(j), {})) for j in range(len(lat))])
        tp = int((truth & found).sum())
        validation[name] = {
            "injected_spans": int(truth.sum()),
            "detected": tp,
            "recall": tp / truth.sum(),
            "precision": tp / found.sum() if found.any() else 0.0,
        }

    # per-cause cost distribution for every interval event on the hot thread
    cause_stats = defaultdict(list)
    tlb = run.events[(run.events["type"] == EV_TLB) & (run.events["arg0"] == TLB_REMOTE_SHOOTDOWN)]["ts"].astype(np.int64)
    for e in run.events:
        if int(e["type"]) in (EV_FAULT, EV_TLB, EV_MIGRATE):
            continue
        s0, d0 = int(e["ts"]), int(e["dur"])
        inside = False
        if int(e["type"]) == EV_VECTOR and len(tlb):
            k = np.searchsorted(tlb, s0)
            inside = k < len(tlb) and tlb[k] < s0 + d0
        cause_stats[label(run, e, inside)[1]].append(d0)
    nf = int((run.events["type"] == EV_FAULT).sum())
    cause_table = {}
    for k, v in sorted(cause_stats.items(), key=lambda kv: -sum(kv[1])):
        v = np.array(v)
        cause_table[k] = {"count": int(len(v)), "total_ns": int(v.sum()), "p50_ns": int(np.median(v)),
                          "p99_ns": int(np.percentile(v, 99)), "max_ns": int(v.max())}
    if nf:
        cause_table["page fault"] = {"count": nf, "total_ns": nf * (fault_cost or 0),
                                     "p50_ns": fault_cost or 0, "p99_ns": 0, "max_ns": 0}

    # hypervisor steal on the engine CPU (guest tracepoints can't see it, /proc/stat can)
    steal_ns = None
    try:
        def steal(fn):
            for line in open(os.path.join(d, fn)):
                f = line.split()
                if f[0] == f"cpu{run.meta['cpu']}":
                    return int(f[8])
        hz = os.sysconf("SC_CLK_TCK")
        steal_ns = (steal("stat_after.txt") - steal("stat_before.txt")) * 1_000_000_000 // hz
    except Exception:
        pass

    ev_counts = {}
    names = {1: "offcpu", 2: "hardirq", 3: "softirq", 4: "vector", 5: "fault", 6: "tlb",
             7: "syscall", 8: "reclaim", 9: "compaction", 10: "migrate"}
    for t, n in zip(*np.unique(run.events["type"], return_counts=True)):
        ev_counts[names.get(int(t), str(t))] = int(n)

    out = {
        "run": os.path.basename(os.path.normpath(d)),
        "meta": run.meta,
        "latency_ns": pct,
        "tick_to_book_ns": e2e,
        "baseline_ns_by_type": baselines,
        "fault_cost_ns_estimate": fault_cost,
        "fault_cost_samples": len(clean),
        "event_counts": ev_counts,
        "tail_threshold": f"p{tail_q}",
        "tail": tail,
        "buckets": buckets,
        "slowest": slow,
        "validation": validation,
        "cause_stats": cause_table,
        "steal_ns_engine_cpu": steal_ns,
        "run_ns": run.meta["t_end"] - run.meta["t_start"],
    }
    with open(os.path.join(d, "attribution.json"), "w") as f:
        json.dump(out, f, indent=2, default=float)
    with open(os.path.join(d, "slow_spans.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["seq", "type", "latency_ns", "excess_ns", "unexplained_ns", "causes"])
        for s in slow:
            w.writerow([s["seq"], s["type"], s["latency_ns"], s["excess_ns"], s["unexplained_ns"],
                        "; ".join(f"{k}={v}" for k, v in s["causes_ns"].items())])
    write_report(out, os.path.join(d, "report.md"))
    return out


def us(ns):
    return f"{ns / 1000:.2f} µs" if ns < 1_000_000 else f"{ns / 1e6:.2f} ms"


def write_report(o, path):
    L = []
    m = o["meta"]
    L.append(f"# kslat report: {o['run']}\n")
    if m.get("mode", "synthetic") == "synthetic":
        cfg = [f"{m['n']:,} msgs @ {m['rate']:,}/s on CPU {m['cpu']}"]
    else:
        cfg = [f"live {m['mode']} {m['symbol']} on CPU {m['cpu']}"]
    if not m["prefault"]: cfg.append("no-prefault")
    if m["fault_every"]: cfg.append(f"fault-every {m['fault_every']}")
    if m["syscall_every"]: cfg.append(f"syscall-every {m['syscall_every']}")
    if m["tlb_noise"]: cfg.append("tlb-noise")
    L.append("Config: " + ", ".join(cfg) + "\n")
    p = o["latency_ns"]
    L.append("| p50 | p99 | p99.9 | p99.99 | max |\n|---|---|---|---|---|")
    L.append(f"| {us(p['p50'])} | {us(p['p99'])} | {us(p['p99.9'])} | {us(p['p99.99'])} | {us(p['max'])} |\n")
    q = o.get("tick_to_book_ns")
    if q:
        L.append("Tick-to-book (receive/due -> book updated, includes queueing): "
                 f"p50 {us(q['p50'])}, p99 {us(q['p99'])}, p99.9 {us(q['p99.9'])}, max {us(q['max'])}\n")
    t = o["tail"]
    L.append(f"## Tail (> {o['tail_threshold']}): {t['spans']} msgs, "
             f"{t['attributed_frac']*100:.0f}% of excess latency attributed to kernel events\n")
    L.append("| cause | time | share of tail excess |\n|---|---|---|")
    for k, v in list(t["causes_ns"].items())[:12]:
        L.append(f"| {k} | {us(v)} | {v / t['excess_ns'] * 100:.1f}% |")
    un = t["excess_ns"] - t["attributed_ns"]
    L.append(f"| *unexplained (user-space / hypervisor)* | {us(un)} | {un / max(t['excess_ns'],1) * 100:.1f}% |\n")
    if o.get("steal_ns_engine_cpu") is not None:
        L.append(f"Cross-check: unexplained tail time {us(un)} vs hypervisor steal on the engine CPU "
                 f"{us(o['steal_ns_engine_cpu'])} (/proc/stat).\n")
    L.append("## By percentile bucket\n")
    L.append("| bucket | msgs | with kernel event | attributed | top cause |\n|---|---|---|---|---|")
    for b, s in o["buckets"].items():
        top = next(iter(s["causes_ns"].items()), ("-", 0))
        L.append(f"| {b} | {s['spans']} | {s['spans_with_kernel_event']} | "
                 f"{s['attributed_frac']*100:.0f}% | {top[0]} |")
    L.append("")
    if o["fault_cost_ns_estimate"]:
        L.append(f"Estimated cost per minor page fault: **{us(o['fault_cost_ns_estimate'])}** "
                 f"(from {o['fault_cost_samples']} single-fault spans)\n")
    if o["validation"]:
        L.append("## Validation against injected ground truth\n")
        L.append("| injected | spans | detected | recall | precision |\n|---|---|---|---|---|")
        for k, v in o["validation"].items():
            L.append(f"| {k} | {v['injected_spans']} | {v['detected']} | "
                     f"{v['recall']*100:.1f}% | {v['precision']*100:.1f}% |")
        L.append("")
    L.append("## Every kernel interruption of the hot thread\n")
    L.append("| cause | count | total | p50 | p99 | max |\n|---|---|---|---|---|---|")
    for k, v in list(o["cause_stats"].items())[:12]:
        L.append(f"| {k} | {v['count']} | {us(v['total_ns'])} | {us(v['p50_ns'])} | "
                 f"{us(v['p99_ns']) if v['p99_ns'] else '-'} | {us(v['max_ns']) if v['max_ns'] else '-'} |")
    L.append("")
    if o["steal_ns_engine_cpu"] is not None:
        L.append(f"Hypervisor steal on CPU {o['meta']['cpu']} during the run: "
                 f"**{us(o['steal_ns_engine_cpu'])}** ({o['steal_ns_engine_cpu'] / o['run_ns'] * 100:.2f}% of wall time; "
                 f"tick-granular, from /proc/stat). Invisible to guest tracepoints, so it lands in *unexplained*.\n")
    L.append("## 10 slowest messages\n")
    L.append("| seq | type | latency | attributed to |\n|---|---|---|---|")
    for s in o["slowest"][:10]:
        c = ", ".join(f"{k} {us(v)}" for k, v in s["causes_ns"].items()) or "—"
        if s["unexplained_ns"] > 1000:
            c += f", unexplained {us(s['unexplained_ns'])}"
        L.append(f"| {s['seq']} | {s['type']} | {us(s['latency_ns'])} | {c} |")
    L.append("")
    L.append("Kernel events on the hot thread: " +
             ", ".join(f"{k}={v}" for k, v in o["event_counts"].items()) + "\n")
    open(path, "w").write("\n".join(L))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("rundir")
    ap.add_argument("--tail", type=float, default=99.9)
    a = ap.parse_args()
    o = analyze(a.rundir, a.tail)
    print(open(os.path.join(a.rundir, "report.md")).read())
