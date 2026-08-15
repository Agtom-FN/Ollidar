#!/usr/bin/env bash
# Fetch the prebuilt Filament release used by spike S3.
#
# CORRECTION (C8, 2026-08-15): this header used to claim the macOS tarball was
# "a universal (arm64 + x86_64) static-lib bundle". It is NOT. filament-v1.75.0-mac.tgz
# ships **lib/arm64 only** — there is no lib/x86_64 anywhere in it. C1 found this
# and recorded it as a C8 blocker (NOTES.md §3.2); C8 resolved it by building the
# x86_64 slice from source. For an arm64-only dev build this script is still all
# you need. For the universal DMG, also run:
#     ./tools/build_filament_x86_64.sh v1.75.0
#     ./tools/make_universal_filament.sh
# What IS true: the tarball ships matc/matinfo as native arm64 host tools, and
# those compile materials/points.mat for either target architecture.
set -euo pipefail

VERSION="${1:-v1.75.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$HERE/third_party/filament"

case "$(uname -s)" in
  Darwin) ASSET="filament-${VERSION}-mac.tgz" ;;
  Linux)  ASSET="filament-${VERSION}-linux.tgz" ;;
  *)      echo "unsupported host: $(uname -s)"; exit 1 ;;
esac

URL="https://github.com/google/filament/releases/download/${VERSION}/${ASSET}"
mkdir -p "$DEST"
echo "fetching $URL"
curl -fL --progress-bar -o "/tmp/${ASSET}" "$URL"
tar xzf "/tmp/${ASSET}" -C "$DEST"
rm -f "/tmp/${ASSET}"
echo "unpacked into $DEST/filament"
"$DEST/filament/bin/matc" --version >/dev/null && echo "matc OK"
