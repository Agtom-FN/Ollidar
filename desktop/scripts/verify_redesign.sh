#!/usr/bin/env bash
#
# verify_redesign.sh — the evidence run for the owner-approved UI redesign
# (docs/design/REVIEW_FEEDBACK.md, docs/design/redesign-exports/*.png,
# docs/design/lidarscan-interfaces.html). Same shape as verify.sh /
# verify_c2c3.sh / verify_c4c5.sh / verify_c6c7.sh: build, then drive the REAL
# app through the REAL flows and write screenshots + a log into evidence/.
#
# What each step proves, and what it does not:
#
#   1  fonts        The .qrc reached the binary and QFontDatabase registered
#                   all ten faces of all three bundled families. If it printed
#                   "BUNDLE MISSING" the app would still run on platform
#                   fallbacks — the point of the check is that it does not have
#                   to.
#   2  workspaces   Every one of the six rail workspaces is reachable and
#                   renders. Driven through MainWindow::showWorkspaceNamed(),
#                   which is the same onRailActivated() path a rail click takes.
#   3  review       The full-bleed review workspace with the floating inspector
#                   over a real replayed cloud.
#   4  inspector    The inspector's point-size slider, moved through its own
#                   QSlider (not the model directly), changes the rendered
#                   frame. Measured as mean |per-pixel delta| over the frame,
#                   NOT as a brightness change — see main.cpp's note.
#   5  reflow       Below 880 px the floating card becomes an ordinary dock and
#                   the window still lays out.
#   6  capture      The record cluster in every state the C2 machine reaches:
#                   gated (no hardware needed), armed / recording / paused
#                   against the S2 Mid-360 simulator on loopback.
#
# Usage: desktop/scripts/verify_redesign.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESKTOP="$(dirname "$HERE")"
REPO="$(dirname "$DESKTOP")"
APP="${DESKTOP}/build/lidarscan"
OUT="${DESKTOP}/evidence"
LOG="${OUT}/verify_redesign.log"
SIM="${REPO}/spikes/s2-mid360-sim/build/spike/mid360_sim"

mkdir -p "$OUT"
: > "$LOG"

say() { echo "$@" | tee -a "$LOG"; }
run() { say "\$ $*"; "$@" 2>&1 | tee -a "$LOG"; }

say "=== redesign verification — $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
say "host: $(uname -srm)"

# ---------------------------------------------------------------- build ----
say ""
say "--- 0. clean build (zero warnings from desktop/src is the bar) ---"
cmake -S "$DESKTOP" -B "${DESKTOP}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    >> "$LOG" 2>&1
rm -rf "${DESKTOP}/build/CMakeFiles/lidarscan.dir"
cmake --build "${DESKTOP}/build" > "${OUT}/redesign-build.log" 2>&1
BUILD_RC=$?
say "build exit: ${BUILD_RC}"
DESKTOP_WARNINGS=$(grep -E "warning:" "${OUT}/redesign-build.log" \
    | grep -v "engine/third_party" | wc -l | tr -d ' ')
say "warnings from desktop/src: ${DESKTOP_WARNINGS} (vendored Livox SDK2 warnings excluded, as in NOTES.md §6)"
[ "$BUILD_RC" -eq 0 ] || { say "BUILD FAILED — stopping"; exit 1; }

# ---------------------------------------------------------------- fonts ----
say ""
say "--- 1. bundled typefaces (QFontDatabase::addApplicationFont from :/fonts) ---"
run "$APP" --font-report --quit-after 2

# ----------------------------------------------------------- workspaces ----
say ""
say "--- 2. every rail workspace renders ---"
for WS in projects capture review plan merge jobs; do
  run "$APP" --plan-fixture --workspace "$WS" \
      --shot "${OUT}/redesign-ws-${WS}.png" --shot-delay 3 --quit-after 6 \
      --size 1760x980
done

# --------------------------------------------------------------- review ----
say ""
say "--- 3. review workspace over a REPLAYED capture (not a fixture) ---"
run "$APP" --project "${OUT}/synth.lscan" --replay=0 --workspace review \
    --shot "${OUT}/redesign-review-replay.png" --shot-delay 6 --quit-after 9 \
    --size 1760x980

# ------------------------------------------------------------ inspector ----
say ""
say "--- 4. inspector slider -> viewport ---"
run "$APP" --plan-fixture --workspace review \
    --inspector-demo "${OUT}/redesign-slider" --shot-delay 5 --quit-after 12 \
    --size 1760x980

# --------------------------------------------------------------- reflow ----
say ""
say "--- 5. the 880 px reflow (floating card -> ordinary dock) ---"
run "$APP" --plan-fixture --workspace review \
    --shot "${OUT}/redesign-reflow-narrow.png" --shot-delay 3 --quit-after 6 \
    --size 820x760
run "$APP" --plan-fixture --workspace review \
    --shot "${OUT}/redesign-reflow-wide.png" --shot-delay 3 --quit-after 6 \
    --size 1400x760

# -------------------------------------------------------------- capture ----
say ""
say "--- 6. the record cluster, every state (REVIEW_FEEDBACK round-1 item 2) ---"
if [ ! -x "$SIM" ]; then
  say "SKIPPED the armed/recording/paused states: ${SIM} is not built."
  say "  (build it with: cmake -S ${REPO}/spikes/s2-mid360-sim -B ${REPO}/spikes/s2-mid360-sim/build && cmake --build ...)"
  say "  The GATED state needs no hardware and is still shot below."
  run "$APP" --capture-cluster-demo "${OUT}/redesign-capture" --quit-after 5
else
  rm -rf "${OUT}/redesign-capture.lscan"
  # The loopback quirk engine/tests/test_mid360_driver.cpp documents and
  # verify_c2c3.sh already relies on: the SDK's self-IP filter compares
  # host_ip as a STRING, so "127.000.000.001" (numerically 127.0.0.1) is what
  # lets a loopback simulator's packets through. Not for real deployments.
  "$SIM" --lidar-ip 127.0.0.1 --host-ip 127.0.0.1 --duration 30 \
      > "${OUT}/redesign-sim-stdout.log" 2>&1 &
  SIM_PID=$!
  trap 'kill "$SIM_PID" 2>/dev/null || true' EXIT
  sleep 0.6
  run "$APP" --mid360-selftest "127.000.000.001:127.0.0.1" \
      --mid360-record-into "${OUT}/redesign-capture.lscan" \
      --capture-cluster-demo "${OUT}/redesign-capture" \
      --quit-after 18 --size 1760x980
  kill "$SIM_PID" 2>/dev/null || true
  trap - EXIT
fi

say ""
say "=== screenshots written ==="
ls -la "${OUT}"/redesign-*.png 2>/dev/null | tee -a "$LOG"
say ""
say "verification complete — see ${OUT} and ${LOG}"
