#!/usr/bin/env bash
# prepare_livox_sdk2.sh — make the vendored, patched Livox-SDK2 tree present
# and Android-ready before AGP runs CMake. Invoked by the `prepareLivoxSdk2`
# Gradle task in android/app/build.gradle.kts (B3).
#
# WHY A SCRIPT AND NOT JUST CMake:
#   engine/CMakeLists.txt's ENGINE_WITH_LIVOX_SDK2 is AUTO/ON/OFF over a tree
#   that must ALREADY EXIST at configure time — `ON` with the tree missing is
#   a hard FATAL_ERROR, by design (engine/CMakeLists.txt line ~97). CMake is
#   therefore the wrong place to fetch it: by the time the configure step runs
#   it is too late. Gradle runs this first instead.
#
# WHAT IT DOES, in order:
#   1. Runs engine/third_party/fetch_sdk2.sh — the ENGINE's own committed,
#      reproducible recipe (pinned tarball + its 3 patches). This task does
#      NOT reimplement it and does not modify it: engine/ is read-only for
#      B3. If the tree is already there the script is a no-op and exits 0.
#   2. Applies android/third_party/patches-android/*.patch — this task's OWN
#      overlay, on top of the engine's patches, for anything bionic/Android
#      needs that the engine's macOS/Linux-oriented patches do not cover.
#      Idempotent: each patch is skipped when it already applies in reverse
#      (i.e. is already in the tree), so a re-run never half-applies.
#
# THE OVERLAY'S ONE HARD RULE (see patches-android/README.md): every overlay
# patch MUST be guarded by `#ifdef __ANDROID__` (or an equivalent CMake
# `if(ANDROID)`), because it is applied IN PLACE to the shared tree at
# engine/third_party/Livox-SDK2 that the engine's own macOS/Linux/CI builds
# also configure against. The tree is gitignored and script-generated, so
# mutating it is not "editing engine/", but it IS shared, and an unguarded
# change would silently alter a build this task does not own.
#
# As of B3 the overlay is EMPTY — see patches-android/README.md for the
# verification that stock-plus-engine-patches SDK2 compiles clean under NDK
# r27d/bionic with no further changes.
#
# Usage:
#   android/scripts/prepare_livox_sdk2.sh            # fetch (if needed) + overlay
#   android/scripts/prepare_livox_sdk2.sh --force    # re-fetch from scratch, then overlay
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_DIR="$(cd "$HERE/.." && pwd)"
REPO_ROOT="$(cd "$ANDROID_DIR/.." && pwd)"
ENGINE_TP="$REPO_ROOT/engine/third_party"
SDK_DIR="$ENGINE_TP/Livox-SDK2"
OVERLAY_DIR="$ANDROID_DIR/third_party/patches-android"

force=""
for arg in "$@"; do
  case "$arg" in
    --force) force="--force" ;;
    -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

if [ ! -x "$ENGINE_TP/fetch_sdk2.sh" ]; then
  echo "prepare_livox_sdk2: expected $ENGINE_TP/fetch_sdk2.sh (repo layout changed?)" >&2
  exit 1
fi

echo "prepare_livox_sdk2: engine fetch+patch ..."
"$ENGINE_TP/fetch_sdk2.sh" $force

if [ ! -f "$SDK_DIR/CMakeLists.txt" ]; then
  echo "prepare_livox_sdk2: $SDK_DIR is still missing after fetch_sdk2.sh" >&2
  exit 1
fi

shopt -s nullglob
overlays=("$OVERLAY_DIR"/*.patch)
shopt -u nullglob

if [ ${#overlays[@]} -eq 0 ]; then
  echo "prepare_livox_sdk2: no Android overlay patches (see $OVERLAY_DIR/README.md)"
else
  echo "prepare_livox_sdk2: applying ${#overlays[@]} Android overlay patch(es):"
  for p in "${overlays[@]}"; do
    name="$(basename "$p")"
    # Already applied? `patch -R --dry-run` succeeds only when the tree
    # already contains the change, which is exactly the idempotency test.
    if patch -p1 -R --dry-run -s -f -d "$SDK_DIR" < "$p" >/dev/null 2>&1; then
      echo "  $name (already applied, skipped)"
      continue
    fi
    echo "  $name"
    patch -p1 -s -d "$SDK_DIR" < "$p"
  done
fi

echo "prepare_livox_sdk2: ready at $SDK_DIR"
