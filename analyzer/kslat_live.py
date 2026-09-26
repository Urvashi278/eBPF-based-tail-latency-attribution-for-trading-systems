#!/usr/bin/env python3
"""Live kslat dashboard: tails spans.bin + events.bin of a running engine, re-runs the
attribution over a rolling window every couple of seconds, and serves a web page.

  kslat_live.py RUNDIR [--port 8080] [--host 127.0.0.1] [--window 60] [--tick 2]

Open http://127.0.0.1:8080 . JSON is at /api/state.
"""
import argparse
import json
import os
import sys
import threading
import time
from collections import defaultdict, deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kslat_analyze import (EV_DT, EV_FAULT, EV_MIGRATE, EV_TLB, MSG_TYPES, SPAN_DT, SPAN_V1, SPAN_V2,  # noqa: E402
                           TLB_REMOTE_SHOOTDOWN, Run, attribute, label, load_irq_names,
                           load_threads, normalize_spans)

HERE = os.path.dirname(os.path.abspath(__file__))


def mono_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


class Tail:
    """Incrementally reads whole fixed-size records appended to a growing file."""

    def __init__(self, path, dtype):
        self.path, self.dt, self.off = path, dtype, 0

    def read(self):
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return np.zeros(0, dtype=self.dt)
        n = (size - self.off) // self.dt.itemsize
        if n <= 0:
            return np.zeros(0, dtype=self.dt)
        a = np.fromfile(self.path, dtype=self.dt, count=n, offset=self.off)
        self.off += n * self.dt.itemsize
        return a


def pcts(x, qs=(50, 99, 99.9)):
    if len(x) == 0:
        return {f"p{q}": None for q in qs} | {"max": None}
    out = {f"p{q}": int(np.percentile(x, q)) for q in qs}
    out["max"] = int(x.max())
    return out


class Live:
    def __init__(self, d, window_s, tick_s, max_spans=3_000_000):
        self.d, self.window_ns, self.tick_s, self.max_spans = d, int(window_s * 1e9), tick_s, max_spans
        self.meta = None
        self.spans = np.zeros(0, dtype=SPAN_DT)
        self.events = np.zeros(0, dtype=EV_DT)
        self.span_tail = self.ev_tail = None
        self.history = deque(maxlen=900)
        self.totals = defaultdict(lambda: [0, 0])   # label -> [count, ns] since start
        self.total_updates = 0
        self.fault_cost = None
        self.state = {"ready": False}
        self.lock = threading.Lock()
        self.irq_names = {}
        self.steal_prev = None

    def _steal_ns(self):
        """hypervisor steal on the engine CPU since the last call (tick granular)"""
        try:
            for line in open("/proc/stat"):
                f = line.split()
                if f[0] == f"cpu{self.meta['cpu']}":
                    v = int(f[8]) * 1_000_000_000 // os.sysconf("SC_CLK_TCK")
                    d = None if self.steal_prev is None else v - self.steal_prev
                    self.steal_prev = v
                    return d
        except Exception:
            return None

    def _init(self):
        mp = os.path.join(self.d, "meta.json")
        if not os.path.exists(mp):
            return False
        self.meta = json.load(open(mp))
        v = self.meta.get("span_version", 1)
        self.span_ver = v
        self.span_tail = Tail(os.path.join(self.d, "spans.bin"), SPAN_V2 if v >= 2 else SPAN_V1)
        self.ev_tail = Tail(os.path.join(self.d, "events.bin"), EV_DT)
        ip = os.path.join(self.d, "interrupts.txt")
        self.irq_names = load_irq_names(ip if os.path.exists(ip) else "/proc/interrupts")
        return True

    def step(self):
        if self.meta is None and not self._init():
            return
        new_s = normalize_spans(self.span_tail.read(), self.span_ver)
        new_e = self.ev_tail.read()
        self.total_updates += len(new_s)
        now = mono_ns()
        lo = now - self.window_ns
        self.spans = np.concatenate([self.spans, new_s])
        self.spans = self.spans[self.spans["t0"] >= lo][-self.max_spans:]
        self.events = np.concatenate([self.events, new_e])
        self.events = self.events[self.events["ts"] + self.events["dur"] >= lo]

        # running totals of every kernel interruption of the hot thread
        hot = new_e[new_e["tid"] == self.meta["tid"]]
        tlb = hot[(hot["type"] == EV_TLB) & (hot["arg0"] == TLB_REMOTE_SHOOTDOWN)]["ts"].astype(np.int64)
        r0 = Run(self.meta, new_s[:0], hot, self.irq_names, 0, np.iinfo(np.uint64).max)
        r0.own_threads = self.own_threads = load_threads(self.d)
        for e in hot:
            if int(e["type"]) in (EV_TLB, EV_MIGRATE):
                continue
            s0, d0 = int(e["ts"]), int(e["dur"])
            inside = False
            if len(tlb):
                k = np.searchsorted(tlb, s0)
                inside = k < len(tlb) and tlb[k] < s0 + d0
            lab = label(r0, e, inside)[1]
            self.totals[lab][0] += 1
            self.totals[lab][1] += d0 if int(e["type"]) != EV_FAULT else (self.fault_cost or 0)

        st = self._status()
        sp = self.spans
        tick_mask = sp["t0"] >= now - int(self.tick_s * 1e9)
        steal = self._steal_ns()
        point = {"t": round((now - self.meta["t_start"]) / 1e9, 1),
                 "rate": round(int(tick_mask.sum()) / self.tick_s, 1),
                 "svc_p99": None, "e2e_p99": None, "kernel_ns": 0,
                 "steal_ns": steal}
        if tick_mask.any():
            s_t = sp[tick_mask]
            point["svc_p99"] = int(np.percentile((s_t["t1"] - s_t["t0"]).astype(np.int64), 99))
            point["e2e_p99"] = int(np.percentile(s_t["t1"].astype(np.int64) - s_t["t_rx"].astype(np.int64), 99))
            point["svc_max"] = int((s_t["t1"] - s_t["t0"]).max())
        point["kernel_ns"] = int(hot["dur"].sum())
        self.history.append(point)

        state = {"ready": True, "meta": {k: self.meta.get(k) for k in ("mode", "symbol", "cpu", "pid", "tid")},
                 "status": st, "window_s": self.window_ns / 1e9, "updates_total": self.total_updates,
                 "history": list(self.history)}
        if len(sp) >= 20:
            state.update(self._analyze(sp, lo, now))
        state["interruptions_total"] = sorted(
            ({"label": k, "count": c, "total_ns": ns} for k, (c, ns) in self.totals.items()),
            key=lambda x: -x["total_ns"])[:14]
        with self.lock:
            self.state = state

    def _status(self):
        try:
            return json.load(open(os.path.join(self.d, "status.json")))
        except Exception:
            return {}

    def _analyze(self, sp, lo, now):
        run = Run(self.meta, sp, self.events, self.irq_names, lo, now)
        run.own_threads = getattr(self, "own_threads", {})
        lat, e2e = run.lat, run.e2e
        types = sp["type"]
        base = np.zeros(len(lat), dtype=np.int64)
        for t in np.unique(types):
            m = types == t
            base[m] = int(np.median(lat[m]))
        excess = np.maximum(lat - base, 0)

        attr0, fw = attribute(run)
        clean = [j for j, (n, _) in fw.items() if n == 1 and set(attr0[j]) <= {"page fault"}]
        if len(clean) >= 5:
            self.fault_cost = int(np.median(excess[clean]))
        attr, _ = attribute(run, cap_fault_ns=self.fault_cost)

        thr = np.percentile(lat, 99)
        idx = np.nonzero(lat > thr)[0]
        causes = defaultdict(int)
        with_ev = 0
        for j in idx:
            a = attr.get(int(j))
            if not a:
                continue
            with_ev += 1
            tot = sum(a.values())
            scale = min(1.0, excess[j] / tot) if tot else 0
            for k, v in a.items():
                causes[k] += int(v * scale)
        tail_ex = int(excess[idx].sum())
        attributed = sum(causes.values())

        top = np.argsort(lat)[::-1][:15]
        slow = []
        for j in top:
            a = attr.get(int(j), {})
            slow.append({"ago_s": round((now - int(sp["t0"][j])) / 1e9, 1),
                         "seq": int(sp["seq"][j]), "op": MSG_TYPES.get(int(types[j]), "?"),
                         "service_ns": int(lat[j]), "e2e_ns": int(e2e[j]),
                         "causes": dict(sorted(((k, int(v)) for k, v in a.items()), key=lambda kv: -kv[1])),
                         "unexplained_ns": max(0, int(excess[j]) - int(sum(a.values())))})

        wire = sp["wire"][(sp["wire"] != 0) & (sp["last"] == 1)]
        span_s = max((int(sp["t0"][-1]) - int(sp["t0"][0])) / 1e9, 1e-9)
        return {
            "n_window": int(len(sp)),
            "rate_window": round(len(sp) / span_s, 1),
            "service_ns": pcts(lat),
            "e2e_ns": pcts(e2e),
            "wire_ns": pcts(wire, (50, 99)) if len(wire) else None,
            "fault_cost_ns": self.fault_cost,
            "tail": {"threshold_ns": int(thr), "n": int(len(idx)), "with_kernel_event": with_ev,
                     "excess_ns": tail_ex, "attributed_ns": attributed,
                     "attributed_frac": attributed / tail_ex if tail_ex else 0,
                     "causes": [{"label": k, "ns": v} for k, v in sorted(causes.items(), key=lambda kv: -kv[1])][:10]},
            "slowest": slow,
        }

    def loop(self):
        while True:
            t = time.time()
            try:
                self.step()
            except Exception as e:  # keep serving the last good state
                with self.lock:
                    self.state["error"] = repr(e)
            time.sleep(max(0.05, self.tick_s - (time.time() - t)))


def serve(live, host, port):
    page = open(os.path.join(HERE, "dashboard.html"), "rb").read()

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.startswith("/api/state"):
                with live.lock:
                    body = json.dumps(live.state).encode()
                ctype = "application/json"
            elif self.path in ("/", "/index.html"):
                body, ctype = page, "text/html; charset=utf-8"
            else:
                self.send_response(404); self.end_headers(); return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    ThreadingHTTPServer((host, port), H).serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("rundir")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--window", type=float, default=60, help="rolling window, seconds")
    ap.add_argument("--tick", type=float, default=2, help="refresh period, seconds")
    a = ap.parse_args()
    live = Live(a.rundir, a.window, a.tick)
    threading.Thread(target=live.loop, daemon=True).start()
    print(f"kslat live dashboard: http://{a.host}:{a.port}", flush=True)
    serve(live, a.host, a.port)
