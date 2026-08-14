# A9 — export writers

**Status:** complete. PLY (binary_little_endian + RGB), LAS 1.4 (point formats
2 and 7, hand-rolled 375-byte header, WKT VLR), PCD (binary, PCL field
convention) all implemented, dependency-free, streaming from `cloud/PageStore`
without holding the whole cloud in memory.

Files (all under this task's ownership):

```
engine/include/scanengine/export/exporter.h        the public API (SEAM, extended)
engine/include/scanengine/export/las_constants.h    public LAS layout constants + CRS placeholder
engine/src/export/point_source.h                    shared page-select/filter/decimate/2-pass iterator
engine/src/export/byte_io.h                          little-endian byte-packing helpers
engine/src/export/ply_writer.{h,cpp}
engine/src/export/pcd_writer.{h,cpp}
engine/src/export/las_writer.{h,cpp}
engine/src/export/exporter.cpp                        export_points() dispatch + Exporter/make_exporter
engine/tests/test_export.cpp                          17 cases, all with from-scratch readers
```

No CMakeLists.txt edit was needed or made: `add_library(scanengine ...)` already
globs `src/*.cpp` recursively (`CONFIGURE_DEPENDS`) and `scanengine_tests` globs
`tests/test_*.cpp` the same way, so the new files were picked up on the next
configure. No new dependency is required — everything is hand-rolled against
the C++ standard library only, as the task asked.

## API

```cpp
Status export_points(const PageStore& store, Span<const StreamId> streams,
                      ExportFormat format, const std::string& path,
                      const ExportOptions& options,
                      ExportProgressCallback progress_cb = nullptr,
                      void* progress_user_data = nullptr,
                      ExportCancelToken* cancel_token = nullptr);
```

This is the one entry point `jobs/` (A15) calls. `streams` empty = every page
in `store` regardless of stream (a merged multi-sensor export); `options`
carries `include_color` / `include_intensity` (from A1's original seam),
plus A9's additions: `crs_wkt`, `bounds_filter`, `decimate`, `las_gps_time`.
`ExportCancelToken` is a tiny poll-based atomic flag; `ExportProgressCallback`
is a plain function pointer (no allocation, matches the rest of the engine's
C-ABI-friendly style) called with a monotonically non-decreasing fraction in
`[0, 1]`.

The original `Exporter` interface + `make_exporter()` factory are kept (a
test-suite constant, `tests/test_headers.cpp`, already instantiates
`ExportOptions` and is not this task's file to edit) and now delegate to
`export_points()` internally, so there is exactly one code path regardless of
which entry point a caller uses.

## Two-pass streaming, not a full-cloud buffer

Every writer makes two passes over the pages **already resident in the
PageStore** — no copy of the cloud is ever made:

1. **Count (+ for LAS, exact bounds)** of the points that survive
   `streams` / `bounds_filter` / `decimate`, needed because PLY's
   `element vertex N`, PCD's `WIDTH`/`POINTS`, and LAS's header all have to
   be written *before* the point data and must be exact, not estimated.
2. **Write**, re-running the identical selection predicate
   (`point_source.h`'s `point_selected()`), so pass 1 and pass 2 can never
   disagree about which points are included — that would otherwise corrupt a
   header silently.

Cancellation is checked every 4096 raw points (not 4096 *selected* points, so
a heavily-filtered export over a huge cloud still cancels promptly); on
cancellation the partial output file is deleted and `kCancelled` is returned.

## Format notes

**PLY** — `binary_little_endian 1.0`, `x/y/z float32`, optional
`red/green/blue uchar`, optional `intensity uchar`. Property list adapts to
`include_color`/`include_intensity`.

**PCD** — binary, PCL field convention: `x y z [rgb] [intensity]`, all
`FLOAT32`. `rgb` is a `uint32_t` `(r<<16|g<<8|b)` reinterpreted as a float's
bit pattern (via `memcpy`, not a numeric cast — the standard PCL trick).

**LAS 1.4** — the 375-byte Public Header Block is built field-by-field at its
literal spec offsets (`las_writer.cpp`'s `build_header()`), not via a packed
struct, to sidestep any compiler padding/alignment risk. Point counts are
written to **both** the legacy 32-bit fields (offset 107/111, populated only
for point formats < 6 whose count fits in 32 bits — zero otherwise, per
spec) **and** the 64-bit fields (offset 247/255, always populated and
authoritative). Scale/offset are chosen from the exact post-filter bounds:
offset = bounds midpoint rounded to a whole metre, scale = 1 mm, widened by
decades only if that would overflow a signed int32 (PointVertex positions are
already in the session's local metric frame, so this only matters for
pathological synthetic data). One VLR (`LASF_Projection`, record id 2112,
global encoding bit 4 set) carries the CRS as OGC WKT.

- **Point format 2** (26 bytes/point): RGB, no time, LAS 1.2+ compatible.
- **Point format 7** (36 bytes/point): RGB + GPS time, LAS 1.4-only, selected
  via `ExportOptions::las_gps_time`.

## The "no per-point intensity channel" bridge

`cloud/point_page.h`'s `PointVertex` is `{x,y,z,r,g,b,a}` — there is no
separate intensity field today, and `PageStore::append()` takes one
timestamp per batch, not per point. Two documented bridges, both isolated to
`src/export/point_source.h` so they're a one-place swap when real data
exists:

- **Intensity**: `luminance8()`/`luminance16()` derive it from RGB (BT.601
  weights). This is *exact* for an unmodified D6 capture — `d6_driver.cpp`
  currently writes raw sensor intensity straight into R/G/B — and becomes an
  approximation once real camera colorization (A11) overwrites RGB. A real
  channel is an A1/A14 call (extend `PointVertex`, or a parallel per-page
  buffer); nothing in A9's public API would need to change to pick it up —
  only `point_source.h`'s two functions.
- **LAS format 7 GPS time**: linearly interpolated across each page between
  its `t_first_ns`/`t_last_ns` by in-page index, in engine-monotonic seconds
  — not a real GPS time base (there isn't one in the engine yet; that's
  A4/A10). Proves the field's byte layout and the format-7 code path are
  correct; a real per-point GPS time source is a drop-in replacement at one
  call site in `las_writer.cpp`.

## CRS seam

`export/exporter.h`'s original comment said ungeoreferenced sessions should
fail LAS export with `kInvalidState`. That policy needs data `export/` does
not have — `PageStore` carries no georeferencing state, only points — so it
cannot be enforced here. The decision moved up the stack, to whoever calls
`export_points()`: **pass `ExportOptions::crs_wkt`** (the caller's real CRS,
from the session manifest / A10's picker) for a georeferenced export, or
leave it empty. An empty `crs_wkt` does **not** produce a refusal or a
mislabeled file — it embeds `kLasLocalFramePlaceholderWkt`
(`las_constants.h`): an OGC WKT2 `ENGCRS` ("Engineering CRS") named
`"LidarScan Local/Ungeoreferenced Frame"`, with no datum/ellipsoid, so a tool
that tries to reproject it fails loudly instead of silently treating local
metres as a real-world coordinate system. This is what a bench/lab capture
with no GNSS fix should produce, and it's what A10 needs to change nothing
about — just start passing a real `crs_wkt` once it has one.

## Round-trip validation

`tests/test_export.cpp`, 17 cases / 2102 assertions, all green. Every format
is checked with a reader **written from scratch in the test file** — it does
not call anything in `src/export/*.cpp`:

- **PLY**: header parsed as text (property list, `element vertex` count),
  binary body decoded manually. Round-trip is bit-exact (`CHECK(a == b)`, no
  tolerance) because float32 in, float32 out, involves no lossy conversion.
- **PCD**: header parsed as text, `rgb` unpacked by hand
  (`memcpy` + bit-shifts) independent of the writer's packer.
- **LAS**: header fields decoded at their **literal byte offsets copied from
  the ASPRS LAS 1.4 spec table** (`test_export.cpp`'s own `rd_u16`/`rd_u32`/
  `rd_u64`/`rd_f64` helpers over the raw file bytes) — not from
  `las_constants.h`'s offsets, so a wrong offset in the writer would show up
  as a wrong decoded value, not as two copies of the same mistake agreeing.
  Checked: signature, version 1.4, header size 375, point format (2 vs 7),
  point record length (26 vs 36), the WKT global-encoding bit, VLR count,
  **both** the legacy 32-bit point count *and* the new 64-bit point count
  (and that format 7 correctly zeroes the legacy fields), scale/offset,
  min/max bounds (cross-checked against independently-computed bounds from
  the synthetic input, not just internal self-consistency), the VLR's user
  id/record id/payload (placeholder vs. caller-supplied WKT), and per-point
  X/Y/Z decoded via `int32 * scale + offset` and compared to the float32
  input within `scale/2` (the quantization tolerance), plus exact
  intensity/RGB and (format 7) GPS time bounded within the source page's
  `[t_first_ns, t_last_ns]` and monotonic in append order.
- Also covered: multi-page streaming (page_capacity forced small so a 250-point
  export spans several pages — checks no loss/duplication/reordering at a
  page boundary), `include_color`/`include_intensity` toggling in all
  combinations, `decimate` (stride correctness), `bounds_filter` (crop
  correctness), `streams` filtering (multi-stream store, export one stream),
  progress-callback monotonicity and terminal `1.0`, cancellation (partial
  file removed, `kCancelled` returned), the `Exporter`/`make_exporter()`
  interface end to end, and `kDxf`/`kPdf`/empty-path rejection.

## External validator

Checked for both `pdal` and `cloudcompare`/`CloudCompare` via `which` and
`brew list` (formula + cask): **neither is installed on this Mac**, and per
the task instructions no GUI tool was installed to get one. External
validation was not possible this run; the from-scratch reference-reader
round-trip above is what stands in for it. If PDAL is ever added to this
machine, `pdal info --metadata <file>.las` would be the natural one-command
cross-check against the header fields this doc already lists.

## Build/test verification

Clean configure + build (`cmake --preset macos-universal` into a scratch
directory, deleted afterward) + `cmake --build` (95 targets, zero warnings
from anything under `src/export/`, `include/scanengine/export/`, or
`tests/test_export.cpp` — the only warnings anywhere in the build are
pre-existing deprecation warnings inside vendored `third_party/Livox-SDK2`)
+ `ctest -LE sim`: **224/224 passed** for every pre-existing test plus this
task's new `test_export.cpp` (17/17, 2102/2102 assertions). One unrelated
failure was observed in `poses/*` / `pushbroom/*` cases inside
`test_pushbroom.cpp` — that file is untracked, mid-flight work from a
different concurrently-running task (A8, `slam/pushbroom/`), not part of
this task's ownership and not touched by anything in this change; excluding
just those two prefixes, the full remaining suite (`test-case-exclude`)
passes 224/224 with 13,360/13,360 assertions.

No CMakeLists.txt edit was made or is needed; A9 has no new dependency to
hand off to A8.
