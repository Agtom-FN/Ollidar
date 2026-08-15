#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build_filament_x86_64.sh — build the x86_64 slice of Filament from source on
# an Apple-Silicon Mac, so that C8 can ship a TRUE universal macOS DMG.
#
# WHY THIS EXISTS
#   NOTES.md §3.2 recorded the blocker: `filament-v1.75.0-mac.tgz` (what
#   tools/fetch_filament.sh downloads) ships **lib/arm64 only**. There is no
#   lib/x86_64 in the macOS release tarball for v1.75.0, even though Filament's
#   own release process is capable of producing one. Tech Spec §3.13 wants a
#   universal (Intel + Apple Silicon) DMG and the engine already builds
#   universal via its `macos-universal` preset, so the ONLY missing slice in the
#   whole app is Filament's x86_64 static libs. This script produces them.
#
#   Owner decision (recorded 2026-08-15): build v1.75.0 x86_64 from source and
#   lipo it against the prebuilt arm64 set. Not: ship two DMGs, not: drop Intel.
#
# HOW IT CROSS-COMPILES
#   Filament's CMake honours CMAKE_OSX_ARCHITECTURES, so `-DCMAKE_OSX_ARCHITECTURES=x86_64`
#   is enough to get x86_64 object code out of an arm64 host. The catch is that
#   the Filament build RUNS tools it just built as part of the build:
#
#     * `matc`      compiles filament's ~40 built-in .mat files (default material,
#                   skybox, blit/post-process, DoF, bloom, …) into .filamat blobs
#     * `resgen`    turns those blobs into .S/.apple.S assembly + headers that get
#                   compiled straight into libfilament.a
#
#   Under `-DCMAKE_OSX_ARCHITECTURES=x86_64` those tools are ALSO built x86_64,
#   i.e. the build wants to execute x86_64 binaries on an arm64 host. Two ways
#   out, and this script takes the first:
#
#     (a) Rosetta 2 — if `arch -x86_64 /usr/bin/uname -m` prints x86_64, the OS
#         transparently translates the just-built x86_64 matc/resgen and the
#         build simply works. This is what this machine has and what was used.
#     (b) `-DIMPORT_EXECUTABLES_DIR=<arm64 build dir>` — Filament's own
#         cross-compilation escape hatch (it is how the Android/iOS/WebGL builds
#         get their host tools). It requires a COMPLETE prior arm64 CMake build
#         tree of the same source revision, because it consumes the
#         `ImportExecutables-<Config>.cmake` that build emitted. The prebuilt
#         release tarball is NOT such a tree — it has bin/matc but no CMake
#         export file — so taking route (b) means building Filament twice.
#
#   The script checks for Rosetta and tells you to use (b) if it is missing.
#
# WHAT IT BUILDS
#   Only the libraries desktop/CMakeLists.txt actually links (FILAMENT_LIBS):
#     filament backend bluegl bluevk filabridge filaflat utils geometry
#     smol-v ibl abseil zstd
#   …passed to ninja as explicit targets. That skips gltfio, viewer, matdbg,
#   the samples, image/imageio, libassimp, the tests and the Java bindings —
#   about two thirds of a full Filament build.
#
#   Host tools (matc/matinfo/resgen) are NOT an output of this script. The app
#   compiles materials/points.mat with the arm64 `matc` from the prebuilt
#   release, which is a build-time step on the developer's/CI's own machine and
#   has nothing to do with the target architecture of the shipped app.
#
# USAGE
#   ./tools/build_filament_x86_64.sh [VERSION]        # default v1.75.0
#   FILAMENT_JOBS=8 ./tools/build_filament_x86_64.sh  # override parallelism
#
# OUTPUT
#   third_party/filament-x86_64/lib/x86_64/lib*.a     (gitignored)
#   third_party/filament-x86_64/BUILDINFO.txt         exact toolchain provenance
#
# Then run tools/make_universal_filament.sh to lipo these against the arm64
# prebuilt set into third_party/filament-universal/.
# ---------------------------------------------------------------------------
set -euo pipefail

VERSION="${1:-v1.75.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$HERE/third_party/filament-src"
BUILD="$HERE/third_party/filament-build-x86_64"
OUT="$HERE/third_party/filament-x86_64"
JOBS="${FILAMENT_JOBS:-$(sysctl -n hw.ncpu)}"

# The exact library list desktop/CMakeLists.txt links. Keep in sync with
# FILAMENT_LIBS there. libzstd is load-bearing and easy to forget: Filament's
# own README link list omits it, but libfilament's MaterialParser/ZstdHelper
# hard-needs ZSTD_decompress (S3 REPORT.md §2.3).
#
# NOTE ON TARGET NAMES vs SHIPPED LIB NAMES — found the hard way:
# `ninja abseil` fails with "unknown target". The release tarball's
# `libabseil.a` is built by the CMake target **filament-abseil** (Filament
# vendors abseil under a prefixed target so it cannot collide with a system
# abseil) and renamed on the way into the release bundle. Every other library
# in the link list has target name == lib name. The left side below is the
# ninja target; the right side is the file name desktop/CMakeLists.txt looks
# for, i.e. the name the arm64 prebuilt set uses.
TARGETS=(filament backend bluegl bluevk filabridge filaflat utils geometry smol-v ibl filament-abseil zstd)
LIBS=(   filament backend bluegl bluevk filabridge filaflat utils geometry smol-v ibl abseil          zstd)

log() { printf '\033[1;36m[filament-x86_64]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[filament-x86_64] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "macOS only (host is $(uname -s))"

# --- preflight ------------------------------------------------------------
HOST_ARCH="$(uname -m)"
if [ "$HOST_ARCH" = "arm64" ]; then
  if ! arch -x86_64 /usr/bin/uname -m >/dev/null 2>&1; then
    die "Rosetta 2 is not installed, so the x86_64 matc/resgen this build
     produces cannot be executed on this arm64 host. Either:
       softwareupdate --install-rosetta --agree-to-license
     or build Filament arm64 first and re-run this script with
       IMPORT_EXECUTABLES_DIR=<that build dir>"
  fi
  log "Rosetta 2 present — the x86_64 matc/resgen this build produces will be"
  log "translated transparently when the build runs them."
fi
command -v cmake >/dev/null || die "cmake not found"
command -v ninja >/dev/null || die "ninja not found"
command -v git   >/dev/null || die "git not found"

# --- source ---------------------------------------------------------------
# Shallow clone at the tag. Filament vendors all of its third_party in-tree
# (no submodules), so --depth 1 is genuinely sufficient.
if [ -d "$SRC/.git" ]; then
  CUR="$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || echo unknown)"
  if [ "$CUR" != "$VERSION" ]; then
    die "$SRC exists at '$CUR', not '$VERSION'. Remove it or pass $CUR."
  fi
  log "reusing existing clone at $VERSION"
else
  log "cloning google/filament @ $VERSION (shallow) → $SRC"
  rm -rf "$SRC"
  git clone --depth 1 --branch "$VERSION" https://github.com/google/filament.git "$SRC"
fi

# --- configure ------------------------------------------------------------
# CMAKE_POLICY_VERSION_MINIMUM: CMake 4.x refuses projects whose
# cmake_minimum_required() is < 3.5, and several of Filament's vendored
# third_party CMakeLists (libpng, basisu, …) still declare old minimums. This
# is the documented escape hatch and does not change any generated code.
log "configuring (arch x86_64, Release, $JOBS jobs)"
mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DFILAMENT_SKIP_SAMPLES=ON \
  -DFILAMENT_SKIP_SDL2=ON \
  -DFILAMENT_ENABLE_JAVA=OFF \
  -DFILAMENT_SUPPORTS_METAL=ON \
  -DFILAMENT_SUPPORTS_VULKAN=ON \
  -DFILAMENT_SUPPORTS_OPENGL=ON \
  ${IMPORT_EXECUTABLES_DIR:+-DIMPORT_EXECUTABLES_DIR="$IMPORT_EXECUTABLES_DIR"} \
  2>&1 | tail -30

# --- build ----------------------------------------------------------------
log "building targets: ${TARGETS[*]}"
START=$(date +%s)
cmake --build "$BUILD" --parallel "$JOBS" --target "${TARGETS[@]}"
ELAPSED=$(( $(date +%s) - START ))
log "build finished in $((ELAPSED/60))m $((ELAPSED%60))s"

# --- collect --------------------------------------------------------------
#
# SECOND GOTCHA, and this one silently produces a broken universal binary if you
# miss it: for `geometry` and `abseil` the archive ninja links is NOT the archive
# Filament ships. Both use Filament's own `combine_static_libs()` helper
# (CMakeLists.txt:777 → build/linux/combine-static-libs.sh), which runs as a
# POST_BUILD step and merges the target plus all of its static dependencies into
# `lib<name>_combined.a`; the release tarball's `install(FILES ... RENAME)` rules
# then ship THAT under the plain name. The plain ninja output for
# `filament-abseil` is an **empty 656-byte archive** (the tnt target aggregates
# dependencies and has no sources of its own), so copying it gets you a build
# that configures and links right up until every absl symbol comes up undefined.
# Prefer the combined archive whenever one exists.
mkdir -p "$OUT/lib/x86_64"
MISSING=()
for i in "${!TARGETS[@]}"; do
  t="${TARGETS[$i]}"; l="${LIBS[$i]}"
  f="$(find "$BUILD" -name "lib${l}_combined.a" -type f -print -quit)"
  [ -n "$f" ] && log "  lib${l}.a <- $(basename "$f") (combine_static_libs output)"
  if [ -z "$f" ]; then
    # Filament scatters its .a files across the build tree; find the one target
    # ninja actually produced rather than guessing at the layout.
    f="$(find "$BUILD" -name "lib${t}.a" -type f -print -quit)"
  fi
  if [ -z "$f" ]; then MISSING+=("$t"); continue; fi
  cp "$f" "$OUT/lib/x86_64/lib${l}.a"
done
[ ${#MISSING[@]} -eq 0 ] || die "these libraries were not produced: ${MISSING[*]}"

# Verify every collected archive is really x86_64 and nothing else.
BAD=()
for f in "$OUT"/lib/x86_64/*.a; do
  info="$(lipo -info "$f" 2>&1)"
  case "$info" in
    *"is architecture: x86_64"*) ;;
    *) BAD+=("$(basename "$f"): $info") ;;
  esac
  # An "empty archive" tripwire — the generalisation of the combine_static_libs
  # gotcha above. The smallest real library in this set is ~35 KB.
  sz="$(stat -f%z "$f")"
  if [ "$sz" -lt 4096 ]; then
    BAD+=("$(basename "$f"): only $sz bytes — almost certainly an empty archive")
  fi
done
if [ ${#BAD[@]} -ne 0 ]; then
  printf '%s\n' "${BAD[@]}" >&2
  die "archives above are not pure x86_64"
fi

# --- provenance -----------------------------------------------------------
{
  echo "Filament x86_64 static libraries — built from source by"
  echo "desktop/tools/build_filament_x86_64.sh"
  echo
  echo "filament version   : $VERSION"
  echo "filament commit    : $(git -C "$SRC" rev-parse HEAD)"
  echo "built              : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "build time         : $((ELAPSED/60))m $((ELAPSED%60))s on $JOBS jobs"
  echo "host               : $(uname -m) macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
  echo "compiler           : $(clang --version | head -1)"
  echo "macOS SDK          : $(xcrun --show-sdk-version) @ $(xcrun --show-sdk-path)"
  echo "cmake              : $(cmake --version | head -1)"
  echo "ninja              : $(ninja --version)"
  echo "deployment target  : 11.0"
  echo "host-tool strategy : ${IMPORT_EXECUTABLES_DIR:-Rosetta 2 (x86_64 matc/resgen translated)}"
  echo
  echo "libraries:"
  for f in "$OUT"/lib/x86_64/*.a; do
    printf '  %-28s %10s bytes  %s\n' "$(basename "$f")" "$(stat -f%z "$f")" "$(lipo -info "$f" | sed 's/.*architecture: //')"
  done
} > "$OUT/BUILDINFO.txt"

log "wrote $OUT/lib/x86_64 ($(ls "$OUT/lib/x86_64" | wc -l | tr -d ' ') archives)"
cat "$OUT/BUILDINFO.txt"
