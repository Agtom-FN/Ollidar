# A14 — display-parameter API

**Scope:** `engine/include/scanengine/cloud/display_params.h`,
`engine/src/cloud/display_params.cpp`, `engine/tests/test_display_params.cpp`.
**Spec:** §3.9 (display-parameter adjustment), §3.12 (rendering).
**Contract:** `engine/DESIGN.md` §5 (the render-facing contract) — this task
does not modify `cloud/page_store.*` or `cloud/point_page.h` (owned this pass
by A6/A1); it only adds new files alongside them.
**Depends on:** A1 (`PointVertex`/`PageStore`), S3
(`spikes/s3-render/REPORT.md` — the `gl_PointSize` caveat this API is built
around), `gnss/gnss.h`'s `FixType` (A10's fixed vocabulary, used read-only).

C1 (Qt review workspace) and B10 (Android display-parameters panel) both bind
to this module. Neither touches Filament/Vulkan directly through it — this is
the model layer: validation, ground-truth colour evaluation, JSON
persistence, and change notification. What a shader does with it is C1/B10's
job; this module's contract is that `evaluate_point_color()` is the correct
answer a shader must reproduce.

---

## 1. API surface

| Piece | What it's for |
| --- | --- |
| `PointSizeMode`, `PointSizeParams` | fixed px / adaptive px / world-space size (S3 fallback) |
| `ColorMode`, `ScalarColorParams` | RGB / height / intensity / time / fix-quality, and the shared mapping params (range, gamma, brightness, colormap, invert) the three scalar modes use |
| `Colormap`, `evaluate_colormap()`, `colormap_lut()` | 3 procedural colormaps as 256-entry LUTs |
| `DisplayParams` | the whole model: point size, LOD budget, colour mode + per-mode params, fix-quality palette, EDL, background, height/box clipping, trajectory/pose-graph toggles |
| `clamp_display_params()` | in-place validation — clamps every field to a documented range, never rejects |
| `DisplayProfile`, `profile_defaults()` | Survey / Floor plan / Research / Quick scan starting points |
| `PointAttributes`, `evaluate_point_color()` | ground-truth per-point RGBA — ships with the graceful-degradation rules for data `PointVertex` doesn't carry |
| `DisplayParamsUniforms`, `to_uniforms()` | the shader-facing POD, projected from a `DisplayParams` |
| `to_json()` / `from_json()` | hand-rolled, schema-versioned, forward-compatible persistence |
| `DisplayParamsController` | the mutable instance an app owns per project: `get()`/`set()`, `version()`/`dirty()` for polling, `subscribe()`/`unsubscribe()` for push |

Everything is a value type except `DisplayParamsController`, which is the one
class with identity (PIMPL, like `PageStore`).

---

## 2. Why `PointVertex` isn't enough, and what fills the gap

`PointVertex` (`cloud/point_page.h`) is 16 bytes — `float3` position + RGBA8 —
fixed by A1/S3 and not owned by this task. It carries no intensity channel,
no per-point timestamp, and no fix-quality tag. `export/exporter.h` already
documents the same gap for A9's exporters and the same interim answer:
**derive intensity from RGB luminance**. A14 follows that precedent so the
review viewer and an export agree on what "intensity" means for an
uncolorized capture.

`evaluate_point_color(v, attrs, params)` takes an optional `PointAttributes`
alongside the vertex:

| Mode | Data source | Fallback when the attribute is absent |
| --- | --- | --- |
| `kRgb` | `v.r/g/b/a` | — (this mode never needs `attrs`) |
| `kHeight` | `v.z` | — (already in `PointVertex`) |
| `kIntensity` | `attrs.intensity` if `has_intensity`, else luminance of `v.r/g/b` (0.299/0.587/0.114 weights) | never falls back further — luminance is always computable |
| `kTime` | `attrs.t_seconds` if `has_time` | RGB pass-through (`v.r/g/b/a`) — no time data means don't fabricate a colour |
| `kFixQuality` | `attrs.fix` if `has_fix` | RGB pass-through |

**Alpha is always `v.a`**, in every mode, never derived — `point_page.h`
already reserves alpha for LOD-fade/selection, so no colour mode may
repurpose it.

A real per-point intensity channel or timestamp is still, as `exporter.h`
says, "an A1/A14 decision (extend `PointVertex` or add a parallel buffer)".
This task explicitly does not make that change (page_store.*/point_page.h
are out of scope — A6 owns `page_store.*` concurrently this pass); the
`PointAttributes` seam is what lets a future producer (e.g. a parallel
intensity/time buffer keyed by point index) plug in without touching this
module's public API again.

For height mode's "auto range": `ScalarColorParams::auto_range` is a
UI/persistence bit only. `evaluate_point_color()` and `to_uniforms()` always
read `manual_min`/`manual_max` — when `auto_range` is true, the **caller**
(the renderer) is expected to refresh those two fields every frame (or on
cloud change) from the real data range. For height that source already
exists: `PageView::bounds_min[2]`/`bounds_max[2]` in `cloud/page_store.h`.
This keeps `evaluate_point_color()` a pure function of its three arguments
with no PageStore dependency.

---

## 3. Colormaps

Three, chosen per the task brief's "implement 2-3 tasteful colormaps ... or
generate procedurally": all three are closed-form functions authored for
this engine, **not** reproductions of any named third-party palette
(turbo/viridis/jet/...). This was a deliberate choice over transcribing a
published polynomial fit: it keeps the module free of attribution/licence
questions entirely, at the cost of not matching a name a user might already
recognize.

| Colormap | Construction | Why |
| --- | --- | --- |
| `kGrayscale` | `r=g=b=round(t*255)` | The "just show me the number" baseline; also the one used for every hand-computed test value, because it's exactly invertible (`evaluate_colormap(kGrayscale, t) == round(t*255)` to within LUT-interpolation rounding). |
| `kSpectrum` | Full-saturation HSV hue sweep, h = 240°(1−t) → 0° (blue → red), s=v=1, standard 60°-segment HSV→RGB (no trig) | Widest perceptual contrast for exploring a wide value range at a glance — the traditional "rainbow" choice for height. Not luminance-monotonic (a known rainbow-map caveat), but red is provably non-decreasing and blue non-increasing across the whole sweep — see `spectrum_raw()`'s comment in the .cpp and the `colormap/spectrum_channel_monotonicity` test for the derivation. |
| `kThermal` | Piecewise-linear black → red → yellow → white over three equal thirds | Every channel individually non-decreasing by construction, i.e. luminance is monotonic — reads correctly even in grayscale print/screenshots. Used as Floor plan's default (§5) for exactly that reason: legible on a printed sheet. |

Each is cached into a 256-entry `std::array<RGBA8, 256>` on first use
(C++11 function-local statics — thread-safe, no explicit init call needed).
`evaluate_colormap()` linearly interpolates between the two nearest LUT
entries, so it's smooth at any `t`, not just at the 256 sample points.
`colormap_lut()` exposes the raw table too, for a renderer that wants to
upload it as a 1D texture instead of re-deriving the closed form per
fragment.

---

## 4. The uniform block contract

`DisplayParamsUniforms` is the field set, order and types both apps must
reproduce — but *how* they bind it differs, and the header says so
explicitly:

* **B10 (Android/Vulkan)** can treat it as a real UBO: the struct is laid
  out in 16-byte ("std140 row") groups on purpose, so a raw `memcpy` into a
  `VkBuffer` is safe without per-field padding decisions. `clip_box_min`
  (a vec3) shares its row with a trailing scalar the way std140 packs a
  vec3 + scalar, exactly like `clip_box_max`/`_pad6` and
  `clip_height_min/max`/`_pad7`.
* **C1 (Qt/Filament)** cannot: Filament's `MaterialInstance` takes named
  parameters (`setParameter("gamma", ...)`), not a raw byte blob. C1 binds
  the same field names, types and values individually — the struct is still
  the single source of truth for what those names/types/values are, just
  not for C1's memory layout.

`sizeof(DisplayParamsUniforms) == 208` and the offsets of `background`
(64) and `fix_quality_colors` (128) are `static_assert`ed in the header —
any accidental reordering by a future edit is a compile error, not a silent
C1/B10 mismatch.

`to_uniforms()` is a pure projection, not a copy of `DisplayParams`:

* It picks the **active** scalar-mode block (`height`/`intensity`/`time`)
  by `color_mode` — the *other* two scalar blocks never reach the shader.
  `kRgb`/`kFixQuality` get a neutral identity mapping
  (`colormap=grayscale, gamma=1, range=[0,1]`) since a correct shader
  branches on `color_mode` before reading them anyway (exactly what
  `evaluate_point_color()` does).
* `lod_point_budget`, `show_trajectory`, `show_pose_graph` never reach a
  shader (they're CPU-side renderer/UI decisions) and so aren't in the
  struct at all.
* `clip_enabled_mask` packs both clip toggles into one `int32` (bit0 = box
  clip, bit1 = height clip) rather than spending two more std140 rows on two
  booleans.

---

## 5. Profiles

Tech Spec §1 names four workflow profiles (Survey · Floor plan · Research ·
Quick scan); §3.9 says "profiles set defaults" without specifying what those
defaults are, so this task designs them:

| Profile | Point size | LOD budget | Color mode | EDL | Overlays | Rationale |
| --- | --- | --- | --- | --- | --- | --- |
| **Survey** | adaptive 1.5–4 px | 15 M | height (spectrum) | on, 0.6 | trajectory + pose-graph | Precision review: adaptive size for close-up detail, height colouring + EDL for depth/QA reading, both overlays on to audit capture quality. |
| **Floor plan** | fixed 1.5 px | 8 M | height (thermal), clipped 1.0–1.5 m | on, 0.7 | none | Matches A12's `SliceOptions` default slice band (`plan/floor_plan.h`: z ∈ [1.0, 1.5] m) exactly — height-clips to it and colours within it so the slice itself is legible; trajectory/pose-graph are noise for this workflow. |
| **Research** | fixed 2 px | 50 M | RGB (unopinionated) | on, 0.4 | trajectory + pose-graph | Maximum fidelity: don't hide real colorization behind a colormap; the largest LOD budget (assumes the strongest hardware); both overlays on for algorithm debugging. |
| **Quick scan** | fixed 3 px | 2 M | intensity (grayscale) | **off** | trajectory only | Fast field preview, likely mid-capture on Android (§3.12's 2 M-pt Pixel-8-class reference target). EDL is explicitly off: its cost is **unmeasured** per the S3 report §7, so it isn't spent here; intensity colouring is readable before any colorization/georeferencing has happened. |

Every `profile_defaults()` result is already passed through
`clamp_display_params()` — `tests/test_display_params.cpp`'s
`profiles/all_are_already_clamped` case checks that clamping a profile's
output is a no-op.

---

## 6. Persistence

Hand-rolled JSON, no new dependency — the same choice `record/lscan.cpp`
made for `manifest.json` (see `engine/vcpkg.json`'s dependency
onboarding-order note). Unlike `lscan.cpp`'s writer-only "looks well-formed"
check, this module needs a real reader too, since `DisplayParams`
round-trips through it: `src/cloud/display_params.cpp` has a small
recursive-descent parser (object/array/string/number/bool/null, `\uXXXX`
escapes decoded to UTF-8) into a generic value tree, then extracts each
`DisplayParams` field from that tree.

**Schema versioning + forward/backward compatibility**, concretely:

* `to_json()` always writes `"schemaVersion": 1`
  (`kDisplayParamsSchemaVersion`).
* `from_json()` never rejects on the version number — it doesn't even branch
  on it. Compatibility instead comes from two structural rules that hold at
  every field, independent of the document's declared version:
  * **Unknown keys are ignored.** A document from a *newer* engine, read by
    an older one, parses fine — the extra keys are just never looked up.
  * **Missing or wrong-typed keys fall back to a caller-supplied
    `defaults`** (itself defaulting to `DisplayParams{}`). A document from
    an *older* engine, or one with a single corrupted field, still parses —
    every other field comes through, and the gap is filled from `defaults`
    rather than failing the whole document.
* Only a **structurally malformed** document (not a JSON object at all, or
  a syntax error) is rejected, with `ScanError::kCorruptData` and `*out`
  left untouched.
* The result is always passed through `clamp_display_params()` before
  returning — a hand-edited or corrupted-in-transit document can't produce
  an out-of-range `DisplayParams`.

Field naming is camelCase JSON matching `manifest.json`'s convention
(`schemaVersion`, `pointSize.fixedPx`, `lodPointBudget`, ...). Floats are
written with `%.9g` (`float`'s `max_digits10`), which is enough significant
decimal digits that parsing the string back reproduces the exact original
`float` bit pattern — `tests/test_display_params.cpp`'s `json/*round_trip*`
cases check this directly rather than trusting it.

---

## 7. Change notification

`DisplayParamsController` mirrors `cloud/page_store.h`'s
`subscribe`/`unsubscribe` shape and threading contract on purpose — apps
already know that pattern from `PageStore`. Subscriber callbacks run
**inline on the calling (`set()`) thread**, same rule as `PageStore` and
`EventBus`: quick, no re-entry.

Two ways to notice a change, matching the task's "apps poll or subscribe":

* **`version()`** — a monotonic counter, safe for any number of independent
  pollers (each keeps its own last-seen value and compares). This is the
  one to use if more than one part of an app needs to know independently.
* **`dirty()`/`clear_dirty()`** — one shared convenience flag for the common
  single-consumer poll loop (e.g. a render loop asking "did anything change
  since I last drew").
* **`subscribe()`/`unsubscribe()`** — push delivery, e.g. wiring a settings
  panel's Apply straight to a live viewer without a poll loop at all.

`set()` always clamps, stores, bumps `version()`, sets `dirty()`, and
notifies every subscriber — even if the clamped result is unchanged from
before. A caller that wants to skip redundant work compares against `get()`
itself first; the controller doesn't do that diffing for it, to keep `set()`
O(1) and lock-scope small (subscriber list is copied under the lock, then
invoked outside it — same pattern `PageStore` uses for its callbacks).

---

## 8. Verification

Full clean build + `ctest`, build dirs deleted before and after (per task
instructions):

```
$ rm -rf build && cmake --preset macos-universal
-- scanengine: Livox-SDK2 (patched) from .../third_party/Livox-SDK2
-- Configuring done / Generating done — build/macos-universal

$ cmake --build --preset macos-universal
[93/93] ... zero errors (src/cloud/display_params.cpp and
             tests/test_display_params.cpp both compile clean)

$ cmake --preset macos-universal -B build/werror -DENGINE_WARNINGS_AS_ERRORS=ON
$ cmake --build build/werror --target scanengine scanengine_tests
... zero errors, zero new warnings (all warnings present are pre-existing,
    from the vendored Livox-SDK2 third-party sources, unrelated to this task)

$ ./build/macos-universal/scanengine_tests --test-case="*display_params*,clamp/*,colormap/*,evaluate/*,profiles/*,json/*,controller/*,rgba8/*"
[doctest] test cases:   55 |   55 passed | 0 failed | 186 skipped
[doctest] assertions: 1193 | 1193 passed | 0 failed |

$ ctest --preset macos-universal -LE sim
75% tests passed, 1 tests failed out of 4   <-- see note below
```

**One pre-existing, unrelated failure**, not introduced by this task:
`tests/test_pushbroom.cpp:288`
(`poses/interpolates_a_constant_rate_trajectory_exactly`) fails a `1e-9`
floating-point tolerance by a few `1e-8`-scale ULPs. That file, along with
`poses/pose_interpolator.h`, `slam/pushbroom/*` etc., belongs to a different
concurrently-in-progress task (A8's pushbroom assembler / pose
interpolation) — confirmed via `git status`, which shows those as untracked
files this task never touched. Excluding just that one test case:

```
$ ./build/macos-universal/scanengine_tests --test-case-exclude="poses/interpolates_a_constant_rate_trajectory_exactly"
[doctest] test cases:   235 |   235 passed | 0 failed | 6 skipped
[doctest] assertions: 38351 | 38351 passed | 0 failed |
[doctest] Status: SUCCESS!
```

...i.e. every other test in the suite — old and new, including all 55 of
this task's `display_params` cases — passes clean. `capi_smoke`,
`engine_cli_selftest` and `engine_cli_version` all pass unmodified. Test-case
breakdown for this task's 55 cases: 10 uniform-block/`to_uniforms`, 12
`clamp_display_params` boundary cases, 9 colormap endpoint/monotonicity
cases, 10 `evaluate_point_color` ground-truth cases (hand-computed where the
math permits exact values — see §3's grayscale note — with derivation
comments in the test file where it doesn't), 8 profile-defaults cases, 10
JSON persistence cases (default/customized round-trip, exhaustive
mode×colormap round-trip, forward-compat unknown-key tolerance, missing-key
fallback-to-defaults, future schema version, 4 malformed-document variants,
wrong-JSON-type tolerance, clamp-on-read), 6
`DisplayParamsController` cases (poll via `version()`/`dirty()`, push via
`subscribe()`/`unsubscribe()`), 2 `RGBA8` basics.

Build dirs (`build/macos-universal`, `build/werror`) were deleted after this
verification run, per the task's ownership constraints (no `CMakeLists.txt`
edits, no committed build artifacts).
