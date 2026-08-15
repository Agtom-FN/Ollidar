#!/usr/bin/env bash
# C4 (processing queue) + C5 (floor plan workspace) verification run.
#
#   09-post-process*.png
#       a REAL A15 kPostProcess job, submitted through ProcessingDock's own
#       JobQueue (main.cpp --post-e2e), run to completion against a synthetic
#       Mid-360 .lscan this script builds first (--build-synth-mid360 — see
#       app/SyntheticMid360.h), with the result loaded into the viewport
#       exactly as the "Load result" button does.
#   10-plan.png / floorplan.dxf / floorplan.pdf / floorplan.pdf.png
#       A12's synthetic two-room-plus-corridor test building (see
#       app/SyntheticBuilding.h), extracted through the Plan dock
#       (--plan-fixture --plan-extract), then exported both ways
#       (--plan-export-dxf/--plan-export-pdf) and Quick-Look-thumbnailed —
#       the same verification A12's own doc uses for its PDF, since no CAD
#       package is installed on this machine to open the DXF either (same
#       standing caveat A9/A12 both record).
#   floorplan.dxf.check.txt
#       scripts/check_dxf.py's from-scratch DXF reader, independent of A12's
#       own writer/tests.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${HERE}/build"
OUT="${HERE}/evidence"
APP="${BUILD}/lidarscan"

mkdir -p "$OUT"
LOG="${OUT}/verify_c4c5.log"
: > "$LOG"

run() { echo "\$ $*" | tee -a "$LOG"; "$@" 2>&1 | tee -a "$LOG"; }

echo "=== 0. build a REAL synthetic Mid-360 .lscan (kMid360Points/kMid360Imu chunks) ===" | tee -a "$LOG"
rm -rf "${OUT}/c4-synth-mid360.lscan"
run "$APP" --build-synth-mid360 "${OUT}/c4-synth-mid360.lscan" --quit-after 3

echo "=== 1. C4: a REAL post-process job through ProcessingDock's JobQueue, end to end ===" | tee -a "$LOG"
run "$APP" --post-e2e "${OUT}/c4-synth-mid360.lscan" \
    --shot "${OUT}/09-post-process.png" --shot-delay 6 --quit-after 10

echo "=== 1b. C4: Submit-to-cloud fails GRACEFULLY against a real socket, no server present ===" | tee -a "$LOG"
run "$APP" --cloud-submit-selftest "https://127.0.0.1:1/v1" --quit-after 40

echo "=== 2. C5: extract + export the A12 synthetic building fixture ===" | tee -a "$LOG"
rm -f "${OUT}/floorplan.dxf" "${OUT}/floorplan.pdf"
run "$APP" --plan-fixture --plan-extract \
    --plan-export-dxf "${OUT}/floorplan.dxf" --plan-export-pdf "${OUT}/floorplan.pdf" \
    --plan-shot "${OUT}/10-plan.png" --plan-delay 2 --quit-after 12

echo "=== 3. DXF: from-scratch reader, independent of A12's own writer/tests ===" | tee -a "$LOG"
run python3 "${HERE}/scripts/check_dxf.py" "${OUT}/floorplan.dxf"

echo "=== 4. PDF: Quick Look thumbnail (A12's own verification style — no CAD/DXF viewer here either) ===" | tee -a "$LOG"
rm -f "${OUT}/floorplan.pdf.png"
run qlmanage -t -s 1000 -o "$OUT" "${OUT}/floorplan.pdf"
run ls -la "${OUT}/floorplan.dxf" "${OUT}/floorplan.pdf" "${OUT}/floorplan.pdf.png"

echo "verification complete — see ${OUT}" | tee -a "$LOG"
