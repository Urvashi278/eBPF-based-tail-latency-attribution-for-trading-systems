# kslat report: 06_cpu_contention

Config: 2,000,000 msgs @ 500,000/s on CPU 1

| p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|
| 0.10 µs | 0.86 µs | 1.77 µs | 23.95 µs | 8.30 ms |

Tick-to-book (receive/due -> book updated, includes queueing): p50 596.06 µs, p99 4.04 ms, p99.9 7.65 ms, max 8.54 ms

## Tail (> p99.9): 1999 msgs, 88% of excess latency attributed to kernel events

| cause | time | share of tail excess |
|---|---|---|
| preempted by sh | 197.53 ms | 87.4% |
| timer tick | 927.87 µs | 0.4% |
| softirq TIMER | 67.30 µs | 0.0% |
| softirq NET_RX | 56.13 µs | 0.0% |
| softirq BLOCK | 22.32 µs | 0.0% |
| softirq SCHED | 21.68 µs | 0.0% |
| hardirq virtio7-output.0 | 17.20 µs | 0.0% |
| hardirq virtio1-req.0 | 5.24 µs | 0.0% |
| softirq RCU | 4.22 µs | 0.0% |
| *unexplained (user-space / hypervisor)* | 27.29 ms | 12.1% |

Cross-check: unexplained tail time 27.29 ms vs hypervisor steal on the engine CPU 60.00 ms (/proc/stat).

## By percentile bucket

| bucket | msgs | with kernel event | attributed | top cause |
|---|---|---|---|---|
| p99-p99.9 | 17965 | 0 | 0% | - |
| p99.9-p99.99 | 1799 | 3 | 0% | softirq NET_RX |
| >p99.99 | 200 | 56 | 93% | preempted by sh |

## Every kernel interruption of the hot thread

| cause | count | total | p50 | p99 | max |
|---|---|---|---|---|---|
| preempted by sh | 491 | 1985.20 ms | 4.00 ms | 5.93 ms | 7.99 ms |
| preempted by kworker/1:2 | 4 | 15.85 ms | 3.96 ms | 3.98 ms | 3.98 ms |
| timer tick | 501 | 8.23 ms | 14.95 µs | 32.06 µs | 41.80 µs |
| softirq TIMER | 38 | 682.23 µs | 2.19 µs | 60.07 µs | 61.02 µs |
| softirq SCHED | 20 | 147.40 µs | 7.07 µs | 9.77 µs | 9.81 µs |
| syscall openat | 1 | 88.35 µs | 88.35 µs | 88.35 µs | 88.35 µs |
| softirq NET_RX | 12 | 80.39 µs | 6.18 µs | 12.54 µs | 13.06 µs |
| softirq BLOCK | 3 | 74.69 µs | 24.25 µs | 28.04 µs | 28.12 µs |
| softirq RCU | 17 | 69.89 µs | 1.61 µs | 18.51 µs | 19.19 µs |
| syscall clone3 | 1 | 43.99 µs | 43.99 µs | 43.99 µs | 43.99 µs |
| syscall sched_setaffinity | 1 | 30.52 µs | 30.52 µs | 30.52 µs | 30.52 µs |
| hardirq virtio7-output.0 | 12 | 25.07 µs | 2.02 µs | 2.90 µs | 2.94 µs |

Hypervisor steal on CPU 1 during the run: **60.00 ms** (1.50% of wall time; tick-granular, from /proc/stat). Invisible to guest tracepoints, so it lands in *unexplained*.

## 10 slowest messages

| seq | type | latency | attributed to |
|---|---|---|---|
| 437814 | cancel | 8.30 ms | preempted by sh 5.73 ms, timer tick 22.03 µs, softirq SCHED 7.99 µs, unexplained 2.54 ms |
| 1415949 | cancel | 4.17 ms | preempted by sh 4.06 ms, timer tick 41.80 µs, unexplained 66.81 µs |
| 733950 | cancel | 4.09 ms | preempted by sh 4.05 ms, timer tick 17.52 µs, unexplained 28.68 µs |
| 1079949 | cancel | 4.08 ms | preempted by sh 4.03 ms, timer tick 12.49 µs, unexplained 33.07 µs |
| 217950 | cancel | 4.07 ms | preempted by sh 4.01 ms, timer tick 19.69 µs, unexplained 45.23 µs |
| 1255949 | cancel | 4.07 ms | preempted by sh 4.02 ms, timer tick 16.16 µs, softirq RCU 1.75 µs, unexplained 30.35 µs |
| 1991950 | cancel | 4.07 ms | preempted by sh 4.00 ms, timer tick 18.99 µs, unexplained 48.07 µs |
| 121949 | cancel | 4.06 ms | preempted by sh 4.01 ms, timer tick 18.81 µs, unexplained 36.30 µs |
| 1443951 | cancel | 4.06 ms | preempted by sh 3.99 ms, timer tick 30.16 µs, unexplained 38.35 µs |
| 213952 | cancel | 4.06 ms | preempted by sh 3.98 ms, timer tick 28.71 µs, unexplained 47.74 µs |

Kernel events on the hot thread: offcpu=495, hardirq=15, softirq=90, vector=502, fault=1, tlb=492, syscall=14
