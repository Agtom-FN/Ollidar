#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# make_icon.sh — generate packaging/macos/lidarscan.icns (the app icon
# Info.plist's CFBundleIconFile names) plus the .ico/.png the Windows and Linux
# packages need, from one procedurally-drawn source image.
#
# WHY GENERATED AND NOT COMMITTED
#   No designer has produced a LidarScan mark yet. Committing a binary .icns
#   that nobody can regenerate or diff is worse than a script that draws a
#   deliberately simple placeholder: the packaging pipeline gets a REAL icon so
#   the bundle/DMG/installer are genuinely complete, and swapping in the real
#   artwork later is "replace icon.png, re-run this" rather than a hunt for
#   whoever made the blob.
#
#   The drawing is a plain-stdlib Python PNG writer (no Pillow, no ImageMagick,
#   no network) so this runs on a bare macOS/Linux box and in CI.
#
# OUTPUTS
#   packaging/macos/lidarscan.icns    macOS bundle icon (iconutil, 10 sizes)
#   packaging/macos/lidarscan.png     1024x1024 master
#   packaging/linux/lidarscan.png     256x256, for the .desktop / AppImage
#   packaging/windows/lidarscan.ico   multi-size ICO for NSIS + the .exe
#
# iconutil and sips are macOS-only; on Linux the script still writes the PNGs
# and the ICO and skips the .icns with a note.
# ---------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAC="$HERE/packaging/macos"
LIN="$HERE/packaging/linux"
WIN="$HERE/packaging/windows"
mkdir -p "$MAC" "$LIN" "$WIN"

log() { printf '\033[1;36m[icon]\033[0m %s\n' "$*"; }

PY="${PYTHON:-python3}"

"$PY" - "$MAC/lidarscan.png" <<'PYEOF'
# A 1024x1024 RGBA PNG written with nothing but zlib + struct.
# Design: a dark slate rounded square, a lidar origin dot, and three sweep arcs
# of points fanning out of it — i.e. the thing the app renders, at icon scale.
import math, struct, sys, zlib

N = 1024
CORNER = 0.2237 * N          # Apple's squircle-ish corner radius for macOS icons
px = bytearray(N * N * 4)

def put(x, y, r, g, b, a):
    if not (0 <= x < N and 0 <= y < N) or a <= 0: return
    i = (y * N + x) * 4
    da = a / 255.0
    px[i]   = int(px[i]   * (1 - da) + r * da)
    px[i+1] = int(px[i+1] * (1 - da) + g * da)
    px[i+2] = int(px[i+2] * (1 - da) + b * da)
    px[i+3] = max(px[i+3], a)

def rounded_rect_alpha(x, y):
    """Antialiased coverage of the rounded square, 0..1."""
    cx = min(max(x, CORNER), N - 1 - CORNER)
    cy = min(max(y, CORNER), N - 1 - CORNER)
    d = math.hypot(x - cx, y - cy)
    return max(0.0, min(1.0, (CORNER - d) + 0.5))

# Background: vertical gradient, deep slate -> near black (reads on both the
# light and the dark Finder backgrounds).
for y in range(N):
    t = y / (N - 1)
    r = int(24 + (10 - 24) * t)
    g = int(32 + (14 - 32) * t)
    b = int(44 + (20 - 44) * t)
    for x in range(N):
        a = rounded_rect_alpha(x, y)
        if a <= 0: continue
        put(x, y, r, g, b, int(255 * a))

def dot(cx, cy, rad, r, g, b, alpha=1.0):
    x0, x1 = int(cx - rad - 1), int(cx + rad + 2)
    y0, y1 = int(cy - rad - 1), int(cy + rad + 2)
    for y in range(y0, y1):
        for x in range(x0, x1):
            d = math.hypot(x - cx, y - cy)
            cov = max(0.0, min(1.0, (rad - d) + 0.5))
            if cov > 0:
                put(x, y, r, g, b, int(255 * cov * alpha))

ORIGIN = (N * 0.5, N * 0.72)

# Three sweep arcs. Cyan near, teal mid, deep blue far — the same "far is
# dimmer" reading a real range-coloured cloud has.
ARCS = [
    (N * 0.20, (110, 240, 255), 9.0, 46, 1.00),
    (N * 0.30, ( 70, 200, 235), 7.5, 62, 0.85),
    (N * 0.40, ( 46, 150, 205), 6.0, 80, 0.70),
]
SPAN = math.radians(132)     # the fan's total angular width, opening upward
for radius, (r, g, b), size, count, alpha in ARCS:
    for i in range(count):
        th = -math.pi / 2 - SPAN / 2 + SPAN * (i / (count - 1))
        # Break the arcs up a little so they read as sampled returns, not a line.
        jitter = 1.0 + 0.035 * math.sin(i * 2.399 + radius)
        x = ORIGIN[0] + math.cos(th) * radius * jitter
        y = ORIGIN[1] + math.sin(th) * radius * jitter * 0.86
        dot(x, y, size, r, g, b, alpha)

# The sensor itself.
dot(ORIGIN[0], ORIGIN[1], 30, 235, 245, 255, 1.0)
dot(ORIGIN[0], ORIGIN[1], 17, 20, 30, 42, 1.0)

raw = b''.join(b'\x00' + bytes(px[y*N*4:(y+1)*N*4]) for y in range(N))
def chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data
            + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', N, N, 8, 6, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(raw, 9))
       + chunk(b'IEND', b''))
open(sys.argv[1], 'wb').write(png)
print(f"wrote {sys.argv[1]} ({len(png)} bytes)")
PYEOF

log "master PNG: $MAC/lidarscan.png"

if command -v sips >/dev/null && command -v iconutil >/dev/null; then
  IS="$MAC/lidarscan.iconset"
  rm -rf "$IS"; mkdir -p "$IS"
  # The exact 10-file set iconutil expects; anything missing and it warns.
  for spec in "16:icon_16x16" "32:icon_16x16@2x" "32:icon_32x32" "64:icon_32x32@2x" \
              "128:icon_128x128" "256:icon_128x128@2x" "256:icon_256x256" \
              "512:icon_256x256@2x" "512:icon_512x512" "1024:icon_512x512@2x"; do
    sz="${spec%%:*}"; name="${spec#*:}"
    sips -z "$sz" "$sz" "$MAC/lidarscan.png" --out "$IS/$name.png" >/dev/null
  done
  iconutil -c icns "$IS" -o "$MAC/lidarscan.icns"
  rm -rf "$IS"
  log "macOS icon: $MAC/lidarscan.icns ($(stat -f%z "$MAC/lidarscan.icns") bytes)"
  sips -z 256 256 "$MAC/lidarscan.png" --out "$LIN/lidarscan.png" >/dev/null
  log "Linux icon: $LIN/lidarscan.png"
else
  log "sips/iconutil not available (non-macOS host) — skipping .icns"
  cp "$MAC/lidarscan.png" "$LIN/lidarscan.png"
fi

# Windows .ico: a plain ICO container holding PNG-compressed images, which
# every Windows version since Vista (and NSIS 3) reads. Written here rather
# than shelling out to ImageMagick so the Windows package can be produced on a
# machine with nothing installed.
"$PY" - "$MAC/lidarscan.png" "$WIN/lidarscan.ico" "$MAC" <<'PYEOF'
import struct, subprocess, sys, os, shutil
src, out, workdir = sys.argv[1], sys.argv[2], sys.argv[3]
sizes = [16, 32, 48, 64, 128, 256]
imgs = []
if shutil.which('sips'):
    for s in sizes:
        p = os.path.join(workdir, f'_ico_{s}.png')
        subprocess.run(['sips', '-z', str(s), str(s), src, '--out', p],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        imgs.append((s, open(p, 'rb').read()))
        os.remove(p)
else:
    # No resampler available: ship the 1024 master as the single 256-slot
    # entry. Windows scales it down itself; ugly at 16x16 but valid.
    imgs = [(256, open(src, 'rb').read())]

hdr = struct.pack('<HHH', 0, 1, len(imgs))
offset = 6 + 16 * len(imgs)
entries, blobs = b'', b''
for s, data in imgs:
    entries += struct.pack('<BBBBHHII', 0 if s >= 256 else s, 0 if s >= 256 else s,
                           0, 0, 1, 32, len(data), offset)
    blobs += data
    offset += len(data)
open(out, 'wb').write(hdr + entries + blobs)
print(f"wrote {out} ({len(hdr+entries+blobs)} bytes, {len(imgs)} sizes)")
PYEOF

log "Windows icon: $WIN/lidarscan.ico"
