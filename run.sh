#!/usr/bin/env bash
# Run the engine under the kslat tracer, then analyze.
#   ./run.sh OUTDIR [engine args...]
# Env: NOISE="cmd" runs a noise generator during the run (killed afterwards).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$1; shift
rm -rf "$OUT"; mkdir -p "$OUT"

./engine/engine --out "$OUT" --wait "$@" 2> "$OUT/engine.log" &
EPID=$!
while [ ! -s "$OUT/ready" ]; do sleep 0.05; done
PID=$(cut -d' ' -f1 "$OUT/ready")
TID=$(cut -d' ' -f2 "$OUT/ready")
TRACER_CPU=${TRACER_CPU:-0}

taskset -c "$TRACER_CPU" ./tracer/kslat -p "$PID" -t "$TID" -o "$OUT/events.bin" 2> "$OUT/tracer.log" &
TPID=$!
until grep -q "tracing pid" "$OUT/tracer.log" 2>/dev/null; do
  if ! kill -0 $TPID 2>/dev/null; then cat "$OUT/tracer.log"; kill $EPID; exit 1; fi
  sleep 0.05
done

NPID=""
if [ -n "${NOISE:-}" ]; then
  bash -c "$NOISE" > /dev/null 2>&1 &
  NPID=$!
  sleep 0.3
fi

cp /proc/interrupts "$OUT/interrupts.txt"
grep "^cpu[0-9]" /proc/stat > "$OUT/stat_before.txt"
kill -USR1 "$PID"
wait $EPID
grep "^cpu[0-9]" /proc/stat > "$OUT/stat_after.txt"
[ -n "$NPID" ] && { pkill -P $NPID 2>/dev/null || true; kill $NPID 2>/dev/null || true; }
wait $TPID || true
cat "$OUT/engine.log" "$OUT/tracer.log" | grep -v "^generating\|^ready"
