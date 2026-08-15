#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build_appimage.sh — build LidarScan on Linux x86_64 and produce an AppImage.
#
# Tech Spec §3.13 / C8: "AppImage + .deb with udev rule (Linux)".
# The .deb is build_deb.sh; this is the AppImage half.
#
# !! NEVER EXECUTED !!
#   This repo's only host is macOS (desktop/NOTES.md §6). Staged for the first
#   Linux CI run — packaging/ci/linux-packages.yml.snippet is the job that will
#   run it. Nothing below has been observed working.
#
#   And note the bigger unknown behind it: NOTES.md §3.1 marks the Linux
#   renderer UNVERIFIED. NativeSurface_linux.cpp (X11 Window → Vulkan
#   swapchain) has never been compiled by anyone, **Wayland is refused with an
#   explanatory error** because it needs a wl_surface plus an explicit size,
#   and gl_PointSize on Vulkan may clamp to 1.0 on Intel/AMD. An AppImage that
#   builds is not an app that renders. On a Wayland session (the default on
#   Ubuntu 22.04+ and Fedora) the app must be started with
#   `QT_QPA_PLATFORM=xcb` to get XWayland — the .desktop file below does that
#   for exactly this reason.
#
# WHY linuxdeploy AND NOT linuxdeployqt
#   linuxdeployqt hard-refuses to run on anything newer than the oldest still-
#   supported Ubuntu LTS (it exits with "package on the oldest still supported
#   Ubuntu LTS"), which makes it unusable on a modern CI image. linuxdeploy plus
#   its Qt plugin does the same job with no such gate. Both produce an AppImage
#   whose glibc floor is whatever the BUILD machine has, so the CI job builds on
#   the oldest reasonable image (ubuntu-22.04) for the widest compatibility.
#
# USAGE
#   ./packaging/linux/build_appimage.sh [VERSION]
# ---------------------------------------------------------------------------
set -euo pipefail

VERSION="${1:-0.1.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESKTOP="$(cd "$HERE/../.." && pwd)"
BUILD="$DESKTOP/build-linux"
APPDIR="$HERE/AppDir"
TOOLS="${LINUXDEPLOY_DIR:-$HERE/.tools}"

log() { printf '\033[1;36m[appimage]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[appimage] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "Linux only (host is $(uname -s))"

# --- build ----------------------------------------------------------------
if [ "${LIDARSCAN_SKIP_BUILD:-0}" != "1" ]; then
  log "configure + build"
  cmake -S "$DESKTOP" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" --parallel "$(nproc)"
fi
[ -x "$BUILD/lidarscan" ] || die "$BUILD/lidarscan not produced"

# --- AppDir ---------------------------------------------------------------
log "staging AppDir"
rm -rf "$APPDIR"
install -Dm755 "$BUILD/lidarscan"        "$APPDIR/usr/bin/lidarscan"
# ViewportWindow.cpp loads points.filamat from applicationDirPath(), which for
# an AppImage is usr/bin inside the mounted squashfs — so it goes next to the
# binary, not into usr/share.
install -Dm644 "$BUILD/points.filamat"   "$APPDIR/usr/bin/points.filamat"
install -Dm644 "$HERE/lidarscan.desktop" "$APPDIR/usr/share/applications/lidarscan.desktop"
install -Dm644 "$HERE/lidarscan.png"     "$APPDIR/usr/share/icons/hicolor/256x256/apps/lidarscan.png"
install -Dm644 "$HERE/lidarscan.xml"     "$APPDIR/usr/share/mime/packages/lidarscan.xml"
# linuxdeploy also wants them at the AppDir root.
cp "$HERE/lidarscan.desktop" "$APPDIR/lidarscan.desktop"
cp "$HERE/lidarscan.png"     "$APPDIR/lidarscan.png"

# The udev rule is INFORMATIONAL inside an AppImage: an AppImage cannot install
# anything into /etc, so the rule is carried along and the user is told how to
# install it. The .deb (build_deb.sh) installs it properly. Without it, opening
# /dev/ttyUSB* requires the user to be in the `dialout` group.
install -Dm644 "$HERE/99-lidarscan.rules" \
               "$APPDIR/usr/share/lidarscan/99-lidarscan.rules"
install -Dm644 "$HERE/README-udev.txt" "$APPDIR/usr/share/lidarscan/README-udev.txt"

# --- tools ----------------------------------------------------------------
mkdir -p "$TOOLS"
fetch() {  # url -> $TOOLS/name, made executable
  local url="$1"
  local out
  out="$TOOLS/$(basename "$url")"
  [ -x "$out" ] || { log "fetching $(basename "$1")"; curl -fL -o "$out" "$url"; chmod +x "$out"; }
  echo "$out"
}
BASE="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
QBASE="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous"
LD="$(fetch "$BASE/linuxdeploy-x86_64.AppImage")"
fetch "$QBASE/linuxdeploy-plugin-qt-x86_64.AppImage" >/dev/null
export PATH="$TOOLS:$PATH"

# --- build the AppImage ---------------------------------------------------
log "linuxdeploy --plugin qt"
export VERSION
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
# EXTRA_QT_PLUGINS: the qt plugin's auto-detection finds the platform and
# imageformat plugins from the binary's own dependencies, but the SerialPort
# module has no plugin and the wayland ones are pulled in only if named.
# platformthemes keeps native file dialogs looking right on GNOME/KDE.
export EXTRA_QT_PLUGINS="platformthemes;iconengines"
"$LD" --appdir "$APPDIR" --plugin qt --output appimage \
      -d "$APPDIR/lidarscan.desktop" -i "$APPDIR/lidarscan.png"

OUT="$(ls -t "$HERE"/LidarScan*.AppImage "$PWD"/LidarScan*.AppImage 2>/dev/null | head -1 || true)"
[ -n "$OUT" ] || die "no AppImage produced"
mv -f "$OUT" "$HERE/LidarScan-${VERSION}-x86_64.AppImage"
chmod +x "$HERE/LidarScan-${VERSION}-x86_64.AppImage"
log "AppImage: $HERE/LidarScan-${VERSION}-x86_64.AppImage"

# --- smoke test -----------------------------------------------------------
# --help needs no GPU, no display and no device, so it is the one check that
# can run in a headless CI container and still prove the binary, the bundled Qt
# and the dynamic loader all agree.
log "smoke test: --help"
"$HERE/LidarScan-${VERSION}-x86_64.AppImage" --help >/dev/null && log "OK" \
  || die "the AppImage cannot even print its own help"
