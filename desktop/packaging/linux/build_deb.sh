#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build_deb.sh — build the Debian/Ubuntu package for LidarScan x86_64.
#
# Tech Spec §3.13 / C8: "AppImage + .deb with udev rule (Linux)".
# The AppImage is build_appimage.sh; this is the .deb half.
#
# !! NEVER EXECUTED !! — see build_appimage.sh's header. macOS-only host.
#
# HOW THIS DIFFERS FROM THE APPIMAGE, and why both exist
#   * The .deb DEPENDS on the distro's Qt 6 packages instead of bundling them.
#     That is the whole point of a .deb: security updates to Qt reach the user
#     through apt. It also keeps the package at a few MB instead of ~50.
#   * The .deb can install /etc/udev/rules.d/99-lidarscan.rules and run
#     update-mime-database — the two things an AppImage structurally cannot do.
#     So D6 capture works out of the box and .lscan.zip file association works
#     ONLY in the .deb.
#   * The AppImage runs anywhere with no root and no apt; the .deb is pinned to
#     the Qt version the target release ships. Ubuntu 24.04 has Qt 6.4.2,
#     Debian 13 has 6.8 — both older than the 6.11.1 this app is developed
#     against. The Depends line below therefore says >= 6.4, and THIS IS AN
#     UNVERIFIED ASSUMPTION: nobody has compiled the app against Qt 6.4. If it
#     needs a 6.5+ API, the fix is to raise the floor here and drop the older
#     targets, not to bundle Qt into the .deb.
#
# USAGE
#   ./packaging/linux/build_deb.sh [VERSION]
# ---------------------------------------------------------------------------
set -euo pipefail

# Owner rule (2026-08-17): the version comes from the repo-root VERSION file.
# An explicit argument still wins (a one-off build of an older tag), but the
# DEFAULT is never a frozen literal that silently ships last year's number.
_VERSION_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/VERSION"
VERSION="${1:-$( [ -f "$_VERSION_FILE" ] && tr -d "[:space:]" < "$_VERSION_FILE" || echo 0.0.0 )}"
ARCH="amd64"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESKTOP="$(cd "$HERE/../.." && pwd)"
BUILD="$DESKTOP/build-linux"
PKG="$HERE/deb/lidarscan_${VERSION}_${ARCH}"

log() { printf '\033[1;36m[deb]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[deb] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "Linux only (host is $(uname -s))"

if [ "${LIDARSCAN_SKIP_BUILD:-0}" != "1" ]; then
  cmake -S "$DESKTOP" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" --parallel "$(nproc)"
fi
[ -x "$BUILD/lidarscan" ] || die "$BUILD/lidarscan not produced"

log "staging $PKG"
rm -rf "$PKG"
install -Dm755 "$BUILD/lidarscan"      "$PKG/usr/bin/lidarscan"
# ViewportWindow.cpp resolves points.filamat from applicationDirPath(), which
# for an FHS install is /usr/bin — so it goes there, NOT into /usr/share.
# (Moving it to /usr/share/lidarscan/ would be more FHS-correct and needs a
# one-line search-path addition in ViewportWindow.cpp; deliberately not done
# here because that file is shared with the macOS/Windows layouts.)
install -Dm644 "$BUILD/points.filamat" "$PKG/usr/bin/points.filamat"
install -Dm644 "$HERE/lidarscan.desktop" "$PKG/usr/share/applications/lidarscan.desktop"
install -Dm644 "$HERE/lidarscan.png"     "$PKG/usr/share/icons/hicolor/256x256/apps/lidarscan.png"
install -Dm644 "$HERE/lidarscan.xml"     "$PKG/usr/share/mime/packages/lidarscan.xml"
install -Dm644 "$HERE/99-lidarscan.rules" "$PKG/lib/udev/rules.d/99-lidarscan.rules"
install -Dm644 "$HERE/README-udev.txt"   "$PKG/usr/share/doc/lidarscan/README-udev.txt"

# The .desktop's Exec line has no absolute path, so /usr/bin/lidarscan on PATH
# is what it finds — correct for an FHS install.
sed -i 's|^Exec=env QT_QPA_PLATFORM=xcb lidarscan|Exec=env QT_QPA_PLATFORM=xcb /usr/bin/lidarscan|' \
    "$PKG/usr/share/applications/lidarscan.desktop"

INSTALLED_KB="$(du -ks "$PKG" | cut -f1)"

mkdir -p "$PKG/DEBIAN"
sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/" -e "s/@INSTALLED_SIZE@/$INSTALLED_KB/" \
    "$HERE/debian-control.in" > "$PKG/DEBIAN/control"
install -m755 "$HERE/debian-postinst" "$PKG/DEBIAN/postinst"
install -m755 "$HERE/debian-postrm"   "$PKG/DEBIAN/postrm"

log "dpkg-deb"
dpkg-deb --build --root-owner-group "$PKG" "$HERE/lidarscan_${VERSION}_${ARCH}.deb"

if command -v lintian >/dev/null; then
  log "lintian (informational — a failure here is not fatal)"
  lintian "$HERE/lidarscan_${VERSION}_${ARCH}.deb" || true
fi

log "package: $HERE/lidarscan_${VERSION}_${ARCH}.deb"
dpkg-deb --info "$HERE/lidarscan_${VERSION}_${ARCH}.deb"
dpkg-deb --contents "$HERE/lidarscan_${VERSION}_${ARCH}.deb"
