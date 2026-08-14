#!/usr/bin/env python3
"""check_ply.py — a tiny, from-scratch PLY reader used ONLY as an independent
sanity check on desktop's export round trip (C3 task 4: "export a replayed
synthetic project ... and re-import the PLY as a sanity check").

Deliberately does not reuse any of the engine's own PLY code (A9's
export/ply_writer.cpp) or its test-file reader (tests/test_export.cpp) — the
point is a reader written from the PLY spec, independent of the writer, the
same principle A9's own round-trip tests already use one level down. This is
the desktop-side rerun of that principle against the file the GUI/CLI export
path actually produced end to end.

Usage: check_ply.py FILE.ply [--expect-points N]
Exits 0 and prints a one-line summary on success; exits 1 with a message on
any structural problem (header/body mismatch, wrong file size, NaN/garbage
coordinates).
"""
import struct
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check_ply.py FILE.ply [--expect-points N]", file=sys.stderr)
        return 2
    path = sys.argv[1]
    expect_points = None
    if "--expect-points" in sys.argv:
        expect_points = int(sys.argv[sys.argv.index("--expect-points") + 1])

    with open(path, "rb") as f:
        data = f.read()

    nl = b"\n"
    pos = 0
    header_lines = []
    while True:
        idx = data.index(nl, pos)
        line = data[pos:idx].decode("ascii", errors="strict")
        header_lines.append(line)
        pos = idx + 1
        if line == "end_header":
            break

    if header_lines[0] != "ply":
        print(f"FAIL: not a PLY file (magic={header_lines[0]!r})", file=sys.stderr)
        return 1
    if "format binary_little_endian 1.0" not in header_lines:
        print(f"FAIL: unexpected format line(s): {header_lines[1]}", file=sys.stderr)
        return 1

    vertex_count = None
    props = []  # (name, size_bytes)
    type_sizes = {"float": 4, "float32": 4, "uchar": 1, "uint8": 1}
    in_vertex_element = False
    for line in header_lines:
        if line.startswith("element vertex "):
            vertex_count = int(line.split()[-1])
            in_vertex_element = True
        elif line.startswith("element ") and not line.startswith("element vertex"):
            in_vertex_element = False
        elif line.startswith("property ") and in_vertex_element:
            parts = line.split()
            ptype, pname = parts[1], parts[2]
            if ptype not in type_sizes:
                print(f"FAIL: unsupported property type {ptype!r}", file=sys.stderr)
                return 1
            props.append((pname, type_sizes[ptype]))

    if vertex_count is None:
        print("FAIL: no 'element vertex N' in header", file=sys.stderr)
        return 1

    stride = sum(sz for _, sz in props)
    body = data[pos:]
    expected_body_bytes = vertex_count * stride
    if len(body) != expected_body_bytes:
        print(
            f"FAIL: body is {len(body)} bytes, header (N={vertex_count}, "
            f"stride={stride}) implies {expected_body_bytes}",
            file=sys.stderr,
        )
        return 1

    if expect_points is not None and vertex_count != expect_points:
        print(
            f"FAIL: expected {expect_points} points, PLY declares {vertex_count}",
            file=sys.stderr,
        )
        return 1

    # Unpack every point's x/y/z (always the first three properties per
    # A9's ply_writer.cpp) and sanity-check the range — NaN/Inf or an
    # absurd magnitude would mean the writer or this reader disagrees about
    # the layout.
    names = [n for n, _ in props]
    if names[:3] != ["x", "y", "z"]:
        print(f"FAIL: expected x,y,z first, got {names[:3]}", file=sys.stderr)
        return 1

    xs, ys, zs = [], [], []
    off = 0
    for i in range(vertex_count):
        x, y, z = struct.unpack_from("<fff", body, off)
        xs.append(x)
        ys.append(y)
        zs.append(z)
        off += stride  # x,y,z plus whatever color/intensity properties follow

    import math

    for name, vals in (("x", xs), ("y", ys), ("z", zs)):
        for v in vals:
            if math.isnan(v) or math.isinf(v) or abs(v) > 1e6:
                print(f"FAIL: {name} has a non-finite/absurd value: {v}", file=sys.stderr)
                return 1

    lo = (min(xs), min(ys), min(zs)) if vertex_count else (0, 0, 0)
    hi = (max(xs), max(ys), max(zs)) if vertex_count else (0, 0, 0)
    print(
        f"OK: {path}: {vertex_count} points, properties={names}, "
        f"bounds=({lo[0]:.3f},{lo[1]:.3f},{lo[2]:.3f})..({hi[0]:.3f},{hi[1]:.3f},{hi[2]:.3f})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
