#!/usr/bin/env bash
# fetch_sdk2.sh — bring the pinned, PATCHED Livox-SDK2 into engine/third_party.
#
# WHY THIS EXISTS (S2 spike, spikes/s2-mid360-sim/REPORT.md §3): stock
# Livox-SDK2 does not build with CMake >= 4.0 (cmake_minimum_required 3.0),
# does not compile with modern clang (-Werror on warnings raised inside its
# own vendored third-party headers), and — the hard blocker — does not RUN on
# macOS at all: DeviceManager::CreateDetectionChannel() bind()s the literal
# address 255.255.255.255, which Darwin rejects with EADDRNOTAVAIL, so
# LivoxLidarSdkInit() fails outright before a single packet moves. The three
# diffs in patches/ are the minimal fix for each. They are carried here rather
# than in a vcpkg port precisely because a plain port would give us the broken
# upstream.
#
# The SDK tree itself is gitignored (see .gitignore); this script plus
# patches/ is the committed, reproducible recipe. No git, no sudo, no vcpkg.
#
# Usage:
#   engine/third_party/fetch_sdk2.sh            # fetch + patch (no-op if present)
#   engine/third_party/fetch_sdk2.sh --force    # remove and re-fetch
#   LIVOX_SDK2_TARBALL=/path/to.tar.gz  engine/third_party/fetch_sdk2.sh
#       ... use a local tarball instead of the network (air-gapped CI)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$HERE/Livox-SDK2"

# Pin: master as of 2026-08-15, SDK version 1.4.3 (include/livox_lidar_def.h).
# This is the exact tree S2 soaked for 10 minutes at 200k pts/s. Swap to a
# tag/commit tarball once upstream cuts a release that carries the Darwin fix;
# the patches are written to apply to this pin and `patch` will refuse loudly
# rather than half-apply if the tree moves.
URL="${LIVOX_SDK2_URL:-https://github.com/Livox-SDK/Livox-SDK2/archive/refs/heads/master.tar.gz}"

force=0
for arg in "$@"; do
  case "$arg" in
    --force) force=1 ;;
    -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

# Presence = the real tree, not just a directory: Gradle pre-creates the
# empty output dir for its declared marker file, which made a bare `-d` test
# skip the fetch and fail the build one step later (engine-ci #2).
if [ -f "$DEST/CMakeLists.txt" ]; then
  if [ "$force" -eq 1 ]; then
    echo "Removing existing $DEST (--force)"
    rm -rf "$DEST"
  else
    echo "Livox-SDK2 already present at $DEST -- pass --force to re-fetch."
    exit 0
  fi
elif [ -d "$DEST" ]; then
  echo "Removing incomplete $DEST (no CMakeLists.txt)"
  rm -rf "$DEST"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if [ -n "${LIVOX_SDK2_TARBALL:-}" ]; then
  echo "Using local tarball $LIVOX_SDK2_TARBALL"
  cp "$LIVOX_SDK2_TARBALL" "$tmp/sdk2.tar.gz"
else
  echo "Fetching $URL ..."
  curl -fsSL -o "$tmp/sdk2.tar.gz" "$URL"
fi

tar xzf "$tmp/sdk2.tar.gz" -C "$tmp"
mv "$tmp"/Livox-SDK2-* "$DEST"

echo "Applying patches (S2 spike, REPORT.md section 3):"
for p in "$HERE"/patches/*.patch; do
  echo "  $(basename "$p")"
  patch -p1 -d "$DEST" -s < "$p"
done

echo
echo "Done: $DEST"
echo "Re-run cmake so ENGINE_WITH_LIVOX_SDK2 auto-detects it, e.g."
echo "  cmake --preset macos-universal && cmake --build --preset macos-universal"
