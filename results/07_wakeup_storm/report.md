# kslat report: 07_wakeup_storm

Config: 2,000,000 msgs @ 500,000/s on CPU 1

| p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|
| 0.10 µs | 0.90 µs | 5.43 µs | 40.87 µs | 2.66 ms |

Tick-to-book (receive/due -> book updated, includes queueing): p50 0.22 µs, p99 109.34 µs, p99.9 2.81 ms, max 4.09 ms

## Tail (> p99.9): 2000 msgs, 39% of excess latency attributed to kernel events

| cause | time | share of tail excess |
|---|---|---|
| preempted by python3 | 14.27 ms | 21.8% |
| timer tick | 11.39 ms | 17.4% |
| softirq BLOCK | 48.59 µs | 0.1% |
| softirq SCHED | 27.41 µs | 0.0% |
| softirq NET_RX | 26.54 µs | 0.0% |
| hardirq virtio1-req.0 | 11.44 µs | 0.0% |
| hardirq virtio7-output.0 | 8.33 µs | 0.0% |
| softirq TIMER | 2.88 µs | 0.0% |
| *unexplained (user-space / hypervisor)* | 39.62 ms | 60.6% |

Cross-check: unexplained tail time 39.62 ms vs hypervisor steal on the engine CPU 30.00 ms (/proc/stat).

## By percentile bucket

| bucket | msgs | with kernel event | attributed | top cause |
|---|---|---|---|---|
| p99-p99.9 | 17910 | 0 | 0% | - |
| p99.9-p99.99 | 1800 | 1162 | 47% | preempted by python3 |
| >p99.99 | 200 | 186 | 26% | preempted by python3 |

## Every kernel interruption of the hot thread

| cause | count | total | p50 | p99 | max |
|---|---|---|---|---|---|
| preempted by python3 | 14547 | 147.20 ms | 9.20 µs | 28.05 µs | 361.36 µs |
| timer tick | 15086 | 126.20 ms | 7.23 µs | 21.78 µs | 616.08 µs |
| softirq TIMER | 79 | 1.41 ms | 2.13 µs | 69.03 µs | 73.25 µs |
| preempted by kworker/1:2 | 8 | 1.25 ms | 15.48 µs | 1.08 ms | 1.16 ms |
| softirq SCHED | 39 | 278.41 µs | 7.04 µs | 10.01 µs | 10.15 µs |
| softirq RCU | 42 | 218.30 µs | 1.77 µs | 15.41 µs | 15.94 µs |
| softirq BLOCK | 7 | 157.99 µs | 22.18 µs | 26.31 µs | 26.33 µs |
| softirq NET_RX | 23 | 147.65 µs | 6.30 µs | 9.79 µs | 9.82 µs |
| syscall openat | 1 | 52.63 µs | 52.63 µs | 52.63 µs | 52.63 µs |
| hardirq virtio1-req.0 | 7 | 45.96 µs | 5.63 µs | 12.30 µs | 12.71 µs |
| hardirq virtio7-output.0 | 23 | 45.29 µs | 1.88 µs | 2.88 µs | 2.90 µs |
| syscall clone3 | 1 | 23.50 µs | 23.50 µs | 23.50 µs | 23.50 µs |

Hypervisor steal on CPU 1 during the run: **30.00 ms** (0.75% of wall time; tick-granular, from /proc/stat). Invisible to guest tracepoints, so it lands in *unexplained*.

## 10 slowest messages

| seq | type | latency | attributed to |
|---|---|---|---|
| 299872 | add | 2.66 ms | preempted by python3 33.38 µs, timer tick 18.63 µs, unexplained 2.61 ms |
| 1372110 | aggress | 2.47 ms | preempted by python3 28.47 µs, timer tick 16.67 µs, unexplained 2.42 ms |
| 1130857 | add | 1.90 ms | preempted by python3 37.02 µs, timer tick 30.27 µs, unexplained 1.83 ms |
| 1372170 | aggress | 1.57 ms | preempted by python3 21.60 µs, timer tick 16.80 µs, unexplained 1.53 ms |
| 322348 | cancel | 1.42 ms | preempted by python3 23.25 µs, timer tick 17.67 µs, unexplained 1.38 ms |
| 1386913 | add | 852.82 µs | preempted by python3 21.20 µs, timer tick 12.38 µs, unexplained 819.17 µs |
| 78691 | cancel | 363.49 µs | preempted by python3 29.71 µs, timer tick 9.15 µs, unexplained 324.43 µs |
| 1634617 | add | 361.45 µs | preempted by python3 34.55 µs, timer tick 17.58 µs, unexplained 309.27 µs |
| 1127357 | cancel | 283.87 µs | preempted by python3 12.74 µs, timer tick 9.24 µs, unexplained 261.69 µs |
| 215406 | add | 276.47 µs | preempted by python3 10.59 µs, timer tick 8.16 µs, unexplained 257.66 µs |

Kernel events on the hot thread: offcpu=14556, hardirq=30, softirq=190, vector=15087, fault=1, tlb=14548, syscall=14
