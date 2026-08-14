#!/usr/bin/env bash
# Fetch the prebuilt Filament release used by spike S3.
# The macOS release tarball is a universal (arm64 + x86_64) static-lib bundle and
# ships matc/matinfo, so nothing has to be built from source.
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
