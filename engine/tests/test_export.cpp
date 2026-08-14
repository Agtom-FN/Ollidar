// A9: export writers (PLY binary+RGB, LAS 1.4, PCD) + round-trip validation.
//
// Every format is verified by a reader written FROM SCRATCH in this file —
// it never calls into src/export/*.cpp — so a bug shared between the writer
// and a "reader" that reused the writer's own encode helpers could not hide
// from these tests. The LAS cases decode header fields at their literal
// byte offsets (the ASPRS LAS 1.4 spec's own numbers, not any constant
// exposed by las_constants.h) so a wrong offset in the writer shows up as a
// wrong value here, not as two implementations agreeing with each other.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/export/exporter.h"

using namespace scanengine;

namespace {

namespace fs = std::filesystem;

// --- fixtures ----------------------------------------------------------------

std::string make_temp_path(const char* tag, const char* ext) {
  static std::atomic<long long> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path p = fs::temp_directory_path() /
                      (std::string("scanengine_export_test_") + tag + "_" + std::to_string(now) +
                       "_" + std::to_string(id) + "." + ext);
  std::error_code ec;
  fs::remove(p, ec);
  return p.string();
}

Span<const PointVertex> span_of(const std::vector<PointVertex>& v) {
  return Span<const PointVertex>(v.data(), v.size());
}

// A deterministic, easily-inverted point set: x = base + i*step (so a point
// can be identified by its x alone in decimate/bounds tests), y/z and RGB
// vary independently so component mixups (e.g. y written where z should be)
// would fail a round-trip check.
std::vector<PointVertex> make_points(int n, float base = 0.f, float step = 1.f,
                                      std::uint8_t rgb_seed = 10) {
  std::vector<PointVertex> v(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    v[static_cast<std::size_t>(i)] = PointVertex{
        base + step * static_cast<float>(i),
        3.f - 0.5f * static_cast<float>(i),
        -1.f + 0.25f * static_cast<float>(i),
        static_cast<std::uint8_t>((rgb_seed + i * 7) % 256),
        static_cast<std::uint8_t>((rgb_seed + i * 13 + 40) % 256),
        static_cast<std::uint8_t>((rgb_seed + i * 19 + 80) % 256),
        255,
    };
  }
  return v;
}

std::uint8_t expected_luma8(const PointVertex& p) {
  const unsigned l = (299u * p.r + 587u * p.g + 114u * p.b + 500u) / 1000u;
  return static_cast<std::uint8_t>(l > 255u ? 255u : l);
}

std::uint16_t expected_luma16(const PointVertex& p) {
  const std::uint8_t l = expected_luma8(p);
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(l) << 8) | l);
}

std::uint16_t widen16(std::uint8_t v) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(v) << 8) | v);
}

std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
}

// --- independent PLY reader ---------------------------------------------------

struct PlyParsed {
  std::uint64_t count = 0;
  bool has_color = false;
  bool has_intensity = false;
  std::vector<float> x, y, z;
  std::vector<std::uint8_t> r, g, b, intensity;
};

PlyParsed read_ply(const std::string& path) {
  PlyParsed out;
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());

  std::string line;
  std::vector<std::string> props;
  bool saw_ply = false, saw_format = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "ply") {
      saw_ply = true;
      continue;
    }
    if (line == "end_header") break;
    if (line.rfind("format ", 0) == 0) {
      CHECK(line == "format binary_little_endian 1.0");
      saw_format = true;
    } else if (line.rfind("element vertex", 0) == 0) {
      std::istringstream iss(line);
      std::string a, b_;
      iss >> a >> b_ >> out.count;
    } else if (line.rfind("property", 0) == 0) {
      std::istringstream iss(line);
      std::string a, type, name;
      iss >> a >> type >> name;
      props.push_back(name);
    }
  }
  CHECK(saw_ply);
  CHECK(saw_format);
  REQUIRE(!props.empty());
  CHECK(props[0] == "x");
  CHECK(props[1] == "y");
  CHECK(props[2] == "z");
  out.has_color = std::find(props.begin(), props.end(), "red") != props.end();
  out.has_intensity = std::find(props.begin(), props.end(), "intensity") != props.end();

  out.x.resize(out.count);
  out.y.resize(out.count);
  out.z.resize(out.count);
  if (out.has_color) {
    out.r.resize(out.count);
    out.g.resize(out.count);
    out.b.resize(out.count);
  }
  if (out.has_intensity) out.intensity.resize(out.count);

  for (std::uint64_t i = 0; i < out.count; ++i) {
    float fx = 0, fy = 0, fz = 0;
    in.read(reinterpret_cast<char*>(&fx), 4);
    in.read(reinterpret_cast<char*>(&fy), 4);
    in.read(reinterpret_cast<char*>(&fz), 4);
    out.x[i] = fx;
    out.y[i] = fy;
    out.z[i] = fz;
    if (out.has_color) {
      char cr = 0, cg = 0, cb = 0;
      in.read(&cr, 1);
      in.read(&cg, 1);
      in.read(&cb, 1);
      out.r[i] = static_cast<std::uint8_t>(cr);
      out.g[i] = static_cast<std::uint8_t>(cg);
      out.b[i] = static_cast<std::uint8_t>(cb);
    }
    if (out.has_intensity) {
      char ci = 0;
      in.read(&ci, 1);
      out.intensity[i] = static_cast<std::uint8_t>(ci);
    }
  }
  const auto pos_before_eof_check = in.tellg();
  (void)pos_before_eof_check;
  CHECK(!in.fail());
  return out;
}

// --- independent PCD reader ---------------------------------------------------

struct PcdParsed {
  std::uint64_t count = 0;
  std::vector<std::string> fields;
  bool has_rgb = false;
  bool has_intensity = false;
  std::vector<float> x, y, z;
  std::vector<std::uint8_t> r, g, b;
  std::vector<float> intensity;
};

PcdParsed read_pcd(const std::string& path) {
  PcdParsed out;
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());

  std::string line;
  std::uint64_t width = 0, points = 0;
  int height = -1;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream iss(line);
    std::string key;
    iss >> key;
    if (key == "FIELDS") {
      std::string f;
      while (iss >> f) out.fields.push_back(f);
    } else if (key == "WIDTH") {
      iss >> width;
    } else if (key == "HEIGHT") {
      iss >> height;
    } else if (key == "POINTS") {
      iss >> points;
    } else if (key == "DATA") {
      std::string mode;
      iss >> mode;
      CHECK(mode == "binary");
      break;
    }
  }
  CHECK(height == 1);
  CHECK(width == points);
  out.count = points;
  out.has_rgb = std::find(out.fields.begin(), out.fields.end(), "rgb") != out.fields.end();
  out.has_intensity =
      std::find(out.fields.begin(), out.fields.end(), "intensity") != out.fields.end();
  CHECK(out.fields[0] == "x");
  CHECK(out.fields[1] == "y");
  CHECK(out.fields[2] == "z");

  out.x.resize(out.count);
  out.y.resize(out.count);
  out.z.resize(out.count);
  if (out.has_rgb) {
    out.r.resize(out.count);
    out.g.resize(out.count);
    out.b.resize(out.count);
  }
  if (out.has_intensity) out.intensity.resize(out.count);

  for (std::uint64_t i = 0; i < out.count; ++i) {
    float fx = 0, fy = 0, fz = 0;
    in.read(reinterpret_cast<char*>(&fx), 4);
    in.read(reinterpret_cast<char*>(&fy), 4);
    in.read(reinterpret_cast<char*>(&fz), 4);
    out.x[i] = fx;
    out.y[i] = fy;
    out.z[i] = fz;
    if (out.has_rgb) {
      float rgbf = 0;
      in.read(reinterpret_cast<char*>(&rgbf), 4);
      std::uint32_t packed = 0;
      std::memcpy(&packed, &rgbf, 4);
      out.r[i] = static_cast<std::uint8_t>((packed >> 16) & 0xFFu);
      out.g[i] = static_cast<std::uint8_t>((packed >> 8) & 0xFFu);
      out.b[i] = static_cast<std::uint8_t>(packed & 0xFFu);
    }
    if (out.has_intensity) {
      float fi = 0;
      in.read(reinterpret_cast<char*>(&fi), 4);
      out.intensity[i] = fi;
    }
  }
  CHECK(!in.fail());
  return out;
}

// --- independent LAS 1.4 reader -----------------------------------------------
// Every offset below is copied from the ASPRS LAS 1.4 spec's Public Header
// Block table, not from src/export/las_writer.cpp.

std::uint16_t rd_u16(const std::vector<std::uint8_t>& b, std::size_t o) {
  std::uint16_t v = 0;
  std::memcpy(&v, b.data() + o, 2);
  return v;
}
std::uint32_t rd_u32(const std::vector<std::uint8_t>& b, std::size_t o) {
  std::uint32_t v = 0;
  std::memcpy(&v, b.data() + o, 4);
  return v;
}
std::uint64_t rd_u64(const std::vector<std::uint8_t>& b, std::size_t o) {
  std::uint64_t v = 0;
  std::memcpy(&v, b.data() + o, 8);
  return v;
}
std::int32_t rd_i32(const std::vector<std::uint8_t>& b, std::size_t o) {
  std::int32_t v = 0;
  std::memcpy(&v, b.data() + o, 4);
  return v;
}
double rd_f64(const std::vector<std::uint8_t>& b, std::size_t o) {
  double v = 0;
  std::memcpy(&v, b.data() + o, 8);
  return v;
}

struct LasHeaderRead {
  std::string signature;
  std::uint16_t global_encoding = 0;
  std::uint8_t ver_major = 0, ver_minor = 0;
  std::uint16_t header_size = 0;
  std::uint32_t offset_to_point_data = 0;
  std::uint32_t num_vlr = 0;
  std::uint8_t point_format = 0;
  std::uint16_t point_record_len = 0;
  std::uint32_t legacy_num_points = 0;
  std::uint32_t legacy_num_by_return0 = 0;
  double scale[3] = {0, 0, 0};
  double offset[3] = {0, 0, 0};
  double max_x = 0, min_x = 0, max_y = 0, min_y = 0, max_z = 0, min_z = 0;
  std::uint64_t num_evlr = 0;
  std::uint64_t num_points = 0;
  std::uint64_t num_by_return0 = 0;
  std::uint64_t num_by_return1 = 0;
};

LasHeaderRead read_las_header(const std::vector<std::uint8_t>& b) {
  REQUIRE(b.size() >= 375);
  LasHeaderRead h;
  h.signature.assign(reinterpret_cast<const char*>(b.data()), 4);
  h.global_encoding = rd_u16(b, 6);
  h.ver_major = b[24];
  h.ver_minor = b[25];
  h.header_size = rd_u16(b, 94);
  h.offset_to_point_data = rd_u32(b, 96);
  h.num_vlr = rd_u32(b, 100);
  h.point_format = b[104];
  h.point_record_len = rd_u16(b, 105);
  h.legacy_num_points = rd_u32(b, 107);
  h.legacy_num_by_return0 = rd_u32(b, 111);
  h.scale[0] = rd_f64(b, 131);
  h.scale[1] = rd_f64(b, 139);
  h.scale[2] = rd_f64(b, 147);
  h.offset[0] = rd_f64(b, 155);
  h.offset[1] = rd_f64(b, 163);
  h.offset[2] = rd_f64(b, 171);
  h.max_x = rd_f64(b, 179);
  h.min_x = rd_f64(b, 187);
  h.max_y = rd_f64(b, 195);
  h.min_y = rd_f64(b, 203);
  h.max_z = rd_f64(b, 211);
  h.min_z = rd_f64(b, 219);
  h.num_evlr = rd_u32(b, 243);
  h.num_points = rd_u64(b, 247);
  h.num_by_return0 = rd_u64(b, 255);
  h.num_by_return1 = rd_u64(b, 255 + 8);
  return h;
}

struct LasVlrRead {
  std::uint16_t reserved = 0;
  std::string user_id;
  std::uint16_t record_id = 0;
  std::uint16_t record_len = 0;
  std::string description;
  std::string payload;
};

// Reads the VLR that starts immediately at byte 375 (true for every file A9
// writes: exactly one WKT VLR, no padding).
LasVlrRead read_first_vlr(const std::vector<std::uint8_t>& b) {
  const std::size_t off = 375;
  REQUIRE(b.size() >= off + 54);
  LasVlrRead v;
  v.reserved = rd_u16(b, off + 0);
  v.user_id.assign(reinterpret_cast<const char*>(b.data() + off + 2));
  v.record_id = rd_u16(b, off + 18);
  v.record_len = rd_u16(b, off + 20);
  v.description.assign(reinterpret_cast<const char*>(b.data() + off + 22));
  REQUIRE(b.size() >= off + 54 + v.record_len);
  v.payload.assign(reinterpret_cast<const char*>(b.data() + off + 54), v.record_len);
  return v;
}

struct LasPtRead {
  double x = 0, y = 0, z = 0;
  std::uint16_t intensity = 0, red = 0, green = 0, blue = 0;
  double gps_time = 0;
};

std::vector<LasPtRead> read_las_points(const std::vector<std::uint8_t>& b, std::size_t offset,
                                        std::uint64_t n, const LasHeaderRead& h, bool fmt7) {
  std::vector<LasPtRead> pts(n);
  const std::size_t rec_len = fmt7 ? 36 : 26;
  REQUIRE(b.size() >= offset + rec_len * n);
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::size_t o = offset + static_cast<std::size_t>(i) * rec_len;
    const std::int32_t xi = rd_i32(b, o + 0);
    const std::int32_t yi = rd_i32(b, o + 4);
    const std::int32_t zi = rd_i32(b, o + 8);
    pts[i].x = static_cast<double>(xi) * h.scale[0] + h.offset[0];
    pts[i].y = static_cast<double>(yi) * h.scale[1] + h.offset[1];
    pts[i].z = static_cast<double>(zi) * h.scale[2] + h.offset[2];
    pts[i].intensity = rd_u16(b, o + 12);
    if (!fmt7) {
      pts[i].red = rd_u16(b, o + 20);
      pts[i].green = rd_u16(b, o + 22);
      pts[i].blue = rd_u16(b, o + 24);
    } else {
      pts[i].gps_time = rd_f64(b, o + 22);
      pts[i].red = rd_u16(b, o + 30);
      pts[i].green = rd_u16(b, o + 32);
      pts[i].blue = rd_u16(b, o + 34);
    }
  }
  return pts;
}

}  // namespace

// --- PLY -----------------------------------------------------------------------

TEST_CASE("export/ply_round_trip_basic") {
  PageStore store(PageStoreConfig{1000, 4});
  const auto pts = make_points(37);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1000).ok());

  const std::string path = make_temp_path("ply_basic", "ply");
  ExportOptions opts;
  const Status st = export_points(store, {}, ExportFormat::kPlyBinary, path, opts);
  REQUIRE(st.ok());

  const PlyParsed parsed = read_ply(path);
  REQUIRE(parsed.count == pts.size());
  CHECK(parsed.has_color);
  CHECK(parsed.has_intensity);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(parsed.x[i] == pts[i].x);  // float32 in, float32 out: exact
    CHECK(parsed.y[i] == pts[i].y);
    CHECK(parsed.z[i] == pts[i].z);
    CHECK(parsed.r[i] == pts[i].r);
    CHECK(parsed.g[i] == pts[i].g);
    CHECK(parsed.b[i] == pts[i].b);
    CHECK(parsed.intensity[i] == expected_luma8(pts[i]));
  }

  fs::remove(path);
}

TEST_CASE("export/ply_spans_multiple_pages_without_loss_or_duplication") {
  PageStoreConfig cfg;
  cfg.page_capacity = 37;  // deliberately not a divisor of the point count
  cfg.max_pages = 16;
  PageStore store(cfg);
  const auto pts = make_points(250);
  std::uint32_t appended = 0;
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1, &appended).ok());
  REQUIRE(appended == 250);
  REQUIRE(store.page_count() > 1);  // the point of this test

  const std::string path = make_temp_path("ply_multipage", "ply");
  const Status st = export_points(store, {}, ExportFormat::kPlyBinary, path, ExportOptions{});
  REQUIRE(st.ok());

  const PlyParsed parsed = read_ply(path);
  REQUIRE(parsed.count == 250);
  // PageStore pages are FIFO in append order, so output order should equal
  // input order across the page boundary.
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(parsed.x[i] == pts[i].x);
  }

  fs::remove(path);
}

TEST_CASE("export/ply_color_and_intensity_are_independently_toggleable") {
  PageStore store(PageStoreConfig{1000, 4});
  const auto pts = make_points(10);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  {
    ExportOptions opts;
    opts.include_color = false;
    opts.include_intensity = false;
    const std::string path = make_temp_path("ply_bare", "ply");
    REQUIRE(export_points(store, {}, ExportFormat::kPlyBinary, path, opts).ok());
    const PlyParsed parsed = read_ply(path);
    CHECK_FALSE(parsed.has_color);
    CHECK_FALSE(parsed.has_intensity);
    REQUIRE(parsed.count == 10);
    CHECK(parsed.x[3] == pts[3].x);
    fs::remove(path);
  }
  {
    ExportOptions opts;
    opts.include_color = true;
    opts.include_intensity = false;
    const std::string path = make_temp_path("ply_color_only", "ply");
    REQUIRE(export_points(store, {}, ExportFormat::kPlyBinary, path, opts).ok());
    const PlyParsed parsed = read_ply(path);
    CHECK(parsed.has_color);
    CHECK_FALSE(parsed.has_intensity);
    fs::remove(path);
  }
  {
    ExportOptions opts;
    opts.include_color = false;
    opts.include_intensity = true;
    const std::string path = make_temp_path("ply_intensity_only", "ply");
    REQUIRE(export_points(store, {}, ExportFormat::kPlyBinary, path, opts).ok());
    const PlyParsed parsed = read_ply(path);
    CHECK_FALSE(parsed.has_color);
    CHECK(parsed.has_intensity);
    REQUIRE(parsed.count == 10);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      CHECK(parsed.x[i] == pts[i].x);
      CHECK(parsed.intensity[i] == expected_luma8(pts[i]));
    }
    fs::remove(path);
  }
}

// --- PCD -----------------------------------------------------------------------

TEST_CASE("export/pcd_round_trip_basic") {
  PageStore store(PageStoreConfig{1000, 4});
  const auto pts = make_points(41);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  const std::string path = make_temp_path("pcd_basic", "pcd");
  REQUIRE(export_points(store, {}, ExportFormat::kPcd, path, ExportOptions{}).ok());

  const PcdParsed parsed = read_pcd(path);
  REQUIRE(parsed.count == pts.size());
  CHECK(parsed.has_rgb);
  CHECK(parsed.has_intensity);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(parsed.x[i] == pts[i].x);
    CHECK(parsed.y[i] == pts[i].y);
    CHECK(parsed.z[i] == pts[i].z);
    CHECK(parsed.r[i] == pts[i].r);
    CHECK(parsed.g[i] == pts[i].g);
    CHECK(parsed.b[i] == pts[i].b);
    CHECK(parsed.intensity[i] == static_cast<float>(expected_luma8(pts[i])));
  }

  fs::remove(path);
}

TEST_CASE("export/pcd_fields_omitted_when_disabled") {
  PageStore store(PageStoreConfig{1000, 4});
  const auto pts = make_points(5);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  ExportOptions opts;
  opts.include_color = false;
  const std::string path = make_temp_path("pcd_nocolor", "pcd");
  REQUIRE(export_points(store, {}, ExportFormat::kPcd, path, opts).ok());
  const PcdParsed parsed = read_pcd(path);
  CHECK_FALSE(parsed.has_rgb);
  CHECK(parsed.has_intensity);
  REQUIRE(parsed.count == 5);

  fs::remove(path);
}

// --- LAS 1.4 ---------------------------------------------------------------------

TEST_CASE("export/las_format2_header_fields_are_byte_correct") {
  PageStore store(PageStoreConfig{1000, 4});
  const auto pts = make_points(60, /*base=*/-30.f, /*step=*/1.f);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  const std::string path = make_temp_path("las2", "las");
  ExportOptions opts;  // las_gps_time = false -> point format 2
  REQUIRE(export_points(store, {}, ExportFormat::kLas14, path, opts).ok());

  const std::vector<std::uint8_t> raw = read_file_bytes(path);
  const LasHeaderRead h = read_las_header(raw);

  CHECK(h.signature == "LASF");
  CHECK(h.ver_major == 1);
  CHECK(h.ver_minor == 4);
  CHECK(h.header_size == 375);
  CHECK(h.point_format == 2);
  CHECK(h.point_record_len == 26);
  CHECK((h.global_encoding & (1u << 4)) != 0);  // WKT bit
  CHECK(h.num_vlr == 1);

  // point counts: both the legacy 32-bit field AND the new 64-bit field.
  CHECK(h.legacy_num_points == 60);
  CHECK(h.legacy_num_by_return0 == 60);
  CHECK(h.num_points == 60);
  CHECK(h.num_by_return0 == 60);
  CHECK(h.num_by_return1 == 0);

  // bounds: exact, because the header stores the float32 bounds widened to
  // double with no lossy quantization involved.
  float expect_min[3], expect_max[3];
  expect_min[0] = expect_max[0] = pts[0].x;
  expect_min[1] = expect_max[1] = pts[0].y;
  expect_min[2] = expect_max[2] = pts[0].z;
  for (const auto& p : pts) {
    expect_min[0] = std::min(expect_min[0], p.x);
    expect_max[0] = std::max(expect_max[0], p.x);
    expect_min[1] = std::min(expect_min[1], p.y);
    expect_max[1] = std::max(expect_max[1], p.y);
    expect_min[2] = std::min(expect_min[2], p.z);
    expect_max[2] = std::max(expect_max[2], p.z);
  }
  CHECK(h.min_x == doctest::Approx(expect_min[0]));
  CHECK(h.max_x == doctest::Approx(expect_max[0]));
  CHECK(h.min_y == doctest::Approx(expect_min[1]));
  CHECK(h.max_y == doctest::Approx(expect_max[1]));
  CHECK(h.min_z == doctest::Approx(expect_min[2]));
  CHECK(h.max_z == doctest::Approx(expect_max[2]));

  CHECK(h.offset_to_point_data == static_cast<std::uint32_t>(raw.size() - 60u * 26u));

  const LasVlrRead vlr = read_first_vlr(raw);
  CHECK(vlr.user_id == "LASF_Projection");
  CHECK(vlr.record_id == 2112);
  CHECK(vlr.payload.find("ENGCRS") != std::string::npos);
  CHECK(vlr.payload.find("Ungeoreferenced") != std::string::npos);

  // point round-trip: coordinates within one scale-quantum, color/intensity exact.
  const auto decoded = read_las_points(raw, h.offset_to_point_data, 60, h, /*fmt7=*/false);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(std::fabs(decoded[i].x - pts[i].x) <= h.scale[0] * 0.5 + 1e-9);
    CHECK(std::fabs(decoded[i].y - pts[i].y) <= h.scale[1] * 0.5 + 1e-9);
    CHECK(std::fabs(decoded[i].z - pts[i].z) <= h.scale[2] * 0.5 + 1e-9);
    CHECK(decoded[i].intensity == expected_luma16(pts[i]));
    CHECK(decoded[i].red == widen16(pts[i].r));
    CHECK(decoded[i].green == widen16(pts[i].g));
    CHECK(decoded[i].blue == widen16(pts[i].b));
  }

  fs::remove(path);
}

TEST_CASE("export/las_format7_has_gps_time_and_zeroed_legacy_counts") {
  // Multiple append() calls with increasing timestamps onto ONE page, so
  // t_first_ns < t_last_ns and the per-point GPS-time interpolation is
  // non-degenerate.
  PageStore store(PageStoreConfig{500, 4});
  const int per_batch = 20;
  const int batches = 5;
  std::vector<PointVertex> all;
  for (int bi = 0; bi < batches; ++bi) {
    const auto batch = make_points(per_batch, static_cast<float>(bi * per_batch));
    REQUIRE(store.append(StreamId::kLidarD6, span_of(batch), 1000 + 1000 * bi).ok());
    all.insert(all.end(), batch.begin(), batch.end());
  }
  REQUIRE(store.page_count() == 1);
  const PageView pv = store.page_view(store.page_ids().front());
  REQUIRE(pv.t_first_ns == 1000);
  REQUIRE(pv.t_last_ns == 1000 + 1000 * (batches - 1));

  const std::string path = make_temp_path("las7", "las");
  ExportOptions opts;
  opts.las_gps_time = true;
  REQUIRE(export_points(store, {}, ExportFormat::kLas14, path, opts).ok());

  const std::vector<std::uint8_t> raw = read_file_bytes(path);
  const LasHeaderRead h = read_las_header(raw);
  CHECK(h.point_format == 7);
  CHECK(h.point_record_len == 36);
  // Extended point formats (>=6) MUST leave the legacy 32-bit fields zero —
  // only the 64-bit fields are authoritative.
  CHECK(h.legacy_num_points == 0);
  CHECK(h.legacy_num_by_return0 == 0);
  CHECK(h.num_points == static_cast<std::uint64_t>(all.size()));
  CHECK(h.num_by_return0 == static_cast<std::uint64_t>(all.size()));

  const auto decoded =
      read_las_points(raw, h.offset_to_point_data, all.size(), h, /*fmt7=*/true);
  double prev_t = -1.0;
  const double t_first_s = static_cast<double>(pv.t_first_ns) * 1e-9;
  const double t_last_s = static_cast<double>(pv.t_last_ns) * 1e-9;
  for (std::size_t i = 0; i < all.size(); ++i) {
    CHECK(decoded[i].gps_time >= t_first_s - 1e-9);
    CHECK(decoded[i].gps_time <= t_last_s + 1e-9);
    CHECK(decoded[i].gps_time >= prev_t);  // monotonic across in-page order
    prev_t = decoded[i].gps_time;
    CHECK(std::fabs(decoded[i].x - all[i].x) <= h.scale[0] * 0.5 + 1e-9);
    CHECK(decoded[i].intensity == expected_luma16(all[i]));
  }

  fs::remove(path);
}

TEST_CASE("export/las_crs_seam_placeholder_vs_caller_supplied") {
  PageStore store(PageStoreConfig{1000, 4});
  const auto pts = make_points(4);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  SUBCASE("empty crs_wkt embeds the documented local-frame placeholder") {
    const std::string path = make_temp_path("las_crs_default", "las");
    REQUIRE(export_points(store, {}, ExportFormat::kLas14, path, ExportOptions{}).ok());
    const auto raw = read_file_bytes(path);
    const LasVlrRead vlr = read_first_vlr(raw);
    CHECK(vlr.payload.find("ENGCRS") != std::string::npos);
    fs::remove(path);
  }

  SUBCASE("non-empty crs_wkt is embedded verbatim (A10 seam)") {
    ExportOptions opts;
    opts.crs_wkt =
        "PROJCRS[\"WGS 84 / UTM zone 33N\",BASEGEOGCRS[\"WGS 84\","
        "DATUM[\"World Geodetic System 1984\",ELLIPSOID[\"WGS "
        "84\",6378137,298.257223563]]]]";
    const std::string path = make_temp_path("las_crs_custom", "las");
    REQUIRE(export_points(store, {}, ExportFormat::kLas14, path, opts).ok());
    const auto raw = read_file_bytes(path);
    const LasVlrRead vlr = read_first_vlr(raw);
    CHECK(vlr.payload == opts.crs_wkt);
    CHECK(vlr.payload.find("ENGCRS") == std::string::npos);
    fs::remove(path);
  }
}

TEST_CASE("export/las_empty_store_writes_a_valid_zero_point_file") {
  PageStore store(PageStoreConfig{1000, 4});
  const std::string path = make_temp_path("las_empty", "las");
  REQUIRE(export_points(store, {}, ExportFormat::kLas14, path, ExportOptions{}).ok());
  const auto raw = read_file_bytes(path);
  const LasHeaderRead h = read_las_header(raw);
  CHECK(h.num_points == 0);
  CHECK(raw.size() == h.offset_to_point_data);  // header + VLR, no point data
  fs::remove(path);
}

// --- selection: decimate / bounds_filter / streams --------------------------

TEST_CASE("export/decimate_keeps_every_nth_point") {
  PageStore store(PageStoreConfig{1000, 4});
  const auto pts = make_points(100);  // x = 0..99
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  ExportOptions opts;
  opts.decimate = 4;
  const std::string path = make_temp_path("decimate", "ply");
  REQUIRE(export_points(store, {}, ExportFormat::kPlyBinary, path, opts).ok());

  const PlyParsed parsed = read_ply(path);
  REQUIRE(parsed.count == 25);  // indices 0,4,...,96
  for (std::uint64_t i = 0; i < parsed.count; ++i) {
    CHECK(parsed.x[i] == static_cast<float>(i * 4));
  }
  fs::remove(path);
}

TEST_CASE("export/bounds_filter_crops_points") {
  PageStore store(PageStoreConfig{1000, 4});
  auto pts = make_points(100);
  for (auto& p : pts) {
    p.y = 0.f;  // constant, so only x drives the crop below
    p.z = 0.f;
  }
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  ExportOptions opts;
  opts.bounds_filter.enabled = true;
  opts.bounds_filter.min[0] = 10.f;
  opts.bounds_filter.max[0] = 50.f;
  opts.bounds_filter.min[1] = -1.f;
  opts.bounds_filter.max[1] = 1.f;
  opts.bounds_filter.min[2] = -1.f;
  opts.bounds_filter.max[2] = 1.f;

  const std::string path = make_temp_path("bounds", "ply");
  REQUIRE(export_points(store, {}, ExportFormat::kPlyBinary, path, opts).ok());
  const PlyParsed parsed = read_ply(path);
  REQUIRE(parsed.count == 41);  // x in [10, 50] inclusive
  for (std::uint64_t i = 0; i < parsed.count; ++i) {
    CHECK(parsed.x[i] >= 10.f);
    CHECK(parsed.x[i] <= 50.f);
  }
  fs::remove(path);
}

TEST_CASE("export/streams_filter_selects_only_requested_streams") {
  PageStore store(PageStoreConfig{1000, 4});
  const auto d6_pts = make_points(15, /*base=*/0.f);
  const auto mid360_pts = make_points(9, /*base=*/1000.f);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(d6_pts), 1).ok());
  REQUIRE(store.append(StreamId::kLidarMid360, span_of(mid360_pts), 2).ok());
  REQUIRE(store.page_count() == 2);  // pages are single-stream

  const StreamId only_d6[] = {StreamId::kLidarD6};
  const std::string path = make_temp_path("streams", "ply");
  REQUIRE(
      export_points(store, Span<const StreamId>(only_d6, 1), ExportFormat::kPlyBinary, path,
                     ExportOptions{})
          .ok());
  const PlyParsed parsed = read_ply(path);
  REQUIRE(parsed.count == 15);
  for (std::uint64_t i = 0; i < parsed.count; ++i) CHECK(parsed.x[i] < 100.f);
  fs::remove(path);

  const std::string path_all = make_temp_path("streams_all", "ply");
  REQUIRE(export_points(store, {}, ExportFormat::kPlyBinary, path_all, ExportOptions{}).ok());
  const PlyParsed parsed_all = read_ply(path_all);
  CHECK(parsed_all.count == 24);  // both streams, empty filter = everything
  fs::remove(path_all);
}

// --- progress + cancellation --------------------------------------------------

TEST_CASE("export/progress_callback_is_monotonic_and_reaches_one") {
  PageStore store(PageStoreConfig{100000, 4});
  const auto pts = make_points(20001);  // deliberately not a multiple of the 8192 stride
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  std::vector<float> observed;
  struct Ctx {
    std::vector<float>* out;
  } ctx{&observed};
  const auto cb = [](float fraction, void* user) {
    static_cast<Ctx*>(user)->out->push_back(fraction);
  };

  const std::string path = make_temp_path("progress", "ply");
  const Status st =
      export_points(store, {}, ExportFormat::kPlyBinary, path, ExportOptions{}, cb, &ctx);
  REQUIRE(st.ok());

  REQUIRE(!observed.empty());
  for (std::size_t i = 1; i < observed.size(); ++i) CHECK(observed[i] >= observed[i - 1]);
  CHECK(observed.back() == doctest::Approx(1.0f));
  CHECK(observed.front() > 0.f);

  fs::remove(path);
}

TEST_CASE("export/cancellation_stops_early_and_deletes_partial_file") {
  PageStore store(PageStoreConfig{100000, 4});
  const auto pts = make_points(50000);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  ExportCancelToken token;
  struct Ctx {
    ExportCancelToken* token;
    int calls = 0;
  } ctx{&token, 0};
  const auto cb = [](float, void* user) {
    auto* c = static_cast<Ctx*>(user);
    if (++c->calls == 1) c->token->request_cancel();
  };

  const std::string path = make_temp_path("cancel", "ply");
  const Status st = export_points(store, {}, ExportFormat::kPlyBinary, path, ExportOptions{}, cb,
                                   &ctx, &token);
  CHECK(st.error() == ScanError::kCancelled);
  CHECK_FALSE(fs::exists(path));
  CHECK(ctx.calls >= 1);
}

// --- Exporter interface + make_exporter --------------------------------------

TEST_CASE("export/exporter_interface_write_progress_cancel") {
  Result<Exporter*> made = make_exporter(ExportFormat::kPcd);
  REQUIRE(made.ok());
  Exporter* exporter = made.value();
  REQUIRE(exporter != nullptr);
  CHECK(exporter->format() == ExportFormat::kPcd);
  CHECK(exporter->progress() == 0.f);

  PageStore store(PageStoreConfig{1000, 4});
  const auto pts = make_points(12);
  REQUIRE(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  ExportOptions opts;
  opts.output_path = make_temp_path("exporter_iface", "pcd");
  const Status st = exporter->write(store, opts);
  REQUIRE(st.ok());
  CHECK(exporter->progress() == doctest::Approx(1.0f));

  const PcdParsed parsed = read_pcd(opts.output_path);
  CHECK(parsed.count == 12);

  fs::remove(opts.output_path);
  delete exporter;
}

TEST_CASE("export/make_exporter_and_export_points_reject_unimplemented_formats") {
  const Result<Exporter*> dxf = make_exporter(ExportFormat::kDxf);
  CHECK_FALSE(dxf.ok());
  CHECK(dxf.error() == ScanError::kUnimplemented);

  const Result<Exporter*> pdf = make_exporter(ExportFormat::kPdf);
  CHECK_FALSE(pdf.ok());
  CHECK(pdf.error() == ScanError::kUnimplemented);

  PageStore store(PageStoreConfig{100, 4});
  const Status st =
      export_points(store, {}, ExportFormat::kDxf, "/tmp/should_not_be_created.dxf",
                     ExportOptions{});
  CHECK(st.error() == ScanError::kUnimplemented);
  CHECK_FALSE(fs::exists("/tmp/should_not_be_created.dxf"));
}

TEST_CASE("export/export_points_rejects_empty_path") {
  PageStore store(PageStoreConfig{100, 4});
  const Status st = export_points(store, {}, ExportFormat::kPlyBinary, "", ExportOptions{});
  CHECK(st.error() == ScanError::kInvalidArgument);
}
