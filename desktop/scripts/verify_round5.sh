#!/usr/bin/env bash
# Round-5 capture-workflow verification (NOTES.md §17).
#
# Runs the REAL app against spikes/s2-mid360-sim's mid360_sim on loopback and
# against scripts/replay_mid360_heartbeat.py's replay of the field session's own
# captured heartbeat, and produces evidence/18-*.png plus this log. Every hook
# below drives the shipped widgets — there is no test-only UI path.
#
#   18-preview-window.png            live preview, display controls moved
#   18-recording-window.png          recording, REC badge + elapsed clock
#   18-trail-window.png              the trajectory trail (item 18)
#   18-projects-window.png           the sealed scan in the Projects library
#   18-autodetect-inline.png         inline auto-detect (no dialog), real beacon
#   18-refresh-downshift-window.png  the measured refresh downshift (item 17)
#   18-projects-actions-*.png        Process/Export/Merge folded into Projects
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "${HERE}/.." && pwd)"
BUILD="${HERE}/build"
OUT="${HERE}/evidence"
APP="${BUILD}/lidarscan"
SIM="${REPO}/spikes/s2-mid360-sim/build/spike/mid360_sim"
LOOPBACK="127.000.000.001:127.0.0.1"   # the SDK self-IP string quirk; see verify_c2c3.sh

mkdir -p "$OUT"
LOG="${OUT}/verify_round5.log"
: > "$LOG"
run() { echo "\$ $*" | tee -a "$LOG"; "$@" 2>&1 | tee -a "$LOG"; }

sim_up() {  # $1 = seconds
  [ -x "$SIM" ] || { echo "SKIPPED: $SIM not built" | tee -a "$LOG"; return 1; }
  "$SIM" --lidar-ip 127.0.0.1 --host-ip 127.0.0.1 --duration "$1" \
      > "${OUT}/round5-sim-stdout.log" 2>&1 &
  SIM_PID=$!
  trap 'kill "$SIM_PID" 2>/dev/null || true' EXIT
  sleep 1
}
sim_down() { wait "$SIM_PID" 2>/dev/null || true; trap - EXIT; }

echo "=== 1. the whole capture flow: inline auto-detect -> auto-arm -> live preview ===" | tee -a "$LOG"
echo "===    -> display params -> Start with an EMPTY name -> sealed -> Projects  ===" | tee -a "$LOG"
if sim_up 45; then
  run "$APP" --capture-flow-demo "${OUT}/18" --mid360-selftest "$LOOPBACK" --quit-after 34
  sim_down
fi

echo "=== 2. headless CI hooks: --mid360-selftest + --mid360-record-into ===" | tee -a "$LOG"
if sim_up 25; then
  rm -rf "${OUT}/round5-record.lscan"
  run "$APP" --mid360-selftest "$LOOPBACK" \
      --mid360-record-into "${OUT}/round5-record.lscan" --quit-after 15
  sim_down
fi

echo "=== 3. discovery <-> device serialization, both directions (NOTES §16.7/§17.2) ===" | tee -a "$LOG"
if sim_up 25; then
  run "$APP" --auto-detect-cancel-selftest --mid360-selftest "$LOOPBACK" --quit-after 14
  sim_down
fi

echo "=== 4. inline auto-detect against the REAL captured heartbeat ===" | tee -a "$LOG"
python3 "${HERE}/scripts/replay_mid360_heartbeat.py" > "${OUT}/round5-heartbeat.log" 2>&1 &
HB_PID=$!
trap 'kill "$HB_PID" 2>/dev/null || true' EXIT
sleep 1
run "$APP" --auto-detect-selftest --auto-detect-shot "${OUT}/18-autodetect-inline.png" \
    --quit-after 14
kill "$HB_PID" 2>/dev/null || true
trap - EXIT

echo "=== 5. item 17: the live refresh cap downshifts under load, capture unaffected ===" | tee -a "$LOG"
if sim_up 30; then
  run "$APP" --mid360-selftest "$LOOPBACK" --workspace capture --live-refresh 60 \
      --resize-storm 12 --shot "${OUT}/18-refresh-downshift.png" --shot-delay 14 --quit-after 18
  sim_down
fi

echo "=== 5b. ROUND-5 FIELD BUGS (NOTES §17.9-17.11) ===" | tee -a "$LOG"
echo "===     A: a gait-free source must read 0.00 m/s for a whole minute      ===" | tee -a "$LOG"
echo "===     B: the refresh governor recovers instead of ratcheting to 5 fps  ===" | tee -a "$LOG"
echo "===     C: N record cycles on ONE connect, and re-arm across link drops  ===" | tee -a "$LOG"

# A, part 1 — the estimator and the IMU gate, no hardware at all. The POSITIVE
# case (a real walk registers, a brisk walk trips the hint) lives here because
# mid360_sim has no motion options and no gait to emulate one with.
run "$APP" --walk-speed-selftest

# A, part 2 — the same hint, driven by the SHIPPED 10 Hz poll over real
# LioOdometry output. The pre-fix build answered 4.36 m/s here and fired the
# "ease off" hint 226 times in 60 s.
if sim_up 75; then
  run "$APP" --mid360-selftest "$LOOPBACK" --walk-soak 60 --quit-after 80
  sim_down
fi

# C, part 1 — four back-to-back Start/Stop cycles on one arm, through the record
# cluster's own Start (auto-named project, no explicit directory). Nonzero
# chunks in every cycle or this exits 7.
if sim_up 50; then
  run "$APP" --mid360-selftest "$LOOPBACK" --record-cycles 4 \
      --record-cycles-dir "${OUT}/round5-cycles" --quit-after 55
  sim_down
fi

# C, part 2 — the same, across repeated 14 s transport outages: the post-restart
# data watch has to re-arm the Mid-360 IN PLACE (recorder left open) so a cycle
# that begins inside an outage still records once the link returns.
if [ -x "$SIM" ]; then
  "$SIM" --lidar-ip 127.0.0.1 --host-ip 127.0.0.1 --duration 70 \
      --drop-link-after 12 --link-down-for 14 --repeat \
      > "${OUT}/round5-sim-drop.log" 2>&1 &
  SIM_PID=$!
  trap 'kill "$SIM_PID" 2>/dev/null || true' EXIT
  sleep 1
  run "$APP" --mid360-selftest "$LOOPBACK" --record-cycles 4 --record-cycle-seconds 12 \
      --record-cycles-dir "${OUT}/round5-cycles-drop" --quit-after 75
  sim_down
fi

echo "=== 6. item 4 (5.1): Process / Export / Merge folded into the Projects tab ===" | tee -a "$LOG"
run "$APP" --projects-actions-demo "${OUT}/18-projects-actions" --quit-after 18

echo "=== 6b. app version comes from the repo-root VERSION file (NOTES §18) ===" | tee -a "$LOG"
echo "repo VERSION file: $(tr -d '[:space:]' < "${REPO}/VERSION")" | tee -a "$LOG"
run "$APP" --version

echo "=== 7. no regressions in the other workstreams' hooks ===" | tee -a "$LOG"
for w in projects capture review plan merge jobs; do
  run "$APP" --workspace "$w" --quit-after 2
done
if [ -d "${OUT}/c4-synth-mid360.lscan" ]; then
  run "$APP" --post-e2e "${OUT}/c4-synth-mid360.lscan" --quit-after 25
fi

echo "verification complete — see ${OUT}" | tee -a "$LOG"
