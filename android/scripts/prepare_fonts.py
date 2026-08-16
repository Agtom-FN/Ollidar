#!/usr/bin/env python3
"""Regenerate app/src/main/res/font/*.ttf from the upstream variable masters.

The redesign bundles three SIL OFL 1.1 families — Space Grotesk (display),
Inter (UI), JetBrains Mono (telemetry). What ships in `res/font/` is **not**
the upstream file: each family is downloaded from google/fonts as its variable
master, then

  1. `instancer.instantiateVariableFont` pins the weights this app actually
     asks for, so the runtime never has to apply `FontVariation` settings (and
     the minSdk-29 floor never has to care whether it can), and
  2. `pyftsubset` cuts the charset to Latin plus the punctuation and symbols
     the UI genuinely draws — `·`, `→`, `✓`, `Δ`, `±`, `—`, the arrow and math
     blocks the chips and readouts use.

That is the difference between ~1.2 MB and ~690 KB of APK, for glyphs no
screen in this app can reach.

Run from anywhere:

    pip3 install --user fonttools brotli
    python3 android/scripts/prepare_fonts.py

Licences live beside the fonts in the APK, at `assets/fonts/OFL-*.txt`, so the
attribution travels with the binary rather than only with the repo. This
script refreshes those too.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

REPO_FONT_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "app", "src", "main", "res", "font"
)
REPO_LICENSE_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "app", "src", "main", "assets", "fonts"
)

RAW = "https://github.com/google/fonts/raw/main"

# Latin + exactly the non-Latin marks the UI draws. Adding a glyph here is
# cheaper than discovering a tofu box on a device.
UNICODES = (
    "U+0000-00FF,U+0131,U+0152-0153,U+02BB-02BC,U+02C6,U+02DA,U+02DC,"
    "U+0300-036F,U+0394,U+03A9,U+03BC,U+2000-206F,U+2070-209F,U+20AC,"
    "U+2122,U+2190-21FF,U+2200-22FF,U+2713,U+25A0-25FF,U+2B1B,U+FEFF,U+FFFD"
)

# family -> (upstream path, output basename, {output suffix: weight})
FAMILIES = {
    "Space Grotesk": (
        "ofl/spacegrotesk/SpaceGrotesk%5Bwght%5D.ttf",
        "space_grotesk",
        {"medium": 500, "semibold": 600, "bold": 700},
        "ofl/spacegrotesk/OFL.txt",
        "OFL-spacegrotesk.txt",
    ),
    "Inter": (
        "ofl/inter/Inter%5Bopsz,wght%5D.ttf",
        "inter",
        {"regular": 400, "medium": 500, "semibold": 600},
        "ofl/inter/OFL.txt",
        "OFL-inter.txt",
    ),
    "JetBrains Mono": (
        "ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf",
        "jetbrains_mono",
        {"regular": 400, "medium": 500},
        "ofl/jetbrainsmono/OFL.txt",
        "OFL-jetbrainsmono.txt",
    ),
}


def fetch(url: str, dest: str) -> None:
    subprocess.run(["curl", "-sSL", "--fail", "-o", dest, url], check=True)


def main() -> int:
    os.makedirs(REPO_FONT_DIR, exist_ok=True)
    os.makedirs(REPO_LICENSE_DIR, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        for family, (path, base, weights, license_path, license_name) in FAMILIES.items():
            master = os.path.join(tmp, base + ".ttf")
            fetch(f"{RAW}/{path}", master)
            fetch(f"{RAW}/{license_path}", os.path.join(REPO_LICENSE_DIR, license_name))

            for name, weight in weights.items():
                font = TTFont(master)
                axes = {"wght": weight}
                # Inter carries an optical-size axis; pin it at the UI size
                # rather than letting the instancer pick a default.
                if any(a.axisTag == "opsz" for a in font["fvar"].axes):
                    axes["opsz"] = 14.0
                instance = instancer.instantiateVariableFont(
                    font, axes, inplace=True, updateFontNames=False
                )

                options = subset.Options()
                options.layout_features = ["*"]
                options.name_IDs = [1, 2, 3, 4, 5, 6]
                options.notdef_outline = True
                options.recalc_bounds = True
                options.drop_tables += ["DSIG"]
                options.glyph_names = False
                sub = subset.Subsetter(options=options)
                sub.populate(unicodes=subset.parse_unicodes(UNICODES))
                sub.subset(instance)

                out = os.path.join(REPO_FONT_DIR, f"{base}_{name}.ttf")
                instance.flavor = None
                instance.save(out)
                print(f"{family} {weight} -> {out} ({os.path.getsize(out):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
