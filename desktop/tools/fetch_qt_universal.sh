#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# fetch_qt_universal.sh — put a UNIVERSAL (arm64 + x86_64) Qt 6 into
# third_party/qt-universal/ so the macOS app can be built for both
# architectures.
#
# WHY THIS EXISTS
#   `brew install qt` — what C1/S3 used and what NOTES.md §5 documents — is
#   **arm64-only** on Apple Silicon:
#       $ lipo -info /opt/homebrew/opt/qt/lib/QtCore.framework/QtCore
#       Non-fat file: ... is architecture: arm64
#   Homebrew builds bottles per-architecture on purpose; there is no universal
#   Qt bottle and there will not be one. So a universal LidarScan.app cannot be
#   linked against Homebrew Qt at all — its x86_64 slice would have nothing to
#   link Qt against. This is the Qt-side twin of the Filament arm64-only
#   blocker NOTES.md §3.2 recorded.
#
#   The Qt Company's OWN macOS desktop binaries, by contrast, have been
#   universal since Qt 6.2 — one `clang_64` bundle containing both slices (the
#   archive file names say so out loud:
#   `qtbase-MacOS-MacOS_15-Clang-MacOS-MacOS_15-X86_64-ARM64.7z`).
#
# OPTIONS CONSIDERED
#   (a) Qt online installer — universal, official, but it is an interactive GUI
#       that requires a Qt account login. Unusable headless and unusable in CI.
#   (b) aqtinstall — a small Python tool that downloads the SAME official
#       artifacts from the same Qt CDN/mirrors, non-interactively, no account.
#       This is what the script uses and what the CI snippets use.
#   (c) build Qt from source universal — hours, and pointless when (b) fetches
#       the vendor's own binaries.
#
# LICENSING NOTE: this keeps Qt DYNAMICALLY linked (frameworks in
# Contents/Frameworks), which is what Tech Spec §1's "LGPLv3 with dynamic
# linking (no commercial license needed)" requires. Do not switch to a static
# Qt without a commercial licence.
#
# USAGE
#   ./tools/fetch_qt_universal.sh [QT_VERSION]      # default 6.11.1
# ---------------------------------------------------------------------------
set -euo pipefail

QT_VERSION="${1:-6.11.1}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$HERE/third_party/qt-universal"
VENV="$HERE/third_party/.aqt-venv"

log() { printf '\033[1;36m[qt]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[qt] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "macOS only"

if [ -f "$DEST/$QT_VERSION/macos/lib/QtCore.framework/QtCore" ]; then
  log "already present at $DEST/$QT_VERSION/macos"
else
  if [ ! -x "$VENV/bin/aqt" ]; then
    log "creating aqtinstall venv at $VENV"
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q --upgrade pip aqtinstall
  fi
  # 6.11.1 is deliberate: it is the exact Qt version S3 and C1-C7 built and
  # measured against (NOTES.md §6 "Qt 6.11.1"), so switching to the official
  # binaries changes the ARCHITECTURE and nothing else.
  #
  # Modules: qtserialport is a separate module (the D6 capture path needs it);
  # Core/Gui/Widgets/Network are all in qtbase and come by default.
  log "downloading official Qt $QT_VERSION macOS desktop (clang_64, universal)"
  "$VENV/bin/aqt" install-qt mac desktop "$QT_VERSION" clang_64 -m qtserialport -O "$DEST"
fi

QT_ROOT="$DEST/$QT_VERSION/macos"
log "verifying the download really is fat"
FAIL=0
for fw in QtCore QtGui QtWidgets QtNetwork QtSerialPort; do
  f="$QT_ROOT/lib/$fw.framework/$fw"
  [ -f "$f" ] || { echo "  MISSING $fw" >&2; FAIL=1; continue; }
  info="$(lipo -info "$f")"
  printf '    %-14s %s\n' "$fw" "${info#*: }"
  case "$info" in *x86_64*arm64*|*arm64*x86_64*) ;; *) FAIL=1 ;; esac
done
info="$(lipo -info "$QT_ROOT/bin/macdeployqt")"
printf '    %-14s %s\n' "macdeployqt" "${info#*: }"
[ "$FAIL" -eq 0 ] || die "Qt at $QT_ROOT is not universal"

log "Qt root: $QT_ROOT"
log "configure the app with: -DCMAKE_PREFIX_PATH=$QT_ROOT"
