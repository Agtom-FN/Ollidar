#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# package_macos.sh — build LidarScan.app as a TRUE universal (arm64 + x86_64)
# bundle and wrap it in a DMG.
#
# Tech Spec §3.13 / §Workstream C row C8: "notarized universal DMG (Intel +
# Apple Silicon)". This script produces everything except the notarization,
# which needs an Apple Developer ID that this environment does not have — the
# exact commands, entitlements and CI wiring for that step are in
# packaging/PACKAGING.md, and the DMG this script produces is ad-hoc signed
# (`codesign -s -`) so it is honestly labelled as unsigned rather than
# pretending otherwise.
#
# PREREQUISITES (each fails loudly with the command to run)
#   ./tools/fetch_filament.sh v1.75.0          arm64 Filament (prebuilt)
#   ./tools/build_filament_x86_64.sh v1.75.0   x86_64 Filament (from source)
#   ./tools/make_universal_filament.sh         lipo → filament-universal/
#   ./tools/fetch_qt_universal.sh              universal Qt via aqtinstall
#   ./tools/make_icon.sh                       lidarscan.icns
#
# USAGE
#   ./tools/package_macos.sh                   # full build + DMG
#   LIDARSCAN_SKIP_BUILD=1 ./tools/package_macos.sh   # re-package an existing build
#   CODESIGN_IDENTITY="Developer ID Application: ACME (TEAMID)" ./tools/package_macos.sh
#
# OUTPUT
#   build-universal/LidarScan.app
#   dist/LidarScan-<version>-universal.dmg
# ---------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$HERE/build-universal"
DIST="$HERE/dist"
QT_ROOT="$HERE/third_party/qt-universal/6.11.1/macos"
APP_NAME="LidarScan"
IDENTITY="${CODESIGN_IDENTITY:--}"      # `-` == ad-hoc

log()  { printf '\033[1;36m[package]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[package] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1;33m=== %s ===\033[0m\n' "$*"; }

[ "$(uname -s)" = "Darwin" ] || die "macOS only"

# --- preflight ------------------------------------------------------------
step "preflight"
[ -f "$HERE/third_party/filament-universal/filament/lib/universal/libfilament.a" ] \
  || die "universal Filament missing. Run:
     ./tools/fetch_filament.sh v1.75.0
     ./tools/build_filament_x86_64.sh v1.75.0
     ./tools/make_universal_filament.sh"
[ -x "$QT_ROOT/bin/macdeployqt" ] \
  || die "universal Qt missing at $QT_ROOT. Run: ./tools/fetch_qt_universal.sh"

# Qt from Homebrew is arm64-ONLY on Apple Silicon — linking against it makes a
# universal app impossible (the x86_64 slice would have no Qt to link). Assert
# the Qt we are about to use really is fat before spending a build on it.
qtinfo="$(lipo -info "$QT_ROOT/lib/QtCore.framework/QtCore")"
case "$qtinfo" in
  *x86_64*arm64*|*arm64*x86_64*) log "Qt is universal: $qtinfo" ;;
  *) die "Qt at $QT_ROOT is NOT universal: $qtinfo" ;;
esac
filinfo="$(lipo -info "$HERE/third_party/filament-universal/filament/lib/universal/libfilament.a")"
case "$filinfo" in
  *x86_64*arm64*|*arm64*x86_64*) log "Filament is universal: ${filinfo#*: }" ;;
  *) die "Filament is NOT universal: $filinfo" ;;
esac
[ -f "$HERE/packaging/macos/lidarscan.icns" ] || { log "no icon yet — generating"; "$HERE/tools/make_icon.sh"; }

# --- build ----------------------------------------------------------------
if [ "${LIDARSCAN_SKIP_BUILD:-0}" != "1" ]; then
  step "configure + build (arm64 + x86_64)"
  # CMAKE_OSX_ARCHITECTURES is what makes this universal, and desktop/CMakeLists
  # keys off it to demand the universal Filament tree (rather than silently
  # falling back to the arm64-only prebuilt release, which would link with
  # warnings and produce an Intel slice missing every Filament symbol).
  #
  # CMAKE_OSX_DEPLOYMENT_TARGET 12.0 matches engine/CMakePresets.json's
  # macos-universal preset. Info.plist's LSMinimumSystemVersion says 13.0, which
  # is the stricter of the two and therefore the one that governs.
  cmake -S "$HERE" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DLIDARSCAN_MACOS_BUNDLE=ON
  cmake --build "$BUILD" --parallel "$(sysctl -n hw.ncpu)"
fi

APP="$BUILD/$APP_NAME.app"
[ -d "$APP" ] || die "$APP was not produced"
BIN="$APP/Contents/MacOS/$APP_NAME"

step "architectures of the freshly linked binary"
lipo -info "$BIN"
lipo -info "$BIN" | grep -q "x86_64" || die "no x86_64 slice"
lipo -info "$BIN" | grep -q "arm64"  || die "no arm64 slice"

# --- deploy Qt ------------------------------------------------------------
step "macdeployqt"
# -always-overwrite so repeated packaging runs do not keep a stale framework.
# NOT -codesign: macdeployqt's signing pass runs before we have finished putting
# things in the bundle, so signing is done explicitly (inside-out) below.
"$QT_ROOT/bin/macdeployqt" "$APP" -always-overwrite -verbose=1 2>&1 | tail -20

log "Qt frameworks now in the bundle:"
ls "$APP/Contents/Frameworks" 2>/dev/null | sed 's/^/    /'

step "architectures of the deployed Qt frameworks"
DEPLOY_FAIL=0
for fw in "$APP/Contents/Frameworks"/*.framework; do
  [ -d "$fw" ] || continue
  name="$(basename "$fw" .framework)"
  bin="$fw/Versions/A/$name"
  [ -f "$bin" ] || bin="$fw/$name"
  [ -f "$bin" ] || continue
  info="$(lipo -info "$bin")"
  printf '    %-24s %s\n' "$name" "${info#*: }"
  case "$info" in *x86_64*arm64*|*arm64*x86_64*) ;; *) DEPLOY_FAIL=1 ;; esac
done
[ "$DEPLOY_FAIL" -eq 0 ] || die "a deployed Qt framework is not universal"

# --- sign -----------------------------------------------------------------
step "codesign (identity: $IDENTITY)"
# Inside-out signing: every nested Mach-O first, the bundle last. Required for
# any signature to validate, ad-hoc or Developer ID alike.
find "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" -type f \
     \( -name "*.dylib" -o -perm +111 \) 2>/dev/null | while read -r f; do
  file "$f" | grep -q "Mach-O" && codesign --force --timestamp=none -s "$IDENTITY" "$f" 2>/dev/null || true
done
for fw in "$APP/Contents/Frameworks"/*.framework; do
  [ -d "$fw" ] && codesign --force --timestamp=none -s "$IDENTITY" "$fw" || true
done
if [ "$IDENTITY" = "-" ]; then
  codesign --force --deep -s - "$APP"
else
  # A Developer ID signature additionally needs the hardened runtime and this
  # app's entitlements (see packaging/PACKAGING.md for why each is needed).
  codesign --force --options runtime --timestamp \
           --entitlements "$HERE/packaging/macos/entitlements.plist" \
           -s "$IDENTITY" "$APP"
fi
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | sed 's/^/    /'

# --- DMG ------------------------------------------------------------------
step "DMG"
VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist")"
mkdir -p "$DIST"
DMG="$DIST/LidarScan-${VERSION}-universal.dmg"
rm -f "$DMG"

if command -v create-dmg >/dev/null; then
  # create-dmg gives the nicer window (background, icon positions). Optional:
  # hdiutil below is the dependency-free path and is what CI uses.
  create-dmg --volname "LidarScan $VERSION" \
    --icon "$APP_NAME.app" 160 190 --app-drop-link 480 190 \
    --window-size 640 400 --icon-size 110 \
    "$DMG" "$APP" >/dev/null
else
  STAGE="$(mktemp -d)"
  cp -R "$APP" "$STAGE/"
  ln -s /Applications "$STAGE/Applications"     # the drag-to-install target
  # UDZO = zlib-compressed read-only, the normal shipping format.
  hdiutil create -volname "LidarScan $VERSION" -srcfolder "$STAGE" \
                 -ov -format UDZO "$DMG" | sed 's/^/    /'
  rm -rf "$STAGE"
fi
[ "$IDENTITY" = "-" ] || codesign --force --timestamp -s "$IDENTITY" "$DMG"

log "DMG: $DMG ($(du -h "$DMG" | cut -f1))"

# --- verify ---------------------------------------------------------------
step "verification"
echo "app binary:"
lipo -info "$BIN" | sed 's/^/    /'
echo
echo "codesign:"
codesign -dv "$APP" 2>&1 | sed 's/^/    /'
echo
echo "spctl (Gatekeeper assessment):"
# EXPECTED TO FAIL for an ad-hoc signature. That is the honest state of an
# un-notarized build and is recorded rather than hidden: on another machine
# this app would be quarantined and refused until notarized (or until the user
# right-click-Opens it). PACKAGING.md §"Real signing and notarization" has the
# steps that make this line say "accepted".
spctl -a -vvv -t exec "$APP" 2>&1 | sed 's/^/    /' || true
echo
echo "DMG contents:"
hdiutil imageinfo "$DMG" | grep -E "^Format|Checksum Type" | sed 's/^/    /'

log "done"
