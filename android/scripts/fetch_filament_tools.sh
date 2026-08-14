#!/usr/bin/env bash
# Fetch the prebuilt Filament release that ships `matc` (the material
# compiler), version-matched to the Filament **Android AAR** this app
# depends on (see gradle/libs.versions.toml's `filament` version).
#
# WHY THIS IS A SEPARATE FETCH FROM desktop/tools/fetch_filament.sh
# ------------------------------------------------------------------
# The desktop app pins native Filament v1.75.0 (desktop/tools/fetch_filament.sh).
# `matc`'s output format (.filamat) is versioned and matched against the
# Filament *runtime* it will be loaded by — matc is not forward/backward
# compatible across releases in general. The Android AAR
# (com.google.android.filament:filament-android) is only published to Maven
# Central up to v1.71.5 as of this task (v1.75.0 has no Android AAR release
# on Maven Central), so this script fetches matc from the **v1.71.5** host
# tools release instead of reusing desktop's v1.75.0 binary — using the
# wrong-version matc against the app's v1.71.5 runtime risks a material that
# fails to load at runtime with a version mismatch, or silently miscompiles.
# Always keep this version argument's default in sync with the `filament`
# entry in gradle/libs.versions.toml.
set -euo pipefail

VERSION="${1:-v1.71.5}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$HERE/third_party/filament-tools-${VERSION}"

if [ -x "$DEST/filament/bin/matc" ]; then
  echo "matc already present at $DEST/filament/bin/matc"
  "$DEST/filament/bin/matc" --version
  exit 0
fi

case "$(uname -s)" in
  Darwin) ASSET="filament-${VERSION}-mac.tgz" ;;
  Linux)  ASSET="filament-${VERSION}-linux.tgz" ;;
  *)      echo "unsupported host: $(uname -s)"; exit 1 ;;
esac

URL="https://github.com/google/filament/releases/download/${VERSION}/${ASSET}"
mkdir -p "$DEST"
echo "fetching $URL"
curl -fL --progress-bar -o "/tmp/${ASSET}" "$URL"
tar xzf "/tmp/${ASSET}" -C "$DEST"
rm -f "/tmp/${ASSET}"
echo "unpacked into $DEST/filament"
"$DEST/filament/bin/matc" --version >/dev/null && echo "matc OK"
