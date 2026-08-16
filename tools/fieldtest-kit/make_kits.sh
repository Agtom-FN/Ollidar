#!/bin/bash
# make_kits.sh -- build and package the LidarScan field-test kits.
#
#   tools/LidarScan-FieldTestKit-Windows.zip
#   tools/LidarScan-FieldTestKit-macOS.zip
#
# The macOS kit BUNDLES three pre-compiled arm64 binaries so the runner needs
# nothing installed:
#   bin/d6cli      built here from spikes/s1-d6-parser via cmake (the real S1
#                  COIN-D6 parser, so the macOS D6 verdict is a genuine
#                  checksum pass rate)
#   bin/mid360cap  built here from src/mid360cap.c
#   bin/um982cap   built here from src/um982cap.c
#
# The Windows kit is script-only on purpose: PowerShell 5.1 ships in the box on
# Windows 10/11, so there is nothing to compile, sign, or get blocked.
#
# Usage:
#   bash tools/fieldtest-kit/make_kits.sh            build binaries + both zips
#   bash tools/fieldtest-kit/make_kits.sh --no-build reuse macos/bin as-is
#   bash tools/fieldtest-kit/make_kits.sh --test     also run both smoke suites

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(dirname "$KIT_DIR")"
REPO_ROOT="$(dirname "$TOOLS_DIR")"
D6_SPIKE="$REPO_ROOT/spikes/s1-d6-parser"

DO_BUILD=1
DO_TEST=0
for a in "$@"; do
  case "$a" in
    --no-build) DO_BUILD=0 ;;
    --test) DO_TEST=1 ;;
    -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- build ----
if [ "$DO_BUILD" = "1" ]; then
  say "building macOS arm64 tools"
  mkdir -p "$KIT_DIR/macos/bin"

  # _DARWIN_C_SOURCE: um982cap uses memmem(), which is behind that guard.
  cc -O2 -std=c99 -Wall -Wextra -D_DARWIN_C_SOURCE \
     -o "$KIT_DIR/macos/bin/mid360cap" "$KIT_DIR/src/mid360cap.c"
  cc -O2 -std=c99 -Wall -Wextra -D_DARWIN_C_SOURCE \
     -o "$KIT_DIR/macos/bin/um982cap" "$KIT_DIR/src/um982cap.c"
  say "  built mid360cap, um982cap"

  if [ -d "$D6_SPIKE" ]; then
    BUILD_DIR="${TMPDIR:-/tmp}/lidarscan-fieldtest-d6build"
    cmake -S "$D6_SPIKE" -B "$BUILD_DIR" \
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
    cmake --build "$BUILD_DIR" --target d6cli -j4 >/dev/null
    cp "$BUILD_DIR/d6cli" "$KIT_DIR/macos/bin/d6cli"
    say "  built d6cli from $D6_SPIKE"
  else
    echo "WARNING: $D6_SPIKE missing -- keeping whatever bin/d6cli is there" >&2
  fi

  chmod +x "$KIT_DIR/macos/bin/"* "$KIT_DIR/macos/"*.command

  for b in d6cli mid360cap um982cap; do
    arch="$(file -b "$KIT_DIR/macos/bin/$b" | grep -o 'arm64\|x86_64' | head -1)"
    [ "$arch" = "arm64" ] || { echo "ERROR: bin/$b is $arch, expected arm64" >&2; exit 1; }
  done
  say "  all three binaries are arm64"
fi

# --------------------------------------------------------------- syntax ----
say "syntax-checking the shipped scripts"
if command -v pwsh >/dev/null 2>&1; then
  for f in "$KIT_DIR"/windows/scripts/*.ps1; do
    pwsh -NoProfile -Command "
      \$e = \$null; \$t = \$null
      [System.Management.Automation.Language.Parser]::ParseFile('$f', [ref]\$t, [ref]\$e) | Out-Null
      if (\$e -and \$e.Count -gt 0) {
        \$e | ForEach-Object { Write-Host ('  ' + \$_.Extent.StartLineNumber + ': ' + \$_.Message) }
        exit 1
      }" || { echo "ERROR: PowerShell parse error in $f" >&2; exit 1; }
  done
  say "  PowerShell scripts parse clean"
else
  echo "WARNING: pwsh not installed -- skipped the PowerShell parse check" >&2
fi
for f in "$KIT_DIR"/macos/lib/*.sh "$KIT_DIR"/macos/*.command; do
  bash -n "$f" || { echo "ERROR: shell syntax error in $f" >&2; exit 1; }
done
say "  shell scripts parse clean"

# ----------------------------------------------------------------- test ----
if [ "$DO_TEST" = "1" ]; then
  say "running the smoke suites"
  bash "$KIT_DIR/tests/smoke_macos.sh"
  if command -v pwsh >/dev/null 2>&1; then
    pwsh -NoProfile -File "$KIT_DIR/tests/smoke_windows.ps1"
  fi
fi

# ---------------------------------------------------------------- stage ----
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

say "staging Windows kit"
mkdir -p "$STAGE/LidarScan-FieldTest-Windows"
cp -R "$KIT_DIR/windows/." "$STAGE/LidarScan-FieldTest-Windows/"

say "staging macOS kit"
mkdir -p "$STAGE/LidarScan-FieldTest-macOS"
cp -R "$KIT_DIR/macos/." "$STAGE/LidarScan-FieldTest-macOS/"
chmod +x "$STAGE/LidarScan-FieldTest-macOS/"*.command \
         "$STAGE/LidarScan-FieldTest-macOS/bin/"*

# Nothing else in tools/fieldtest-kit/ ships: src/ and tests/ are dev-side,
# and android/ belongs to a different deliverable.
find "$STAGE" -name '.DS_Store' -delete

# ------------------------------------------------------------------ zip ----
WIN_ZIP="$TOOLS_DIR/LidarScan-FieldTestKit-Windows.zip"
MAC_ZIP="$TOOLS_DIR/LidarScan-FieldTestKit-macOS.zip"
rm -f "$WIN_ZIP" "$MAC_ZIP"

( cd "$STAGE" && zip -q -r "$WIN_ZIP" LidarScan-FieldTest-Windows )
# -y is not used: there are no symlinks, and the exec bit must survive, which
# the default zip on macOS preserves.
( cd "$STAGE" && zip -q -r "$MAC_ZIP" LidarScan-FieldTest-macOS )

say "wrote:"
ls -lh "$WIN_ZIP" "$MAC_ZIP" | sed 's/^/    /'

say "contents:"
unzip -l "$WIN_ZIP" | sed -n '4,40p' | sed 's/^/    /'
echo
unzip -l "$MAC_ZIP" | sed -n '4,40p' | sed 's/^/    /'
