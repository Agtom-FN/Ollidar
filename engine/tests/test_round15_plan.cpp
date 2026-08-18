// test_round15_plan.cpp — ROUND 15 item 56. The floor plan, on the phone.
//
// Two things are new and each has its own risk:
//
//   * A PNG WRITER. The risk is the FORMAT, so the test reads the bytes back
//     with an independent decoder written here — signature, IHDR fields, every
//     chunk CRC recomputed, the zlib header and Adler-32 checked, the STORED
//     deflate blocks walked and the scanlines compared pixel for pixel against
//     what was handed in. Exactly the posture plan_writers.h takes for DXF
//     ("the test's independent reader is a group-code tokenizer rather than a
//     second binary decoder").
//
//   * CONTAINER -> PLAN. The risk is the UP AXIS. A12 defaults to Z-up
//     because a Mid-360 session is gravity-aligned that way; a D6 session's
//     world is ARCore's, where +Y is up. Slicing a D6 cloud at Z 1.0-1.5 m
//     cuts a VERTICAL slab through the room. The last case builds a synthetic
//     room with a known footprint and asserts that the Y-up slice recovers it
//     and the Z-up slice does not — which is the shipped Android bug this
//     round found (processing_engine.cpp hard-coded UpAxis::kZ).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/plan/plan_raster.h"

using namespace scanengine;

namespace {

// --- an independent PNG reader ----------------------------------------------

std::uint32_t be32(const std::string& s, std::size_t off) {
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[off])) << 24) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[off + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[off + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[off + 3]));
}

std::uint32_t crc32_ref(const std::uint8_t* d, std::size_t n) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return c ^ 0xFFFFFFFFu;
}

struct DecodedPng {
  bool ok = false;
  std::uint32_t w = 0, h = 0;
  std::uint8_t depth = 0, color = 0;
  std::vector<std::uint8_t> rgb;
  std::string why;
};

DecodedPng decode_png(const std::string& s) {
  DecodedPng d;
  const char sig[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
  if (s.size() < 8 || std::string(sig, 8) != s.substr(0, 8)) {
    d.why = "signature";
    return d;
  }
  std::size_t off = 8;
  std::string idat;
  bool have_ihdr = false, have_iend = false;
  while (off + 12 <= s.size()) {
    const std::uint32_t len = be32(s, off);
    const std::string type = s.substr(off + 4, 4);
    if (off + 12 + len > s.size()) {
      d.why = "truncated chunk " + type;
      return d;
    }
    const std::uint32_t want = be32(s, off + 8 + len);
    const std::uint32_t got =
        crc32_ref(reinterpret_cast<const std::uint8_t*>(s.data() + off + 4), len + 4);
    if (want != got) {
      d.why = "crc on " + type;
      return d;
    }
    if (type == "IHDR") {
      d.w = be32(s, off + 8);
      d.h = be32(s, off + 12);
      d.depth = static_cast<std::uint8_t>(s[off + 16]);
      d.color = static_cast<std::uint8_t>(s[off + 17]);
      if (s[off + 18] != 0 || s[off + 19] != 0 || s[off + 20] != 0) {
        d.why = "compression/filter/interlace";
        return d;
      }
      have_ihdr = true;
    } else if (type == "IDAT") {
      idat += s.substr(off + 8, len);
    } else if (type == "IEND") {
      have_iend = true;
    }
    off += 12 + len;
  }
  if (!have_ihdr || !have_iend || off != s.size()) {
    d.why = "chunk structure";
    return d;
  }

  // zlib: 2-byte header, STORED deflate blocks, 4-byte Adler-32.
  if (idat.size() < 6 || (static_cast<std::uint8_t>(idat[0]) & 0x0Fu) != 8) {
    d.why = "zlib header";
    return d;
  }
  const std::uint32_t hdr = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(idat[0])) << 8) |
                            static_cast<std::uint8_t>(idat[1]);
  if (hdr % 31u != 0u) {
    d.why = "zlib FCHECK";
    return d;
  }
  std::vector<std::uint8_t> raw;
  std::size_t p = 2;
  bool last = false;
  while (!last && p + 5 <= idat.size() - 4) {
    const std::uint8_t bh = static_cast<std::uint8_t>(idat[p]);
    if ((bh & 0x06u) != 0u) {
      d.why = "not a STORED block";
      return d;
    }
    last = (bh & 1u) != 0u;
    const std::size_t n = static_cast<std::uint8_t>(idat[p + 1]) |
                          (static_cast<std::size_t>(static_cast<std::uint8_t>(idat[p + 2])) << 8);
    const std::size_t nlen = static_cast<std::uint8_t>(idat[p + 3]) |
                             (static_cast<std::size_t>(static_cast<std::uint8_t>(idat[p + 4])) << 8);
    if ((n ^ 0xFFFFu) != nlen) {
      d.why = "LEN/NLEN";
      return d;
    }
    p += 5;
    raw.insert(raw.end(), idat.begin() + static_cast<std::ptrdiff_t>(p),
               idat.begin() + static_cast<std::ptrdiff_t>(p + n));
    p += n;
  }
  std::uint32_t a = 1, b = 0;
  for (std::uint8_t v : raw) {
    a = (a + v) % 65521u;
    b = (b + a) % 65521u;
  }
  const std::uint32_t adler = be32(idat, idat.size() - 4);
  if (adler != ((b << 16) | a)) {
    d.why = "adler32";
    return d;
  }

  const std::size_t stride = static_cast<std::size_t>(d.w) * 3 + 1;
  if (raw.size() != stride * d.h) {
    d.why = "raw size";
    return d;
  }
  d.rgb.reserve(static_cast<std::size_t>(d.w) * d.h * 3);
  for (std::uint32_t y = 0; y < d.h; ++y) {
    if (raw[y * stride] != 0) {
      d.why = "filter byte";
      return d;
    }
    d.rgb.insert(d.rgb.end(), raw.begin() + static_cast<std::ptrdiff_t>(y * stride + 1),
                 raw.begin() + static_cast<std::ptrdiff_t>((y + 1) * stride));
  }
  d.ok = true;
  return d;
}

// --- a synthetic room --------------------------------------------------------
//
// A 6.0 x 4.0 m rectangle of wall, both faces, from 0.1 m to 2.4 m high, in a
// Y-UP world (ARCore's). 12 mm point spacing so the 2 cm plan grid is
// comfortably fed on both faces — which is what lets face pairing measure a
// thickness rather than assume one.
constexpr double kRoomX = 6.0;
constexpr double kRoomZ = 4.0;
constexpr double kWallT = 0.12;

std::vector<PointVertex> synth_room_y_up() {
  std::vector<PointVertex> pts;
  const double step = 0.012;
  auto add = [&](double x, double y, double z) {
    PointVertex v{};
    v.x = static_cast<float>(x);
    v.y = static_cast<float>(y);
    v.z = static_cast<float>(z);
    v.a = 255;
    pts.push_back(v);
  };
  for (double y = 0.10; y <= 2.40; y += 0.05) {
    for (double x = 0.0; x <= kRoomX; x += step) {
      add(x, y, 0.0);
      add(x, y, -kWallT);
      add(x, y, kRoomZ);
      add(x, y, kRoomZ + kWallT);
    }
    for (double z = 0.0; z <= kRoomZ; z += step) {
      add(0.0, y, z);
      add(-kWallT, y, z);
      add(kRoomX, y, z);
      add(kRoomX + kWallT, y, z);
    }
  }
  return pts;
}

plan::PlanModel extract(const std::vector<PointVertex>& pts, plan::UpAxis up) {
  plan::PlanInput in;
  in.points = Span<const PointVertex>(pts.data(), pts.size());
  in.up = up;
  plan::PlanOptions po;
  po.slice.up = up;
  po.slice.z_min_m = 1.0f;
  po.slice.z_max_m = 1.5f;
  plan::PlanModel m;
  const Status s = plan::extract_floor_plan(in, po, &m);
  CHECK(s.ok());
  return m;
}

}  // namespace

// ===========================================================================
// 1. THE PNG IS A PNG
// ===========================================================================
TEST_CASE("round15/the_png_encoder_writes_something_a_decoder_accepts") {
  constexpr std::uint32_t kW = 37, kH = 11;  // not multiples of anything
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(kW) * kH * 3);
  for (std::size_t i = 0; i < rgb.size(); ++i) {
    rgb[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
  }
  std::string png;
  REQUIRE(plan::encode_png_rgb8(rgb.data(), kW, kH, &png).ok());

  const DecodedPng d = decode_png(png);
  INFO("decode said: ", d.why);
  REQUIRE(d.ok);
  CHECK(d.w == kW);
  CHECK(d.h == kH);
  CHECK(d.depth == 8);
  CHECK(d.color == 2);  // truecolour RGB
  REQUIRE(d.rgb.size() == rgb.size());
  CHECK(std::equal(rgb.begin(), rgb.end(), d.rgb.begin()));

  // Determinism: the same pixels give the same bytes. There is no clock, no
  // compression level and no library version in this path, and that is the
  // whole reason it is hand-rolled.
  std::string again;
  REQUIRE(plan::encode_png_rgb8(rgb.data(), kW, kH, &again).ok());
  CHECK(png == again);

  // A block boundary: 65535 stored bytes per block, so an image whose raw
  // stream crosses it exercises the multi-block path.
  constexpr std::uint32_t kBigW = 400, kBigH = 100;  // 400*3+1 = 1201 B/row
  std::vector<std::uint8_t> big(static_cast<std::size_t>(kBigW) * kBigH * 3, 0x5Au);
  std::string bigpng;
  REQUIRE(plan::encode_png_rgb8(big.data(), kBigW, kBigH, &bigpng).ok());
  const DecodedPng bd = decode_png(bigpng);
  INFO("big decode said: ", bd.why);
  REQUIRE(bd.ok);
  CHECK(bd.w == kBigW);
  CHECK(bd.h == kBigH);
  CHECK(bd.rgb == big);

  // And the refusals.
  std::string dummy;
  CHECK_FALSE(plan::encode_png_rgb8(rgb.data(), 0, kH, &dummy).ok());
  CHECK_FALSE(plan::encode_png_rgb8(nullptr, kW, kH, &dummy).ok());
}

// ===========================================================================
// 2. THE PLAN RENDERER DRAWS THE MODEL, AND SAYS WHICH MODE IT IS IN
// ===========================================================================
TEST_CASE("round15/the_plan_renderer_is_deterministic_and_states_its_mode") {
  const std::vector<PointVertex> pts = synth_room_y_up();
  const plan::PlanModel model = extract(pts, plan::UpAxis::kY);
  REQUIRE(model.walls.size() >= 4);

  plan::PlanInput in;
  in.points = Span<const PointVertex>(pts.data(), pts.size());
  in.up = plan::UpAxis::kY;
  plan::SliceOptions so;
  so.up = plan::UpAxis::kY;
  plan::OccupancyGrid grid;
  REQUIRE(plan::build_occupancy(in, plan::main_band(so), &grid).ok());

  plan::PlanRasterOptions ro;
  ro.max_dimension_px = 600;
  ro.title = "ROOM";
  plan::PlanRasterInfo info;
  std::string a, b;
  REQUIRE(plan::build_plan_png(model, &grid, ro, &a, &info).ok());
  REQUIRE(plan::build_plan_png(model, &grid, ro, &b, nullptr).ok());
  CHECK(a == b);  // byte-identical run to run

  CHECK(info.mode == plan::PlanRenderMode::kWalls);
  CHECK(info.px_per_m > 0.0);
  // A 6 m room at a 600 px sheet: the scale bar picks off the 1/2/5 ladder.
  CHECK((info.scale_bar_m == doctest::Approx(1.0) || info.scale_bar_m == doctest::Approx(2.0)));
  const DecodedPng d = decode_png(a);
  INFO("decode said: ", d.why);
  REQUIRE(d.ok);
  CHECK(d.w == info.width_px);
  CHECK(d.h == info.height_px);

  // An empty model over the same grid is the DENSITY fallback, not an empty
  // sheet — the honest answer when nothing could be fitted.
  plan::PlanModel empty;
  empty.slice_z_min_m = model.slice_z_min_m;
  empty.slice_z_max_m = model.slice_z_max_m;
  plan::PlanRasterInfo dinfo;
  std::string dpng;
  REQUIRE(plan::build_plan_png(empty, &grid, ro, &dpng, &dinfo).ok());
  CHECK(dinfo.mode == plan::PlanRenderMode::kDensity);
  CHECK(dinfo.density_cells_drawn > 100);
  CHECK(decode_png(dpng).ok);

  // Nothing at all is an error, not a blank sheet with a scale bar on it.
  std::string none;
  CHECK_FALSE(plan::build_plan_png(empty, nullptr, ro, &none, nullptr).ok());
}

// ===========================================================================
// 3. THE UP AXIS — THE BUG THIS ROUND FOUND ON THE PHONE
// ===========================================================================
TEST_CASE("round15/a_d6_plan_must_be_sliced_on_the_arcore_up_axis") {
  const std::vector<PointVertex> pts = synth_room_y_up();

  const plan::PlanModel correct = extract(pts, plan::UpAxis::kY);
  const plan::PlanModel shipped = extract(pts, plan::UpAxis::kZ);

  // Y-up recovers the room: four walls, both faces of each, so the thickness
  // is MEASURED rather than assumed, and the footprint is the one built.
  REQUIRE(correct.walls.size() >= 4);
  std::size_t paired = 0;
  for (const plan::WallSegment& w : correct.walls) {
    if (w.evidence == plan::WallEvidence::kPairedFaces) ++paired;
  }
  CHECK(paired >= 4);
  for (const plan::WallSegment& w : correct.walls) {
    if (w.evidence != plan::WallEvidence::kPairedFaces) continue;
    CHECK(w.thickness_m == doctest::Approx(kWallT).epsilon(0.25));
  }
  // plan_model.h's UpAxis table: kY maps plan x = world z, plan y = world x.
  // Asserting it the other way round would still "pass" on a square room,
  // which is why the fixture is 6 x 4 and not 5 x 5.
  CHECK(correct.bounds.width() == doctest::Approx(kRoomZ + 2 * kWallT).epsilon(0.05));
  CHECK(correct.bounds.height() == doctest::Approx(kRoomX + 2 * kWallT).epsilon(0.05));
  REQUIRE(correct.rooms.size() >= 1);
  double biggest = 0.0;
  for (const plan::Room& r : correct.rooms) biggest = std::max(biggest, r.area_m2);
  CHECK(biggest == doctest::Approx(kRoomX * kRoomZ).epsilon(0.03));

  // Z-up slices a 50 cm VERTICAL slab out of the same room: the "plan" it
  // produces is 2.3 m tall in one axis (the wall height) instead of 4 m deep,
  // and no room can close because there are no four walls in that cut. This
  // is what android/app/src/main/cpp/processing_engine.cpp shipped.
  CHECK(shipped.rooms.empty());
  CHECK(shipped.bounds.height() < 2.6);
  CHECK(shipped.bounds.height() < correct.bounds.height() * 0.7);
}
