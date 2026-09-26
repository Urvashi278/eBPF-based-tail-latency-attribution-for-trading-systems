# kslat report: 03_fault_inject

Config: 2,000,000 msgs @ 500,000/s on CPU 1, fault-every 1000

| p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|
| 0.10 µs | 0.96 µs | 5.05 µs | 35.54 µs | 1.39 ms |

Tick-to-book (receive/due -> book updated, includes queueing): p50 0.19 µs, p99 346.90 µs, p99.9 2.19 ms, max 3.70 ms

## Tail (> p99.9): 1999 msgs, 13% of excess latency attributed to kernel events

| cause | time | share of tail excess |
|---|---|---|
| page fault | 3.96 ms | 9.9% |
| timer tick | 1.16 ms | 2.9% |
| softirq TIMER | 114.58 µs | 0.3% |
| softirq NET_RX | 17.91 µs | 0.0% |
| softirq SCHED | 5.94 µs | 0.0% |
| hardirq virtio7-output.0 | 5.76 µs | 0.0% |
| softirq RCU | 2.42 µs | 0.0% |
| *unexplained (user-space / hypervisor)* | 34.57 ms | 86.8% |

Cross-check: unexplained tail time 34.57 ms vs hypervisor steal on the engine CPU 60.00 ms (/proc/stat).

## By percentile bucket

| bucket | msgs | with kernel event | attributed | top cause |
|---|---|---|---|---|
| p99-p99.9 | 17930 | 1112 | 15% | page fault |
| p99.9-p99.99 | 1799 | 827 | 20% | page fault |
| >p99.99 | 200 | 123 | 7% | timer tick |

Estimated cost per minor page fault: **4.48 µs** (from 1999 single-fault spans)

## Validation against injected ground truth

| injected | spans | detected | recall | precision |
|---|---|---|---|---|
| injected page fault | 2000 | 2000 | 100.0% | 100.0% |

## Every kernel interruption of the hot thread

| cause | count | total | p50 | p99 | max |
|---|---|---|---|---|---|
| timer tick | 1003 | 15.05 ms | 12.46 µs | 37.47 µs | 189.43 µs |
| softirq TIMER | 83 | 1.57 ms | 2.02 µs | 87.13 µs | 122.97 µs |
| softirq RCU | 37 | 240.68 µs | 2.33 µs | 26.11 µs | 27.79 µs |
| softirq SCHED | 41 | 222.58 µs | 5.55 µs | 7.36 µs | 7.37 µs |
| softirq BLOCK | 8 | 193.26 µs | 22.99 µs | 31.61 µs | 31.80 µs |
| softirq NET_RX | 30 | 184.47 µs | 5.76 µs | 11.19 µs | 11.20 µs |
| preempted by kworker/1:0 | 8 | 177.13 µs | 12.08 µs | 88.91 µs | 94.54 µs |
| preempted by claude | 1 | 75.48 µs | 75.48 µs | 75.48 µs | 75.48 µs |
| hardirq virtio7-output.0 | 30 | 58.12 µs | 2.06 µs | 2.45 µs | 2.45 µs |
| hardirq virtio1-req.0 | 8 | 48.99 µs | 5.98 µs | 9.85 µs | 10.08 µs |
| syscall openat | 1 | 32.87 µs | 32.87 µs | 32.87 µs | 32.87 µs |
| syscall sched_setaffinity | 1 | 23.48 µs | 23.48 µs | 23.48 µs | 23.48 µs |

Hypervisor steal on CPU 1 during the run: **60.00 ms** (1.50% of wall time; tick-granular, from /proc/stat). Invisible to guest tracepoints, so it lands in *unexplained*.

## 10 slowest messages

| seq | type | latency | attributed to |
|---|---|---|---|
| 193999 | cancel | 1.39 ms | timer tick 35.17 µs, page fault 4.48 µs, unexplained 1.35 ms |
| 1673464 | add | 1.20 ms | —, unexplained 1.20 ms |
| 835832 | add | 1.02 ms | timer tick 20.30 µs, unexplained 995.58 µs |
| 222927 | add | 460.66 µs | —, unexplained 460.61 µs |
| 1371054 | cancel | 436.79 µs | —, unexplained 436.58 µs |
| 1002262 | cancel | 355.78 µs | —, unexplained 355.58 µs |
| 225673 | cancel | 344.26 µs | —, unexplained 344.06 µs |
| 1876614 | cancel | 317.26 µs | timer tick 21.93 µs, unexplained 295.13 µs |
| 1864134 | add | 302.31 µs | —, unexplained 302.25 µs |
| 1866212 | add | 295.42 µs | —, unexplained 295.36 µs |

Kernel events on the hot thread: offcpu=10, hardirq=39, softirq=199, vector=1004, fault=2001, tlb=2, syscall=14
