#!/usr/bin/env bash
# C6 (merge workbench) + C7 (transfer import/export) verification run.
#
#   11-merge-fixture.png
#       the merged, colour-by-session viewport: 3 synthetic overlapping
#       sessions (mirrors engine/tests/test_merge.cpp's own building fixture —
#       see app/MergeFixture.h), georeferenced auto-align, a manual 3-point
#       override on session 1, ICP refine + global relaxation, then Build with
#       "colour by session" on — run-table ranges rendered as distinct RGB
#       tints (--merge-fixture-evidence, --display-profile research so RGB
#       colour mode is actually on screen).
#   12-merge-dock.png
#       the Merge dock itself: session table (georef badges + sigma, align
#       source, kept/dropped), pairs table (rms before/after, both overlaps,
#       iterations, converged/low_overlap/blocker), and the QPainter residual
#       chart for pair 0<->1.
#   transfer-roundtrip.lscan.zip / transfer-roundtrip-imported.lscan
#       A5's zip_export()/zip_import(), headless, round-tripping the C1
#       evidence capture (evidence/synth.lscan, 193 D6 chunks) — chunk/byte
#       counts compared via FileRecordReader summaries on both ends.
#   13-transfer-import-report.png
#       the REAL TransferImportDialog — bundle path, destination, progress
#       bar at 100%, and the manifest sanity report table — driven by
#       --transfer-import-dialog-shot (clicks Import exactly as the button
#       would; not a synthetic screen click, an internal call to the same
#       onImport() the button calls).
#   14-transfer-export-dialog.png
#       the REAL TransferExportDialog after a completed export, same posture.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${HERE}/build"
OUT="${HERE}/evidence"
APP="${BUILD}/lidarscan"

mkdir -p "$OUT"
LOG="${OUT}/verify_c6c7.log"
: > "$LOG"

run() { echo "\$ $*" | tee -a "$LOG"; "$@" 2>&1 | tee -a "$LOG"; }

echo "=== 1. C6: merge workbench end to end (georef auto -> 3-point manual -> ICP -> build) ===" | tee -a "$LOG"
run "$APP" --display-profile research --merge-fixture-evidence \
    --shot "${OUT}/11-merge-fixture.png" --shot-delay 3 \
    --merge-dock-shot "${OUT}/12-merge-dock.png" --merge-dock-shot-delay 3.5 \
    --quit-after 6

echo "=== 1b. C6: MergeSessionLoader against a real .lscan (the 'add from project' path) ===" | tee -a "$LOG"
run "$APP" --merge-add-project "${OUT}/synth.lscan:real-project-test" --quit-after 3

echo "=== 2. C7: zip_export()/zip_import() round trip, headless ===" | tee -a "$LOG"
rm -rf "${OUT}/transfer-roundtrip.lscan.zip" "${OUT}/transfer-roundtrip-imported.lscan"
run "$APP" --transfer-export "${OUT}/synth.lscan:${OUT}/transfer-roundtrip.lscan.zip" \
    --transfer-import "${OUT}/transfer-roundtrip.lscan.zip:${OUT}/transfer-roundtrip-imported.lscan" \
    --quit-after 1

echo "=== 3. C7: the real TransferImportDialog, including the manifest sanity report ===" | tee -a "$LOG"
rm -rf "${OUT}/transfer-dialog-imported.lscan"
run "$APP" --transfer-import-dialog-shot \
    "${OUT}/transfer-roundtrip.lscan.zip:${OUT}/transfer-dialog-imported.lscan:${OUT}/13-transfer-import-report.png" \
    --quit-after 2.5

echo "=== 4. C7: the real TransferExportDialog ===" | tee -a "$LOG"
rm -f "${OUT}/synth.lscan/exports/synth.lscan.zip"
run "$APP" --transfer-export-dialog-shot \
    "${OUT}/synth.lscan:${OUT}/synth.lscan/exports/synth.lscan.zip:${OUT}/14-transfer-export-dialog.png" \
    --quit-after 2.5
# Leave the C1 evidence fixture (evidence/synth.lscan) exactly as this task
# found it — the export above is a real, deliberate write into its own
# exports/ directory (the standard .lscan skeleton has one), removed here so
# a repeat run of this script diffs clean.
rm -f "${OUT}/synth.lscan/exports/synth.lscan.zip"

echo "verification complete — see ${OUT}" | tee -a "$LOG"
