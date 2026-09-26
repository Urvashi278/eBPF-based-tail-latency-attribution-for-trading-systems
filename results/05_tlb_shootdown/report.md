# kslat report: 05_tlb_shootdown

Config: 2,000,000 msgs @ 500,000/s on CPU 1, tlb-noise

| p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|
| 0.10 µs | 0.87 µs | 1.69 µs | 19.99 µs | 1.65 ms |

Tick-to-book (receive/due -> book updated, includes queueing): p50 0.20 µs, p99 146.22 µs, p99.9 1.47 ms, max 2.85 ms

## Tail (> p99.9): 2000 msgs, 14% of excess latency attributed to kernel events

| cause | time | share of tail excess |
|---|---|---|
| TLB shootdown IPI | 3.03 ms | 10.7% |
| timer tick | 521.22 µs | 1.8% |
| softirq RCU | 238.21 µs | 0.8% |
| softirq BLOCK | 24.91 µs | 0.1% |
| softirq NET_RX | 18.76 µs | 0.1% |
| softirq SCHED | 14.08 µs | 0.0% |
| hardirq virtio1-req.0 | 10.59 µs | 0.0% |
| hardirq virtio7-output.0 | 3.54 µs | 0.0% |
| *unexplained (user-space / hypervisor)* | 24.58 ms | 86.4% |

Cross-check: unexplained tail time 24.58 ms vs hypervisor steal on the engine CPU 40.00 ms (/proc/stat).

## By percentile bucket

| bucket | msgs | with kernel event | attributed | top cause |
|---|---|---|---|---|
| p99-p99.9 | 17963 | 0 | 0% | - |
| p99.9-p99.99 | 1800 | 874 | 15% | TLB shootdown IPI |
| >p99.99 | 200 | 127 | 11% | timer tick |

## Every kernel interruption of the hot thread

| cause | count | total | p50 | p99 | max |
|---|---|---|---|---|---|
| TLB shootdown IPI | 10832 | 33.99 ms | 2.91 µs | 6.08 µs | 99.34 µs |
| timer tick | 1002 | 12.78 ms | 11.69 µs | 22.60 µs | 214.98 µs |
| softirq RCU | 989 | 5.34 ms | 1.49 µs | 18.89 µs | 48.12 µs |
| softirq TIMER | 48 | 1.52 ms | 28.30 µs | 109.56 µs | 113.16 µs |
| preempted by kworker/u8:0 | 17 | 409.29 µs | 12.91 µs | 81.23 µs | 89.14 µs |
| preempted by JITWorker | 1 | 369.35 µs | 369.35 µs | 369.35 µs | 369.35 µs |
| softirq SCHED | 41 | 229.07 µs | 5.47 µs | 8.04 µs | 8.07 µs |
| softirq NET_RX | 26 | 154.17 µs | 5.87 µs | 10.03 µs | 10.67 µs |
| preempted by kworker/1:2 | 7 | 123.20 µs | 11.36 µs | 49.45 µs | 51.55 µs |
| softirq BLOCK | 8 | 105.98 µs | 13.89 µs | 18.19 µs | 18.32 µs |
| preempted by rcu_preempt | 8 | 81.58 µs | 7.34 µs | 29.18 µs | 30.55 µs |
| hardirq virtio7-output.0 | 26 | 57.48 µs | 1.85 µs | 10.21 µs | 12.79 µs |

Hypervisor steal on CPU 1 during the run: **40.00 ms** (1.00% of wall time; tick-granular, from /proc/stat). Invisible to guest tracepoints, so it lands in *unexplained*.

## 10 slowest messages

| seq | type | latency | attributed to |
|---|---|---|---|
| 1286419 | add | 1.65 ms | timer tick 15.15 µs, softirq RCU 14.46 µs, unexplained 1.62 ms |
| 775983 | cancel | 814.41 µs | TLB shootdown IPI 4.90 µs, unexplained 809.31 µs |
| 351927 | cancel | 699.22 µs | —, unexplained 699.02 µs |
| 1077187 | cancel | 450.00 µs | —, unexplained 449.81 µs |
| 1290051 | add | 323.23 µs | —, unexplained 323.18 µs |
| 693012 | cancel | 228.87 µs | —, unexplained 228.68 µs |
| 803599 | add | 163.65 µs | —, unexplained 163.59 µs |
| 695214 | cancel | 147.59 µs | —, unexplained 147.39 µs |
| 1472407 | aggress | 133.75 µs | —, unexplained 133.59 µs |
| 1645020 | add | 123.92 µs | TLB shootdown IPI 2.88 µs, unexplained 120.98 µs |

Kernel events on the hot thread: offcpu=36, hardirq=35, softirq=1112, vector=11841, fault=2, tlb=10835, syscall=20
