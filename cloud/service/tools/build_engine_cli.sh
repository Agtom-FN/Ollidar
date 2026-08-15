#!/usr/bin/env bash
# Build the engine CLI this service shells out to, into a location the
# integration test knows about (cloud/service/.engine-build/engine_cli).
#
#   ./tools/build_engine_cli.sh          # host build (macOS/Linux)
#
# The service itself never needs the engine sources — it execs a binary. This
# script exists so `pytest tests/test_engine_cli_integration.py` has something
# real to run, and so a deployer can produce the worker binary the systemd
# unit points at. For the real Linux worker image, see D1.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="$here/.engine-build"

command -v cmake >/dev/null || { echo "cmake not found" >&2; exit 1; }

generator=()
if command -v ninja >/dev/null; then generator=(-G Ninja); fi

cmake -S "$repo/engine" -B "$build" "${generator[@]}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENGINE_BUILD_TESTS=OFF \
  -DENGINE_BUILD_TOOLS=ON

cmake --build "$build" --target engine_cli --parallel

echo
echo "engine_cli: $build/engine_cli"
"$build/engine_cli" --version
