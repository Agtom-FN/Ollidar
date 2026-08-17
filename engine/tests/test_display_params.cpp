// A14: DisplayParams — validation clamps, colormap ground truth, JSON
// persistence (incl. forward-compat), profile defaults, change notification.
//
// scanengine/cloud/display_params.h is included FIRST, alone, before
// doctest.h — same self-containment discipline as tests/test_headers.cpp.
#include "scanengine/cloud/display_params.h"

#include <cmath>
#include <cstddef>
#include <limits>

#include "doctest.h"

using namespace scanengine;

namespace {

PointVertex vtx(float x, float y, float z, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                 std::uint8_t a = 255) {
  PointVertex v{};
  v.x = x; v.y = y; v.z = z;
  v.r = r; v.g = g; v.b = b; v.a = a;
  return v;
}

}  // namespace

// ===========================================================================
// Uniform block layout — the "documented POD struct" itself
// ===========================================================================

TEST_CASE("display_params/uniform_block_layout") {
  CHECK(sizeof(DisplayParamsUniforms) == 208);
  CHECK(offsetof(DisplayParamsUniforms, color_mode) == 0);
  CHECK(offsetof(DisplayParamsUniforms, background) == 64);
  CHECK(offsetof(DisplayParamsUniforms, fix_quality_colors) == 128);
  // Every std140 "row" boundary is 16-byte aligned.
  CHECK(offsetof(DisplayParamsUniforms, value_min) % 16 == 0);
  CHECK(offsetof(DisplayParamsUniforms, point_size_min_px) % 16 == 0);
  CHECK(offsetof(DisplayParamsUniforms, edl_enabled) % 16 == 0);
  CHECK(offsetof(DisplayParamsUniforms, clip_box_min) % 16 == 0);
  CHECK(offsetof(DisplayParamsUniforms, clip_box_max) % 16 == 0);
  CHECK(offsetof(DisplayParamsUniforms, clip_height_min) % 16 == 0);
}

TEST_CASE("display_params/to_uniforms_selects_active_scalar_mode") {
  DisplayParams p;
  p.color_mode = ColorMode::kHeight;
  p.height.colormap = Colormap::kThermal;
  p.height.gamma = 2.0f;
  p.height.manual_min = -1.0f;
  p.height.manual_max = 4.0f;
  p.height.invert = true;
  p.intensity.colormap = Colormap::kSpectrum;  // must NOT leak into the uniforms

  const DisplayParamsUniforms u = to_uniforms(p);
  CHECK(u.color_mode == static_cast<std::int32_t>(ColorMode::kHeight));
  CHECK(u.colormap == static_cast<std::int32_t>(Colormap::kThermal));
  CHECK(u.gamma == doctest::Approx(2.0f));
  CHECK(u.value_min == doctest::Approx(-1.0f));
  CHECK(u.value_max == doctest::Approx(4.0f));
  CHECK(u.invert == 1);
}

TEST_CASE("display_params/to_uniforms_neutral_for_rgb_and_fix_quality") {
  DisplayParams p;
  p.color_mode = ColorMode::kRgb;
  DisplayParamsUniforms u = to_uniforms(p);
  CHECK(u.colormap == static_cast<std::int32_t>(Colormap::kGrayscale));
  CHECK(u.value_min == doctest::Approx(0.0f));
  CHECK(u.value_max == doctest::Approx(1.0f));
  CHECK(u.gamma == doctest::Approx(1.0f));

  p.color_mode = ColorMode::kFixQuality;
  u = to_uniforms(p);
  CHECK(u.colormap == static_cast<std::int32_t>(Colormap::kGrayscale));
}

TEST_CASE("display_params/to_uniforms_packs_clip_and_background") {
  DisplayParams p;
  p.clip_box_enabled = true;
  p.clip_height_enabled = false;
  p.background = RGBA8{0, 128, 255, 255};
  p.clip_box_min[0] = -2.f; p.clip_box_min[1] = -3.f; p.clip_box_min[2] = -4.f;
  p.clip_box_max[0] = 2.f; p.clip_box_max[1] = 3.f; p.clip_box_max[2] = 4.f;

  const DisplayParamsUniforms u = to_uniforms(p);
  CHECK(u.clip_enabled_mask == 1);  // bit0 only
  CHECK(u.background[0] == doctest::Approx(0.0f));
  CHECK(u.background[1] == doctest::Approx(128.0f / 255.0f));
  CHECK(u.background[2] == doctest::Approx(1.0f));
  CHECK(u.clip_box_min[0] == doctest::Approx(-2.f));
  CHECK(u.clip_box_max[2] == doctest::Approx(4.f));

  p.clip_height_enabled = true;
  CHECK(to_uniforms(p).clip_enabled_mask == 3);  // bit0 + bit1
}

// ===========================================================================
// Validation clamps
// ===========================================================================

TEST_CASE("clamp/point_size_ranges") {
  DisplayParams p;
  p.point_size.fixed_px = -5.0f;
  p.point_size.adaptive_min_px = 999.0f;
  p.point_size.adaptive_max_px = 1.0f;  // inverted vs. min
  p.point_size.world_size_m = -1.0f;
  p.point_size.adaptive_reference_m = 0.0f;
  clamp_display_params(p);

  CHECK(p.point_size.fixed_px == doctest::Approx(0.1f));  // floor lowered per owner round 5.1
  // adaptive_min_px (999) clamps to 64.0, adaptive_max_px (1.0) stays 1.0,
  // then the inverted pair is swapped: min=1.0, max=64.0.
  CHECK(p.point_size.adaptive_min_px == doctest::Approx(1.0f));
  CHECK(p.point_size.adaptive_max_px == doctest::Approx(64.0f));
  CHECK(p.point_size.adaptive_min_px <= p.point_size.adaptive_max_px);
  CHECK(p.point_size.world_size_m == doctest::Approx(0.0005f));
  CHECK(p.point_size.adaptive_reference_m == doctest::Approx(0.01f));
}

TEST_CASE("clamp/point_size_mode_out_of_range_falls_back_to_adaptive") {
  DisplayParams p;
  p.point_size.mode = static_cast<PointSizeMode>(200);
  clamp_display_params(p);
  CHECK(p.point_size.mode == PointSizeMode::kAdaptive);
}

TEST_CASE("clamp/lod_budget_range") {
  DisplayParams p;
  p.lod_point_budget = 0;
  clamp_display_params(p);
  CHECK(p.lod_point_budget == 1000u);

  p.lod_point_budget = 999'999'999u;
  clamp_display_params(p);
  CHECK(p.lod_point_budget == 200'000'000u);

  p.lod_point_budget = 42'000u;
  clamp_display_params(p);
  CHECK(p.lod_point_budget == 42'000u);  // in-range values pass through untouched
}

TEST_CASE("clamp/color_mode_out_of_range_falls_back_to_rgb") {
  DisplayParams p;
  p.color_mode = static_cast<ColorMode>(123);
  clamp_display_params(p);
  CHECK(p.color_mode == ColorMode::kRgb);
}

TEST_CASE("clamp/scalar_params_range_and_swap") {
  DisplayParams p;
  p.height.manual_min = 10.0f;
  p.height.manual_max = -5.0f;  // inverted
  p.height.gamma = 100.0f;
  p.height.brightness = -1.0f;
  p.height.colormap = static_cast<Colormap>(200);
  clamp_display_params(p);

  CHECK(p.height.manual_min == doctest::Approx(-5.0f));
  CHECK(p.height.manual_max == doctest::Approx(10.0f));
  CHECK(p.height.gamma == doctest::Approx(4.0f));
  CHECK(p.height.brightness == doctest::Approx(0.1f));
  CHECK(p.height.colormap == Colormap::kGrayscale);
}

TEST_CASE("clamp/nan_and_inf_replaced_with_defaults") {
  DisplayParams p;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  p.point_size.fixed_px = nan;
  p.edl_strength = inf;
  p.height.gamma = nan;
  p.clip_height_min = -inf;
  clamp_display_params(p);

  CHECK(std::isfinite(p.point_size.fixed_px));
  CHECK(std::isfinite(p.edl_strength));
  CHECK(std::isfinite(p.height.gamma));
  CHECK(std::isfinite(p.clip_height_min));
  CHECK(p.edl_strength >= 0.0f);
  CHECK(p.edl_strength <= 1.0f);
}

TEST_CASE("clamp/edl_strength_range") {
  DisplayParams p;
  p.edl_strength = -3.0f;
  clamp_display_params(p);
  CHECK(p.edl_strength == doctest::Approx(0.0f));

  p.edl_strength = 3.0f;
  clamp_display_params(p);
  CHECK(p.edl_strength == doctest::Approx(1.0f));
}

TEST_CASE("clamp/height_clip_swap") {
  DisplayParams p;
  p.clip_height_min = 5.0f;
  p.clip_height_max = 1.0f;
  clamp_display_params(p);
  CHECK(p.clip_height_min == doctest::Approx(1.0f));
  CHECK(p.clip_height_max == doctest::Approx(5.0f));
}

TEST_CASE("clamp/box_clip_swap_per_axis") {
  DisplayParams p;
  p.clip_box_min[0] = 5.0f; p.clip_box_max[0] = -5.0f;  // x inverted
  p.clip_box_min[1] = -2.0f; p.clip_box_max[1] = 2.0f;  // y already sane
  p.clip_box_min[2] = 9.0f; p.clip_box_max[2] = 1.0f;   // z inverted
  clamp_display_params(p);
  CHECK(p.clip_box_min[0] == doctest::Approx(-5.0f));
  CHECK(p.clip_box_max[0] == doctest::Approx(5.0f));
  CHECK(p.clip_box_min[1] == doctest::Approx(-2.0f));
  CHECK(p.clip_box_max[1] == doctest::Approx(2.0f));
  CHECK(p.clip_box_min[2] == doctest::Approx(1.0f));
  CHECK(p.clip_box_max[2] == doctest::Approx(9.0f));
}

TEST_CASE("clamp/is_idempotent") {
  DisplayParams p;
  p.point_size.fixed_px = -1.0f;
  p.lod_point_budget = 0;
  p.height.manual_min = 99.0f;
  p.height.manual_max = -99.0f;
  clamp_display_params(p);
  DisplayParams once = p;
  clamp_display_params(p);
  CHECK(p == once);
}

// ===========================================================================
// Colormaps: endpoints, monotonicity, hand-computed interior values
// ===========================================================================

TEST_CASE("colormap/grayscale_endpoints_and_hand_computed_midpoint") {
  CHECK(evaluate_colormap(Colormap::kGrayscale, 0.0f) == RGBA8{0, 0, 0, 255});
  CHECK(evaluate_colormap(Colormap::kGrayscale, 1.0f) == RGBA8{255, 255, 255, 255});
  // Grayscale is an exact linear ramp (LUT[i] == i for all 256 entries, so
  // linear interpolation reproduces round(t*255) exactly) — hand-computed:
  // t=0.5 -> 0.5*255 = 127.5 -> round-half-up -> 128.
  const RGBA8 mid = evaluate_colormap(Colormap::kGrayscale, 0.5f);
  CHECK(mid.r == 128);
  CHECK(mid.g == 128);
  CHECK(mid.b == 128);
  // t=0.2 -> 0.2*255 = 51.0 exactly.
  CHECK(evaluate_colormap(Colormap::kGrayscale, 0.2f).r == 51);
}

TEST_CASE("colormap/grayscale_monotonic") {
  std::uint8_t prev = 0;
  for (int i = 0; i <= 20; ++i) {
    const float t = static_cast<float>(i) / 20.0f;
    const RGBA8 c = evaluate_colormap(Colormap::kGrayscale, t);
    CHECK(c.r >= prev);
    prev = c.r;
  }
}

TEST_CASE("colormap/spectrum_endpoints_exact") {
  // Hand-derived from the HSV construction (see the header/impl comments):
  // t=0 -> h=240 (pure blue), t=1 -> h=0 (pure red). Both land exactly on a
  // LUT sample (pos=0 and pos=255), so there is no interpolation rounding.
  CHECK(evaluate_colormap(Colormap::kSpectrum, 0.0f) == RGBA8{0, 0, 255, 255});
  CHECK(evaluate_colormap(Colormap::kSpectrum, 1.0f) == RGBA8{255, 0, 0, 255});
}

TEST_CASE("colormap/spectrum_channel_monotonicity") {
  // Derived by hand from the six HSV 60-degree segments swept from h=240
  // down to h=0 (see src/cloud/display_params.cpp's spectrum_raw comment):
  // red is non-decreasing and blue is non-increasing across the whole
  // sweep, even though green is not monotonic (rises then falls).
  std::uint8_t prev_r = 0, prev_b = 255;
  for (int i = 0; i <= 40; ++i) {
    const float t = static_cast<float>(i) / 40.0f;
    const RGBA8 c = evaluate_colormap(Colormap::kSpectrum, t);
    CHECK(c.r >= prev_r);
    CHECK(c.b <= prev_b);
    prev_r = c.r;
    prev_b = c.b;
  }
}

TEST_CASE("colormap/thermal_endpoints_exact") {
  CHECK(evaluate_colormap(Colormap::kThermal, 0.0f) == RGBA8{0, 0, 0, 255});
  CHECK(evaluate_colormap(Colormap::kThermal, 1.0f) == RGBA8{255, 255, 255, 255});
}

TEST_CASE("colormap/thermal_channel_monotonicity") {
  // By construction (black -> red -> yellow -> white, piecewise linear)
  // every channel individually is non-decreasing across the whole ramp.
  std::uint8_t prev_r = 0, prev_g = 0, prev_b = 0;
  for (int i = 0; i <= 40; ++i) {
    const float t = static_cast<float>(i) / 40.0f;
    const RGBA8 c = evaluate_colormap(Colormap::kThermal, t);
    CHECK(c.r >= prev_r);
    CHECK(c.g >= prev_g);
    CHECK(c.b >= prev_b);
    prev_r = c.r; prev_g = c.g; prev_b = c.b;
  }
}

TEST_CASE("colormap/out_of_range_t_is_clamped") {
  CHECK(evaluate_colormap(Colormap::kGrayscale, -5.0f) == evaluate_colormap(Colormap::kGrayscale, 0.0f));
  CHECK(evaluate_colormap(Colormap::kGrayscale, 5.0f) == evaluate_colormap(Colormap::kGrayscale, 1.0f));
}

TEST_CASE("colormap/lut_alpha_is_always_opaque") {
  for (int cm = 0; cm < kColormapCount; ++cm) {
    const auto& lut = colormap_lut(static_cast<Colormap>(cm));
    for (const RGBA8& c : lut) CHECK(c.a == 255);
  }
}

// ===========================================================================
// Ground-truth per-point colour evaluation
// ===========================================================================

TEST_CASE("evaluate/rgb_mode_is_passthrough") {
  DisplayParams p;
  p.color_mode = ColorMode::kRgb;
  const PointVertex v = vtx(1, 2, 3, 10, 20, 30, 200);
  const RGBA8 c = evaluate_point_color(v, PointAttributes{}, p);
  CHECK(c == RGBA8{10, 20, 30, 200});
}

TEST_CASE("evaluate/height_mode_hand_computed") {
  DisplayParams p;
  p.color_mode = ColorMode::kHeight;
  p.height.auto_range = false;
  p.height.manual_min = 0.0f;
  p.height.manual_max = 10.0f;
  p.height.gamma = 1.0f;
  p.height.brightness = 1.0f;
  p.height.invert = false;
  p.height.colormap = Colormap::kGrayscale;

  // z=5 -> t=(5-0)/(10-0)=0.5 -> grayscale(0.5) -> 128 (see the grayscale
  // hand-computation above). Alpha must be v.a (170), never derived.
  const PointVertex v = vtx(0, 0, 5.0f, 1, 2, 3, 170);
  const RGBA8 c = evaluate_point_color(v, PointAttributes{}, p);
  CHECK(c.r == 128);
  CHECK(c.g == 128);
  CHECK(c.b == 128);
  CHECK(c.a == 170);
}

TEST_CASE("evaluate/height_mode_clamps_outside_range") {
  DisplayParams p;
  p.color_mode = ColorMode::kHeight;
  p.height.manual_min = 0.0f;
  p.height.manual_max = 10.0f;
  p.height.colormap = Colormap::kGrayscale;

  CHECK(evaluate_point_color(vtx(0, 0, -100.f, 0, 0, 0), {}, p) == RGBA8{0, 0, 0, 255});
  CHECK(evaluate_point_color(vtx(0, 0, 1000.f, 0, 0, 0), {}, p) == RGBA8{255, 255, 255, 255});
}

TEST_CASE("evaluate/height_mode_invert") {
  DisplayParams p;
  p.color_mode = ColorMode::kHeight;
  p.height.manual_min = 0.0f;
  p.height.manual_max = 10.0f;
  p.height.colormap = Colormap::kGrayscale;
  p.height.invert = true;
  // z=2.5 -> t=0.25 -> inverted -> 0.75 -> 0.75*255=191.25 -> round -> 191.
  const RGBA8 c = evaluate_point_color(vtx(0, 0, 2.5f, 0, 0, 0), {}, p);
  CHECK(c.r == 191);
}

TEST_CASE("evaluate/intensity_mode_defaults_to_luminance_of_rgb") {
  DisplayParams p;
  p.color_mode = ColorMode::kIntensity;
  p.intensity.manual_min = 0.0f;
  p.intensity.manual_max = 1.0f;
  p.intensity.colormap = Colormap::kGrayscale;
  // Hand-computed luminance: 0.299*100 + 0.587*150 + 0.114*200 = 140.75;
  // /255 = 0.55196...; through [0,1] grayscale that's round(140.75) = 141
  // (this is exactly the same value as the raw luminance sum because the
  // manual range is 0..1 and the colormap re-multiplies by 255).
  const PointVertex v = vtx(0, 0, 0, 100, 150, 200, 255);
  const RGBA8 c = evaluate_point_color(v, PointAttributes{}, p);
  CHECK(c.r == 141);
  CHECK(c.g == 141);
  CHECK(c.b == 141);
}

TEST_CASE("evaluate/intensity_mode_explicit_attribute_overrides_luminance") {
  DisplayParams p;
  p.color_mode = ColorMode::kIntensity;
  p.intensity.manual_min = 0.0f;
  p.intensity.manual_max = 1.0f;
  p.intensity.colormap = Colormap::kGrayscale;

  PointAttributes attrs;
  attrs.has_intensity = true;
  attrs.intensity = 1.0f;  // deliberately contradicts the vertex's dark RGB
  const PointVertex v = vtx(0, 0, 0, 5, 5, 5, 255);
  const RGBA8 c = evaluate_point_color(v, attrs, p);
  CHECK(c.r == 255);
}

TEST_CASE("evaluate/time_mode_falls_back_to_rgb_without_time_attribute") {
  DisplayParams p;
  p.color_mode = ColorMode::kTime;
  const PointVertex v = vtx(0, 0, 0, 7, 8, 9, 255);
  const RGBA8 c = evaluate_point_color(v, PointAttributes{}, p);
  CHECK(c == RGBA8{7, 8, 9, 255});
}

TEST_CASE("evaluate/time_mode_with_attribute") {
  DisplayParams p;
  p.color_mode = ColorMode::kTime;
  p.time.manual_min = 0.0;
  p.time.manual_max = 10.0;
  p.time.colormap = Colormap::kGrayscale;

  PointAttributes attrs;
  attrs.has_time = true;
  attrs.t_seconds = 5.0;
  const RGBA8 c = evaluate_point_color(vtx(0, 0, 0, 0, 0, 0), attrs, p);
  CHECK(c.r == 128);  // same 0.5 -> 128 hand computation as the height case
}

TEST_CASE("evaluate/fix_quality_mode_table_lookup") {
  DisplayParams p;
  p.color_mode = ColorMode::kFixQuality;

  PointAttributes attrs;
  attrs.has_fix = true;
  attrs.fix = FixType::kRtkFixed;
  const PointVertex v = vtx(0, 0, 0, 1, 1, 1, 42);
  const RGBA8 c = evaluate_point_color(v, attrs, p);
  CHECK(c.r == p.fix_quality_colors[static_cast<int>(FixType::kRtkFixed)].r);
  CHECK(c.g == p.fix_quality_colors[static_cast<int>(FixType::kRtkFixed)].g);
  CHECK(c.b == p.fix_quality_colors[static_cast<int>(FixType::kRtkFixed)].b);
  CHECK(c.a == 42);  // alpha is still v.a, not the palette's alpha
}

TEST_CASE("evaluate/fix_quality_mode_falls_back_to_rgb_without_attribute") {
  DisplayParams p;
  p.color_mode = ColorMode::kFixQuality;
  const PointVertex v = vtx(0, 0, 0, 11, 22, 33, 255);
  CHECK(evaluate_point_color(v, PointAttributes{}, p) == RGBA8{11, 22, 33, 255});
}

// ===========================================================================
// Profile defaults
// ===========================================================================

TEST_CASE("profiles/all_are_already_clamped") {
  for (int i = 0; i < kDisplayProfileCount; ++i) {
    DisplayParams p = profile_defaults(static_cast<DisplayProfile>(i));
    DisplayParams clamped = p;
    clamp_display_params(clamped);
    CHECK(p == clamped);
  }
}

TEST_CASE("profiles/survey") {
  const DisplayParams p = profile_defaults(DisplayProfile::kSurvey);
  CHECK(p.color_mode == ColorMode::kHeight);
  CHECK(p.show_trajectory == true);
  CHECK(p.show_pose_graph == true);
  CHECK(p.edl_enabled == true);
}

TEST_CASE("profiles/floor_plan_matches_slice_band") {
  const DisplayParams p = profile_defaults(DisplayProfile::kFloorPlan);
  // Matches plan/floor_plan.h's SliceOptions default z band (1.0..1.5 m).
  CHECK(p.clip_height_enabled == true);
  CHECK(p.clip_height_min == doctest::Approx(1.0f));
  CHECK(p.clip_height_max == doctest::Approx(1.5f));
  CHECK(p.show_trajectory == false);
}

TEST_CASE("profiles/research_is_rgb_and_highest_budget") {
  const DisplayParams p = profile_defaults(DisplayProfile::kResearch);
  CHECK(p.color_mode == ColorMode::kRgb);
  const DisplayParams survey = profile_defaults(DisplayProfile::kSurvey);
  const DisplayParams floor = profile_defaults(DisplayProfile::kFloorPlan);
  const DisplayParams quick = profile_defaults(DisplayProfile::kQuickScan);
  CHECK(p.lod_point_budget >= survey.lod_point_budget);
  CHECK(p.lod_point_budget >= floor.lod_point_budget);
  CHECK(p.lod_point_budget >= quick.lod_point_budget);
}

TEST_CASE("profiles/quick_scan_is_cheapest") {
  const DisplayParams p = profile_defaults(DisplayProfile::kQuickScan);
  CHECK(p.edl_enabled == false);
  CHECK(p.lod_point_budget == 2'000'000u);  // §3.12 Android reference target
}

TEST_CASE("profiles/pairwise_distinct") {
  DisplayParams all[4];
  for (int i = 0; i < 4; ++i) all[i] = profile_defaults(static_cast<DisplayProfile>(i));
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      CHECK(all[i] != all[j]);
    }
  }
}

TEST_CASE("profiles/to_string_round_trips_through_names") {
  CHECK(std::string(to_string(DisplayProfile::kSurvey)) == "survey");
  CHECK(std::string(to_string(DisplayProfile::kFloorPlan)) == "floorPlan");
  CHECK(std::string(to_string(DisplayProfile::kResearch)) == "research");
  CHECK(std::string(to_string(DisplayProfile::kQuickScan)) == "quickScan");
}

// ===========================================================================
// JSON persistence: round-trip, schema versioning, forward-compat
// ===========================================================================

TEST_CASE("json/default_round_trip") {
  const DisplayParams original;
  const std::string j = to_json(original);
  DisplayParams parsed;
  const Status st = from_json(j, &parsed);
  REQUIRE(st.ok());
  CHECK(parsed == original);
}

TEST_CASE("json/customized_round_trip") {
  DisplayParams original = profile_defaults(DisplayProfile::kFloorPlan);
  original.point_size.mode = PointSizeMode::kWorldSize;
  original.point_size.world_size_m = 0.0234f;
  original.color_mode = ColorMode::kIntensity;
  original.intensity.colormap = Colormap::kThermal;
  original.intensity.gamma = 1.75f;
  original.intensity.manual_min = -0.125f;
  original.background = RGBA8{9, 200, 33, 128};
  original.clip_box_enabled = true;
  original.clip_box_min[0] = -3.5f;
  original.fix_quality_colors[2] = RGBA8{1, 2, 3, 4};
  original.show_pose_graph = true;
  clamp_display_params(original);

  const std::string j = to_json(original);
  DisplayParams parsed;
  const Status st = from_json(j, &parsed);
  REQUIRE(st.ok());
  CHECK(parsed == original);
}

TEST_CASE("json/every_colormap_and_color_mode_round_trips") {
  for (int cm = 0; cm < kColorModeCount; ++cm) {
    for (int lut = 0; lut < kColormapCount; ++lut) {
      DisplayParams p;
      p.color_mode = static_cast<ColorMode>(cm);
      p.height.colormap = static_cast<Colormap>(lut);
      p.intensity.colormap = static_cast<Colormap>(lut);
      p.time.colormap = static_cast<Colormap>(lut);
      clamp_display_params(p);
      DisplayParams parsed;
      REQUIRE(from_json(to_json(p), &parsed).ok());
      CHECK(parsed == p);
    }
  }
}

TEST_CASE("json/unknown_top_level_and_nested_keys_are_ignored") {
  const std::string j = R"({
    "schemaVersion": 1,
    "somethingFromTheFuture": {"a": 1, "b": [1,2,3]},
    "colorMode": "height",
    "pointSize": {"mode": "fixedPixels", "fixedPx": 3.5, "aFutureField": true},
    "height": {"manualMin": 0.0, "manualMax": 2.0, "colormap": "thermal"}
  })";
  DisplayParams parsed;
  const Status st = from_json(j, &parsed);
  REQUIRE(st.ok());
  CHECK(parsed.color_mode == ColorMode::kHeight);
  CHECK(parsed.point_size.mode == PointSizeMode::kFixedPixels);
  CHECK(parsed.point_size.fixed_px == doctest::Approx(3.5f));
  CHECK(parsed.height.manual_max == doctest::Approx(2.0f));
  CHECK(parsed.height.colormap == Colormap::kThermal);
}

TEST_CASE("json/missing_keys_fall_back_to_supplied_defaults") {
  DisplayParams defaults = profile_defaults(DisplayProfile::kResearch);
  const std::string j = R"({"schemaVersion": 1, "colorMode": "fixQuality"})";
  DisplayParams parsed;
  REQUIRE(from_json(j, &parsed, defaults).ok());
  CHECK(parsed.color_mode == ColorMode::kFixQuality);       // overridden
  CHECK(parsed.lod_point_budget == defaults.lod_point_budget);  // inherited
  CHECK(parsed.point_size.mode == defaults.point_size.mode);    // inherited
}

TEST_CASE("json/future_schema_version_still_parses") {
  const std::string j = R"({"schemaVersion": 99, "colorMode": "time", "extraFutureTopLevelThing": 42})";
  DisplayParams parsed;
  REQUIRE(from_json(j, &parsed).ok());
  CHECK(parsed.color_mode == ColorMode::kTime);
}

TEST_CASE("json/malformed_document_returns_corrupt_data_and_leaves_out_untouched") {
  DisplayParams sentinel = profile_defaults(DisplayProfile::kQuickScan);
  DisplayParams out = sentinel;

  SUBCASE("not an object") {
    CHECK(from_json("[1, 2, 3]", &out) == Status(ScanError::kCorruptData));
  }
  SUBCASE("garbage") {
    CHECK(from_json("not json at all {{{", &out) == Status(ScanError::kCorruptData));
  }
  SUBCASE("truncated") {
    CHECK(from_json(R"({"colorMode": "hei)", &out) == Status(ScanError::kCorruptData));
  }
  SUBCASE("empty string") {
    CHECK(from_json("", &out) == Status(ScanError::kCorruptData));
  }

  CHECK(out == sentinel);
}

TEST_CASE("json/wrong_typed_field_falls_back_to_default_rather_than_corrupting") {
  const std::string j = R"({"colorMode": 12345, "lodPointBudget": "not a number"})";
  DisplayParams defaults;
  DisplayParams parsed;
  REQUIRE(from_json(j, &parsed, defaults).ok());
  CHECK(parsed.color_mode == defaults.color_mode);
  CHECK(parsed.lod_point_budget == defaults.lod_point_budget);
}

TEST_CASE("json/output_is_clamped") {
  // A hand-authored document with an out-of-range value must come back
  // clamped, exactly like a mutated-in-memory DisplayParams would.
  const std::string j = R"({"lodPointBudget": 999999999, "edl": {"enabled": true, "strength": 5.0}})";
  DisplayParams parsed;
  REQUIRE(from_json(j, &parsed).ok());
  CHECK(parsed.lod_point_budget == 200'000'000u);
  CHECK(parsed.edl_strength == doctest::Approx(1.0f));
}

// ===========================================================================
// Change notification: version, dirty flag, subscribe/unsubscribe
// ===========================================================================

TEST_CASE("controller/get_returns_clamped_initial_value") {
  DisplayParams initial;
  initial.lod_point_budget = 0;  // invalid
  DisplayParamsController ctl(initial);
  CHECK(ctl.get().lod_point_budget == 1000u);
}

TEST_CASE("controller/set_bumps_version_and_dirty") {
  DisplayParamsController ctl;
  const std::uint64_t v0 = ctl.version();
  CHECK(ctl.dirty() == false);

  DisplayParams p = ctl.get();
  p.color_mode = ColorMode::kHeight;
  ctl.set(p);

  CHECK(ctl.version() == v0 + 1);
  CHECK(ctl.dirty() == true);
  CHECK(ctl.get().color_mode == ColorMode::kHeight);

  ctl.clear_dirty();
  CHECK(ctl.dirty() == false);
}

TEST_CASE("controller/set_clamps_before_storing") {
  DisplayParamsController ctl;
  DisplayParams p = ctl.get();
  p.edl_strength = 42.0f;
  ctl.set(p);
  CHECK(ctl.get().edl_strength == doctest::Approx(1.0f));
}

namespace {
struct CallbackRecorder {
  int calls = 0;
  ColorMode last_mode = ColorMode::kRgb;
};

void record_callback(const DisplayParams& p, void* user) {
  auto* r = static_cast<CallbackRecorder*>(user);
  ++r->calls;
  r->last_mode = p.color_mode;
}
}  // namespace

TEST_CASE("controller/subscribers_are_notified_on_set") {
  DisplayParamsController ctl;
  CallbackRecorder rec;
  const DisplayParamsSubscriptionId id = ctl.subscribe(&record_callback, &rec);

  DisplayParams p = ctl.get();
  p.color_mode = ColorMode::kIntensity;
  ctl.set(p);

  CHECK(rec.calls == 1);
  CHECK(rec.last_mode == ColorMode::kIntensity);

  p.color_mode = ColorMode::kTime;
  ctl.set(p);
  CHECK(rec.calls == 2);
  CHECK(rec.last_mode == ColorMode::kTime);

  CHECK(ctl.unsubscribe(id).ok());
  p.color_mode = ColorMode::kFixQuality;
  ctl.set(p);
  CHECK(rec.calls == 2);  // no further notification after unsubscribe
}

TEST_CASE("controller/unsubscribe_unknown_id_fails") {
  DisplayParamsController ctl;
  CHECK_FALSE(ctl.unsubscribe(9999).ok());
}

TEST_CASE("controller/multiple_independent_pollers_via_version") {
  DisplayParamsController ctl;
  std::uint64_t poller_a = ctl.version();
  std::uint64_t poller_b = ctl.version();

  DisplayParams p = ctl.get();
  p.show_trajectory = !p.show_trajectory;
  ctl.set(p);

  CHECK(ctl.version() != poller_a);
  CHECK(ctl.version() != poller_b);
  poller_a = ctl.version();
  CHECK(poller_a == poller_b + 1);
}

// ===========================================================================
// RGBA8 basics
// ===========================================================================

TEST_CASE("rgba8/equality") {
  CHECK(RGBA8{1, 2, 3, 4} == RGBA8{1, 2, 3, 4});
  CHECK(RGBA8{1, 2, 3, 4} != RGBA8{1, 2, 3, 5});
}
