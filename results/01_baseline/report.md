# kslat report: 01_baseline

Config: 2,000,000 msgs @ 500,000/s on CPU 1

| p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|
| 0.13 µs | 0.98 µs | 5.33 µs | 62.53 µs | 9.20 ms |

Tick-to-book (receive/due -> book updated, includes queueing): p50 0.22 µs, p99 6.17 ms, p99.9 17.23 ms, max 18.14 ms

## Tail (> p99.9): 1999 msgs, 5% of excess latency attributed to kernel events

| cause | time | share of tail excess |
|---|---|---|
| timer tick | 4.09 ms | 3.8% |
| softirq SCHED | 708.37 µs | 0.7% |
| softirq TIMER | 642.02 µs | 0.6% |
| preempted by mi-scavenger | 252.75 µs | 0.2% |
| softirq RCU | 36.21 µs | 0.0% |
| preempted by kcompactd0 | 34.67 µs | 0.0% |
| softirq NET_RX | 10.46 µs | 0.0% |
| hardirq virtio7-output.0 | 3.96 µs | 0.0% |
| IPI/reschedule | 0.96 µs | 0.0% |
| *unexplained (user-space / hypervisor)* | 100.90 ms | 94.6% |

Cross-check: unexplained tail time 100.90 ms vs hypervisor steal on the engine CPU 50.00 ms (/proc/stat).

## By percentile bucket

| bucket | msgs | with kernel event | attributed | top cause |
|---|---|---|---|---|
| p99-p99.9 | 17940 | 0 | 0% | - |
| p99.9-p99.99 | 1799 | 197 | 6% | timer tick |
| >p99.99 | 200 | 52 | 5% | timer tick |

## Every kernel interruption of the hot thread

| cause | count | total | p50 | p99 | max |
|---|---|---|---|---|---|
| timer tick | 998 | 16.40 ms | 12.94 µs | 53.63 µs | 168.41 µs |
| softirq TIMER | 79 | 2.05 ms | 2.57 µs | 215.43 µs | 375.35 µs |
| softirq SCHED | 41 | 935.38 µs | 7.89 µs | 356.98 µs | 535.96 µs |
| preempted by kworker/1:0 | 7 | 310.05 µs | 9.94 µs | 232.14 µs | 246.06 µs |
| softirq BLOCK | 8 | 292.49 µs | 22.34 µs | 107.64 µs | 111.56 µs |
| softirq RCU | 41 | 289.84 µs | 7.32 µs | 27.77 µs | 33.45 µs |
| preempted by mi-scavenger | 1 | 252.75 µs | 252.75 µs | 252.75 µs | 252.75 µs |
| preempted by Bun Pool 0 | 2 | 192.82 µs | 96.41 µs | 167.02 µs | 168.46 µs |
| softirq NET_RX | 24 | 166.86 µs | 6.54 µs | 10.45 µs | 10.46 µs |
| hardirq virtio7-output.0 | 25 | 53.84 µs | 2.23 µs | 3.52 µs | 3.78 µs |
| hardirq virtio1-req.0 | 8 | 47.69 µs | 5.53 µs | 7.97 µs | 8.01 µs |
| preempted by kcompactd0 | 1 | 34.67 µs | 34.67 µs | 34.67 µs | 34.67 µs |

Hypervisor steal on CPU 1 during the run: **50.00 ms** (1.25% of wall time; tick-granular, from /proc/stat). Invisible to guest tracepoints, so it lands in *unexplained*.

## 10 slowest messages

| seq | type | latency | attributed to |
|---|---|---|---|
| 1001785 | add | 9.20 ms | timer tick 35.70 µs, softirq RCU 2.19 µs, unexplained 9.16 ms |
| 699124 | cancel | 4.86 ms | timer tick 168.41 µs, unexplained 4.69 ms |
| 699491 | cancel | 3.73 ms | timer tick 156.12 µs, unexplained 3.57 ms |
| 895471 | add | 3.38 ms | timer tick 22.25 µs, unexplained 3.36 ms |
| 1815340 | add | 3.16 ms | timer tick 14.37 µs, softirq TIMER 2.43 µs, unexplained 3.15 ms |
| 518365 | cancel | 3.09 ms | timer tick 17.89 µs, unexplained 3.07 ms |
| 513610 | cancel | 3.04 ms | timer tick 21.80 µs, unexplained 3.02 ms |
| 697570 | cancel | 2.86 ms | timer tick 91.22 µs, softirq SCHED 88.51 µs, unexplained 2.68 ms |
| 895399 | cancel | 2.61 ms | —, unexplained 2.61 ms |
| 511776 | cancel | 2.37 ms | timer tick 23.78 µs, softirq TIMER 1.20 µs, unexplained 2.35 ms |

Kernel events on the hot thread: offcpu=13, hardirq=34, softirq=193, vector=1004, fault=1, tlb=4, syscall=14
