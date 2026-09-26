#!/usr/bin/env bash
# Reproduce every scenario in results/. Engine on CPU 1, tracer + noise on CPU 0.
set -e
cd "$(dirname "$0")/.."
R=${1:-results}; N=${N:-2000000}
./run.sh $R/01_baseline            --n $N
./run.sh $R/02_no_prefault         --n $N --no-prefault
./run.sh $R/03_fault_inject        --n $N --fault-every 1000
./run.sh $R/04_syscall_on_hot_path --n $N --syscall-every 2000
./run.sh $R/05_tlb_shootdown       --n $N --tlb-noise --noise-cpu 0
NOISE="taskset -c 1 sh -c 'while :; do :; done'" \
  ./run.sh $R/06_cpu_contention    --n $N
NOISE="taskset -c 1 python3 -c 'import time
while True: time.sleep(0.0002)'" \
  ./run.sh $R/07_wakeup_storm      --n $N
for d in $R/0*/; do python3 analyzer/kslat_analyze.py "$d" > /dev/null; done
python3 analyzer/kslat_summary.py $R
python3 analyzer/kslat_plot.py $R
