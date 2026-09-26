#!/usr/bin/env bash
# Live mode: engine on a real feed + kslat tracer + web dashboard. Ctrl-C stops everything.
#
#   sudo ./live.sh bitstamp [btcusd]        L3, every order
#   sudo ./live.sh binance  [BTCUSDT]       L2 depth, 100ms diffs
#   sudo ./live.sh mock                     offline: local mock exchange (Bitstamp protocol)
#
# Env: OUT=runs/live-<ts>  PORT=8080  HOST=127.0.0.1  CPU=1 (hot thread)  AUX_CPU=0
#      WINDOW=60 (dashboard rolling window, s)  DURATION=0 (0 = until Ctrl-C)
#      EXTRA="--no-prefault ..." extra engine flags
set -euo pipefail
cd "$(dirname "$0")"
FEED=${1:-bitstamp}; SYM=${2:-}
OUT=${OUT:-runs/live-$(date +%Y%m%d-%H%M%S)}
PORT=${PORT:-8080}; HOST=${HOST:-127.0.0.1}; CPU=${CPU:-1}; AUX_CPU=${AUX_CPU:-0}
WINDOW=${WINDOW:-60}; DURATION=${DURATION:-0}
mkdir -p "$OUT"
PIDS=()
cleanup() {
  trap - INT TERM EXIT
  [ -n "${EPID:-}" ] && kill -TERM "$EPID" 2>/dev/null || true
  sleep 0.5
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  echo "stopped. run data in $OUT (analyze offline: python3 analyzer/kslat_analyze.py $OUT)"
}
trap cleanup INT TERM EXIT

ARGS=(--out "$OUT" --wait --cpu "$CPU" --aux-cpu "$AUX_CPU" --duration "$DURATION")
case "$FEED" in
  mock)
    MPORT=${MPORT:-8765}
    taskset -c "$AUX_CPU" python3 scenarios/mock_exchange.py --port "$MPORT" --rate "${MOCK_RATE:-2000}" > "$OUT/mock.log" 2>&1 &
    PIDS+=($!); sleep 0.5
    ARGS+=(--feed "${MOCK_PROTO:-bitstamp}" --ws-url "ws://127.0.0.1:$MPORT" --rest-url "http://127.0.0.1:$MPORT") ;;
  bitstamp|binance) ARGS+=(--feed "$FEED") ;;
  *) echo "usage: $0 bitstamp|binance|mock [symbol]"; exit 1 ;;
esac
[ -n "$SYM" ] && ARGS+=(--symbol "$SYM")
# shellcheck disable=SC2206
[ -n "${EXTRA:-}" ] && ARGS+=($EXTRA)

./engine/engine "${ARGS[@]}" 2> >(tee "$OUT/engine.log" >&2) &
EPID=$!
while [ ! -s "$OUT/ready" ]; do sleep 0.05; kill -0 $EPID 2>/dev/null || exit 1; done
read -r PID TID < "$OUT/ready"
cp /proc/interrupts "$OUT/interrupts.txt"

taskset -c "$AUX_CPU" ./tracer/kslat -p "$PID" -t "$TID" -o "$OUT/events.bin" 2> "$OUT/tracer.log" &
PIDS+=($!)
until grep -q "tracing pid" "$OUT/tracer.log" 2>/dev/null; do sleep 0.05; done

taskset -c "$AUX_CPU" python3 analyzer/kslat_live.py "$OUT" --host "$HOST" --port "$PORT" --window "$WINDOW" &
PIDS+=($!)
kill -USR1 "$PID"
echo "dashboard: http://$HOST:$PORT   (Ctrl-C to stop)"
wait $EPID || true
