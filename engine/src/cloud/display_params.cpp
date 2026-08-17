#include "scanengine/cloud/display_params.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace scanengine {
namespace {

// --- small numeric helpers -----------------------------------------------

float sanitize(float v, float def) noexcept { return std::isfinite(v) ? v : def; }

float clampf(float v, float lo, float hi) noexcept {
  if (!std::isfinite(v)) return lo;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

std::uint8_t to_u8_round(float v) noexcept {
  if (v < 0.f) v = 0.f;
  if (v > 255.f) v = 255.f;
  return static_cast<std::uint8_t>(v + 0.5f);
}

RGBA8 lerp_rgba(RGBA8 a, RGBA8 b, float t) noexcept {
  t = clampf(t, 0.f, 1.f);
  return RGBA8{
      to_u8_round(a.r + (static_cast<float>(b.r) - a.r) * t),
      to_u8_round(a.g + (static_cast<float>(b.g) - a.g) * t),
      to_u8_round(a.b + (static_cast<float>(b.b) - a.b) * t),
      255,
  };
}

}  // namespace

// ===========================================================================
// Colormaps
// ===========================================================================
//
// All three are closed-form functions of t in [0,1], authored for this
// engine (see the header comment: deliberately NOT a reproduction of any
// named third-party palette, to keep this module attribution-free). Each is
// cached into a 256-entry LUT on first use.

namespace {

// Full-saturation HSV hue sweep, h = 240*(1-t) .. 0 degrees (blue -> red).
// s = v = 1. Standard 60-degree-segment HSV->RGB, no trig required.
RGBA8 spectrum_raw(float t) noexcept {
  const float h = 240.0f * (1.0f - clampf(t, 0.f, 1.f));  // 240 (blue) .. 0 (red)
  const float hp = h / 60.0f;
  const float x = 1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f);
  float r1 = 0.f, g1 = 0.f, b1 = 0.f;
  if (hp < 1.0f) {
    r1 = 1.f; g1 = x; b1 = 0.f;
  } else if (hp < 2.0f) {
    r1 = x; g1 = 1.f; b1 = 0.f;
  } else if (hp < 3.0f) {
    r1 = 0.f; g1 = 1.f; b1 = x;
  } else if (hp < 4.0f) {
    r1 = 0.f; g1 = x; b1 = 1.f;
  } else if (hp < 5.0f) {
    r1 = x; g1 = 0.f; b1 = 1.f;
  } else {
    r1 = 1.f; g1 = 0.f; b1 = x;
  }
  return RGBA8{to_u8_round(r1 * 255.f), to_u8_round(g1 * 255.f), to_u8_round(b1 * 255.f), 255};
}

// Black -> red -> yellow -> white, piecewise-linear over three equal
// thirds. Near-monotonic luminance by construction (every channel is
// individually non-decreasing across the whole ramp) — see
// docs/A14-display.md §3 for why that property was the design goal.
RGBA8 thermal_raw(float t) noexcept {
  t = clampf(t, 0.f, 1.f);
  constexpr RGBA8 kBlack{0, 0, 0, 255};
  constexpr RGBA8 kRed{255, 0, 0, 255};
  constexpr RGBA8 kYellow{255, 255, 0, 255};
  constexpr RGBA8 kWhite{255, 255, 255, 255};
  if (t < 1.0f / 3.0f) return lerp_rgba(kBlack, kRed, t * 3.0f);
  if (t < 2.0f / 3.0f) return lerp_rgba(kRed, kYellow, (t - 1.0f / 3.0f) * 3.0f);
  return lerp_rgba(kYellow, kWhite, (t - 2.0f / 3.0f) * 3.0f);
}

RGBA8 grayscale_raw(float t) noexcept {
  const std::uint8_t v = to_u8_round(clampf(t, 0.f, 1.f) * 255.f);
  return RGBA8{v, v, v, 255};
}

RGBA8 colormap_raw(Colormap cm, float t) noexcept {
  switch (cm) {
    case Colormap::kGrayscale: return grayscale_raw(t);
    case Colormap::kSpectrum: return spectrum_raw(t);
    case Colormap::kThermal: return thermal_raw(t);
  }
  return grayscale_raw(t);
}

std::array<RGBA8, kColormapLutSize> build_lut(Colormap cm) noexcept {
  std::array<RGBA8, kColormapLutSize> lut{};
  for (std::uint32_t i = 0; i < kColormapLutSize; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kColormapLutSize - 1);
    lut[i] = colormap_raw(cm, t);
  }
  return lut;
}

}  // namespace

const char* to_string(Colormap cm) noexcept {
  switch (cm) {
    case Colormap::kGrayscale: return "grayscale";
    case Colormap::kSpectrum: return "spectrum";
    case Colormap::kThermal: return "thermal";
  }
  return "grayscale";
}

const std::array<RGBA8, kColormapLutSize>& colormap_lut(Colormap cm) noexcept {
  // Thread-safe by C++11 magic statics; each colormap's table is built once.
  static const std::array<RGBA8, kColormapLutSize> kGrayscale = build_lut(Colormap::kGrayscale);
  static const std::array<RGBA8, kColormapLutSize> kSpectrum = build_lut(Colormap::kSpectrum);
  static const std::array<RGBA8, kColormapLutSize> kThermal = build_lut(Colormap::kThermal);
  switch (cm) {
    case Colormap::kGrayscale: return kGrayscale;
    case Colormap::kSpectrum: return kSpectrum;
    case Colormap::kThermal: return kThermal;
  }
  return kGrayscale;
}

RGBA8 evaluate_colormap(Colormap cm, float t) noexcept {
  t = clampf(t, 0.f, 1.f);
  const auto& lut = colormap_lut(cm);
  const float pos = t * static_cast<float>(kColormapLutSize - 1);
  const auto i0 = static_cast<std::uint32_t>(pos);
  const std::uint32_t i1 = std::min(i0 + 1u, kColormapLutSize - 1u);
  const float frac = pos - static_cast<float>(i0);
  return lerp_rgba(lut[i0], lut[i1], frac);
}

// ===========================================================================
// to_string()
// ===========================================================================

const char* to_string(PointSizeMode m) noexcept {
  switch (m) {
    case PointSizeMode::kFixedPixels: return "fixedPixels";
    case PointSizeMode::kAdaptive: return "adaptive";
    case PointSizeMode::kWorldSize: return "worldSize";
  }
  return "adaptive";
}

const char* to_string(ColorMode m) noexcept {
  switch (m) {
    case ColorMode::kRgb: return "rgb";
    case ColorMode::kHeight: return "height";
    case ColorMode::kIntensity: return "intensity";
    case ColorMode::kTime: return "time";
    case ColorMode::kFixQuality: return "fixQuality";
  }
  return "rgb";
}

const char* to_string(DisplayProfile p) noexcept {
  switch (p) {
    case DisplayProfile::kSurvey: return "survey";
    case DisplayProfile::kFloorPlan: return "floorPlan";
    case DisplayProfile::kResearch: return "research";
    case DisplayProfile::kQuickScan: return "quickScan";
  }
  return "research";
}

// ===========================================================================
// clamp_display_params()
// ===========================================================================

namespace {

void clamp_scalar(ScalarColorParams& s, float def_min, float def_max) noexcept {
  s.manual_min = sanitize(s.manual_min, def_min);
  s.manual_max = sanitize(s.manual_max, def_max);
  if (s.manual_min > s.manual_max) std::swap(s.manual_min, s.manual_max);
  s.gamma = clampf(sanitize(s.gamma, 1.0f), 0.1f, 4.0f);
  s.brightness = clampf(sanitize(s.brightness, 1.0f), 0.1f, 3.0f);
  if (static_cast<std::uint8_t>(s.colormap) >= kColormapCount) s.colormap = Colormap::kGrayscale;
}

}  // namespace

void clamp_display_params(DisplayParams& p) noexcept {
  PointSizeParams& ps = p.point_size;
  if (static_cast<std::uint8_t>(ps.mode) > static_cast<std::uint8_t>(PointSizeMode::kWorldSize)) {
    ps.mode = PointSizeMode::kAdaptive;
  }
  ps.fixed_px = clampf(sanitize(ps.fixed_px, 2.0f), 0.1f, 64.0f);
  ps.adaptive_min_px = clampf(sanitize(ps.adaptive_min_px, 1.0f), 0.1f, 64.0f);
  ps.adaptive_max_px = clampf(sanitize(ps.adaptive_max_px, 6.0f), 0.1f, 64.0f);
  if (ps.adaptive_min_px > ps.adaptive_max_px) std::swap(ps.adaptive_min_px, ps.adaptive_max_px);
  ps.adaptive_reference_m = clampf(sanitize(ps.adaptive_reference_m, 5.0f), 0.01f, 1000.0f);
  ps.world_size_m = clampf(sanitize(ps.world_size_m, 0.01f), 0.0005f, 1.0f);

  if (p.lod_point_budget < 1'000u) p.lod_point_budget = 1'000u;
  if (p.lod_point_budget > 200'000'000u) p.lod_point_budget = 200'000'000u;

  if (static_cast<std::uint8_t>(p.color_mode) >= kColorModeCount) p.color_mode = ColorMode::kRgb;

  clamp_scalar(p.height, 0.0f, 3.0f);
  clamp_scalar(p.intensity, 0.0f, 1.0f);
  clamp_scalar(p.time, 0.0f, 1.0f);

  // fix_quality_colors are uint8 channels — already structurally in range.

  p.edl_strength = clampf(sanitize(p.edl_strength, 0.5f), 0.0f, 1.0f);

  p.clip_height_min = sanitize(p.clip_height_min, 0.0f);
  p.clip_height_max = sanitize(p.clip_height_max, 3.0f);
  if (p.clip_height_min > p.clip_height_max) std::swap(p.clip_height_min, p.clip_height_max);

  for (int i = 0; i < 3; ++i) {
    p.clip_box_min[i] = sanitize(p.clip_box_min[i], -10.f);
    p.clip_box_max[i] = sanitize(p.clip_box_max[i], 10.f);
    if (p.clip_box_min[i] > p.clip_box_max[i]) std::swap(p.clip_box_min[i], p.clip_box_max[i]);
  }
}

// ===========================================================================
// operator==
// ===========================================================================

namespace {
bool scalar_eq(const ScalarColorParams& a, const ScalarColorParams& b) noexcept {
  return a.auto_range == b.auto_range && a.manual_min == b.manual_min && a.manual_max == b.manual_max &&
         a.gamma == b.gamma && a.brightness == b.brightness && a.colormap == b.colormap &&
         a.invert == b.invert;
}
}  // namespace

bool operator==(const DisplayParams& a, const DisplayParams& b) noexcept {
  const PointSizeParams& x = a.point_size;
  const PointSizeParams& y = b.point_size;
  if (!(x.mode == y.mode && x.fixed_px == y.fixed_px && x.adaptive_min_px == y.adaptive_min_px &&
        x.adaptive_max_px == y.adaptive_max_px && x.adaptive_reference_m == y.adaptive_reference_m &&
        x.world_size_m == y.world_size_m)) {
    return false;
  }
  if (a.lod_point_budget != b.lod_point_budget) return false;
  if (a.color_mode != b.color_mode) return false;
  if (!scalar_eq(a.height, b.height) || !scalar_eq(a.intensity, b.intensity) || !scalar_eq(a.time, b.time)) {
    return false;
  }
  for (int i = 0; i < 5; ++i) {
    if (a.fix_quality_colors[i] != b.fix_quality_colors[i]) return false;
  }
  if (a.edl_enabled != b.edl_enabled || a.edl_strength != b.edl_strength) return false;
  if (a.background != b.background) return false;
  if (a.clip_height_enabled != b.clip_height_enabled || a.clip_height_min != b.clip_height_min ||
      a.clip_height_max != b.clip_height_max) {
    return false;
  }
  if (a.clip_box_enabled != b.clip_box_enabled) return false;
  for (int i = 0; i < 3; ++i) {
    if (a.clip_box_min[i] != b.clip_box_min[i] || a.clip_box_max[i] != b.clip_box_max[i]) return false;
  }
  if (a.show_trajectory != b.show_trajectory || a.show_pose_graph != b.show_pose_graph) return false;
  return true;
}

// ===========================================================================
// profile_defaults()
// ===========================================================================

DisplayParams profile_defaults(DisplayProfile profile) noexcept {
  DisplayParams p;
  switch (profile) {
    case DisplayProfile::kSurvey:
      // Precision review: adaptive size for fine detail up close, a
      // generous LOD budget (desktop-class review of a large capture),
      // height coloring + EDL for depth/QA reading, both overlays on to
      // audit trajectory/pose-graph quality.
      p.point_size.mode = PointSizeMode::kAdaptive;
      p.point_size.adaptive_min_px = 1.5f;
      p.point_size.adaptive_max_px = 4.0f;
      p.lod_point_budget = 15'000'000;
      p.color_mode = ColorMode::kHeight;
      p.height.auto_range = true;
      p.height.colormap = Colormap::kSpectrum;
      p.edl_enabled = true;
      p.edl_strength = 0.6f;
      p.background = RGBA8{16, 16, 20, 255};
      p.show_trajectory = true;
      p.show_pose_graph = true;
      break;

    case DisplayProfile::kFloorPlan:
      // Tuned around the A12 slice band (plan/floor_plan.h's SliceOptions
      // default: z in [1.0, 1.5] m): height-clip to that band by default,
      // and color by height within it so the slice itself is legible.
      // Trajectory/pose-graph overlays are noise for this workflow.
      p.point_size.mode = PointSizeMode::kFixedPixels;
      p.point_size.fixed_px = 1.5f;
      p.lod_point_budget = 8'000'000;
      p.color_mode = ColorMode::kHeight;
      p.height.auto_range = false;
      p.height.manual_min = 1.0f;
      p.height.manual_max = 1.5f;
      p.height.colormap = Colormap::kThermal;
      p.edl_enabled = true;
      p.edl_strength = 0.7f;
      p.clip_height_enabled = true;
      p.clip_height_min = 1.0f;
      p.clip_height_max = 1.5f;
      p.show_trajectory = false;
      p.show_pose_graph = false;
      break;

    case DisplayProfile::kResearch:
      // Maximum fidelity / least opinionated: RGB passthrough (don't hide
      // real colorization behind a colormap), the largest LOD budget (a
      // researcher's desktop is assumed to be the strongest hardware —
      // still within the S3-proven 10M+ desktop budget), both overlays on
      // for algorithm debugging (SLAM trajectory, pose-graph structure).
      p.point_size.mode = PointSizeMode::kFixedPixels;
      p.point_size.fixed_px = 2.0f;
      p.lod_point_budget = 50'000'000;
      p.color_mode = ColorMode::kRgb;
      p.edl_enabled = true;
      p.edl_strength = 0.4f;
      p.show_trajectory = true;
      p.show_pose_graph = true;
      break;

    case DisplayProfile::kQuickScan:
      // Fast field preview, likely on Android mid-capture (§3.12's 2M-pt
      // Pixel-8-class reference target): small fixed point size (cheapest
      // path, no per-point distance math), EDL off (unmeasured cost per
      // the S3 report — do not spend budget on it here), intensity
      // coloring so a scan is readable before any colorization/height
      // georeferencing has happened; trajectory on so the operator can see
      // live coverage, pose-graph off (irrelevant mid-capture).
      p.point_size.mode = PointSizeMode::kFixedPixels;
      p.point_size.fixed_px = 3.0f;
      p.lod_point_budget = 2'000'000;
      p.color_mode = ColorMode::kIntensity;
      p.intensity.colormap = Colormap::kGrayscale;
      p.edl_enabled = false;
      p.show_trajectory = true;
      p.show_pose_graph = false;
      break;
  }
  clamp_display_params(p);
  return p;
}

// ===========================================================================
// evaluate_point_color()
// ===========================================================================

namespace {

float luminance01(const PointVertex& v) noexcept {
  return (0.299f * static_cast<float>(v.r) + 0.587f * static_cast<float>(v.g) +
          0.114f * static_cast<float>(v.b)) /
         255.0f;
}

float map_scalar(float value, const ScalarColorParams& s) noexcept {
  const float lo = s.manual_min;
  const float hi = s.manual_max;
  float t = (hi > lo) ? (value - lo) / (hi - lo) : 0.0f;
  t = clampf(t, 0.0f, 1.0f);
  if (s.invert) t = 1.0f - t;
  t = std::pow(t, 1.0f / s.gamma);
  return clampf(t, 0.0f, 1.0f);
}

RGBA8 apply_brightness(RGBA8 c, float brightness, std::uint8_t alpha) noexcept {
  return RGBA8{
      to_u8_round(static_cast<float>(c.r) * brightness),
      to_u8_round(static_cast<float>(c.g) * brightness),
      to_u8_round(static_cast<float>(c.b) * brightness),
      alpha,
  };
}

}  // namespace

RGBA8 evaluate_point_color(const PointVertex& v, const PointAttributes& attrs,
                            const DisplayParams& p) noexcept {
  switch (p.color_mode) {
    case ColorMode::kRgb:
      return RGBA8{v.r, v.g, v.b, v.a};

    case ColorMode::kHeight: {
      const float t = map_scalar(v.z, p.height);
      const RGBA8 c = evaluate_colormap(p.height.colormap, t);
      return apply_brightness(c, p.height.brightness, v.a);
    }

    case ColorMode::kIntensity: {
      const float raw = attrs.has_intensity ? clampf(attrs.intensity, 0.f, 1.f) : luminance01(v);
      const float t = map_scalar(raw, p.intensity);
      const RGBA8 c = evaluate_colormap(p.intensity.colormap, t);
      return apply_brightness(c, p.intensity.brightness, v.a);
    }

    case ColorMode::kTime: {
      if (!attrs.has_time) return RGBA8{v.r, v.g, v.b, v.a};
      const float t = map_scalar(static_cast<float>(attrs.t_seconds), p.time);
      const RGBA8 c = evaluate_colormap(p.time.colormap, t);
      return apply_brightness(c, p.time.brightness, v.a);
    }

    case ColorMode::kFixQuality: {
      if (!attrs.has_fix) return RGBA8{v.r, v.g, v.b, v.a};
      int idx = static_cast<int>(attrs.fix);
      if (idx < 0 || idx > 4) idx = 0;
      RGBA8 c = p.fix_quality_colors[idx];
      c.a = v.a;
      return c;
    }
  }
  return RGBA8{v.r, v.g, v.b, v.a};
}

// ===========================================================================
// to_uniforms()
// ===========================================================================

DisplayParamsUniforms to_uniforms(const DisplayParams& p) noexcept {
  DisplayParamsUniforms u;
  u.color_mode = static_cast<std::int32_t>(p.color_mode);

  const ScalarColorParams* active = nullptr;
  switch (p.color_mode) {
    case ColorMode::kHeight: active = &p.height; break;
    case ColorMode::kIntensity: active = &p.intensity; break;
    case ColorMode::kTime: active = &p.time; break;
    case ColorMode::kRgb:
    case ColorMode::kFixQuality:
      break;
  }
  if (active != nullptr) {
    u.colormap = static_cast<std::int32_t>(active->colormap);
    u.gamma = active->gamma;
    u.invert = active->invert ? 1 : 0;
    u.value_min = active->manual_min;
    u.value_max = active->manual_max;
    u.brightness = active->brightness;
  } else {
    u.colormap = static_cast<std::int32_t>(Colormap::kGrayscale);
    u.gamma = 1.0f;
    u.invert = 0;
    u.value_min = 0.0f;
    u.value_max = 1.0f;
    u.brightness = 1.0f;
  }

  u.point_size_mode = static_cast<std::int32_t>(p.point_size.mode);
  switch (p.point_size.mode) {
    case PointSizeMode::kFixedPixels:
      u.point_size_min_px = p.point_size.fixed_px;
      u.point_size_max_px = p.point_size.fixed_px;
      break;
    case PointSizeMode::kAdaptive:
      u.point_size_min_px = p.point_size.adaptive_min_px;
      u.point_size_max_px = p.point_size.adaptive_max_px;
      break;
    case PointSizeMode::kWorldSize:
      u.point_size_min_px = 0.0f;
      u.point_size_max_px = 0.0f;
      break;
  }
  u.adaptive_reference_m = p.point_size.adaptive_reference_m;
  u.world_size_m = p.point_size.world_size_m;

  u.edl_enabled = p.edl_enabled ? 1 : 0;
  u.edl_strength = p.edl_strength;

  u.background[0] = static_cast<float>(p.background.r) / 255.0f;
  u.background[1] = static_cast<float>(p.background.g) / 255.0f;
  u.background[2] = static_cast<float>(p.background.b) / 255.0f;
  u.background[3] = static_cast<float>(p.background.a) / 255.0f;

  for (int i = 0; i < 3; ++i) {
    u.clip_box_min[i] = p.clip_box_min[i];
    u.clip_box_max[i] = p.clip_box_max[i];
  }
  u.clip_enabled_mask = (p.clip_box_enabled ? 1 : 0) | (p.clip_height_enabled ? 2 : 0);

  u.clip_height_min = p.clip_height_min;
  u.clip_height_max = p.clip_height_max;

  for (int i = 0; i < 5; ++i) {
    u.fix_quality_colors[i][0] = static_cast<float>(p.fix_quality_colors[i].r) / 255.0f;
    u.fix_quality_colors[i][1] = static_cast<float>(p.fix_quality_colors[i].g) / 255.0f;
    u.fix_quality_colors[i][2] = static_cast<float>(p.fix_quality_colors[i].b) / 255.0f;
    u.fix_quality_colors[i][3] = static_cast<float>(p.fix_quality_colors[i].a) / 255.0f;
  }

  return u;
}

// ===========================================================================
// Hand-rolled JSON (no new dependency — see src/record/lscan.cpp for the
// established precedent of a dependency-free JSON writer; this module also
// needs a reader, since DisplayParams round-trips through it, so it gets a
// small recursive-descent parser into a generic value tree rather than
// lscan.cpp's "well-formed enough" check).
// ===========================================================================

namespace {

struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JsonValue> arr;
  std::vector<std::pair<std::string, JsonValue>> obj;

  const JsonValue* find(const char* key) const noexcept {
    for (const auto& kv : obj) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& s) : s_(s) {}

  bool parse(JsonValue* out) {
    skip_ws();
    if (!parse_value(out)) return false;
    skip_ws();
    return pos_ == s_.size();
  }

 private:
  const std::string& s_;
  std::size_t pos_ = 0;

  char peek() const noexcept { return pos_ < s_.size() ? s_[pos_] : '\0'; }

  void skip_ws() noexcept {
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool literal(const char* lit) noexcept {
    const std::size_t n = std::strlen(lit);
    if (s_.compare(pos_, n, lit) != 0) return false;
    pos_ += n;
    return true;
  }

  bool parse_value(JsonValue* out) {
    skip_ws();
    if (pos_ >= s_.size()) return false;
    const char c = peek();
    if (c == '{') return parse_object(out);
    if (c == '[') return parse_array(out);
    if (c == '"') return parse_string_value(out);
    if (c == 't') {
      if (!literal("true")) return false;
      out->type = JsonValue::Type::kBool;
      out->b = true;
      return true;
    }
    if (c == 'f') {
      if (!literal("false")) return false;
      out->type = JsonValue::Type::kBool;
      out->b = false;
      return true;
    }
    if (c == 'n') {
      if (!literal("null")) return false;
      out->type = JsonValue::Type::kNull;
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
    return false;
  }

  bool parse_number(JsonValue* out) {
    const char* start = s_.data() + pos_;
    char* end = nullptr;
    const double v = std::strtod(start, &end);
    if (end == start) return false;
    pos_ += static_cast<std::size_t>(end - start);
    out->type = JsonValue::Type::kNumber;
    out->num = v;
    return true;
  }

  // Appends the UTF-8 encoding of a BMP codepoint (surrogate pairs are not
  // supported — no DisplayParams field ever needs one).
  static void append_utf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
      s += static_cast<char>(cp);
    } else if (cp < 0x800) {
      s += static_cast<char>(0xC0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      s += static_cast<char>(0xE0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool parse_raw_string(std::string* out) {
    if (peek() != '"') return false;
    ++pos_;
    out->clear();
    while (true) {
      if (pos_ >= s_.size()) return false;
      const char c = s_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= s_.size()) return false;
        const char e = s_[pos_++];
        switch (e) {
          case '"': *out += '"'; break;
          case '\\': *out += '\\'; break;
          case '/': *out += '/'; break;
          case 'b': *out += '\b'; break;
          case 'f': *out += '\f'; break;
          case 'n': *out += '\n'; break;
          case 'r': *out += '\r'; break;
          case 't': *out += '\t'; break;
          case 'u': {
            if (pos_ + 4 > s_.size()) return false;
            unsigned cp = 0;
            for (int i = 0; i < 4; ++i) {
              const char hc = s_[pos_++];
              cp <<= 4;
              if (hc >= '0' && hc <= '9') cp |= static_cast<unsigned>(hc - '0');
              else if (hc >= 'a' && hc <= 'f') cp |= static_cast<unsigned>(hc - 'a' + 10);
              else if (hc >= 'A' && hc <= 'F') cp |= static_cast<unsigned>(hc - 'A' + 10);
              else return false;
            }
            append_utf8(*out, cp);
            break;
          }
          default: return false;
        }
      } else {
        *out += c;
        ++pos_;
      }
    }
  }

  bool parse_string_value(JsonValue* out) {
    std::string s;
    if (!parse_raw_string(&s)) return false;
    out->type = JsonValue::Type::kString;
    out->str = std::move(s);
    return true;
  }

  bool parse_array(JsonValue* out) {
    if (peek() != '[') return false;
    ++pos_;
    out->type = JsonValue::Type::kArray;
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      JsonValue elem;
      if (!parse_value(&elem)) return false;
      out->arr.push_back(std::move(elem));
      skip_ws();
      const char c = peek();
      if (c == ',') {
        ++pos_;
        skip_ws();
        continue;
      }
      if (c == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool parse_object(JsonValue* out) {
    if (peek() != '{') return false;
    ++pos_;
    out->type = JsonValue::Type::kObject;
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_raw_string(&key)) return false;
      skip_ws();
      if (peek() != ':') return false;
      ++pos_;
      JsonValue val;
      if (!parse_value(&val)) return false;
      out->obj.emplace_back(std::move(key), std::move(val));
      skip_ws();
      const char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }
};

// --- extraction helpers, each with a graceful default fallback -----------

float get_num(const JsonValue* v, float def) noexcept {
  if (v == nullptr || v->type != JsonValue::Type::kNumber) return def;
  return static_cast<float>(v->num);
}

std::uint32_t get_u32(const JsonValue* v, std::uint32_t def) noexcept {
  if (v == nullptr || v->type != JsonValue::Type::kNumber || v->num < 0.0) return def;
  return static_cast<std::uint32_t>(v->num);
}

bool get_bool(const JsonValue* v, bool def) noexcept {
  if (v == nullptr || v->type != JsonValue::Type::kBool) return def;
  return v->b;
}

std::string get_str(const JsonValue* v, const std::string& def) {
  if (v == nullptr || v->type != JsonValue::Type::kString) return def;
  return v->str;
}

std::uint8_t get_channel(const JsonValue* arr, std::size_t idx, std::uint8_t def) noexcept {
  if (arr == nullptr || arr->type != JsonValue::Type::kArray || idx >= arr->arr.size()) return def;
  const JsonValue& e = arr->arr[idx];
  if (e.type != JsonValue::Type::kNumber) return def;
  double d = e.num;
  if (d < 0.0) d = 0.0;
  if (d > 255.0) d = 255.0;
  return static_cast<std::uint8_t>(d);
}

RGBA8 get_rgba(const JsonValue* v, RGBA8 def) noexcept {
  if (v == nullptr || v->type != JsonValue::Type::kArray) return def;
  return RGBA8{
      get_channel(v, 0, def.r),
      get_channel(v, 1, def.g),
      get_channel(v, 2, def.b),
      get_channel(v, 3, def.a),
  };
}

void get_vec3(const JsonValue* v, float (&out)[3]) noexcept {
  if (v == nullptr || v->type != JsonValue::Type::kArray) return;
  for (std::size_t i = 0; i < 3 && i < v->arr.size(); ++i) {
    if (v->arr[i].type == JsonValue::Type::kNumber) out[i] = static_cast<float>(v->arr[i].num);
  }
}

PointSizeMode parse_point_size_mode(const std::string& s, PointSizeMode def) noexcept {
  if (s == "fixedPixels") return PointSizeMode::kFixedPixels;
  if (s == "adaptive") return PointSizeMode::kAdaptive;
  if (s == "worldSize") return PointSizeMode::kWorldSize;
  return def;
}

ColorMode parse_color_mode(const std::string& s, ColorMode def) noexcept {
  if (s == "rgb") return ColorMode::kRgb;
  if (s == "height") return ColorMode::kHeight;
  if (s == "intensity") return ColorMode::kIntensity;
  if (s == "time") return ColorMode::kTime;
  if (s == "fixQuality") return ColorMode::kFixQuality;
  return def;
}

Colormap parse_colormap(const std::string& s, Colormap def) noexcept {
  if (s == "grayscale") return Colormap::kGrayscale;
  if (s == "spectrum") return Colormap::kSpectrum;
  if (s == "thermal") return Colormap::kThermal;
  return def;
}

void scalar_from_json(const JsonValue* v, ScalarColorParams* s, const ScalarColorParams& def) {
  *s = def;
  if (v == nullptr || v->type != JsonValue::Type::kObject) return;
  s->auto_range = get_bool(v->find("autoRange"), def.auto_range);
  s->manual_min = get_num(v->find("manualMin"), def.manual_min);
  s->manual_max = get_num(v->find("manualMax"), def.manual_max);
  s->gamma = get_num(v->find("gamma"), def.gamma);
  s->brightness = get_num(v->find("brightness"), def.brightness);
  s->colormap = parse_colormap(get_str(v->find("colormap"), ""), def.colormap);
  s->invert = get_bool(v->find("invert"), def.invert);
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string num_json(float v) {
  if (!std::isfinite(v)) v = 0.0f;
  char buf[32];
  // max_digits10 for float is 9 — enough significant decimal digits that
  // parsing this string back reproduces the exact original float value
  // (see tests/test_display_params.cpp's "json/*_round_trip" cases).
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
  return std::string(buf);
}

std::string rgba_json(RGBA8 c) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "[%u, %u, %u, %u]", c.r, c.g, c.b, c.a);
  return std::string(buf);
}

std::string vec3_json(const float (&v)[3]) {
  return "[" + num_json(v[0]) + ", " + num_json(v[1]) + ", " + num_json(v[2]) + "]";
}

std::string scalar_json(const ScalarColorParams& s, const char* indent) {
  std::string j = "{\n";
  const std::string in2 = std::string(indent) + "  ";
  j += in2 + "\"autoRange\": " + (s.auto_range ? "true" : "false") + ",\n";
  j += in2 + "\"manualMin\": " + num_json(s.manual_min) + ",\n";
  j += in2 + "\"manualMax\": " + num_json(s.manual_max) + ",\n";
  j += in2 + "\"gamma\": " + num_json(s.gamma) + ",\n";
  j += in2 + "\"brightness\": " + num_json(s.brightness) + ",\n";
  j += in2 + "\"colormap\": \"" + json_escape(to_string(s.colormap)) + "\",\n";
  j += in2 + "\"invert\": " + (s.invert ? "true" : "false") + "\n";
  j += std::string(indent) + "}";
  return j;
}

}  // namespace

std::string to_json(const DisplayParams& p) {
  std::string j;
  j += "{\n";
  j += "  \"schemaVersion\": " + std::to_string(kDisplayParamsSchemaVersion) + ",\n";

  j += "  \"pointSize\": {\n";
  j += "    \"mode\": \"" + json_escape(to_string(p.point_size.mode)) + "\",\n";
  j += "    \"fixedPx\": " + num_json(p.point_size.fixed_px) + ",\n";
  j += "    \"adaptiveMinPx\": " + num_json(p.point_size.adaptive_min_px) + ",\n";
  j += "    \"adaptiveMaxPx\": " + num_json(p.point_size.adaptive_max_px) + ",\n";
  j += "    \"adaptiveReferenceM\": " + num_json(p.point_size.adaptive_reference_m) + ",\n";
  j += "    \"worldSizeM\": " + num_json(p.point_size.world_size_m) + "\n";
  j += "  },\n";

  j += "  \"lodPointBudget\": " + std::to_string(p.lod_point_budget) + ",\n";
  j += "  \"colorMode\": \"" + json_escape(to_string(p.color_mode)) + "\",\n";
  j += "  \"height\": " + scalar_json(p.height, "  ") + ",\n";
  j += "  \"intensity\": " + scalar_json(p.intensity, "  ") + ",\n";
  j += "  \"time\": " + scalar_json(p.time, "  ") + ",\n";

  j += "  \"fixQualityColors\": [\n";
  for (int i = 0; i < 5; ++i) {
    j += "    " + rgba_json(p.fix_quality_colors[i]) + (i < 4 ? ",\n" : "\n");
  }
  j += "  ],\n";

  j += "  \"edl\": {\"enabled\": " + std::string(p.edl_enabled ? "true" : "false") +
       ", \"strength\": " + num_json(p.edl_strength) + "},\n";
  j += "  \"background\": " + rgba_json(p.background) + ",\n";

  j += "  \"clipHeight\": {\"enabled\": " + std::string(p.clip_height_enabled ? "true" : "false") +
       ", \"min\": " + num_json(p.clip_height_min) + ", \"max\": " + num_json(p.clip_height_max) + "},\n";
  j += "  \"clipBox\": {\"enabled\": " + std::string(p.clip_box_enabled ? "true" : "false") +
       ", \"min\": " + vec3_json(p.clip_box_min) + ", \"max\": " + vec3_json(p.clip_box_max) + "},\n";

  j += "  \"overlays\": {\"trajectory\": " + std::string(p.show_trajectory ? "true" : "false") +
       ", \"poseGraph\": " + std::string(p.show_pose_graph ? "true" : "false") + "}\n";
  j += "}\n";
  return j;
}

Status from_json(const std::string& json, DisplayParams* out, const DisplayParams& defaults) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "display_params: from_json out is null");
  }

  JsonValue root;
  JsonParser parser(json);
  if (!parser.parse(&root) || root.type != JsonValue::Type::kObject) {
    return set_last_error(ScanError::kCorruptData,
                           "display_params: document is not a well-formed JSON object");
  }

  DisplayParams p = defaults;

  if (const JsonValue* ps = root.find("pointSize"); ps != nullptr && ps->type == JsonValue::Type::kObject) {
    p.point_size.mode =
        parse_point_size_mode(get_str(ps->find("mode"), ""), defaults.point_size.mode);
    p.point_size.fixed_px = get_num(ps->find("fixedPx"), defaults.point_size.fixed_px);
    p.point_size.adaptive_min_px = get_num(ps->find("adaptiveMinPx"), defaults.point_size.adaptive_min_px);
    p.point_size.adaptive_max_px = get_num(ps->find("adaptiveMaxPx"), defaults.point_size.adaptive_max_px);
    p.point_size.adaptive_reference_m =
        get_num(ps->find("adaptiveReferenceM"), defaults.point_size.adaptive_reference_m);
    p.point_size.world_size_m = get_num(ps->find("worldSizeM"), defaults.point_size.world_size_m);
  }

  p.lod_point_budget = get_u32(root.find("lodPointBudget"), defaults.lod_point_budget);
  p.color_mode = parse_color_mode(get_str(root.find("colorMode"), ""), defaults.color_mode);

  scalar_from_json(root.find("height"), &p.height, defaults.height);
  scalar_from_json(root.find("intensity"), &p.intensity, defaults.intensity);
  scalar_from_json(root.find("time"), &p.time, defaults.time);

  if (const JsonValue* fq = root.find("fixQualityColors"); fq != nullptr && fq->type == JsonValue::Type::kArray) {
    for (std::size_t i = 0; i < 5; ++i) {
      const JsonValue* entry = i < fq->arr.size() ? &fq->arr[i] : nullptr;
      p.fix_quality_colors[i] = get_rgba(entry, defaults.fix_quality_colors[i]);
    }
  }

  if (const JsonValue* edl = root.find("edl"); edl != nullptr && edl->type == JsonValue::Type::kObject) {
    p.edl_enabled = get_bool(edl->find("enabled"), defaults.edl_enabled);
    p.edl_strength = get_num(edl->find("strength"), defaults.edl_strength);
  }

  p.background = get_rgba(root.find("background"), defaults.background);

  if (const JsonValue* ch = root.find("clipHeight"); ch != nullptr && ch->type == JsonValue::Type::kObject) {
    p.clip_height_enabled = get_bool(ch->find("enabled"), defaults.clip_height_enabled);
    p.clip_height_min = get_num(ch->find("min"), defaults.clip_height_min);
    p.clip_height_max = get_num(ch->find("max"), defaults.clip_height_max);
  }

  if (const JsonValue* cb = root.find("clipBox"); cb != nullptr && cb->type == JsonValue::Type::kObject) {
    p.clip_box_enabled = get_bool(cb->find("enabled"), defaults.clip_box_enabled);
    p.clip_box_min[0] = defaults.clip_box_min[0];
    p.clip_box_min[1] = defaults.clip_box_min[1];
    p.clip_box_min[2] = defaults.clip_box_min[2];
    p.clip_box_max[0] = defaults.clip_box_max[0];
    p.clip_box_max[1] = defaults.clip_box_max[1];
    p.clip_box_max[2] = defaults.clip_box_max[2];
    get_vec3(cb->find("min"), p.clip_box_min);
    get_vec3(cb->find("max"), p.clip_box_max);
  }

  if (const JsonValue* ov = root.find("overlays"); ov != nullptr && ov->type == JsonValue::Type::kObject) {
    p.show_trajectory = get_bool(ov->find("trajectory"), defaults.show_trajectory);
    p.show_pose_graph = get_bool(ov->find("poseGraph"), defaults.show_pose_graph);
  }

  clamp_display_params(p);
  *out = p;
  return kOkStatus;
}

// ===========================================================================
// DisplayParamsController
// ===========================================================================

struct DisplayParamsController::Impl {
  mutable std::mutex m;
  DisplayParams params;
  std::uint64_t ver = 1;
  bool dirty_flag = false;

  struct Sub {
    DisplayParamsSubscriptionId id;
    DisplayParamsCallback cb;
    void* user;
  };
  std::vector<Sub> subs;
  DisplayParamsSubscriptionId next_sub_id = 1;
};

DisplayParamsController::DisplayParamsController(const DisplayParams& initial) : impl_(new Impl) {
  impl_->params = initial;
  clamp_display_params(impl_->params);
}

DisplayParamsController::~DisplayParamsController() = default;

DisplayParams DisplayParamsController::get() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->params;
}

void DisplayParamsController::set(const DisplayParams& params) {
  DisplayParams clamped = params;
  clamp_display_params(clamped);

  std::vector<Impl::Sub> subs_copy;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->params = clamped;
    ++impl_->ver;
    impl_->dirty_flag = true;
    subs_copy = impl_->subs;  // copied under the lock; invoked outside it
  }
  for (const auto& s : subs_copy) {
    if (s.cb != nullptr) s.cb(clamped, s.user);
  }
}

std::uint64_t DisplayParamsController::version() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->ver;
}

bool DisplayParamsController::dirty() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->dirty_flag;
}

void DisplayParamsController::clear_dirty() noexcept {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->dirty_flag = false;
}

DisplayParamsSubscriptionId DisplayParamsController::subscribe(DisplayParamsCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lock(impl_->m);
  const DisplayParamsSubscriptionId id = impl_->next_sub_id++;
  impl_->subs.push_back(Impl::Sub{id, cb, user_data});
  return id;
}

Status DisplayParamsController::unsubscribe(DisplayParamsSubscriptionId id) {
  std::lock_guard<std::mutex> lock(impl_->m);
  for (auto it = impl_->subs.begin(); it != impl_->subs.end(); ++it) {
    if (it->id == id) {
      impl_->subs.erase(it);
      return kOkStatus;
    }
  }
  return set_last_error(ScanError::kNotFound, "display_params: no such subscription %u",
                         static_cast<unsigned>(id));
}

}  // namespace scanengine
