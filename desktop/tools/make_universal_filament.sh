#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# make_universal_filament.sh — fuse the prebuilt arm64 Filament release with the
# from-source x86_64 build into one universal static-lib tree.
#
#   third_party/filament/filament/lib/arm64/lib*.a   (fetch_filament.sh)
# + third_party/filament-x86_64/lib/x86_64/lib*.a    (build_filament_x86_64.sh)
# → third_party/filament-universal/lib/universal/lib*.a
#
# The universal tree also gets `include/` and `bin/` symlinked/copied from the
# prebuilt release, so desktop/CMakeLists.txt can point FILAMENT_DIR at it and
# find headers and the (arm64, host-native) matc in the usual places. matc stays
# arm64 on purpose — it is a BUILD-time tool that runs on the developer's/CI's
# machine, and its architecture has nothing to do with the shipped app's.
#
# Why lipo and not "build both arches with CMAKE_OSX_ARCHITECTURES=arm64;x86_64"
# in one Filament build: Filament's own build runs matc/resgen mid-build, and a
# fat host tool is fine, but the whole reason we are here is that the release
# tarball already contains a *known-good, release-process-produced* arm64 slice
# that S3/C1 measured 138 fps and 1,105 swapchain recreates against. Replacing
# it with our own arm64 rebuild would throw that provenance away for no gain.
# lipo keeps the proven arm64 bytes byte-for-byte and only ADDS Intel.
#
# USAGE
#   ./tools/make_universal_filament.sh
#
# Then build the app against it:
#   cmake -S . -B build-universal -G Ninja \
#     -DLIDARSCAN_UNIVERSAL=ON            # picks up filament-universal + both arches
# ---------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARM_DIR="$HERE/third_party/filament/filament"
X86_DIR="$HERE/third_party/filament-x86_64/lib/x86_64"
OUT="$HERE/third_party/filament-universal/filament"

LIBS=(filament backend bluegl bluevk filabridge filaflat utils geometry smol-v ibl abseil zstd)

log() { printf '\033[1;36m[filament-universal]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[filament-universal] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$ARM_DIR/lib/arm64" ] || die "arm64 prebuilt missing — run tools/fetch_filament.sh v1.75.0"
[ -d "$X86_DIR" ]           || die "x86_64 build missing — run tools/build_filament_x86_64.sh v1.75.0"

rm -rf "$HERE/third_party/filament-universal"
mkdir -p "$OUT/lib/universal"

# Headers + host tools come straight from the prebuilt release.
log "copying include/ and bin/ from the prebuilt release"
cp -R "$ARM_DIR/include" "$OUT/include"
cp -R "$ARM_DIR/bin"     "$OUT/bin"
cp    "$ARM_DIR/LICENSE" "$OUT/LICENSE" 2>/dev/null || true

log "lipo-ing ${#LIBS[@]} archives"
for l in "${LIBS[@]}"; do
  a="$ARM_DIR/lib/arm64/lib${l}.a"
  x="$X86_DIR/lib${l}.a"
  [ -f "$a" ] || die "missing arm64 lib${l}.a"
  [ -f "$x" ] || die "missing x86_64 lib${l}.a"
  lipo -create "$a" "$x" -output "$OUT/lib/universal/lib${l}.a"
done

# Verify: every output must report BOTH architectures. A silent single-arch
# result here is exactly the failure mode that produces a "universal" DMG that
# will not launch on Intel, so it is a hard error, not a warning.
log "verifying"
FAIL=0
for f in "$OUT"/lib/universal/*.a; do
  info="$(lipo -info "$f")"
  case "$info" in
    *"are: x86_64 arm64"*|*"are: arm64 x86_64"*) ;;
    *) echo "NOT UNIVERSAL: $info" >&2; FAIL=1 ;;
  esac
  printf '  %-28s %s\n' "$(basename "$f")" "${info#*: }"
done
[ "$FAIL" -eq 0 ] || die "some archives are not universal (see above)"

{
  echo "Filament v1.75.0 universal (arm64 + x86_64) static libraries"
  echo "created $(date -u '+%Y-%m-%dT%H:%M:%SZ') by tools/make_universal_filament.sh"
  echo
  echo "arm64 slice : prebuilt filament-v1.75.0-mac.tgz (Google's release build)"
  echo "x86_64 slice: built from source — see ../filament-x86_64/BUILDINFO.txt"
  echo
  for f in "$OUT"/lib/universal/*.a; do
    printf '  %-28s %10s bytes  %s\n' "$(basename "$f")" "$(stat -f%z "$f")" "$(lipo -info "$f" | sed 's/.*are: //')"
  done
} > "$HERE/third_party/filament-universal/BUILDINFO.txt"

log "wrote $OUT/lib/universal"
cat "$HERE/third_party/filament-universal/BUILDINFO.txt"
