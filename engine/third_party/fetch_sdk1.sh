#!/usr/bin/env bash
# fetch_sdk1.sh — bring the pinned, PATCHED Livox-SDK **v1** into
# engine/third_party. This is the SDK the Mid-70 speaks (A17); the Mid-360
# uses SDK2 and its own fetch_sdk2.sh. The two trees are independent and can
# both be present at once.
#
# WHY THIS EXISTS (S8 spike, spikes/s8-mid70-sdk1/): stock Livox-SDK v1 does
# not build on a current toolchain for exactly two reasons, both measured
# there rather than guessed:
#
#   1. cmake_minimum_required(VERSION 3.0) in every CMakeLists.txt. CMake >= 4
#      refuses to configure a project with a floor below 3.5
#      (build-policy/cmake.log).  -> sdk1-0001
#   2. sdk_core compiles with -Werror, and its own vendored spdlog/fmt trips
#      -Wdeprecated-literal-operator and -Wdeprecated-declarations on
#      AppleClang 21: 6 errors, no SDK (build-policy/build.log). -> sdk1-0002
#
# and one thing that is not a bug upstream but is wrong for us:
#
#   3. sdk_core names its library `${PROJECT_NAME}_static`, which becomes
#      `scanengine_static` when the engine adds it as a subdirectory.
#      Pinned to `livox_sdk_static`.                                -> sdk1-0003
#
# With those three, sdk_core links for arm64 and for the universal
# "arm64;x86_64" slice (build-fix/, build-uni/). Unlike SDK2 there is no
# Darwin bind() bug to patch: v1 discovery *listens* for the device's
# broadcast on UDP 55000 instead of sending to 255.255.255.255.
#
# The SDK tree itself is gitignored (see .gitignore); this script plus
# patches/sdk1-*.patch is the committed, reproducible recipe. No git, no
# sudo, no vcpkg.
#
# Usage:
#   engine/third_party/fetch_sdk1.sh            # fetch + patch (no-op if present)
#   engine/third_party/fetch_sdk1.sh --force    # remove and re-fetch
#   LIVOX_SDK1_TARBALL=/path/to.tar.gz  engine/third_party/fetch_sdk1.sh
#       ... use a local tarball instead of the network (air-gapped CI)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$HERE/Livox-SDK"

# Pin: master as of 2026-09-07, SDK version 2.3.0 (the LIVOX_SDK_*_VERSION
# triple in sdk_core/CMakeLists.txt — v1's *protocol* is what makes it "SDK
# v1"; its own version number is unrelated to SDK2's 1.4.3). Upstream has cut
# no release since; swap to a tag tarball the moment it does. The patches are
# written to apply to this pin and `patch` will refuse loudly rather than
# half-apply if the tree moves.
URL="${LIVOX_SDK1_URL:-https://github.com/Livox-SDK/Livox-SDK/archive/refs/heads/master.tar.gz}"

force=0
for arg in "$@"; do
  case "$arg" in
    --force) force=1 ;;
    -h|--help) sed -n '2,42p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

# Presence = the real tree, not just a directory (same trap fetch_sdk2.sh
# documents: a build system that pre-creates its declared output directory
# turns a bare `-d` test into a skipped fetch and a failure one step later).
if [ -f "$DEST/CMakeLists.txt" ]; then
  if [ "$force" -eq 1 ]; then
    echo "Removing existing $DEST (--force)"
    rm -rf "$DEST"
  else
    echo "Livox-SDK (v1) already present at $DEST -- pass --force to re-fetch."
    exit 0
  fi
elif [ -d "$DEST" ]; then
  echo "Removing incomplete $DEST (no CMakeLists.txt)"
  rm -rf "$DEST"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if [ -n "${LIVOX_SDK1_TARBALL:-}" ]; then
  echo "Using local tarball $LIVOX_SDK1_TARBALL"
  cp "$LIVOX_SDK1_TARBALL" "$tmp/sdk1.tar.gz"
else
  echo "Fetching $URL ..."
  curl -fsSL -o "$tmp/sdk1.tar.gz" "$URL"
fi

mkdir -p "$DEST"
# --strip-components=1: the branch tarball wraps everything in Livox-SDK-master/.
tar xzf "$tmp/sdk1.tar.gz" -C "$DEST" --strip-components=1

echo "Applying patches (S8 spike, spikes/s8-mid70-sdk1/):"
# sdk1-*.patch only: patches/ is shared with fetch_sdk2.sh, whose diffs apply
# to a different tree.
for p in "$HERE"/patches/sdk1-*.patch; do
  echo "  $(basename "$p")"
  patch -p1 -d "$DEST" -s < "$p"
done

echo
echo "Done: $DEST"
echo "Re-run cmake so ENGINE_WITH_LIVOX_SDK1 auto-detects it, e.g."
echo "  cmake --preset macos-universal && cmake --build --preset macos-universal"
