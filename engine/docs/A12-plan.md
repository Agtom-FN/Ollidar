# A12 — floor-plan extraction (§3.6)

**Scope:** `engine/include/scanengine/plan/**`, `engine/src/plan/**`,
`engine/tests/test_plan.cpp`.
**Spec:** §3.6 — *"Gravity-aligned cloud → horizontal slice band (default
1.0–1.5 m, configurable) → 2D occupancy → RANSAC line extraction + merge →
optional orthogonality snapping → wall polylines + opening heuristics → DXF
(layered polylines) and PDF (scaled sheet with dimensions). Editor v1:
slice-height slider + include/exclude regions; CAD-grade editing is Phase 3."*
**Contract:** `engine/DESIGN.md` §1 (module map), §5 (`PointVertex`,
`PageStore`), §6 (how to add a module).
**Consumes:** A7's gravity-aligned final cloud (`docs/A7-post.md`) — either as
the `PageStore` it publishes on `StreamId::kSlamMap`, or as a flat span — and
A7's statistical outlier filter, wired as an option.
**Produces:** `PlanModel`, DXF R12, PDF 1.4.

Everything is hand-rolled against the C++ standard library. `vcpkg.json` and
`CMakeLists.txt` are unchanged: `src/*.cpp` and `tests/test_*.cpp` are globbed
with `CONFIGURE_DEPENDS`, so the new files were picked up on the next
configure. Nothing under `export/` was touched — the DXF and PDF writers live
in `plan/` (see §7).

---

## 1. What ships

| Piece | Header | Impl | Lines (h / cpp) |
| --- | --- | --- | --- |
| Output model, polygon arithmetic, footprint splitting | `plan/plan_model.h` | `plan_model.cpp` | 287 / 230 |
| Slice band + 2D occupancy grid | `plan/occupancy.h` | `occupancy.cpp` | 130 / 296 |
| Options, pipeline entry points, the A1 seam adapter | `plan/floor_plan.h` | `floor_plan.cpp` | 230 / 133 |
| RANSAC, collinear merge, snapping, pairing, openings, corners | *(options in `floor_plan.h`)* | `wall_extract.cpp` | — / 933 |
| Planar-face rooms + inset + area | *(internal)* | `rooms.cpp` | — / 297 |
| Editor v1 (slice slider + include/exclude regions) | `plan/plan_editor.h` | `plan_editor.cpp` | 111 / 172 |
| DXF R12 writer | `plan/plan_writers.h` | `dxf_writer.cpp` | 128 / 370 |
| PDF 1.4 sheet writer | *(same header)* | `pdf_writer.cpp` | — / 457 |
| Locale-free number formatting, internal declarations | *(internal)* | `plan_text.cpp` + `plan_internal.h` | 59 / 86 |
| Tests | — | `tests/test_plan.cpp` | 2,213 |

---

## 2. The pipeline

```
gravity-aligned cloud (PageStore or span) + UpAxis
  │
  ├─ slice: band [z_min, z_max]  ─────────────┐        occupancy.cpp
  │    + include/exclude regions              │
  │    + (optional) A7 outlier filter         │
  │  → 2D occupancy grid, 2 cm, >=3 pts/cell  │
  │                                            │
  ├─ sill re-slice: band [0.35, 0.80] on the SAME lattice
  │
  ├─ sequential RANSAC over occupied cells ────┐        wall_extract.cpp
  ├─ merge collinear lines                     │
  ├─ dominant-direction estimate + ±7° snap    │
  ├─ face pairing → centreline + thickness     │
  ├─ gap analysis → WallSegment + Opening      │
  ├─ sill check → door vs window               │
  ├─ corner intersection trim / join / weld    │
  │
  ├─ planar-face traversal → rooms ────────────┐        rooms.cpp
  ├─ per-edge inward offset → interior polygon │
  └─ shoelace → area                           │
       │
       ├──────────────► DXF R12   (model space, 1 unit = 1 m)
       └──────────────► PDF 1.4   (A4/A3, scaled sheet)
```

Two entry points, one code path:

```cpp
Status extract_floor_plan(const PlanInput&, const PlanOptions&, PlanModel*,
                          PlanProgressCallback = nullptr, void* = nullptr,
                          PlanCancelToken* = nullptr);

Status extract_walls(const OccupancyGrid& grid, const OccupancyGrid* sill,
                     const PlanOptions&, PlanModel*);
```

The first slices and calls the second. The second is what the editor uses when
only a non-slice option changed, so a snap-tolerance slider does not re-touch
the cloud.

---

## 3. The decisions worth arguing about

### 3.1 RANSAC, not a Hough transform

A Hough accumulator over (ρ, θ) needs a bin size, and the bin size is exactly
what decides whether the two faces of a 150 mm partition are one wall or two —
the single most consequential judgement in the module. RANSAC puts that
decision on a distance in metres (`WallOptions::inlier_m`, 35 mm) that a user
can reason about; it hands back an explicit inlier set, so residual RMS,
coverage and a confidence fall out for free; and it degrades gracefully on a
non-Manhattan plan, where a Hough peak-picker starts inventing walls out of
accumulator ridges.

The cost is that sequential RANSAC needs three repairs, all of which are in the
spec anyway:

* **Collinear merge** (`merge_collinear`). One wall face two or three cells
  deep comes back as two or three near-coincident lines: the first pass takes
  the core of the band, a later pass fits the tail the first one's inlier
  distance did not reach. The same step also re-joins one straight wall that a
  long occlusion split into two lines. Lines that agree in heading (±4°) and in
  perpendicular offset (±50 mm, symmetric test) are the same wall.
* **A wall cannot be inside another wall** (`inside_paired_wall`). At a
  permissive `min_cell_points` the noise tails of a partition's two faces meet
  in the middle, and RANSAC will fit a line down the *middle of the wall* once
  the faces have been consumed. That line survives the collinear merge, because
  the merge threshold has to stay below half the thinnest partition. It is
  killed by the one thing that is always true: no wall lives inside another.
  `tests/test_plan.cpp`'s `a_noise_band_between_two_faces_is_not_a_third_wall`
  is that case, and it is why the default plan is correct at
  `min_cell_points` of both 2 and 3.
* **A mediocre hypothesis is fine.** Two seed cells 0.4 m apart in a 40 mm-wide
  band define a line with up to 0.1 rad of slope error, which over an 8 m wall
  is useless. Three total-least-squares refit rounds fix it: the first PCA runs
  over the ~100 cells the bad hypothesis *did* catch, and a 0.7 m sliver of 100
  cells fixes the direction to ~0.002 rad, which is good for the full 8 m. This
  is why 600 iterations is enough and why the adaptive early-out is safe.

### 3.2 `min_cell_points` defaults to 3, and that is measured

At the default 2 cm grid a wall face carrying 2 cm of range noise spreads over
about five cells, and the outer two collect roughly one point each. Accepting
those tails widens every fitted face to ~5 cells — wider than the 150 mm
partition the pipeline is trying to resolve *into two faces* — and RANSAC then
fits a third line down the middle of the wall. Three keeps the band at ~3
cells. A sparse or heavily decimated cloud may need 2 or 1; it is the one knob
that has to track point density, and `PlanStats::occupied_cells` is how you see
that it is wrong.

The A7 statistical outlier filter is available
(`SliceOptions::outlier_filter`) but is **off by default**. It costs a k-NN
pass over every in-band point — A7 §6.3 measured it as the slowest stage of the
whole post pipeline, 1.2 s of 2.3 s — and the cell-count threshold already
removes the isolated speckle that matters here. Clutter rejection in a floor
plan is a *density* question, not an isolation question.

### 3.3 Single-face walls are not inset — this is what makes the areas exact

A scanner inside a building sees the **inner** face of every exterior wall and
**both** faces of every partition it can walk around. So:

* two lines that are parallel, overlapping, and 60–400 mm apart become one
  `WallSegment` with `evidence = kPairedFaces` and a **measured** thickness;
* an unpaired line becomes `evidence = kSingleFace` with an assumed 100 mm
  thickness — but its fitted line **is the interior face**, not a centreline.

`WallSegment::room_inset_m()` therefore returns `thickness/2` for a paired wall
and **zero** for a single-face one, and that is what the room polygon is offset
by. Getting this wrong is not cosmetic: insetting every edge by an assumed
50 mm shrinks a 3.9 × 3.5 m room by 0.35 m², which is 2.6% — the entire error
budget, spent on an assumption.

### 3.4 Rooms are planar faces, not a flood fill

Flood-filling the free cells is the obvious approach and it is wrong twice
over. Door gaps let the fill leak from room to corridor to room, so a
three-room flat comes back as one region. And the area is quantized to the
grid: ±1 cell around a 20 m² room's perimeter at 2 cm is ±1.8% *before any
other error* — again the whole tolerance budget, spent on rasterization.

The wall network already knows where the doorways are (they are bridged
`Opening`s), so its planar faces are the rooms. Half-edges are sorted by angle
at each vertex, `next(h)` is one step clockwise from `twin(h)`, bounded faces
come out counter-clockwise and the unbounded one clockwise. Each face's edges
are then offset inward by their own wall's `room_inset_m()` and re-intersected,
and the area is a shoelace over that. Measured error on the fixture: **0.06% to
0.11%**.

Dangling edges are harmless: a free-standing segment (a bookshelf) is its own
connected component, its face has zero area, and it is dropped by
`min_area_m2` without touching the room it stands in.

### 3.5 Opening heuristics, and how far to trust them

Everything the module emits is a **candidate**. It sees a hole in a horizontal
slice; it cannot see a door leaf, a frame or a glazing bar.

| Gap width | Kind | Confidence |
| --- | --- | --- |
| < 150 mm | closed silently (noise / occlusion) | — |
| 150–600 mm | `kNarrowGap` | 0.25 |
| 600–1200 mm | `kDoorCandidate` | 0.60 |
| 1200–2000 mm | `kWideOpening` | 0.45 |
| > 2000 mm | **not bridged** — the wall genuinely stays broken | — |

Raising `max_bridge_m` above ~2 m starts fusing walls that have a real 3 m
opening between them, which is the wrong answer for an open-plan kitchen. The
honest output there is two walls and no opening.

**The window check** is the one heuristic with real evidence behind it. A
second occupancy grid is built on the *same lattice* from a band **below** the
main one (default 0.35–0.80 m, i.e. below a 0.90 m sill), and the gap is
sampled against it across the full wall thickness plus a cell:

* gap ≥ 55% occupied below → `kSolidBelow` → **window** (there is wall under
  it), confidence 0.70;
* gap ≤ 25% occupied below → `kOpenBelow` → **door** stands, confidence +0.20;
* anything in between, **or** a wall with < 35% sill-band support outside the
  gap → `kNoData`. The check abstains rather than guessing, because a scan
  taken from a tripod in a cluttered room frequently has no low-level coverage
  at all, and reporting every door in such a scan as "open below" would be
  worse than reporting nothing.

On the fixture the window and the two doors are the same width class
(1.10 m vs 0.90 m, both inside the door band). Without the re-slice all three
come back as doors — `the_window_is_told_apart_from_a_door_by_the_sill_reslice`
asserts exactly that, twice, with the check on and off.

### 3.6 Determinism

Two runs over the same cloud with the same options produce a **byte-identical**
`PlanModel`, DXF and PDF. That is the property A6/A7 established for the cloud
and it is a test here, not an aspiration:

* the cell list arrives in row-major order and the RANSAC uses this module's
  own seeded splitmix64 — not `<random>`, whose *distributions* are
  unspecified across implementations;
* nothing iterates a hash container; every sort is a total order;
* corner joins are computed from the pre-join geometry and applied afterwards,
  so the result does not depend on wall order;
* room labels are assigned after a sort whose keys are banded to 0.25 m, so two
  rooms whose centroids differ by a millimetre in y still sort left-to-right;
* every number written to either file goes through `plan_text.cpp`'s
  integer-arithmetic formatter — `std::snprintf("%f")` and
  `std::ostringstream` both honour `LC_NUMERIC`, and an app running under a
  `de_DE` locale would otherwise write `1,234` into a DXF coordinate and
  produce a file no CAD package can read (`src/gnss/nmea.cpp` hit this and
  documents it). `dxf_is_locale_free_ascii` asserts no comma appears anywhere
  in the file;
* the PDF writer has **no** `/CreationDate`, no `/ModDate` and no `std::time()`
  anywhere. The title block's date is a caller-supplied string.

---

## 4. The output model

```cpp
struct WallSegment {  // metres, plan frame
  uint32_t id; Vec2 a, b;            // centreline
  double thickness_m;                // measured if evidence == kPairedFaces
  WallEvidence evidence;             // kSingleFace | kPairedFaces
  double rms_residual_m, coverage;   // fit quality, occupied fraction of [a,b]
  uint32_t support_cells; float confidence; bool snapped;
  double room_inset_m() const;       // thickness/2, or 0 for a single face
};
struct Opening { uint32_t id, wall_id; Vec2 a, b; double width_m;
                 OpeningKind kind; SillCheck sill; double sill_occupancy;
                 float confidence; };
struct Room { uint32_t id; std::string label; std::vector<Vec2> polygon;
              double area_m2, perimeter_m; Vec2 centroid;
              float confidence; bool fully_measured; };
struct PlanModel { walls; openings; rooms; bounds; stats;
                   slice_z_min_m; slice_z_max_m; grid_res_m; up; };
```

`confidence` is `0.40·coverage + 0.35·(1 − rms/inlier_m) + 0.25·support`, plus
0.05 for a measured thickness, clamped to [0,1]. `fully_measured` is false when
any bounding wall's coverage is below 0.85 — a room reached through a doorway
is *not* fully measured, and the model says so instead of pretending.

The A1 seam (`FloorPlanExtractor`, `FloorPlan`, `Polyline2D`) is kept and is
now a **view** of `PlanModel` produced by `to_polylines()`. There is one
extractor, not two. `SliceOptions` keeps its five A1 fields with their A1
names and meanings — `tests/test_headers.cpp` instantiates it — with one
deliberate change: `snap_tolerance_deg` moves 5 → **7**, the figure the task
specifies and the one that actually covers the drift a hand-carried scan puts
into a nominally square room.

---

## 5. Editor v1 (§3.6)

`plan/plan_editor.h`. Everything is a pure function of `(cloud, PlanEditState)`
— no editor object, no session, no engine back-pointer, no thread. Every
mutator returns a **new** state, so the Qt desktop and the Android viewer get
undo/redo by keeping a `std::vector<PlanEditState>` and neither has to reason
about when the engine's copy went stale.

```cpp
struct PlanEditState { PlanOptions options; std::vector<PlanRegion> regions; };

bool edit_accepts(const PlanEditState&, double x, double y);
PlanEditState with_slice_band   (const PlanEditState&, float z_min, float z_max);
PlanEditState with_slice_center (const PlanEditState&, float z_center);
PlanEditState with_grid_resolution(const PlanEditState&, float res_m);
PlanEditState with_orthogonality(const PlanEditState&, bool, float tol_deg);
PlanEditState with_up_axis      (const PlanEditState&, UpAxis);
PlanEditState with_sill_check   (const PlanEditState&, bool, float, float);
PlanEditState with_include_region(const PlanEditState&, minx, miny, maxx, maxy);
PlanEditState with_exclude_region(const PlanEditState&, minx, miny, maxx, maxy);
PlanEditState without_region    (const PlanEditState&, size_t index);
PlanEditState with_regions_cleared(const PlanEditState&);
PlanRegion    normalized_region (const PlanRegion&);

Status recompute_grids(const PlanInput&, const PlanEditState&,
                       OccupancyGrid* main, OccupancyGrid* sill);
Status recompute_walls(const OccupancyGrid& main, const OccupancyGrid* sill,
                       const PlanEditState&, PlanModel*);
Status recompute_plan (const PlanInput&, const PlanEditState&, PlanModel*,
                       progress_cb, user_data, PlanCancelToken*);
```

**Region rule, in one sentence:** a point is kept when *(there is no include
region, or it is inside at least one include region)* **and** it is inside no
exclude region. Exclude always wins; no regions at all means keep everything,
so an untouched plan is the unedited plan.

**The cost model a slider needs.** Moving the slice band or a rectangle changes
which points land in the grid, so the grid must be rebuilt — one streaming pass
over the cloud, no copy. Changing snapping, tolerances, opening widths, or any
`WallOptions`/`RoomOptions` value does **not** touch the cloud: hold the two
`OccupancyGrid`s from `recompute_grids()` and call `recompute_walls()`, which
runs in milliseconds. `editor_slice_slider_and_the_cached_grid_path_agree`
asserts the fast path and the slow path produce coordinate-identical plans —
otherwise the live preview would lie about what the export will contain.

Mutators validate so a UI can wire a slider straight to them: inverted handles
are swapped, a zero-height band is widened to 1 cm, resolution clamps to
[2 mm, 1 m], snap tolerance to [0°, 45°], rectangles are normalized, and an
out-of-range `without_region()` index is a no-op rather than an error.

---

## 6. Extraction quality on the synthetic building

`tests/test_plan.cpp` builds a two-room-plus-corridor building whose true
geometry is declared in metres at the top of the file. Only the faces a
scanner inside would see are sampled: the **inner** face of each exterior wall,
**both** faces of each 150 mm partition. Every face carries **σ = 20 mm** of
Gaussian noise on its normal plus 5 mm along it and in z, sampled every 20 mm
horizontally and 40 mm vertically from 0.05 m to 2.40 m. Clutter: a low table
at 0.72–0.76 m, a 0.5 × 0.4 m cabinet standing 1.85 m tall in the middle of
room B, 400 speckle points inside the band, and a full floor and ceiling.
**146,436 points; 30,243 in the 1.0–1.5 m band; 4,992 occupied cells; 8 RANSAC
lines.** The case `plan/extraction_quality_report` prints the table below on
every run (`scanengine_tests --test-case=plan/extraction_quality_report -s`),
so these figures are regenerated, not remembered:

```
walls: 6 found, 6 expected, 0 false positives

wall          endpoint err   thickness (true)   fit RMS    coverage   confidence
south            0.86 mm     0.100  (assumed)   16.9 mm     1.000       0.83
north            0.65 mm     0.100  (assumed)   16.8 mm     0.861       0.76   <- window
west             0.86 mm     0.100  (assumed)   17.0 mm     1.000       0.83
east             0.65 mm     0.100  (assumed)   17.4 mm     0.826       0.83
partition-h      0.38 mm     0.150011 (0.150)   15.8 mm     0.775       0.80   <- 2 doors
partition-v      0.75 mm     0.149845 (0.150)   15.8 mm     1.000       0.89

  endpoint err = worst of the two trimmed corners against the declared geometry
  fit RMS      = the 20 mm face noise, clipped by the 35 mm inlier distance
  thickness    = MEASURED for the two paired partitions, to 0.2 mm

orthogonality: all 6 walls within 0.05 deg of the two dominant axes
dominant direction: 89.996 deg (i.e. 0 deg mod 90), 8 lines snapped, 2 paired

openings      true width   extracted   kind      sill verdict     occupancy
door A          0.90 m      0.920 m    door      open-below         0.02
door B          0.90 m      0.900 m    door      open-below         0.01
window          1.10 m      1.120 m    window    solid-below        0.97
  window centre: x = 1.540 m (true 1.550), y = 5.000 m (true 5.000)

rooms          true          extracted     error
corridor    11.2000 m2      11.1963      -0.033 %
room A      13.2825 m2      13.2843      +0.014 %
room B      13.8000 m2      13.7979      -0.015 %
```

Room areas land within **0.04%** — twenty times inside the 2% the task asks
for — because the areas come from re-intersected fitted lines rather than from
counting cells (§3.4), and because single-face walls are not inset (§3.3).
Opening widths are within **20 mm**, which is one grid cell: a gap is measured
between the padded ends of two cell runs, so one cell is the floor of what a
2 cm grid can resolve.

Runtime: **~0.2 s** for the whole extraction (4,992 cells, 8 RANSAC lines) on
an M-series Mac, RelWithDebInfo.

Variants asserted in the same file:

* **Rotated 4°** — the dominant-direction estimate returns 3.99°, all six walls
  snap to the building's own axes (within 0.05°), and the total area is
  unchanged. Snapping follows the *building*, not the world.
* **A 30° wall added** — 23° outside the ±7° window, so it comes back at 30.0°
  with `snapped == false`, while the six Manhattan walls still snap.
* **Snapping off** — six walls, each within 1° of square without any help.
* **`min_cell_points = 2`** — still six walls and three correct rooms (§3.1).
* **4 cm grid** — six walls, areas within 3%.
* **A different RANSAC seed** — six walls, areas within 2%.
* **A band at 2.15–2.35 m** (above the window head and the door heads) — no
  opening anywhere near the window.
* **`max_bridge_m = 0.5`** — the partition comes back as three separate wall
  segments and no opening, which is the correct answer when the pipeline is
  told not to bridge a 0.9 m gap.
* **`PageStore` input across 8 pages** — coordinate-identical to the span path.

---

## 7. DXF and PDF, and why they live in `plan/`

`export/` writes point clouds *streamed out of a `PageStore`*; these write a
`PlanModel`, which is already in memory and is a completely different shape.
`ExportFormat::kDxf` / `kPdf` stay declared in `export/exporter.h` as the
app-facing enum values — routing those two to `plan::write_dxf` /
`plan::write_pdf` is the job layer's (A15) one-line dispatch, and is not a
reason to give `export/` a dependency on `plan/`. A9's test that `kDxf`/`kPdf`
are rejected by `export_points()` is still correct and still green.

Both writers take the **same model**. There is no "DXF plan" and "PDF plan":
what is drawn, and where, was decided once by the extractor; the writers only
choose representation. Both share `wall_footprints()`, which splits a wall's
footprint at its openings so a doorway is a **hole in the wall** rather than a
line drawn on top of a solid one.

### DXF — R12 (AC1009), ASCII

R12 is the last revision every CAD, GIS and drafting tool reads without
argument: no LWPOLYLINE (so a polyline is `POLYLINE` + `VERTEX`s + `SEQEND`),
no OBJECTS or CLASSES section, no required handles, no encoding declaration. A
floor plan of straight walls, gap markers and area labels needs nothing newer,
and every feature we would gain costs a reader somewhere. ASCII rather than
binary so a failure is diffable. Line endings are `\n` (accepted by every
reader; byte-identical on every platform this engine builds for).

Model space, **1 drawing unit = 1 metre**, `$INSUNITS = 6`, `$EXTMIN`/`$EXTMAX`
bracketing everything drawn *including* the dimension lines that sit outside
the geometry.

| Layer | Colour | Contents |
| --- | --- | --- |
| `WALLS` | 7 | one closed 4-vertex `POLYLINE` per **solid piece** of wall |
| `OPENINGS` | 1 | a 2-vertex `POLYLINE` across each gap + a `DOOR 0.88` / `WINDOW 1.10` label |
| `ROOMS` | 3 | one closed `POLYLINE` per room, on the interior face |
| `DIMENSIONS` | 4 | room name + `13.28 m2` text, and the overall bounding dimensions |

Layer names are sanitized into R12's alphabet (uppercase, no spaces, ≤ 31
chars) rather than written out and rejected at the far end.
`DxfOptions::wall_footprint = false` emits bare centrelines instead, for a
downstream tool that wants to re-derive its own thickness.

### PDF — 1.4, six objects, no dependencies

Header line, six numbered indirect objects, a cross-reference table of their
byte offsets, a trailer. The six are always the same: catalog, page tree, page,
content stream, Helvetica, Helvetica-Bold. Helvetica is one of the 14 standard
Type 1 fonts a conforming reader must supply, so nothing is embedded and the
file is ~3 kB. No compression: `/FlateDecode` would need zlib, a dependency
this engine does not have and does not want (A9's reasoning), and a floor
plan's content stream is small enough that it does not matter.

Sheet: A4 or A3, portrait or landscape, at a scale off the standard ladder
(1:20, 1:25, 1:50, 1:100, 1:200, 1:500, 1:1000, 1:2000, 1:5000). `auto` walks
it in order and stops at the first entry the plan fits at, so it picks the
largest drawing that fits, which is what a drafter would do; a non-zero
`scale_denominator` pins it and the plan is clipped if it does not fit, because
a pinned scale is an instruction, not a hint. 1 m at 1:D is 1000/D mm =
(1000/D)·72/25.4 pt. The plan is centred in the sheet minus margins minus the
title-block strip, and clipped to that rectangle.

Drawn: poché-filled wall footprints with the openings punched out, dashed
threshold lines across the openings, room name + area at each centroid, a
subdivided scale bar with its round-metre length chosen so it lands between 45
and 150 pt, a **north arrow**, and a title block carrying project, scale,
sheet, slice band, grid resolution, wall/opening/room counts, and any
caller-supplied date/drawn-by/reference.

The north arrow is a **placeholder and is labelled as one on the sheet**:
`N (ASSUMED)`, drawn along plan +y. The engine has no heading for an
ungeoreferenced indoor session. A10's georeferencing is what turns it into a
real bearing — set `north_angle_deg` (clockwise from plan +y) and
`north_known`, and the label becomes a plain `N`.

### How they are verified

`tests/test_plan.cpp` contains readers written **from scratch** — the A9
posture — that share no code, no constant and no helper with the writers.

**DXF:** a group-code tokenizer (alternating code line / value line) that
rebuilds sections, the header variables, the LAYER and STYLE tables, and the
entity list. It parses numbers with its **own** locale-independent parser, so
it can actually catch a writer emitting `1,234` — `strtod` under the same
`de_DE` locale would happily accept it. It checks: `$ACADVER = AC1009`; all
four sections present; the four layers and the `STANDARD` text style defined;
every `POLYLINE` sealed by a `SEQEND` with its layer repeated on every
`VERTEX`; every coordinate inside `$EXTMIN`/`$EXTMAX`; that the number of wall
polylines equals `walls + openings` (each opening splits its wall); that the
east wall's four footprint corners, **recomputed in the test from the model**,
appear in the file to 0.1 mm; that each opening polyline's own length equals
the model's `width_m`; that each room polyline's **shoelace area, recomputed
from the file's vertices**, equals the model's `area_m2`; that the room labels
and their `m2` numbers round-trip; that no comma and no non-ASCII byte appears
anywhere; and that an empty plan still produces a structurally valid file.

**PDF:** a structural reader that seeks `startxref`, checks it points at
`xref`, walks the table asserting every entry is **exactly 20 bytes**
(`nnnnnnnnnn ggggg n \n`), and — the point of the exercise — verifies each
recorded offset lands exactly on `<k> 0 obj` **in the bytes**, not on what the
writer thought it wrote. Then: `/Size` matches the object count, `/Root`
resolves to a `/Catalog`, the page tree and page dictionaries are well formed,
the `MediaBox` is A4 landscape to 0.001 pt, and the content stream's declared
`/Length` equals the actual byte count between `stream\n` and `endstream`.

Geometry is spot-checked by parsing the `m`/`l` operators inside the first
balanced `q … Q` block (where the plan is drawn), then asserting that **every**
footprint corner of every un-opened wall, mapped through the page bounding box
and the scale the sheet claims, is present to 0.05 pt. That pins the scale, the
origin and the shape at once using nothing but the bytes. Also asserted: the
scale label matches the ladder choice, a pinned 1:200 really is 1:200, nothing
falls outside the MediaBox, the room labels are on the sheet, the caller's
title-block strings are there, and `/CreationDate` and `/ModDate` are **not**.

---

## 8. Honest limits

1. **Furniture against a wall reads as a wall.** A 1.4 m bookshelf 0.55 m off a
   wall is flat, long and vertical; nothing in a horizontal slice can tell it
   from architecture. `furniture_against_a_wall_reads_as_a_wall_documented_limit`
   asserts that it *does* appear as a seventh wall, and that it does not
   corrupt anything else — it is a separate connected component in the planar
   graph, so the three room areas are unchanged. The mitigations that exist:
   it is `kSingleFace` (it is further from the wall than `thickness_max_m`, so
   it does not pair and inflate that wall into a 0.55 m thick one), and the
   §3.6 editor's exclude rectangle removes it in one drag. What would actually
   fix it is multi-height consistency (a wall is present in *every* band) —
   that is a Phase 3 change, not a tuning change.
2. **Furniture between 0.06 m and 0.40 m from a wall and longer than 0.6 m will
   pair with it** and shift that wall's centreline by half the gap. A deep
   skirting board or a run of radiators is the realistic case. `thickness_max_m`
   is the only defence and lowering it costs you real 300–400 mm exterior
   walls.
3. **Non-Manhattan rooms.** Curved and angled walls are *extracted* correctly —
   the 30° test proves a wall outside the snap window survives untouched — but
   two things degrade. A curve is approximated by a chain of chords, one per
   RANSAC line, and the chord count depends on `inlier_m`, not on any curvature
   estimate; there is no arc primitive in the model or in either writer. And
   the dominant-direction estimate is a single circular mean with π/2 symmetry,
   so a building with **two** unrelated grids (a wing at 30° to the main block)
   gets one θ₀ somewhere between them and snapping should be turned off. A
   per-cluster dominant direction is the fix and it is not implemented.
4. **The room inset is a naive offset.** Consecutive edges are offset and
   re-intersected with no self-intersection handling, so a deeply non-convex
   room whose inset would collapse a narrow neck produces a self-crossing
   polygon and an area that is wrong by that overlap. Rectilinear and convex
   rooms — which is what §3.6's target audience surveys — are exact.
5. **A room needs a closed cycle.** A wall that RANSAC missed entirely (a fully
   occluded alcove, a glass partition) leaves the face open and the room is not
   reported at all. It fails silently-by-absence, not silently-by-wrong-number,
   which is the better of the two, but a UI should show `walls` and `rooms`
   counts side by side so the user notices.
6. **`max_bridge_m` is a genuine trade.** At 2.0 m a real 2 m archway is
   reported as an opening in a continuous wall; above that, unrelated walls
   start fusing. There is no width at which both are right.
7. **Glass.** A window that returns a specular hit at 1.2 m is a wall in the
   main band and a wall in the sill band, so it is not an opening at all. A
   window that returns nothing is handled correctly. Partial returns land in
   the ambiguous middle and come back `kNoData`. Lidar cannot resolve this;
   camera colorization (A11) could, and that is where a real fix would live.
8. **Multi-storey and sloped floors are out of scope.** One band, one plan. A
   split-level or a ramp puts two different floors in one slice, and the
   pipeline will happily merge them into one nonsensical plan. §3.6 does not
   ask for storey segmentation and this does not attempt it.
9. **The plan frame is not georeferenced.** Coordinates are the session's local
   metric frame, relabelled. The DXF has no CRS and the PDF's north arrow says
   `ASSUMED`. A10 is what changes both.
10. **Performance is O(iterations × cells) per line.** 5,000 cells and 8 lines
    is 0.2 s. A 40 × 40 m building at 2 cm with 60,000 occupied cells and 40
    walls is roughly 2 s — acceptable for a desktop "extract plan" button,
    marginal for a live slider on Android. The mitigation that exists is the
    cached-grid path (§5): the slider's expensive part is the slice, not the
    RANSAC, and `kMaxGridCells` (40 M) fails loudly with a message naming the
    resolution rather than allocating 4 GB.

---

## 9. Verification

Host: macOS 15 (Darwin 25.5.0), Apple silicon, Ninja, AppleClang.

```
$ cmake -S engine -B <scratch> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build <scratch>                       # 133 targets, no warnings
                                                #   from src/plan or tests/
$ ctest -LE 'sim|sim-rtk'
    Start 1: scanengine_tests ......... Passed
    Start 2: scanengine_capi_smoke .... Passed
    Start 3: engine_cli_selftest ...... Passed
    Start 4: engine_cli_version ....... Passed
100% tests passed, 0 tests failed out of 4

$ <scratch>/scanengine_tests
[doctest] test cases:     445 |     445 passed | 0 failed | 7 skipped
[doctest] assertions: 2275831 | 2275831 passed | 0 failed |
```

A separate `-DENGINE_WARNINGS_AS_ERRORS=ON` configure builds clean; the only
warnings anywhere in the tree are the pre-existing deprecation warnings inside
vendored `third_party/Livox-SDK2`. Of the 445 cases, **52 are A12's**
(`plan/*`, 8,633 assertions); all pre-existing cases, including `test_headers.cpp`'s
instantiation of `SliceOptions` and `test_display_params.cpp`'s assertion that
the `kFloorPlan` display profile matches the §3.6 slice band, are untouched and
green.

Both output files were also opened outside the test suite: the PDF renders
correctly through macOS Quick Look (walls poché-filled, doorways and the window
as gaps, three labelled rooms with areas, scale bar, north arrow, title block).
No CAD package is installed on this machine, so the DXF's only verification is
the from-scratch reader above — the same standing caveat A9 records for LAS.

Build directories were deleted afterwards.
