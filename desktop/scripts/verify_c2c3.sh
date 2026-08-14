#!/usr/bin/env bash
# C2 (capture flows) + C3 (review workspace) verification run.
#
# Complements scripts/verify.sh (C1's evidence, still the source of the
# synth-d6.bin / synth.lscan reused here). This script adds:
#
#   06-measure.png / 06-measure-window.png
#       a real measure-tool pick, driven through ViewportWindow's actual
#       QMouseEvent path (main.cpp --measure-selftest), not a private-method
#       call — see NOTES.md for the resulting segment.
#   export-*.ply / .las / .pcd
#       the replayed synthetic project exported in all three formats via the
#       SAME scanengine::export_points() entry point ExportDialog uses
#       (main.cpp --export), driven against the live PageStore a --replay
#       populated.
#   export-*.ply.check.txt
#       scripts/check_ply.py's from-scratch PLY reader re-reading the PLY
#       export, independent of A9's own writer/tests — the "re-import the
#       PLY as a sanity check" step C3 task 4 asks for.
#   mid360-selftest.log
#       CaptureWindow's guided Mid-360 self-test (main.cpp
#       --mid360-selftest) run against spikes/s2-mid360-sim's mid360_sim on
#       loopback, using the exact host/lidar IP loopback quirk
#       engine/tests/test_mid360_driver.cpp documents (127.000.000.001).
#       Only runs if the simulator binary is present — see §2's build note.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "${HERE}/.." && pwd)"
BUILD="${HERE}/build"
OUT="${HERE}/evidence"
APP="${BUILD}/lidarscan"
CLI="${BUILD}/engine/engine_cli"
SIM="${REPO}/spikes/s2-mid360-sim/build/spike/mid360_sim"

mkdir -p "$OUT"
LOG="${OUT}/verify_c2c3.log"
: > "$LOG"

run() { echo "\$ $*" | tee -a "$LOG"; "$@" 2>&1 | tee -a "$LOG"; }

echo "=== 0. reuse (or (re)build) the C1 synthetic capture ===" | tee -a "$LOG"
if [ ! -f "${OUT}/synth-d6.bin" ]; then
  run "$CLI" --synth "${OUT}/synth-d6.bin" 30 --noise
fi
rm -rf "${OUT}/c2c3-synth.lscan"
run "$APP" --project "${OUT}/c2c3-synth.lscan" --import-raw "${OUT}/synth-d6.bin" \
    --quit-after 3

echo "=== 1. measure tool: two real clicks through ViewportWindow's own event path ===" | tee -a "$LOG"
run "$APP" --project "${OUT}/c2c3-synth.lscan" --replay=0 --measure-selftest \
    --shot "${OUT}/06-measure.png" --shot-delay 7 --quit-after 10

echo "=== 2. export the replayed cloud: PLY / LAS / PCD (same export_points() ExportDialog uses) ===" | tee -a "$LOG"
for fmt in ply las pcd; do
  run "$APP" --project "${OUT}/c2c3-synth.lscan" --replay=0 \
      --export "${fmt}:${OUT}/export-c2c3.${fmt}" --export-delay 6 --quit-after 9
done

echo "=== 3. re-import the PLY export with a from-scratch reader (independent of A9's own) ===" | tee -a "$LOG"
run python3 "${HERE}/scripts/check_ply.py" "${OUT}/export-c2c3.ply"

echo "=== 4. file sizes for all three formats ===" | tee -a "$LOG"
run ls -la "${OUT}/export-c2c3.ply" "${OUT}/export-c2c3.las" "${OUT}/export-c2c3.pcd"

echo "=== 5. Mid-360 self-test against the S2 simulator on loopback ===" | tee -a "$LOG"
if [ ! -x "$SIM" ]; then
  echo "SKIPPED: $SIM not built — see spikes/s2-mid360-sim/CMakeLists.txt + scripts/fetch_sdk2.sh" \
      | tee -a "$LOG"
else
  # The loopback quirk engine/tests/test_mid360_driver.cpp documents: the SDK's
  # self-IP filter (device_manager.cpp:472) compares host_ip as a STRING, so
  # "127.000.000.001" (numerically == 127.0.0.1) is what lets a loopback
  # simulator's packets through. Do not copy this into a real deployment.
  "$SIM" --lidar-ip 127.0.0.1 --host-ip 127.0.0.1 --duration 20 \
      > "${OUT}/mid360-sim-stdout.log" 2>&1 &
  SIM_PID=$!
  trap 'kill "$SIM_PID" 2>/dev/null || true' EXIT
  sleep 0.5
  run "$APP" --mid360-selftest "127.000.000.001:127.0.0.1" --quit-after 12 \
      2> "${OUT}/mid360-selftest.log" || true
  cat "${OUT}/mid360-selftest.log" | tee -a "$LOG"
  wait "$SIM_PID" 2>/dev/null || true
  trap - EXIT
fi

echo "verification complete — see ${OUT}" | tee -a "$LOG"
