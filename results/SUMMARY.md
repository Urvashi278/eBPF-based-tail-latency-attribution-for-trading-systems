| scenario | p99 µs | p99.9 µs | p99.99 µs | max µs | tail explained | top causes (share of >p99.9 excess) | ground-truth check |
|---|---|---|---|---|---|---|---|
| 01_baseline | 0.98 | 5.33 | 62.5 | 9202 | 5% | timer tick (4%), softirq SCHED (1%) | — |
| 02_no_prefault | 0.87 | 2.44 | 23.8 | 5724 | 12% | page fault (6%), timer tick (6%) | — |
| 03_fault_inject | 0.96 | 5.05 | 35.5 | 1388 | 13% | page fault (10%), timer tick (3%) | injected page fault: recall 100% / precision 100% |
| 04_syscall_on_hot_path | 0.85 | 2.61 | 15.8 | 2364 | 13% | syscall write (6%), timer tick (3%) | injected syscall: recall 100% / precision 100% |
| 05_tlb_shootdown | 0.87 | 1.69 | 20.0 | 1650 | 14% | TLB shootdown IPI (11%), timer tick (2%) | — |
| 06_cpu_contention | 0.86 | 1.77 | 24.0 | 8301 | 88% | preempted by sh (87%), timer tick (0%) | — |
| 07_wakeup_storm | 0.90 | 5.43 | 40.9 | 2664 | 39% | preempted by python3 (22%), timer tick (17%) | — |
