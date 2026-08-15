#!/usr/bin/env python3
"""check_dxf.py — a from-scratch DXF R12 sanity reader, independent of A12's
own writer/tests (engine/src/plan/dxf_writer.cpp, engine/tests/test_plan.cpp).

Same "two independent implementations must agree" posture desktop/scripts/
check_ply.py already uses against A9's PLY writer, and the one A12's own doc
(engine/docs/A12-plan.md §7) describes for its own from-scratch DXF reader —
this is the desktop's copy of that idea, not a reuse of engine test code
(this task cannot add to engine/tests/).

Checks, using nothing but the DXF group-code grammar (alternating code line /
value line):
  * $ACADVER == AC1009 (R12, per engine/docs/A12-plan.md)
  * HEADER/TABLES/BLOCKS/ENTITIES/EOF sections present
  * every POLYLINE (code 0) is eventually closed by a SEQEND
  * the four expected layers (WALLS/OPENINGS/ROOMS/DIMENSIONS) appear as a
    VERTEX/POLYLINE layer (code 8) at least once
  * no comma appears anywhere in the file (the locale bug A9/A12 both guard
    against: a de_DE locale writing "1,234" for a coordinate)
  * the file is pure ASCII
"""
import sys


def tokenize(path):
    with open(path, "r", encoding="ascii") as f:
        lines = [l.rstrip("\r\n") for l in f]
    pairs = []
    i = 0
    while i + 1 < len(lines):
        code = lines[i].strip()
        value = lines[i + 1]
        pairs.append((code, value))
        i += 2
    return pairs


def main():
    if len(sys.argv) != 2:
        print("usage: check_dxf.py FILE.dxf", file=sys.stderr)
        return 2
    path = sys.argv[1]

    raw = open(path, "rb").read()
    if b"," in raw:
        print(f"FAIL: {path}: comma found (locale-formatted number?)")
        return 1
    try:
        raw.decode("ascii")
    except UnicodeDecodeError as e:
        print(f"FAIL: {path}: non-ASCII byte: {e}")
        return 1

    pairs = tokenize(path)
    if not pairs:
        print(f"FAIL: {path}: no group-code pairs parsed")
        return 1

    acadver = None
    layers = set()
    polylines = 0
    seqends = 0
    entities_seen = 0
    sections = set()
    for idx, (code, value) in enumerate(pairs):
        if code == "9" and value == "$ACADVER" and idx + 1 < len(pairs):
            acadver = pairs[idx + 1][1]
        if code == "2" and value in ("HEADER", "TABLES", "BLOCKS", "ENTITIES", "OBJECTS"):
            sections.add(value)
        if code == "0" and value == "POLYLINE":
            polylines += 1
        if code == "0" and value == "SEQEND":
            seqends += 1
        if code == "0" and value in ("POLYLINE", "TEXT", "LINE"):
            entities_seen += 1
        if code == "8":
            layers.add(value)

    ok = True
    if acadver != "AC1009":
        print(f"FAIL: \\$ACADVER = {acadver!r}, want AC1009")
        ok = False
    for want in ("HEADER", "ENTITIES"):
        if want not in sections:
            print(f"FAIL: section {want} not found")
            ok = False
    if polylines == 0:
        print("FAIL: no POLYLINE entities found")
        ok = False
    if polylines != seqends:
        print(f"FAIL: {polylines} POLYLINE vs {seqends} SEQEND (unclosed polyline)")
        ok = False
    expected_layers = {"WALLS", "OPENINGS", "ROOMS", "DIMENSIONS"}
    missing = expected_layers - layers
    if missing:
        print(f"FAIL: expected layers missing: {sorted(missing)} (found {sorted(layers)})")
        ok = False

    if ok:
        print(
            f"OK: {path}: AC1009, {len(sections)} sections, {polylines} POLYLINE/SEQEND pairs, "
            f"{entities_seen} entities, layers={sorted(layers)}"
        )
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
