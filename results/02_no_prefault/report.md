# kslat report: 02_no_prefault

Config: 2,000,000 msgs @ 500,000/s on CPU 1, no-prefault

| p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|
| 0.08 µs | 0.87 µs | 2.44 µs | 23.77 µs | 5.72 ms |

Tick-to-book (receive/due -> book updated, includes queueing): p50 0.17 µs, p99 424.58 µs, p99.9 3.23 ms, max 5.72 ms

## Tail (> p99.9): 2000 msgs, 12% of excess latency attributed to kernel events

| cause | time | share of tail excess |
|---|---|---|
| page fault | 2.04 ms | 5.7% |
| timer tick | 2.00 ms | 5.6% |
| softirq TIMER | 212.29 µs | 0.6% |
| softirq SCHED | 46.60 µs | 0.1% |
| softirq NET_RX | 40.99 µs | 0.1% |
| softirq RCU | 33.04 µs | 0.1% |
| preempted by mi-scavenger | 24.02 µs | 0.1% |
| hardirq virtio7-output.0 | 13.70 µs | 0.0% |
| preempted by kworker/1:0 | 9.87 µs | 0.0% |
| IPI/reschedule | 2.65 µs | 0.0% |
| *unexplained (user-space / hypervisor)* | 31.37 ms | 87.6% |

Cross-check: unexplained tail time 31.37 ms vs hypervisor steal on the engine CPU 50.00 ms (/proc/stat).

## By percentile bucket

| bucket | msgs | with kernel event | attributed | top cause |
|---|---|---|---|---|
| p99-p99.9 | 17972 | 83 | 1% | page fault |
| p99.9-p99.99 | 1800 | 724 | 18% | page fault |
| >p99.99 | 200 | 102 | 9% | timer tick |

Estimated cost per minor page fault: **3.37 µs** (from 752 single-fault spans)

## Every kernel interruption of the hot thread

| cause | count | total | p50 | p99 | max |
|---|---|---|---|---|---|
| timer tick | 1004 | 13.23 ms | 11.74 µs | 29.59 µs | 36.65 µs |
| preempted by mi-scavenger | 12 | 1.58 ms | 57.82 µs | 484.93 µs | 503.51 µs |
| softirq TIMER | 76 | 1.45 ms | 1.83 µs | 76.19 µs | 86.69 µs |
| softirq RCU | 35 | 221.29 µs | 1.34 µs | 24.51 µs | 26.30 µs |
| softirq SCHED | 41 | 194.64 µs | 4.75 µs | 7.99 µs | 9.06 µs |
| softirq BLOCK | 7 | 151.92 µs | 22.19 µs | 23.70 µs | 23.77 µs |
| softirq NET_RX | 23 | 143.96 µs | 5.69 µs | 10.98 µs | 11.12 µs |
| preempted by kworker/1:0 | 6 | 109.98 µs | 11.79 µs | 49.61 µs | 51.41 µs |
| hardirq virtio7-output.0 | 23 | 42.89 µs | 1.91 µs | 2.40 µs | 2.40 µs |
| syscall openat | 1 | 37.27 µs | 37.27 µs | 37.27 µs | 37.27 µs |
| hardirq virtio1-req.0 | 7 | 37.00 µs | 5.49 µs | 5.87 µs | 5.88 µs |
| preempted by Bun Pool 0 | 1 | 32.09 µs | 32.09 µs | 32.09 µs | 32.09 µs |

Hypervisor steal on CPU 1 during the run: **50.00 ms** (1.25% of wall time; tick-granular, from /proc/stat). Invisible to guest tracepoints, so it lands in *unexplained*.

## 10 slowest messages

| seq | type | latency | attributed to |
|---|---|---|---|
| 643123 | add | 5.72 ms | timer tick 12.29 µs, page fault 3.37 µs, unexplained 5.71 ms |
| 641279 | add | 2.22 ms | timer tick 10.42 µs, softirq SCHED 5.57 µs, page fault 3.37 µs, unexplained 2.20 ms |
| 38639 | cancel | 1.94 ms | timer tick 16.70 µs, unexplained 1.93 ms |
| 1328218 | add | 1.41 ms | —, unexplained 1.41 ms |
| 1847820 | cancel | 785.01 µs | timer tick 17.33 µs, unexplained 767.48 µs |
| 861061 | add | 352.65 µs | —, unexplained 352.59 µs |
| 27697 | add | 302.68 µs | —, unexplained 302.62 µs |
| 891288 | cancel | 257.46 µs | —, unexplained 257.27 µs |
| 59232 | add | 245.02 µs | —, unexplained 244.97 µs |
| 59215 | add | 232.43 µs | —, unexplained 232.37 µs |

Kernel events on the hot thread: offcpu=21, hardirq=31, softirq=182, vector=1018, fault=755, tlb=15, syscall=14
