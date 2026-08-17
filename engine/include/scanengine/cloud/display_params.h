// display_params.h — the display-parameter API (Tech Spec §3.9) that both
// apps (Qt C1/C3, Android B10) bind to.
//
// This is a MODEL, not a renderer: it owns validation, colour-mapping
// ground truth, JSON persistence and change notification. It never touches
// Filament/Vulkan/GL. C1 and B10 each embed a render pipeline that must
// reproduce evaluate_point_color()'s math in shader form; DisplayParamsUniforms
// below is the POD both shaders' uniform buffers are laid out against, so
// that reproduction only has to happen once, correctly, and stays checked by
// tests/test_display_params.cpp's ground-truth cases rather than by eyeball
// comparison between two shader files.
//
// S3 caveat this API is built around (spikes/s3-render/REPORT.md §8.3):
// gl_PointSize is NOT a safe assumption on every backend — Vulkan requires
// the shader to write PointSize *and* the device to expose `largePoints`,
// and point-size support has historically been weak on Intel/AMD Linux
// drivers. PointSizeMode::kWorldSize is the documented fallback: point size
// becomes a world-space diameter expanded in the vertex shader (billboard
// quad or shader-computed screen size from view-space distance) instead of
// relying on the fixed-function point-sprite pipeline. A14 does not decide
// which backend needs the fallback — that is C1/B10's per-platform call —
// but the parameter to express "give me metric-consistent point size
// regardless of what gl_PointSize does on this GPU" has to exist here.
//
// EDL (§3.12): Filament's post-process cost for a full-screen EDL pass is
// UNMEASURED per the S3 report (§7 "Gaps in this spike"). edl_enabled +
// edl_strength are therefore plain data — this module does not gate them on
// any measured budget. C1/B10 are the ones who will find out whether EDL
// fits inside the 60fps budget at 10M/2M points and may need to clamp
// availability themselves (e.g. disable at the top of the LOD budget).
//
// PointVertex (cloud/point_page.h) is 16 bytes: float3 position + RGBA8,
// fixed by S3/A1 and NOT owned by this task (page_store.* is A6's
// concurrently, and point_page.h has no spare bytes for intensity or a
// per-point timestamp — see export/exporter.h's identical note). Colour
// modes that need data PointVertex does not carry (intensity, per-point
// time, fix quality) take it through the optional PointAttributes struct
// below and degrade to an RGB pass-through when it is not supplied; see
// evaluate_point_color()'s doc comment for the exact fallback per mode and
// docs/A14-display.md §4 for the rationale.
//
// Owner: A14. Files: this header, src/cloud/display_params.cpp,
// tests/test_display_params.cpp, docs/A14-display.md. Does NOT modify
// cloud/page_store.* or cloud/point_page.h (A6 owns page_store.* this pass).
#ifndef SCANENGINE_CLOUD_DISPLAY_PARAMS_H
#define SCANENGINE_CLOUD_DISPLAY_PARAMS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/gnss/gnss.h"  // FixType — fixed vocabulary per gnss.h's own header comment

namespace scanengine {

// --- colour -----------------------------------------------------------

// Straight, non-premultiplied RGBA, 0..255. Bit-compatible with
// PointVertex's colour channels but kept as its own type: DisplayParams
// carries colours that are not per-point vertex data (background, the
// discrete fix-quality palette).
struct RGBA8 {
  std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

inline bool operator==(RGBA8 a, RGBA8 b) noexcept {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
inline bool operator!=(RGBA8 a, RGBA8 b) noexcept { return !(a == b); }

// Three procedurally generated ramps — NOT reproductions of any named
// third-party palette (turbo/viridis/jet/...), specifically to keep this
// module dependency- and attribution-free per the task brief ("implement
// 2-3 tasteful colormaps ... document sources/licences or generate
// procedurally"). Each is a closed-form function of t in [0,1], authored in
// src/cloud/display_params.cpp and cached into a 256-entry LUT the first
// time it is used (thread-safe: C++11 function-local statics). See
// docs/A14-display.md §3 for the exact construction of each ramp and why it
// was chosen for its colour mode.
enum class Colormap : std::uint8_t {
  kGrayscale = 0,  // linear luminance ramp — the "just show me the number" default
  kSpectrum = 1,   // full-saturation hue sweep, blue -> red — high perceptual
                    // contrast for exploring a wide range at a glance (height)
  kThermal = 2,    // black -> red -> yellow -> white — near-monotonic
                    // luminance, reads correctly in grayscale print/screenshots
};

inline constexpr std::uint8_t kColormapCount = 3;
inline constexpr std::uint32_t kColormapLutSize = 256;

const char* to_string(Colormap cm) noexcept;

// Evaluates one colormap at `t` (clamped to [0,1] internally), linearly
// interpolating between the two nearest LUT entries. This is the function
// evaluate_point_color() and DisplayParamsUniforms-consuming shaders must
// agree with; ground-truth endpoint/monotonicity tests live in
// tests/test_display_params.cpp.
RGBA8 evaluate_colormap(Colormap cm, float t) noexcept;

// The full 256-entry LUT, e.g. for upload as a 1D texture a shader samples
// instead of re-deriving the closed-form ramp per-fragment. Alpha is always
// 255 in every colormap LUT entry (colormaps never carry alpha; see
// evaluate_point_color()'s alpha rule).
const std::array<RGBA8, kColormapLutSize>& colormap_lut(Colormap cm) noexcept;

// --- point size (S3 §8.3 caveat) ---------------------------------------

enum class PointSizeMode : std::uint8_t {
  kFixedPixels = 0,  // constant screen-space px — needs gl_PointSize or an
                      // equivalent fixed-function point sprite
  kAdaptive = 1,      // screen-space px, scaled between
                      // [adaptive_min_px, adaptive_max_px] by distance from
                      // camera (closer = larger); same backend requirement
                      // as kFixedPixels
  kWorldSize = 2,     // world-space diameter in metres, expanded in the
                      // vertex shader — the portable fallback when
                      // gl_PointSize is unavailable/unreliable (S3 §8.3)
};

const char* to_string(PointSizeMode m) noexcept;

struct PointSizeParams {
  PointSizeMode mode = PointSizeMode::kAdaptive;
  float fixed_px = 2.0f;              // kFixedPixels; clamp [0.1, 64.0]
  float adaptive_min_px = 1.0f;       // kAdaptive; clamp [0.1, 64.0], <= adaptive_max_px
  float adaptive_max_px = 6.0f;       // kAdaptive; clamp [0.1, 64.0], >= adaptive_min_px
  float adaptive_reference_m = 5.0f;  // distance (m) at which size == adaptive_min_px; clamp [0.01, 1000.0]
  float world_size_m = 0.01f;         // kWorldSize; clamp [0.0005, 1.0] (0.5 mm .. 1 m diameter)
};

// --- colour modes --------------------------------------------------------

enum class ColorMode : std::uint8_t {
  kRgb = 0,          // PointVertex.rgb straight through
  kHeight = 1,        // PointVertex.z through a colormap
  kIntensity = 2,     // luminance-derived intensity through a colormap
  kTime = 3,          // per-point capture time through a colormap
  kFixQuality = 4,    // discrete palette keyed by FixType
};

const char* to_string(ColorMode m) noexcept;
inline constexpr std::uint8_t kColorModeCount = 5;

// Mapping parameters shared by the three scalar-valued modes (height,
// intensity, time). `manual_min`/`manual_max` are always what
// evaluate_point_color() reads — `auto_range` is a UI/persistence bit, not
// something this module computes: when true, the caller (renderer) is
// expected to refresh manual_min/manual_max every frame (or on cloud
// change) from the actual data range — for height that is
// PageView::bounds_min[2]/bounds_max[2], already exposed by page_store.h
// for exactly this purpose — and leave manual_min/manual_max alone when
// auto_range is false (the user pinned a range). See
// docs/A14-display.md §4.
struct ScalarColorParams {
  bool auto_range = true;
  float manual_min = 0.0f;
  float manual_max = 1.0f;
  float gamma = 1.0f;        // clamp (0.1, 4.0]; output = pow(t, 1/gamma)
  float brightness = 1.0f;   // clamp [0.1, 3.0]; post-colormap multiplier
  Colormap colormap = Colormap::kSpectrum;
  bool invert = false;
};

// --- the model -------------------------------------------------------

struct DisplayParams {
  PointSizeParams point_size;

  // Coarse-to-fine LOD budget (§3.12): the renderer's target on-screen
  // point ceiling, not the PageStore capacity (cloud/page_store.h's
  // max_pages is a hard ingest cap; this is a soft render-time throttle a
  // renderer applies by skipping/decimating pages once resident points
  // exceed it). Clamp [1'000, 200'000'000].
  std::uint32_t lod_point_budget = 5'000'000;

  ColorMode color_mode = ColorMode::kRgb;
  ScalarColorParams height;      // default range 0..3 m (typical indoor ceiling)
  ScalarColorParams intensity;   // default range 0..1 (already-normalized 0..255 -> 0..1)
  ScalarColorParams time;        // default range 0..1 (seconds; caller rescales per session length)

  // Discrete palette for kFixQuality, indexed by static_cast<int>(FixType).
  // Defaults follow the conventional red=bad/green=good GNSS status read.
  RGBA8 fix_quality_colors[5] = {
      RGBA8{128, 128, 128, 255},  // FixType::kNone
      RGBA8{214, 64, 64, 255},    // FixType::kSingle
      RGBA8{230, 150, 40, 255},   // FixType::kDgps
      RGBA8{230, 210, 40, 255},   // FixType::kRtkFloat
      RGBA8{60, 190, 90, 255},    // FixType::kRtkFixed
  };

  // EDL (§3.12). Cost unmeasured per S3 report §7 — see the module comment.
  bool edl_enabled = true;
  float edl_strength = 0.5f;  // clamp [0, 1]

  RGBA8 background{18, 18, 22, 255};  // dark neutral

  bool clip_height_enabled = false;
  float clip_height_min = 0.0f;
  float clip_height_max = 3.0f;  // clamp_display_params() enforces min <= max

  bool clip_box_enabled = false;
  float clip_box_min[3] = {-10.f, -10.f, -10.f};
  float clip_box_max[3] = {10.f, 10.f, 10.f};  // clamp_display_params() enforces min <= max per axis

  bool show_trajectory = true;
  bool show_pose_graph = false;
};

bool operator==(const DisplayParams& a, const DisplayParams& b) noexcept;
inline bool operator!=(const DisplayParams& a, const DisplayParams& b) noexcept { return !(a == b); }

// Clamps every field of `params` in place to the ranges documented above,
// replaces NaN/Inf with the field's default, swaps min/max pairs that came
// in inverted, and clamps any out-of-range enum byte to its 0 value. Always
// succeeds — there is no input this cannot turn into a valid DisplayParams
// — which is why it returns void rather than Status; see
// tests/test_display_params.cpp's "clamp/*" cases for the exact behaviour
// at each boundary.
void clamp_display_params(DisplayParams& params) noexcept;

// --- profiles (§3.9 "profiles set defaults", §1 "Survey · Floor plan ·
// Research · Quick scan") ------------------------------------------------

enum class DisplayProfile : std::uint8_t {
  kSurvey = 0,
  kFloorPlan = 1,
  kResearch = 2,
  kQuickScan = 3,
};

const char* to_string(DisplayProfile p) noexcept;
inline constexpr std::uint8_t kDisplayProfileCount = 4;

// A fresh, already-clamped DisplayParams tuned for the named workflow. See
// docs/A14-display.md §5 for the rationale behind each profile's choices.
DisplayParams profile_defaults(DisplayProfile profile) noexcept;

// --- ground-truth colour evaluation --------------------------------------

// Per-point data PointVertex does not carry. Every field is optional: a
// mode whose data is not supplied degrades to an RGB pass-through of
// `v.r/g/b/a` rather than fabricating a value (see evaluate_point_color()).
struct PointAttributes {
  bool has_intensity = false;
  float intensity = 0.0f;   // 0..1, used only if has_intensity
  bool has_time = false;
  double t_seconds = 0.0;   // used only if has_time
  bool has_fix = false;
  FixType fix = FixType::kNone;  // used only if has_fix
};

// The ground-truth per-point colour: what a correct shader implementation
// of `params` must produce for vertex `v` (with attributes `attrs`). This
// is what the renderers' shader code is checked against and what CPU-side
// tooling (colormap export, headless snapshots) calls directly.
//
// Alpha is ALWAYS v.a, in every mode — point_page.h reserves alpha for
// LOD-fade/selection, so no colour mode may repurpose it.
//
//   kRgb        -> {v.r, v.g, v.b, v.a} unconditionally.
//   kHeight     -> v.z through `params.height` (auto/manual range is a
//                  UI concept; this function always reads manual_min/max).
//   kIntensity  -> attrs.intensity if has_intensity, else the luminance of
//                  (v.r,v.g,v.b) (0.299/0.587/0.114 weights) — the same
//                  RGB-derived-intensity bridge export/exporter.h documents
//                  for A9, so the review viewer and an export agree.
//   kTime       -> attrs.t_seconds through `params.time` if has_time, else
//                  falls back to the kRgb rule (no time data => don't
//                  fabricate a colour).
//   kFixQuality -> params.fix_quality_colors[attrs.fix] if has_fix, else
//                  falls back to the kRgb rule.
RGBA8 evaluate_point_color(const PointVertex& v, const PointAttributes& attrs,
                            const DisplayParams& params) noexcept;

// --- shader-facing uniform block -----------------------------------------

// The field set, order and types both C1 (Filament MaterialInstance named
// parameters — Filament does not accept a raw UBO blob, so C1 binds these
// fields individually, by name, in this order) and B10 (a Vulkan UBO, where
// this struct's layout matters byte-for-byte) must reproduce. Grouped into
// 16-byte rows so a raw-UBO consumer (B10) gets std140-safe packing without
// per-field padding decisions: every row here is exactly one vec4's worth of
// std140-representable data (4 x 4-byte scalars, or a vec3 + a trailing
// scalar sharing the vec4 slot the way `clip_box_min`/`clip_box_max` do).
// Booleans are int32 (0/1) because std140 has no portable bool.
//
// This is derived from a DisplayParams by to_uniforms() below; it does NOT
// carry every DisplayParams field — lod_point_budget, show_trajectory/
// show_pose_graph and the *unused* scalar-mode params (e.g. `intensity`
// when color_mode == kHeight) never reach a shader, so they are not here.
struct DisplayParamsUniforms {
  // row 0
  std::int32_t color_mode = 0;       // ColorMode
  std::int32_t colormap = 0;         // Colormap — the ACTIVE scalar mode's
  float gamma = 1.0f;                // ACTIVE scalar mode's gamma
  std::int32_t invert = 0;           // ACTIVE scalar mode's invert, 0/1

  // row 1
  float value_min = 0.0f;            // ACTIVE scalar mode's manual_min
  float value_max = 1.0f;            // ACTIVE scalar mode's manual_max
  float brightness = 1.0f;           // ACTIVE scalar mode's brightness
  std::int32_t point_size_mode = 0;  // PointSizeMode

  // row 2
  float point_size_min_px = 2.0f;    // fixed_px when kFixedPixels, else adaptive_min_px
  float point_size_max_px = 2.0f;    // == point_size_min_px when kFixedPixels
  float adaptive_reference_m = 5.0f;
  float world_size_m = 0.01f;

  // row 3
  std::int32_t edl_enabled = 0;
  float edl_strength = 0.0f;
  float _pad3[2] = {0.f, 0.f};

  // row 4 — background RGBA, 0..1 floats (a shader wants float, not u8)
  float background[4] = {0.f, 0.f, 0.f, 1.f};

  // row 5 — box clip min (xyz) + a bitmask packed into the trailing scalar:
  // bit0 = clip_box_enabled, bit1 = clip_height_enabled
  float clip_box_min[3] = {0.f, 0.f, 0.f};
  std::int32_t clip_enabled_mask = 0;

  // row 6 — box clip max (xyz) + pad
  float clip_box_max[3] = {0.f, 0.f, 0.f};
  float _pad6 = 0.f;

  // row 7 — height clip range + pad
  float clip_height_min = 0.0f;
  float clip_height_max = 0.0f;
  float _pad7[2] = {0.f, 0.f};

  // rows 8..12 — discrete fix-quality palette, FixType 0..4, RGBA 0..1 each
  float fix_quality_colors[5][4] = {};
};

static_assert(sizeof(DisplayParamsUniforms) == 208,
              "DisplayParamsUniforms must stay a whole number of 16-byte "
              "std140 rows — C1/B10 both hard-code this size");
static_assert(offsetof(DisplayParamsUniforms, background) == 64,
              "background must start a fresh std140 row");
static_assert(offsetof(DisplayParamsUniforms, fix_quality_colors) == 128,
              "fix_quality_colors must start a fresh std140 row");

// Projects a DisplayParams down to what a shader needs, selecting the
// "active" scalar-mode block (height/intensity/time) by `params.color_mode`.
// When color_mode is kRgb or kFixQuality, the scalar-mode fields (colormap,
// gamma, value_min/max, brightness, invert) are filled from a neutral
// identity mapping — the shader is expected to branch on color_mode before
// reading them, exactly as evaluate_point_color() does.
DisplayParamsUniforms to_uniforms(const DisplayParams& params) noexcept;

// --- persistence (§3.9 "settings persist per project") ------------------

// Bumped only when a field is added/removed/repurposed in a way from_json()
// cannot shim transparently. from_json() tolerates BOTH directions: unknown
// keys in the document (a newer engine's fields, read by an older one) are
// ignored, and missing keys (an older document, read by a newer engine) fall
// back to `defaults`.
inline constexpr int kDisplayParamsSchemaVersion = 1;

// Hand-rolled JSON (no new dependency — see vcpkg.json's dependency
// onboarding-order note and src/record/lscan.cpp's manifest writer for the
// established precedent). Always produces a well-formed, schema-versioned
// document.
std::string to_json(const DisplayParams& params);

// Parses `json`. A structurally malformed document (not a JSON object, or a
// syntax error) returns kCorruptData and leaves `*out` unchanged. Otherwise
// every field is read if present and well-typed, or taken from `defaults`
// (which itself defaults to a fresh DisplayParams{}) if absent, of the
// wrong JSON type, or out of range for its enum — so an old document, a
// document written by a newer schema version with extra keys, and a
// document with one corrupted field all still parse. The result is passed
// through clamp_display_params() before being returned, so callers never
// have to.
Status from_json(const std::string& json, DisplayParams* out,
                  const DisplayParams& defaults = DisplayParams{});

// --- change notification -------------------------------------------------

using DisplayParamsSubscriptionId = std::uint32_t;
using DisplayParamsCallback = void (*)(const DisplayParams& params, void* user_data);

// The one mutable instance an app owns per open project/session and every
// UI surface (settings panel, viewer, measure tool) reads through. Mirrors
// cloud/page_store.h's subscribe/unsubscribe shape and threading contract
// on purpose, since apps already know that pattern from PageStore:
// subscriber callbacks run INLINE ON THE CALLING (set()) THREAD — quick, no
// re-entry into the controller.
//
// Two ways to notice a change, matching the task's "poll or subscribe":
//   - version() is monotonic and safe for any number of independent
//     pollers: each keeps its own last-seen value and compares.
//   - dirty()/clear_dirty() is one shared convenience flag for the common
//     single-consumer poll loop (e.g. a render loop that just wants "has
//     anything changed since I last drew").
//   - subscribe() for push delivery (e.g. wiring a settings panel's Apply
//     button straight to a live viewer).
class DisplayParamsController {
 public:
  explicit DisplayParamsController(const DisplayParams& initial = DisplayParams{});
  ~DisplayParamsController();

  DisplayParamsController(const DisplayParamsController&) = delete;
  DisplayParamsController& operator=(const DisplayParamsController&) = delete;

  DisplayParams get() const;

  // Clamps `params` (clamp_display_params()), stores it, bumps version(),
  // sets dirty(), and notifies every subscriber — even if the clamped
  // result is unchanged from before. A caller that wants to skip redundant
  // notifications compares against get() itself first.
  void set(const DisplayParams& params);

  std::uint64_t version() const noexcept;
  bool dirty() const noexcept;
  void clear_dirty() noexcept;

  DisplayParamsSubscriptionId subscribe(DisplayParamsCallback cb, void* user_data);
  Status unsubscribe(DisplayParamsSubscriptionId id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_CLOUD_DISPLAY_PARAMS_H
