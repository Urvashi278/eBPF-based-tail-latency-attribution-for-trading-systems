# kslat report: 04_syscall_on_hot_path

Config: 2,000,000 msgs @ 500,000/s on CPU 1, syscall-every 2000

| p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|
| 0.08 µs | 0.85 µs | 2.61 µs | 15.81 µs | 2.36 ms |

Tick-to-book (receive/due -> book updated, includes queueing): p50 0.16 µs, p99 79.96 µs, p99.9 2.04 ms, max 4.36 ms

## Tail (> p99.9): 2000 msgs, 13% of excess latency attributed to kernel events

| cause | time | share of tail excess |
|---|---|---|
| syscall write | 1.47 ms | 6.1% |
| timer tick | 775.79 µs | 3.2% |
| preempted by kworker/1:0 | 578.08 µs | 2.4% |
| softirq TIMER | 192.58 µs | 0.8% |
| softirq BLOCK | 100.57 µs | 0.4% |
| softirq SCHED | 21.32 µs | 0.1% |
| hardirq virtio1-req.0 | 16.27 µs | 0.1% |
| softirq RCU | 13.29 µs | 0.1% |
| softirq NET_RX | 8.93 µs | 0.0% |
| hardirq virtio7-output.0 | 2.14 µs | 0.0% |
| *unexplained (user-space / hypervisor)* | 20.82 ms | 86.8% |

Cross-check: unexplained tail time 20.82 ms vs hypervisor steal on the engine CPU 30.00 ms (/proc/stat).

## By percentile bucket

| bucket | msgs | with kernel event | attributed | top cause |
|---|---|---|---|---|
| p99-p99.9 | 17948 | 322 | 2% | syscall write |
| p99.9-p99.99 | 1800 | 677 | 11% | syscall write |
| >p99.99 | 200 | 61 | 16% | timer tick |

## Validation against injected ground truth

| injected | spans | detected | recall | precision |
|---|---|---|---|---|
| injected syscall | 1000 | 1000 | 100.0% | 100.0% |

## Every kernel interruption of the hot thread

| cause | count | total | p50 | p99 | max |
|---|---|---|---|---|---|
| timer tick | 1004 | 13.10 ms | 10.60 µs | 30.99 µs | 384.10 µs |
| syscall write | 1001 | 1.79 ms | 1.33 µs | 5.08 µs | 19.58 µs |
| preempted by kworker/1:0 | 9 | 1.66 ms | 23.73 µs | 563.44 µs | 564.34 µs |
| softirq TIMER | 78 | 1.45 ms | 2.12 µs | 68.98 µs | 80.91 µs |
| preempted by mi-scavenger | 4 | 640.89 µs | 59.55 µs | 502.44 µs | 514.94 µs |
| softirq SCHED | 41 | 285.73 µs | 6.70 µs | 9.12 µs | 9.16 µs |
| softirq RCU | 38 | 270.34 µs | 1.88 µs | 30.48 µs | 30.49 µs |
| softirq BLOCK | 8 | 240.52 µs | 22.69 µs | 74.69 µs | 78.16 µs |
| softirq NET_RX | 26 | 159.56 µs | 5.49 µs | 9.78 µs | 9.99 µs |
| hardirq virtio7-output.0 | 26 | 55.72 µs | 2.15 µs | 2.68 µs | 2.69 µs |
| hardirq virtio1-req.0 | 8 | 52.47 µs | 6.00 µs | 10.38 µs | 10.67 µs |
| syscall openat | 1 | 44.44 µs | 44.44 µs | 44.44 µs | 44.44 µs |

Hypervisor steal on CPU 1 during the run: **30.00 ms** (0.75% of wall time; tick-granular, from /proc/stat). Invisible to guest tracepoints, so it lands in *unexplained*.

## 10 slowest messages

| seq | type | latency | attributed to |
|---|---|---|---|
| 208267 | aggress | 2.36 ms | timer tick 34.54 µs, unexplained 2.33 ms |
| 1998762 | add | 1.13 ms | —, unexplained 1.13 ms |
| 34333 | add | 740.88 µs | —, unexplained 740.83 µs |
| 980320 | aggress | 598.99 µs | preempted by kworker/1:0 564.34 µs, softirq TIMER 8.95 µs, timer tick 8.34 µs, unexplained 17.22 µs |
| 1186523 | cancel | 200.94 µs | —, unexplained 200.74 µs |
| 1975313 | cancel | 134.77 µs | —, unexplained 134.58 µs |
| 1454390 | cancel | 123.73 µs | —, unexplained 123.54 µs |
| 199397 | cancel | 109.85 µs | —, unexplained 109.66 µs |
| 1268940 | add | 104.76 µs | softirq BLOCK 78.16 µs, hardirq virtio1-req.0 10.67 µs, unexplained 15.88 µs |
| 1983039 | aggress | 102.82 µs | —, unexplained 102.69 µs |

Kernel events on the hot thread: offcpu=15, hardirq=34, softirq=191, vector=1009, fault=1, tlb=6, syscall=1014
