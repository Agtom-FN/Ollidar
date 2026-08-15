#!/bin/sh
# cloud/worker/entrypoint.sh — the container's ENTRYPOINT.
#
# Contract with the job service (documented in README.md's "WORKER_CMD
# contract" section — this is D1's side of the seam D2's job service is
# built against; cloud/service does not exist yet as of this writing, so
# this is the interface handed to D2, not one read back from it):
#
#   argv[1] = input directory  — a bind-mounted / volume-mounted directory
#             containing the .lscan session (manifest.json + streams/, per
#             docs/A5-lscan.md and the Tech Spec §3.11 container layout).
#   argv[2] = output directory — a bind-mounted / volume-mounted directory,
#             writable, where the result bundle is written (cloud.ply today;
#             see engine_cli.cpp's cmd_post — a worker gets exactly what a
#             desktop's "Cloud" processing mode gets, §3.8).
#   stdout  = the job's machine-readable product summary (engine_cli's own
#             convention, INT34-wiring.md §6: "a worker's stdout is its
#             product and has to stay machine-readable when it is piped").
#   stderr  = human-readable progress lines ("post: NN%  <stage>"), plus this
#             script's own preamble line.
#   exit    = passed through UNMODIFIED from engine_cli: 0 ok, 1 failed,
#             2 usage, 3 cancelled (engine_cli.cpp's documented exit codes).
#             `exec` below is what makes this exact, not "best effort" —
#             engine_cli replaces this shell as PID 1's child rather than
#             running under it, so there is no wrapper exit code to get
#             wrong.
#
# WORKER_INPUT_DIR / WORKER_OUTPUT_DIR are accepted as a fallback to the
# positional args, in case the eventual job service prefers to configure the
# container via environment rather than a command override — both forms
# reach the identical engine_cli invocation.
#
# WORKER_POST_ARGS, if set, is split on whitespace and appended verbatim to
# the engine_cli --post call (e.g. "--no-loops --dedup 0.02"). Left unset by
# default so a worker's behaviour matches engine/docs/INT34-wiring.md §6's
# "a cloud worker's defaults are A7's defaults" exactly.

set -eu

INPUT_DIR="${1:-${WORKER_INPUT_DIR:-}}"
OUTPUT_DIR="${2:-${WORKER_OUTPUT_DIR:-}}"

if [ -z "$INPUT_DIR" ] || [ -z "$OUTPUT_DIR" ]; then
  echo "usage: entrypoint.sh <input-dir> <output-dir>" >&2
  echo "       (or set WORKER_INPUT_DIR / WORKER_OUTPUT_DIR)" >&2
  # 2: same "usage" exit code engine_cli itself uses for a bad invocation —
  # keeps the contract's exit-code table meaningful even before engine_cli
  # is reached.
  exit 2
fi

if [ ! -d "$INPUT_DIR" ]; then
  echo "entrypoint: input directory '$INPUT_DIR' does not exist or is not mounted" >&2
  exit 2
fi

echo "worker: post-processing '$INPUT_DIR' -> '$OUTPUT_DIR'" >&2

# Word-splitting WORKER_POST_ARGS on whitespace is intentional here (it is
# the simplest thing that lets a caller pass "--no-loops --dedup 0.02" as one
# env var); it is not fed anything shell-quoted or untrusted beyond that.
# shellcheck disable=SC2086
exec /usr/local/bin/engine_cli --post "$INPUT_DIR" --out "$OUTPUT_DIR" ${WORKER_POST_ARGS:-}
