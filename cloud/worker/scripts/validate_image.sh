#!/usr/bin/env bash
# cloud/worker/scripts/validate_image.sh — D1's local/CI verification script.
#
# Builds (or reuses) the worker image, then exercises the exact contract
# documented in cloud/worker/README.md:
#   1. generate a synthetic .lscan with the IMAGE'S OWN engine_cli
#      (--synth-lscan — the same fixture engine_cli_post's ctest uses)
#   2. run the image against it with argv = <input-dir> <output-dir>
#   3. assert exit 0 and a non-trivial cloud.ply landed in the output dir
#   4. assert stdout carries only the product summary and stderr carries
#      progress (INT34-wiring.md §6's "a worker's stdout is its product")
#   5. exercise the three documented failure exit codes (2: usage / missing
#      input dir, 1: engine_cli's own failure on a non-.lscan directory)
#
# This is what `docker build` + `docker run` verification looked like when
# D1 was built (see README.md's "Verification" section for the transcript);
# this script is the reproducible version of that same session, for CI or a
# future re-check after a Dockerfile edit.
#
# Usage:
#   scripts/validate_image.sh                  # build + full run, from repo root
#   scripts/validate_image.sh --image <tag>     # skip the build, use an existing tag
#   scripts/validate_image.sh --no-build        # same, shorthand
#
# Requires: docker (or podman via `CONTAINER_ENGINE=podman`), run from the
# repo root (or pass --repo-root).
set -euo pipefail

ENGINE="${CONTAINER_ENGINE:-docker}"
IMAGE="lidarscan-worker:validate"
DO_BUILD=1
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

while [ $# -gt 0 ]; do
  case "$1" in
    --image) IMAGE="$2"; DO_BUILD=0; shift 2 ;;
    --no-build) DO_BUILD=0; shift ;;
    --repo-root) REPO_ROOT="$2"; shift 2 ;;
    -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if ! command -v "$ENGINE" >/dev/null 2>&1; then
  echo "validate_image: '$ENGINE' not found on PATH." >&2
  echo "  Install Docker Desktop, or 'brew install colima docker' + 'colima start'," >&2
  echo "  or 'brew install podman' + 'podman machine init && podman machine start'," >&2
  echo "  then re-run. See README.md's 'If container tooling is unavailable' section." >&2
  exit 3
fi
if ! "$ENGINE" info >/dev/null 2>&1; then
  echo "validate_image: '$ENGINE' is on PATH but its daemon/VM is not running." >&2
  exit 3
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
IN_DIR="$WORK/input"
OUT_DIR="$WORK/output"
mkdir -p "$IN_DIR" "$OUT_DIR"

pass=0
fail=0
check() {  # check <description> <actual> <expected>
  if [ "$2" = "$3" ]; then
    echo "  PASS: $1 (got $2)"
    pass=$((pass + 1))
  else
    echo "  FAIL: $1 (expected $3, got $2)"
    fail=$((fail + 1))
  fi
}

if [ "$DO_BUILD" -eq 1 ]; then
  echo "== building $IMAGE from $REPO_ROOT =="
  "$ENGINE" build -f "$REPO_ROOT/cloud/worker/Dockerfile" -t "$IMAGE" "$REPO_ROOT"
fi

echo
echo "== 1. synthesizing a .lscan with the image's own engine_cli =="
"$ENGINE" run --rm --entrypoint engine_cli \
  -v "$IN_DIR:/work/session.lscan" \
  "$IMAGE" --synth-lscan /work/session.lscan 2.2
[ -f "$IN_DIR/manifest.json" ] || { echo "FAIL: synth-lscan did not produce manifest.json"; exit 1; }

echo
echo "== 2. running the worker: argv = <input-dir> <output-dir> =="
set +e
"$ENGINE" run --rm \
  -e WORKER_POST_ARGS="--no-loops --no-outlier --dedup 0.05" \
  -v "$IN_DIR:/data/input" \
  -v "$OUT_DIR:/data/output" \
  "$IMAGE" /data/input /data/output \
  >"$WORK/stdout.log" 2>"$WORK/stderr.log"
rc=$?
set -e
check "happy-path exit code" "$rc" "0"
check "cloud.ply exists" "$([ -f "$OUT_DIR/cloud.ply" ] && echo yes || echo no)" "yes"
if [ -f "$OUT_DIR/cloud.ply" ]; then
  sz=$(wc -c < "$OUT_DIR/cloud.ply" | tr -d ' ')
  [ "$sz" -gt 512 ] && check "cloud.ply is non-trivial (>512 bytes)" "big" "big" \
                    || check "cloud.ply is non-trivial (>512 bytes)" "$sz bytes" ">512 bytes"
fi
check "stdout carries the product summary" \
  "$(grep -c '^post: OK$' "$WORK/stdout.log" || true)" "1"
check "stdout does NOT carry progress lines" \
  "$(grep -c '^post: .*%' "$WORK/stdout.log" || true)" "0"
progress_lines="$(grep -c '^post: .*%' "$WORK/stderr.log" || true)"
[ "$progress_lines" -ge 1 ] && check "stderr carries progress lines (>=1)" "yes" "yes" \
                             || check "stderr carries progress lines (>=1)" "$progress_lines lines" ">=1 lines"

echo
echo "== 3. usage error: no args -> exit 2 =="
set +e
"$ENGINE" run --rm "$IMAGE" >/dev/null 2>"$WORK/usage.log"
rc=$?
set -e
check "missing-args exit code" "$rc" "2"

echo
echo "== 4. usage error: nonexistent input dir -> exit 2 =="
set +e
"$ENGINE" run --rm "$IMAGE" /no/such/dir /data/output >/dev/null 2>"$WORK/nodir.log"
rc=$?
set -e
check "nonexistent-input-dir exit code" "$rc" "2"

echo
echo "== 5. engine_cli failure: input dir with no manifest.json -> exit 1 =="
mkdir -p "$WORK/garbage"
set +e
"$ENGINE" run --rm \
  -v "$WORK/garbage:/data/input" \
  -v "$OUT_DIR:/data/output" \
  "$IMAGE" /data/input /data/output >/dev/null 2>"$WORK/badinput.log"
rc=$?
set -e
check "invalid-lscan exit code" "$rc" "1"

echo
echo "== summary: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
