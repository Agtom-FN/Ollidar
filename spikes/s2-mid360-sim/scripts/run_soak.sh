#!/usr/bin/env bash
# S2 exit test: run sdk_client_demo against mid360_sim for a sustained soak.
#
#   scripts/run_soak.sh [DURATION_S] [EXTRA_SIM_ARGS...]
#
# Default 600 s (the S2 10-minute exit criterion). Results land in results/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUR="${1:-600}"; shift || true
BIN="$HERE/build/spike"
OUT="$HERE/results"
mkdir -p "$OUT"
STAMP="$(date +%Y%m%d-%H%M%S)"
TAG="${SOAK_TAG:-soak-${DUR}s}"

[ -x "$BIN/mid360_sim" ] || { echo "build first: cmake --build $HERE/build/spike -j"; exit 1; }

pkill -f "$BIN/mid360_sim"    2>/dev/null || true
pkill -f "$BIN/sdk_client_demo" 2>/dev/null || true
sleep 0.5

SIM_LOG="$OUT/${TAG}-${STAMP}-sim.log"
DEMO_LOG="$OUT/${TAG}-${STAMP}-demo.log"
CSV="$OUT/${TAG}-${STAMP}-demo.csv"
PS_LOG="$OUT/${TAG}-${STAMP}-ps.log"

echo "sim  -> $SIM_LOG"
echo "demo -> $DEMO_LOG"
echo "csv  -> $CSV"

"$BIN/mid360_sim" --duration $((DUR + 40)) --stats-period 60 "$@" > "$SIM_LOG" 2>&1 &
SIM_PID=$!
sleep 1

"$BIN/sdk_client_demo" "$HERE/config/mid360_loopback.json" \
    --duration "$DUR" --report-period 60 --csv "$CSV" > "$DEMO_LOG" 2>&1 &
DEMO_PID=$!

# Independent, external CPU/RSS sampling of both processes (ps, not self-reported).
( echo "iso_time,proc,pid,pcpu,rss_kb"
  while kill -0 "$DEMO_PID" 2>/dev/null; do
    now="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    for pair in "sim:$SIM_PID" "demo:$DEMO_PID"; do
      name="${pair%%:*}"; pid="${pair##*:}"
      line="$(ps -o pcpu=,rss= -p "$pid" 2>/dev/null || true)"
      [ -n "$line" ] && echo "$now,$name,$pid,$(echo $line | tr ' ' ',')"
    done
    sleep 15
  done ) > "$PS_LOG" &

wait "$DEMO_PID" || true
kill "$SIM_PID" 2>/dev/null || true
wait "$SIM_PID" 2>/dev/null || true
sleep 0.5

echo
echo "================= sim tail ================="
tail -8 "$SIM_LOG"
echo
echo "================= demo summary ============="
sed -n '/sdk_client_demo summary/,/^====/p' "$DEMO_LOG"
