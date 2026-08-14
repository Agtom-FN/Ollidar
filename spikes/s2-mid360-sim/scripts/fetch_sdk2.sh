#!/usr/bin/env bash
# Fetch the pinned Livox-SDK2 source and apply the three portability patches.
# No git required (and none used) -- the tarball is fetched over HTTPS.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$HERE/third_party/Livox-SDK2"

# Pin: master as of 2026-08-15, SDK version 1.4.3 (include/livox_lidar_def.h).
# Swap to a tag/commit tarball URL once upstream cuts a release for Mid-360s.
URL="https://github.com/Livox-SDK/Livox-SDK2/archive/refs/heads/master.tar.gz"

if [ -d "$DEST" ]; then
  echo "Livox-SDK2 already present at $DEST -- remove it to re-fetch."
  exit 0
fi

mkdir -p "$HERE/third_party"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "Fetching $URL ..."
curl -sSL -o "$tmp/sdk2.tar.gz" "$URL"
tar xzf "$tmp/sdk2.tar.gz" -C "$tmp"
mv "$tmp"/Livox-SDK2-* "$DEST"

echo "Applying patches..."
for p in "$HERE"/patches/*.patch; do
  echo "  $(basename "$p")"
  patch -p1 -d "$DEST" -s < "$p"
done
echo "Done. Now: cmake -S $HERE -B $HERE/build/spike && cmake --build $HERE/build/spike -j"
