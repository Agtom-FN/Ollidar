#!/usr/bin/env bash
# make_kit.sh -- zip up the remote-capture kit for the remote site.
#
# Packages the three capture scripts + INSTRUCTIONS.md into
# lidarscan-capture-kit.zip, ready to send. verify_capture.py is
# intentionally NOT included -- it runs at the dev machine end, on the
# files that come back, and depends on the S1 spike build.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

KIT_NAME="lidarscan-capture-kit.zip"
FILES=(
  capture_d6.py
  capture_mid360.py
  capture_gnss.py
  INSTRUCTIONS.md
)

for f in "${FILES[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "error: expected file missing: $f" >&2
    exit 1
  fi
done

rm -f "$KIT_NAME"
zip -q -X "$KIT_NAME" "${FILES[@]}"

echo "Wrote $SCRIPT_DIR/$KIT_NAME"
echo
echo "Contents:"
unzip -l "$KIT_NAME"
