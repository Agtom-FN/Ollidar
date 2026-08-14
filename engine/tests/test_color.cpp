// test_color.cpp — task A11: camera colorization (Tech Spec §3.5).
//
// Five groups, in increasing order of how much they can lie to you:
//
//   fidx/*     the frames.idx binary format: record round trip, byte-for-byte
//              agreement with what A5's FileRecordWriter produces, the
//              truncated-tail rule, version and validation refusals.
//   img/*      the vendored stb_image path against a real (tiny) JPEG, and
//              the bilinear sampler against hand-computed values.
//   sweep/*    the wizard's 8-second clock-offset estimator against synthetic
//              rate tracks with a KNOWN offset, plus every refusal it must
//              make.
//   color/*    the colorizer on a synthetic room: analytically rendered
//              images from known poses, so every point has a colour that is
//              exactly right or exactly wrong. Occlusion, the rolling-shutter
//              correction, the motion gate and the coverage flags are each
//              measured against that truth.
//   gate/*     the S6 sync-quality policy and the plumbing around a run
//              (cancel, determinism, the PageStore seam).
//
// The synthetic room is the only place an accuracy number can be checked
// against truth, because it is the only place truth exists — the same
// posture as tests/test_post.cpp and tests/test_mount_calib.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"

#include "scanengine/cloud/page_store.h"
#include "scanengine/color/clock_sweep.h"
#include "scanengine/color/colorizer.h"
#include "scanengine/color/frames_idx.h"
#include "scanengine/color/image_source.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"

using namespace scanengine;
using namespace scanengine::color;

namespace {

namespace fs = std::filesystem;

// --- deterministic noise ----------------------------------------------------
//
// xorshift64 + Box-Muller, deliberately NOT <random>: the standard does not
// specify the output of its distributions, so the five CI legs would disagree
// on every number in this file. Same reason test_timesync.cpp and
// test_mount_calib.cpp do it.
struct Rng {
  std::uint64_t s = 0x9E3779B97F4A7C15ull;
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  double normal() {
    const double u1 = std::max(1e-12, uniform());
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

std::string temp_dir(const char* name) {
  const fs::path p = fs::temp_directory_path() / ("scanengine-a11-" + std::string(name));
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p.string();
}

// --- a keyframe with every field populated ----------------------------------

Keyframe make_keyframe(int i) {
  Keyframe kf;
  kf.t_mono_ns = 1'000'000'000LL + i * 250'000'000LL;
  char name[64];
  std::snprintf(name, sizeof(name), "streams/frames/kf_%06d.jpg", i);
  kf.image_path = name;
  kf.pose.t_mono_ns = kf.t_mono_ns;
  kf.pose.position[0] = 0.125 * i;
  kf.pose.position[1] = -1.5 + 0.0625 * i;
  kf.pose.position[2] = 1.25;
  const double ang = 0.05 * i;
  kf.pose.orientation[0] = 0.0;
  kf.pose.orientation[1] = 0.0;
  kf.pose.orientation[2] = std::sin(ang * 0.5);
  kf.pose.orientation[3] = std::cos(ang * 0.5);
  kf.pose.position_sigma_m = 0.012f;
  kf.pose.orientation_sigma_deg = 0.35f;
  kf.pose.source = StreamId::kPoseAr;
  kf.pose.quality = PoseQuality::kGood;
  kf.pose.tracking_lost = 0;
  kf.intrinsics.fx = 2912.f;
  kf.intrinsics.fy = 2910.5f;
  kf.intrinsics.cx = 2016.f;
  kf.intrinsics.cy = 1512.f;
  kf.intrinsics.distortion[0] = 0.031f;
  kf.intrinsics.distortion[1] = -0.0072f;
  kf.intrinsics.distortion[2] = 0.0004f;
  kf.intrinsics.distortion[3] = -0.0002f;
  kf.intrinsics.distortion[4] = 0.0011f;
  kf.intrinsics.width = 4032;
  kf.intrinsics.height = 3024;
  kf.intrinsics.rolling_shutter_row_time_ns = 20'000'000.f / 3024.f;
  kf.flags = kKeyframeFlagMotionValid | kKeyframeFlagExposureValid;
  kf.exposure_duration_ns = 8'333'333;
  kf.iso = 320.f;
  kf.angular_rate_rad_s = 0.21f;
  kf.linear_speed_m_s = 0.94f;
  kf.image_bytes = 1'234'567u + static_cast<std::uint32_t>(i);
  return kf;
}

bool keyframes_equal(const Keyframe& a, const Keyframe& b) {
  if (a.t_mono_ns != b.t_mono_ns || a.image_path != b.image_path) return false;
  if (a.flags != b.flags || a.exposure_duration_ns != b.exposure_duration_ns) return false;
  if (a.iso != b.iso || a.angular_rate_rad_s != b.angular_rate_rad_s) return false;
  if (a.linear_speed_m_s != b.linear_speed_m_s || a.image_bytes != b.image_bytes) return false;
  for (int i = 0; i < 3; ++i) {
    if (a.pose.position[i] != b.pose.position[i]) return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (a.pose.orientation[i] != b.pose.orientation[i]) return false;
  }
  if (a.pose.position_sigma_m != b.pose.position_sigma_m) return false;
  if (a.pose.orientation_sigma_deg != b.pose.orientation_sigma_deg) return false;
  if (a.pose.quality != b.pose.quality || a.pose.tracking_lost != b.pose.tracking_lost) return false;
  if (a.pose.source != b.pose.source) return false;
  const CameraIntrinsics& x = a.intrinsics;
  const CameraIntrinsics& y = b.intrinsics;
  if (x.fx != y.fx || x.fy != y.fy || x.cx != y.cx || x.cy != y.cy) return false;
  if (x.width != y.width || x.height != y.height) return false;
  if (x.rolling_shutter_row_time_ns != y.rolling_shutter_row_time_ns) return false;
  for (int i = 0; i < 5; ++i) {
    if (x.distortion[i] != y.distortion[i]) return false;
  }
  return true;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  REQUIRE(f != nullptr);
  std::vector<std::uint8_t> out;
  std::uint8_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  REQUIRE(f != nullptr);
  if (!bytes.empty()) REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
  std::fclose(f);
}

// --- a real 8x8 JPEG --------------------------------------------------------
//
// Left half (200, 30, 40), right half (20, 60, 220), quality 95, no chroma
// subsampling. 664 bytes. It exists so the vendored decoder is exercised on
// an actual JFIF stream — a Huffman table, a quantisation table, an SOS
// segment — and not only on the synthetic RGB buffers the colorization tests
// use.
const std::uint8_t kTinyJpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x01, 0x01, 0x01, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
    0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06,
    0x07, 0x09, 0x08, 0x06, 0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08,
    0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08, 0x0b, 0x0c, 0x0b, 0x0a,
    0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x08, 0x03,
    0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
    0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00,
    0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
    0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
    0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
    0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
    0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
    0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
    0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00,
    0x1f, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
    0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
    0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
    0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xda, 0x00,
    0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xf8,
    0x9f, 0xc7, 0x5f, 0xf2, 0xeb, 0xff, 0x00, 0x03, 0xff, 0x00, 0xd9, 0x6b,
    0xf7, 0xef, 0xa1, 0xb7, 0xfc, 0xcf, 0x3f, 0xee, 0x5b, 0xff, 0x00, 0x76,
    0x0f, 0xd9, 0xbe, 0x9a, 0x9f, 0xf3, 0x21, 0xff, 0x00, 0xb9, 0xaf, 0xfd,
    0xd7, 0x3f, 0xff, 0xd9,
};

// --- the synthetic scene ----------------------------------------------------
//
// Everything below is exact: a rectangle in world space, a pinhole camera
// whose image is RAY-CAST from that geometry, and a point cloud sampled on
// the same rectangles. So "what colour should this point be" has an answer
// that owes nothing to the code under test.

struct Rect {
  double o[3] = {0, 0, 0};   // a corner
  double e1[3] = {0, 0, 0};  // full edge vector
  double e2[3] = {0, 0, 0};  // full edge vector
  double n[3] = {0, 0, 0};   // unit normal
  std::uint8_t rgb[3] = {0, 0, 0};
  bool gradient = false;     // colour = triangle wave along e1
  double gradient_period_m = 1.0;
};

Rect make_rect(const double o[3], const double e1[3], const double e2[3],
               std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  Rect q;
  for (int i = 0; i < 3; ++i) {
    q.o[i] = o[i];
    q.e1[i] = e1[i];
    q.e2[i] = e2[i];
  }
  se3::cross3(q.e1, q.e2, q.n);
  se3::normalize3(q.n);
  q.rgb[0] = r;
  q.rgb[1] = g;
  q.rgb[2] = b;
  return q;
}

void rect_color(const Rect& q, double a, std::uint8_t out[3]) {
  if (!q.gradient) {
    out[0] = q.rgb[0];
    out[1] = q.rgb[1];
    out[2] = q.rgb[2];
    return;
  }
  const double len = se3::norm3(const_cast<double*>(q.e1));
  double pos = a * len;
  const double p = q.gradient_period_m;
  double phase = pos / p;
  phase -= std::floor(phase);
  // Triangle wave: continuous, so bilinear interpolation across it is exact
  // rather than an artefact, and one colour level is p/(2*255) of a metre.
  const double v = (phase < 0.5) ? (phase * 2.0) : ((1.0 - phase) * 2.0);
  const std::uint8_t level = static_cast<std::uint8_t>(std::lround(v * 255.0));
  out[0] = level;
  out[1] = static_cast<std::uint8_t>(255 - level);
  out[2] = 0;
}

// Nearest hit along `dir` from `eye`. Returns the rect index or -1.
int trace(const std::vector<Rect>& rects, const double eye[3], const double dir[3],
          std::uint8_t out_rgb[3], double* out_t) {
  int best = -1;
  double best_t = 1e30;
  double best_a = 0.0;
  for (std::size_t i = 0; i < rects.size(); ++i) {
    const Rect& q = rects[i];
    const double denom = se3::dot3(dir, q.n);
    if (std::fabs(denom) < 1e-12) continue;
    const double d[3] = {q.o[0] - eye[0], q.o[1] - eye[1], q.o[2] - eye[2]};
    const double t = se3::dot3(d, q.n) / denom;
    if (t <= 1e-6 || t >= best_t) continue;
    const double p[3] = {eye[0] + t * dir[0] - q.o[0], eye[1] + t * dir[1] - q.o[1],
                         eye[2] + t * dir[2] - q.o[2]};
    const double l1 = se3::dot3(q.e1, q.e1);
    const double l2 = se3::dot3(q.e2, q.e2);
    const double a = se3::dot3(p, q.e1) / l1;
    const double b = se3::dot3(p, q.e2) / l2;
    if (a < 0.0 || a > 1.0 || b < 0.0 || b > 1.0) continue;
    best_t = t;
    best = static_cast<int>(i);
    best_a = a;
  }
  if (best >= 0) {
    rect_color(rects[static_cast<std::size_t>(best)], best_a, out_rgb);
    if (out_t != nullptr) *out_t = best_t;
  }
  return best;
}

// world_from_camera for a camera at `eye` looking at `target`, with the
// OpenCV convention the projector uses: +X right, +Y down, +Z forward.
void look_at(const double eye[3], const double target[3], double R[9]) {
  double f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  se3::normalize3(f);
  const double up[3] = {0.0, 0.0, 1.0};
  double r[3];
  se3::cross3(f, up, r);
  if (!se3::normalize3(r)) {
    const double alt[3] = {1.0, 0.0, 0.0};
    se3::cross3(f, alt, r);
    se3::normalize3(r);
  }
  double d[3];
  se3::cross3(f, r, d);
  se3::normalize3(d);
  // Columns are the camera axes expressed in world.
  R[0] = r[0]; R[1] = d[0]; R[2] = f[0];
  R[3] = r[1]; R[4] = d[1]; R[5] = f[1];
  R[6] = r[2]; R[7] = d[2]; R[8] = f[2];
}

CameraIntrinsics test_intrinsics(std::uint32_t w = 320, std::uint32_t h = 240, float f = 150.f) {
  CameraIntrinsics in;
  in.width = w;
  in.height = h;
  in.fx = f;
  in.fy = f;
  in.cx = static_cast<float>(w) * 0.5f;
  in.cy = static_cast<float>(h) * 0.5f;
  return in;
}

// Renders one image by ray casting. `pose_for_row` supplies the camera pose
// for a given image row, which is how a ROLLING SHUTTER is simulated: row r
// is exposed at t + r*row_time, so it sees a different pose.
template <typename PoseForRow>
DecodedImage render(const std::vector<Rect>& rects, const CameraIntrinsics& in,
                    PoseForRow pose_for_row) {
  DecodedImage img;
  img.width = in.width;
  img.height = in.height;
  img.rgb.assign(static_cast<std::size_t>(in.width) * in.height * 3u, 0);
  for (std::uint32_t y = 0; y < in.height; ++y) {
    double R[9], eye[3];
    pose_for_row(static_cast<int>(y), R, eye);
    for (std::uint32_t x = 0; x < in.width; ++x) {
      const double xn = ((x + 0.5) - in.cx) / in.fx;
      const double yn = ((y + 0.5) - in.cy) / in.fy;
      const double dc[3] = {xn, yn, 1.0};
      double dw[3];
      for (int i = 0; i < 3; ++i) {
        dw[i] = R[i * 3 + 0] * dc[0] + R[i * 3 + 1] * dc[1] + R[i * 3 + 2] * dc[2];
      }
      se3::normalize3(dw);
      std::uint8_t rgb[3] = {0, 0, 0};
      trace(rects, eye, dw, rgb, nullptr);
      const std::size_t o = (static_cast<std::size_t>(y) * in.width + x) * 3u;
      img.rgb[o + 0] = rgb[0];
      img.rgb[o + 1] = rgb[1];
      img.rgb[o + 2] = rgb[2];
    }
  }
  return img;
}

// Samples a rectangle on a regular grid, at the centre of each cell.
void sample_rect(const Rect& q, double spacing, std::vector<PointVertex>* pts,
                 std::vector<int>* owner, int owner_id, std::vector<double>* along) {
  const double l1 = se3::norm3(const_cast<double*>(q.e1));
  const double l2 = se3::norm3(const_cast<double*>(q.e2));
  const int n1 = std::max(1, static_cast<int>(l1 / spacing));
  const int n2 = std::max(1, static_cast<int>(l2 / spacing));
  for (int i = 0; i < n1; ++i) {
    for (int j = 0; j < n2; ++j) {
      const double a = (i + 0.5) / n1;
      const double b = (j + 0.5) / n2;
      PointVertex p;
      p.x = static_cast<float>(q.o[0] + a * q.e1[0] + b * q.e2[0]);
      p.y = static_cast<float>(q.o[1] + a * q.e1[1] + b * q.e2[1]);
      p.z = static_cast<float>(q.o[2] + a * q.e1[2] + b * q.e2[2]);
      // The intensity-derived colour the drivers wrote. Points with no
      // acceptable view must come out of colorization still wearing it.
      p.r = p.g = p.b = 128;
      p.a = 255;
      pts->push_back(p);
      if (owner != nullptr) owner->push_back(owner_id);
      if (along != nullptr) along->push_back(a);
    }
  }
}

// An ImageSource over images the test rendered itself: no JPEG, no disk, so a
// colour mismatch can only be the colorizer's.
class MemoryImageSource final : public ImageSource {
 public:
  void add(const std::string& path, DecodedImage img) { images_[path] = std::move(img); }
  Status load(const Keyframe& kf, DecodedImage* out) override {
    const auto it = images_.find(kf.image_path);
    if (it == images_.end()) return ScanError::kNotFound;
    *out = it->second;
    ++loads;
    return kOkStatus;
  }
  int loads = 0;

 private:
  std::map<std::string, DecodedImage> images_;
};

struct Scene {
  std::vector<Rect> rects;
  std::vector<PointVertex> points;
  std::vector<int> owner;   // rect index per point
  std::vector<double> along;
  std::vector<Keyframe> keyframes;
  MemoryImageSource images;
};

// A 4 x 4 x 2.5 m room: four walls, each a different solid colour, seen from
// a ring of cameras near the middle looking outward. Rect 0..3 are the walls.
// The geometry is chosen so the union of the views covers every wall — 93.6°
// horizontal and 77.3° vertical from 1.7 m — because a coverage hole would
// otherwise be indistinguishable from a colorization bug.
Scene build_room(int camera_count = 8, double point_spacing = 0.1) {
  Scene s;
  const double h = 2.5;
  const double w = 2.0;
  {
    const double o[3] = {-w, w, 0}, e1[3] = {2 * w, 0, 0}, e2[3] = {0, 0, h};
    s.rects.push_back(make_rect(o, e1, e2, 220, 40, 40));  // north, red
  }
  {
    const double o[3] = {w, -w, 0}, e1[3] = {-2 * w, 0, 0}, e2[3] = {0, 0, h};
    s.rects.push_back(make_rect(o, e1, e2, 40, 200, 60));  // south, green
  }
  {
    const double o[3] = {w, w, 0}, e1[3] = {0, -2 * w, 0}, e2[3] = {0, 0, h};
    s.rects.push_back(make_rect(o, e1, e2, 50, 70, 230));  // east, blue
  }
  {
    const double o[3] = {-w, -w, 0}, e1[3] = {0, 2 * w, 0}, e2[3] = {0, 0, h};
    s.rects.push_back(make_rect(o, e1, e2, 230, 200, 40));  // west, yellow
  }
  for (int i = 0; i < 4; ++i) {
    sample_rect(s.rects[static_cast<std::size_t>(i)], point_spacing, &s.points, &s.owner, i,
                &s.along);
  }

  const CameraIntrinsics in = test_intrinsics();
  for (int c = 0; c < camera_count; ++c) {
    const double ang = 6.283185307179586 * c / camera_count;
    const double eye[3] = {0.3 * std::cos(ang), 0.3 * std::sin(ang), 1.25};
    const double target[3] = {eye[0] + 10.0 * std::cos(ang), eye[1] + 10.0 * std::sin(ang), 1.25};
    double R[9];
    look_at(eye, target, R);

    Keyframe kf;
    kf.t_mono_ns = 1'000'000'000LL + c * 300'000'000LL;
    char name[64];
    std::snprintf(name, sizeof(name), "streams/frames/room_%02d.jpg", c);
    kf.image_path = name;
    kf.intrinsics = in;
    kf.pose.t_mono_ns = kf.t_mono_ns;
    for (int i = 0; i < 3; ++i) kf.pose.position[i] = eye[i];
    se3::matrix_to_quat(R, kf.pose.orientation);
    kf.pose.quality = PoseQuality::kGood;
    kf.pose.source = StreamId::kPoseAr;
    kf.flags = kKeyframeFlagMotionValid;
    kf.angular_rate_rad_s = 0.05f;  // ~3 deg/s: comfortably inside every gate
    s.keyframes.push_back(kf);

    s.images.add(kf.image_path, render(s.rects, in, [&](int, double Rr[9], double e[3]) {
                   std::memcpy(Rr, R, sizeof(double) * 9);
                   for (int i = 0; i < 3; ++i) e[i] = eye[i];
                 }));
  }
  return s;
}

// True when a point sits at least `margin` from its rectangle's border — the
// interior, where "the right colour" is unambiguous. At a wall's edge the two
// walls' colours meet inside one pixel, so a bilinear sample there is a blend
// of both by construction, not an error.
bool is_interior(const Scene& s, std::size_t i, double margin) {
  const Rect& q = s.rects[static_cast<std::size_t>(s.owner[i])];
  const double p[3] = {s.points[i].x - q.o[0], s.points[i].y - q.o[1], s.points[i].z - q.o[2]};
  const double l1 = se3::norm3(const_cast<double*>(q.e1));
  const double l2 = se3::norm3(const_cast<double*>(q.e2));
  const double a = se3::dot3(p, q.e1) / l1;
  const double b = se3::dot3(p, q.e2) / l2;
  return a > margin && a < l1 - margin && b > margin && b < l2 - margin;
}

ColorizeConfig room_config() {
  ColorizeConfig cfg;
  cfg.sync_quality = SyncQuality::kGood;
  cfg.normal_radius_m = 0.16f;
  cfg.distance_ref_m = 3.f;
  cfg.max_range_m = 20.f;
  cfg.progress_point_interval = 1024;
  return cfg;
}

}  // namespace

// ===========================================================================
// fidx/* — the frames.idx format
// ===========================================================================

TEST_CASE("fidx/record_round_trips_every_field") {
  const Keyframe a = make_keyframe(7);
  std::vector<std::uint8_t> bytes;
  REQUIRE(encode_keyframe_record(a, &bytes).ok());
  // 160 fixed bytes + the name, relative to streams/frames/.
  CHECK(bytes.size() == kKeyframeRecordFixedBytes + std::strlen("kf_000007.jpg"));

  Keyframe b;
  std::size_t consumed = 0;
  REQUIRE(decode_keyframe_record(ByteSpan(bytes.data(), bytes.size()), &b, &consumed).ok());
  CHECK(consumed == bytes.size());
  CHECK(keyframes_equal(a, b));
  CHECK(b.image_path == "streams/frames/kf_000007.jpg");
  CHECK(keyframe_image_name(b) == "kf_000007.jpg");
  CHECK(keyframe_image_path("/tmp/x.lscan", b) == "/tmp/x.lscan/streams/frames/kf_000007.jpg");
  // Trailing bytes are ignored, which is what lets a record sit inside a
  // longer chunk without a length field of its own.
  bytes.push_back(0xAB);
  Keyframe c;
  REQUIRE(decode_keyframe_record(ByteSpan(bytes.data(), bytes.size()), &c, &consumed).ok());
  CHECK(consumed == bytes.size() - 1);
  CHECK(keyframes_equal(a, c));
}

TEST_CASE("fidx/writer_reader_round_trip") {
  const std::string dir = temp_dir("fidx-rt");
  KeyframeIndexWriter w;
  REQUIRE(w.open(dir).ok());
  for (int i = 0; i < 5; ++i) REQUIRE(w.add(make_keyframe(i)).ok());
  CHECK(w.records() == 5);
  REQUIRE(w.close().ok());

  std::vector<Keyframe> got;
  FrameIndexStats st;
  REQUIRE(read_frame_index(dir, &got, &st).ok());
  CHECK(st.records == 5);
  CHECK(st.truncated_tail_chunks == 0);
  CHECK(st.crc_mismatch_chunks == 0);
  CHECK(st.malformed_records == 0);
  CHECK(st.header_time_mismatches == 0);
  CHECK(st.out_of_order_records == 0);
  REQUIRE(got.size() == 5);
  for (int i = 0; i < 5; ++i) CHECK(keyframes_equal(got[static_cast<std::size_t>(i)], make_keyframe(i)));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("fidx/is_byte_identical_to_what_A5s_recorder_writes") {
  // THE seam claim: B8 does not need a keyframe writer at all — it encodes a
  // record and hands it to the recorder it already has open, and the bytes on
  // disk are the same ones this module's own writer produces.
  const std::string dir_a = temp_dir("fidx-writer");
  const std::string dir_b = temp_dir("fidx-recorder");

  KeyframeIndexWriter w;
  REQUIRE(w.open(dir_a).ok());
  for (int i = 0; i < 3; ++i) REQUIRE(w.add(make_keyframe(i)).ok());
  REQUIRE(w.close().ok());

  lscan::FileRecordWriter rec;
  REQUIRE(rec.open(dir_b).ok());
  for (int i = 0; i < 3; ++i) {
    const Keyframe kf = make_keyframe(i);
    std::vector<std::uint8_t> payload;
    REQUIRE(encode_keyframe_record(kf, &payload).ok());
    REQUIRE(rec.write_chunk(lscan::ChunkType::kCameraFrameIndex, kf.t_mono_ns,
                            ByteSpan(payload.data(), payload.size()))
                .ok());
  }
  REQUIRE(rec.close().ok());

  const auto a = read_file(dir_a + "/" + lscan::kFrameIndexFile);
  const auto b = read_file(dir_b + "/" + lscan::kFrameIndexFile);
  REQUIRE(a.size() == b.size());
  // The stream header's t_start_utc_ns is a wall-clock stamp, so it differs
  // by construction; everything from the first chunk on must match exactly.
  CHECK(std::memcmp(a.data() + lscan::kStreamHeaderBytes, b.data() + lscan::kStreamHeaderBytes,
                    a.size() - lscan::kStreamHeaderBytes) == 0);
  // magic, format version, stream id, and t_start_mono_ns — the first
  // keyframe's stamp in both, because both create the file lazily.
  CHECK(std::memcmp(a.data(), b.data(), 16) == 0);

  // And A5's reader agrees the chunks are hers.
  lscan::FileRecordReader r;
  REQUIRE(r.open(dir_b).ok());
  lscan::ChunkHeader h;
  std::vector<std::uint8_t> payload;
  int seen = 0;
  while (r.next_chunk(&h, &payload).ok()) {
    CHECK(h.type == lscan::ChunkType::kCameraFrameIndex);
    CHECK(r.last_stream() == StreamId::kCameraFrames);
    Keyframe kf;
    REQUIRE(decode_keyframe_record(ByteSpan(payload.data(), payload.size()), &kf).ok());
    CHECK(keyframes_equal(kf, make_keyframe(seen)));
    ++seen;
  }
  CHECK(seen == 3);
  REQUIRE(r.close().ok());

  std::error_code ec;
  fs::remove_all(dir_a, ec);
  fs::remove_all(dir_b, ec);
}

TEST_CASE("fidx/tolerates_a_truncated_tail_and_stops_at_a_bad_crc") {
  const std::string dir = temp_dir("fidx-trunc");
  KeyframeIndexWriter w;
  REQUIRE(w.open(dir).ok());
  for (int i = 0; i < 4; ++i) REQUIRE(w.add(make_keyframe(i)).ok());
  REQUIRE(w.close().ok());
  const std::string idx = dir + "/" + lscan::kFrameIndexFile;
  const auto whole = read_file(idx);

  SUBCASE("mid-payload truncation keeps every complete record") {
    std::vector<std::uint8_t> cut(whole.begin(), whole.end() - 40);
    write_file(idx, cut);
    std::vector<Keyframe> got;
    FrameIndexStats st;
    REQUIRE(read_frame_index(dir, &got, &st).ok());
    CHECK(got.size() == 3);
    CHECK(st.truncated_tail_chunks == 1);
  }
  SUBCASE("a flipped payload byte stops the read at that chunk") {
    std::vector<std::uint8_t> bad = whole;
    bad[lscan::kStreamHeaderBytes + lscan::kChunkHeaderBytes + 30] ^= 0xFF;
    write_file(idx, bad);
    std::vector<Keyframe> got;
    FrameIndexStats st;
    REQUIRE(read_frame_index(dir, &got, &st).ok());
    CHECK(got.empty());
    CHECK(st.crc_mismatch_chunks == 1);
  }
  SUBCASE("an exact frame boundary is not a warning") {
    write_file(idx, whole);
    std::vector<Keyframe> got;
    FrameIndexStats st;
    REQUIRE(read_frame_index(dir, &got, &st).ok());
    CHECK(got.size() == 4);
    CHECK(st.truncated_tail_chunks == 0);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("fidx/version_and_forward_compatibility") {
  std::vector<std::uint8_t> bytes;
  REQUIRE(encode_keyframe_record(make_keyframe(1), &bytes).ok());

  SUBCASE("a newer record version is refused, not misread") {
    std::vector<std::uint8_t> newer = bytes;
    newer[0] = kKeyframeRecordVersion + 1;
    Keyframe kf;
    CHECK(decode_keyframe_record(ByteSpan(newer.data(), newer.size()), &kf).error() ==
          ScanError::kVersionMismatch);
  }
  SUBCASE("extra fixed bytes appended by a future writer are skipped") {
    const Keyframe original = make_keyframe(1);
    const std::string name = keyframe_image_name(original);
    std::vector<std::uint8_t> future(kKeyframeRecordFixedBytes + 16 + name.size(), 0);
    std::memcpy(future.data(), bytes.data(), kKeyframeRecordFixedBytes);
    future[2] = static_cast<std::uint8_t>((kKeyframeRecordFixedBytes + 16) & 0xFF);
    future[3] = static_cast<std::uint8_t>((kKeyframeRecordFixedBytes + 16) >> 8);
    std::memcpy(future.data() + kKeyframeRecordFixedBytes + 16, name.data(), name.size());
    Keyframe kf;
    std::size_t consumed = 0;
    REQUIRE(decode_keyframe_record(ByteSpan(future.data(), future.size()), &kf, &consumed).ok());
    CHECK(consumed == future.size());
    CHECK(keyframes_equal(kf, original));
  }
  SUBCASE("a truncated record is corrupt data, never a silent partial") {
    Keyframe kf;
    CHECK(decode_keyframe_record(ByteSpan(bytes.data(), 100), &kf).error() ==
          ScanError::kCorruptData);
  }
}

TEST_CASE("fidx/validation_refuses_what_cannot_be_projected") {
  const Keyframe good = make_keyframe(2);
  REQUIRE(validate_keyframe(good).ok());
  std::vector<std::uint8_t> tmp;

  Keyframe q = good;
  q.pose.orientation[3] = 0.5;  // no longer unit
  CHECK(validate_keyframe(q).error() == ScanError::kInvalidArgument);
  CHECK(encode_keyframe_record(q, &tmp).error() == ScanError::kInvalidArgument);

  q = good;
  q.intrinsics.cx = 9000.f;  // principal point outside the image
  CHECK(validate_keyframe(q).error() == ScanError::kInvalidArgument);

  q = good;
  q.intrinsics.fx = 0.f;
  CHECK(validate_keyframe(q).error() == ScanError::kInvalidArgument);

  q = good;
  q.intrinsics.width = 0;
  CHECK(validate_keyframe(q).error() == ScanError::kInvalidArgument);

  q = good;
  q.intrinsics.rolling_shutter_row_time_ns = -1.f;
  CHECK(validate_keyframe(q).error() == ScanError::kInvalidArgument);

  // Zip-slip's cousin: an index that names a file outside the container.
  q = good;
  q.image_path = "streams/frames/../../../etc/passwd";
  CHECK(validate_keyframe(q).error() == ScanError::kInvalidArgument);
  q.image_path = "/etc/passwd";
  CHECK(validate_keyframe(q).error() == ScanError::kInvalidArgument);
  q.image_path = "";
  CHECK(validate_keyframe(q).error() == ScanError::kInvalidArgument);
  // A name that merely CONTAINS dots is fine.
  q.image_path = "streams/frames/kf..2.jpg";
  CHECK(validate_keyframe(q).ok());
}

TEST_CASE("fidx/a_session_without_a_camera_reports_not_found") {
  const std::string dir = temp_dir("fidx-none");
  std::vector<Keyframe> got;
  // §3.5: "Desktop-captured sessions have no camera → colorization gracefully
  // unavailable." That is kNotFound, not a corrupt container.
  CHECK(read_frame_index(dir, &got, nullptr).error() == ScanError::kNotFound);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ===========================================================================
// img/* — decoding and sampling
// ===========================================================================

TEST_CASE("img/decodes_a_real_jpeg") {
  DecodedImage img;
  REQUIRE(decode_image(ByteSpan(kTinyJpeg, sizeof(kTinyJpeg)), &img).ok());
  CHECK(img.width == 8);
  CHECK(img.height == 8);
  CHECK(img.valid());
  std::uint8_t left[3], right[3];
  img.at(1, 1, left);
  img.at(6, 6, right);
  // JPEG is lossy; the fixture was encoded at quality 95 without chroma
  // subsampling, which lands within a couple of levels.
  CHECK(std::abs(static_cast<int>(left[0]) - 200) <= 4);
  CHECK(std::abs(static_cast<int>(left[1]) - 30) <= 4);
  CHECK(std::abs(static_cast<int>(left[2]) - 40) <= 4);
  CHECK(std::abs(static_cast<int>(right[0]) - 20) <= 4);
  CHECK(std::abs(static_cast<int>(right[1]) - 60) <= 4);
  CHECK(std::abs(static_cast<int>(right[2]) - 220) <= 4);

  SUBCASE("garbage is corrupt data, not a crash") {
    // Not an image at all.
    std::vector<std::uint8_t> junk(2048);
    for (std::size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::uint8_t>(i * 7 + 3);
    DecodedImage bad;
    CHECK(decode_image(ByteSpan(junk.data(), junk.size()), &bad).error() ==
          ScanError::kCorruptData);
    // A JPEG whose headers were scribbled on (a torn write, a bad sector).
    std::vector<std::uint8_t> torn(kTinyJpeg, kTinyJpeg + sizeof(kTinyJpeg));
    for (std::size_t i = 20; i < 120; ++i) torn[i] = static_cast<std::uint8_t>(i * 13);
    DecodedImage bad2;
    CHECK(decode_image(ByteSpan(torn.data(), torn.size()), &bad2).error() ==
          ScanError::kCorruptData);
    DecodedImage empty;
    CHECK(decode_image(ByteSpan(junk.data(), 0), &empty).error() == ScanError::kInvalidArgument);
  }

  SUBCASE("through FileImageSource, with the intrinsics cross-check") {
    const std::string dir = temp_dir("img-file");
    std::error_code ec;
    fs::create_directories(fs::path(dir) / "streams" / "frames", ec);
    std::vector<std::uint8_t> bytes(kTinyJpeg, kTinyJpeg + sizeof(kTinyJpeg));
    write_file(dir + "/streams/frames/tiny.jpg", bytes);

    Keyframe kf = make_keyframe(0);
    kf.image_path = "streams/frames/tiny.jpg";
    kf.intrinsics = test_intrinsics(8, 8, 4.f);
    kf.image_bytes = static_cast<std::uint32_t>(bytes.size());
    FileImageSource src(dir);
    src.set_verify_size(true);
    DecodedImage out;
    REQUIRE(src.load(kf, &out).ok());
    CHECK(out.width == 8);

    // An intrinsics/image mismatch smears colour over the whole cloud; it is
    // caught per keyframe instead.
    kf.intrinsics = test_intrinsics(4032, 3024);
    CHECK(src.load(kf, &out).error() == ScanError::kInvalidArgument);

    kf.image_path = "streams/frames/missing.jpg";
    CHECK(src.load(kf, &out).error() == ScanError::kNotFound);
    fs::remove_all(dir, ec);
  }
}

TEST_CASE("img/bilinear_sampling_is_exact_at_centres_and_midpoints") {
  DecodedImage img;
  img.width = 2;
  img.height = 2;
  img.rgb = {0, 0, 0, 100, 100, 100, 200, 200, 200, 255, 255, 255};
  std::uint8_t c[3];
  // Pixel centres reproduce the pixel exactly — no half-level bias.
  img.sample_bilinear(0.5, 0.5, c);
  CHECK(static_cast<int>(c[0]) == 0);
  img.sample_bilinear(1.5, 0.5, c);
  CHECK(static_cast<int>(c[0]) == 100);
  img.sample_bilinear(1.5, 1.5, c);
  CHECK(static_cast<int>(c[0]) == 255);
  // Midway between two centres is their mean.
  img.sample_bilinear(1.0, 0.5, c);
  CHECK(static_cast<int>(c[0]) == 50);
  // The centre of the 2x2 is the mean of all four.
  img.sample_bilinear(1.0, 1.0, c);
  CHECK(static_cast<int>(c[0]) == 139);  // (0+100+200+255)/4 = 138.75 -> 139
  // Outside the image, clamped rather than wrapped or zeroed.
  img.sample_bilinear(-5.0, -5.0, c);
  CHECK(static_cast<int>(c[0]) == 0);
  img.sample_bilinear(50.0, 50.0, c);
  CHECK(static_cast<int>(c[0]) == 255);
}

// ===========================================================================
// sweep/* — the wizard's 8-second clock-offset estimator
// ===========================================================================

namespace {

// A wizard sweep: the user waves the rig left and right across the board at
// about 1 Hz. Both sensors see the same motion; the camera's clock is off by
// `offset_ns`, i.e. t_engine = t_camera + offset.
void make_sweep_tracks(std::int64_t offset_ns, double noise, std::int64_t span_ns,
                       std::vector<RateSample>* camera, std::vector<RateSample>* lidar,
                       double freq_hz = 1.0) {
  Rng rng;
  const std::int64_t cam_dt = 33'000'000;  // 30 Hz, ARCore's cadence
  const std::int64_t lid_dt = 5'000'000;   // 200 Hz, the Mid-360 IMU
  auto omega = [&](double t_s) {
    // Not a pure sinusoid: a real sweep has a little asymmetry, and a pure
    // one is exactly the ambiguous case the estimator must be able to refuse.
    return std::sin(6.283185307179586 * freq_hz * t_s) +
           0.35 * std::sin(6.283185307179586 * freq_hz * 2.7 * t_s + 0.7);
  };
  // The tracks are generated over a wider span than the overlap the estimator
  // will use, so shifting one by the search radius stays in range.
  for (std::int64_t t = -offset_ns - 300'000'000; t <= span_ns + 300'000'000; t += cam_dt) {
    RateSample s;
    s.t_ns = t;
    // The camera reports its own clock: at camera time t, the true time is
    // t + offset, so it observes omega(t + offset).
    s.value = omega(static_cast<double>(t + offset_ns) * 1e-9) + noise * rng.normal();
    camera->push_back(s);
  }
  for (std::int64_t t = -300'000'000; t <= span_ns + 300'000'000; t += lid_dt) {
    RateSample s;
    s.t_ns = t;
    s.value = 1.7 * omega(static_cast<double>(t) * 1e-9) + noise * rng.normal();  // different scale
    lidar->push_back(s);
  }
}

}  // namespace

TEST_CASE("sweep/recovers_a_known_offset_to_better_than_1_ms") {
  const std::int64_t truths[] = {37'000'000, -23'000'000, 4'500'000, 0};
  for (const std::int64_t truth : truths) {
    std::vector<RateSample> cam, lid;
    make_sweep_tracks(truth, 0.0, 8'000'000'000LL, &cam, &lid);
    ClockSweepResult r;
    REQUIRE(estimate_clock_offset(Span<const RateSample>(cam.data(), cam.size()),
                                  Span<const RateSample>(lid.data(), lid.size()),
                                  ClockSweepConfig(), &r)
                .ok());
    CHECK(r.accepted);
    CHECK(r.verdict == ClockSweepVerdict::kAccepted);
    CHECK(r.correlation > 0.99);
    const std::int64_t err = std::llabs(r.offset_ns - truth);
    INFO("truth ", truth, " got ", r.offset_ns, " err ", err);
    CHECK(err <= 1'000'000);  // ±1 ms, the task's target
  }
}

TEST_CASE("sweep/degrades_gracefully_with_noise") {
  // How much a noisy sweep costs, measured rather than assumed. The ±1 ms
  // target is the noise-free one above; this says what happens when the board
  // is badly lit and the hand shakes.
  struct Case {
    double noise;
    std::int64_t tolerance_ns;
  };
  const Case cases[] = {{0.02, 1'000'000}, {0.08, 3'000'000}, {0.25, 8'000'000}};
  for (const Case& c : cases) {
    std::vector<RateSample> cam, lid;
    make_sweep_tracks(29'000'000, c.noise, 8'000'000'000LL, &cam, &lid);
    ClockSweepResult r;
    REQUIRE(estimate_clock_offset(Span<const RateSample>(cam.data(), cam.size()),
                                  Span<const RateSample>(lid.data(), lid.size()),
                                  ClockSweepConfig(), &r)
                .ok());
    CHECK(r.accepted);
    const std::int64_t err = std::llabs(r.offset_ns - 29'000'000);
    MESSAGE("sweep noise ", c.noise, " -> error ", err * 1e-6, " ms, rho ", r.correlation,
            ", sigma ", r.sigma_ns * 1e-6, " ms");
    CHECK(err <= c.tolerance_ns);
    CHECK(r.sigma_ns > 0.0);
    // The uncertainty is an honest order of magnitude, not a decoration.
    CHECK(r.sigma_ns < 20'000'000.0);
  }
}

TEST_CASE("sweep/refuses_every_capture_it_cannot_place") {
  ClockSweepConfig cfg;
  ClockSweepResult r;

  SUBCASE("a rig that did not move") {
    std::vector<RateSample> cam, lid;
    for (std::int64_t t = 0; t <= 9'000'000'000LL; t += 33'000'000) {
      cam.push_back(RateSample{t, 0.001});
      lid.push_back(RateSample{t, 0.002});
    }
    REQUIRE(estimate_clock_offset(Span<const RateSample>(cam.data(), cam.size()),
                                  Span<const RateSample>(lid.data(), lid.size()), cfg, &r)
                .ok());
    CHECK_FALSE(r.accepted);
    CHECK(r.verdict == ClockSweepVerdict::kNoMotion);
  }
  SUBCASE("a sweep that was too short") {
    std::vector<RateSample> cam, lid;
    make_sweep_tracks(10'000'000, 0.0, 1'500'000'000LL, &cam, &lid);
    REQUIRE(estimate_clock_offset(Span<const RateSample>(cam.data(), cam.size()),
                                  Span<const RateSample>(lid.data(), lid.size()), cfg, &r)
                .ok());
    CHECK_FALSE(r.accepted);
    CHECK(r.verdict == ClockSweepVerdict::kTooShort);
  }
  SUBCASE("a motion so periodic that a whole period fits in the search window") {
    // A pure 12.5 Hz vibration — an 80 ms period — sampled at 200 Hz on both
    // sides. A rival peak sits one period away, inside the ±100 ms search, so
    // the true lag genuinely cannot be told from lag ± 80 ms. (A wizard sweep
    // is ~1 Hz, where no second period fits and this refusal never fires.)
    std::vector<RateSample> cam, lid;
    const std::int64_t offset = 7'000'000;
    for (std::int64_t t = -400'000'000; t <= 9'000'000'000LL; t += 5'000'000) {
      cam.push_back(
          RateSample{t, std::sin(6.283185307179586 * 12.5 * static_cast<double>(t + offset) * 1e-9)});
      lid.push_back(RateSample{t, std::sin(6.283185307179586 * 12.5 * static_cast<double>(t) * 1e-9)});
    }
    REQUIRE(estimate_clock_offset(Span<const RateSample>(cam.data(), cam.size()),
                                  Span<const RateSample>(lid.data(), lid.size()), cfg, &r)
                .ok());
    CHECK_FALSE(r.accepted);
    CHECK(r.verdict == ClockSweepVerdict::kAmbiguous);
  }
  SUBCASE("a wave in one direction only is not a sweep") {
    std::vector<RateSample> cam, lid;
    for (std::int64_t t = -200'000'000; t <= 9'000'000'000LL; t += 33'000'000) {
      const double v = static_cast<double>(t) * 1e-9;
      cam.push_back(RateSample{t, v});
      lid.push_back(RateSample{t, v});
    }
    REQUIRE(estimate_clock_offset(Span<const RateSample>(cam.data(), cam.size()),
                                  Span<const RateSample>(lid.data(), lid.size()), cfg, &r)
                .ok());
    CHECK_FALSE(r.accepted);
    CHECK(r.verdict == ClockSweepVerdict::kNoSweep);
  }
  SUBCASE("structurally bad input is an error, not a verdict") {
    std::vector<RateSample> few{{0, 1.0}, {1, 2.0}};
    std::vector<RateSample> cam, lid;
    make_sweep_tracks(0, 0.0, 8'000'000'000LL, &cam, &lid);
    CHECK(estimate_clock_offset(Span<const RateSample>(few.data(), few.size()),
                                Span<const RateSample>(lid.data(), lid.size()), cfg, &r)
              .error() == ScanError::kInvalidArgument);
    std::vector<RateSample> unsorted = cam;
    std::swap(unsorted[10], unsorted[40]);
    CHECK(estimate_clock_offset(Span<const RateSample>(unsorted.data(), unsorted.size()),
                                Span<const RateSample>(lid.data(), lid.size()), cfg, &r)
              .error() == ScanError::kInvalidArgument);
  }
}

TEST_CASE("sweep/the_offset_sign_is_the_one_the_colorizer_applies") {
  // The whole point of the estimate is that adding it to a camera stamp puts
  // that stamp on the engine clock. Assert it against a track whose true time
  // base is known, rather than trusting the comment.
  std::vector<RateSample> cam, lid;
  const std::int64_t truth = 41'000'000;
  make_sweep_tracks(truth, 0.0, 8'000'000'000LL, &cam, &lid);
  ClockSweepResult r;
  REQUIRE(estimate_clock_offset(Span<const RateSample>(cam.data(), cam.size()),
                                Span<const RateSample>(lid.data(), lid.size()),
                                ClockSweepConfig(), &r)
              .ok());
  REQUIRE(r.accepted);
  CHECK(r.offset_ns > 0);
  // Corrected camera time lines up with the lidar's: the camera sample nearest
  // engine time T has camera stamp T - offset.
  const std::int64_t T = 4'000'000'000LL;
  std::size_t nearest = 0;
  std::int64_t best = 1LL << 62;
  for (std::size_t i = 0; i < cam.size(); ++i) {
    const std::int64_t d = std::llabs(cam[i].t_ns + r.offset_ns - T);
    if (d < best) {
      best = d;
      nearest = i;
    }
  }
  CHECK(std::llabs(cam[nearest].t_ns - (T - truth)) <= 33'000'000);
}

// ===========================================================================
// gate/* — the S6 policy
// ===========================================================================

TEST_CASE("gate/policy_reproduces_the_S6_verdicts") {
  const ColorizationPolicy good = policy_for(SyncQuality::kGood);
  CHECK(good.colorize);
  const ColorizationPolicy gated = policy_for(SyncQuality::kGated);
  CHECK(gated.colorize);
  CHECK(gated.motion_gate_deg_s == doctest::Approx(15.f));  // S6 §6.3, 15 ms row
  const ColorizationPolicy poor = policy_for(SyncQuality::kPoor);
  CHECK_FALSE(poor.colorize);
  const ColorizationPolicy poor_override = policy_for(SyncQuality::kPoor, true);
  CHECK(poor_override.colorize);
  CHECK(poor_override.motion_gate_deg_s == doctest::Approx(10.f));  // S6 §6.3, 30 ms row
  // A4 §4: unconverged means unknown, and unknown must fail CLOSED.
  CHECK_FALSE(policy_for(SyncQuality::kUnknown).colorize);
  CHECK_FALSE(policy_for(SyncQuality::kUnknown, true).colorize);
}

TEST_CASE("gate/an_unsynchronised_session_is_refused_before_any_work") {
  Scene s = build_room(4);
  ColorizeConfig cfg = room_config();
  cfg.sync_quality = SyncQuality::kUnknown;
  PointColorizer c(cfg);
  for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
  c.set_image_source(&s.images);
  CHECK(c.colorize_points(Span<PointVertex>(s.points.data(), s.points.size())).error() ==
        ScanError::kNotSupported);
  CHECK(c.stage() == ColorStage::kFailed);
  CHECK(s.images.loads == 0);  // nothing was decoded
  CHECK(s.points[0].r == 128);  // and nothing was written

  cfg.sync_quality = SyncQuality::kPoor;
  PointColorizer c2(cfg);
  for (const Keyframe& kf : s.keyframes) REQUIRE(c2.add_keyframe(kf).ok());
  c2.set_image_source(&s.images);
  CHECK(c2.colorize_points(Span<PointVertex>(s.points.data(), s.points.size())).error() ==
        ScanError::kNotSupported);

  cfg.allow_poor_sync = true;
  PointColorizer c3(cfg);
  for (const Keyframe& kf : s.keyframes) REQUIRE(c3.add_keyframe(kf).ok());
  c3.set_image_source(&s.images);
  REQUIRE(c3.colorize_points(Span<PointVertex>(s.points.data(), s.points.size())).ok());
  CHECK(c3.policy().motion_gate_deg_s == doctest::Approx(10.f));
}

// ===========================================================================
// color/* — the colorizer against a scene with known truth
// ===========================================================================

TEST_CASE("color/every_interior_point_gets_its_own_wall_colour") {
  Scene s = build_room(8);
  PointColorizer c(room_config());
  for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
  c.set_image_source(&s.images);
  REQUIRE(c.colorize_points(Span<PointVertex>(s.points.data(), s.points.size())).ok());

  const ColorizeStats& st = c.stats();
  CHECK(st.points_total == s.points.size());
  CHECK(st.keyframes_used == 8);
  CHECK(st.points_low_confidence == 0);
  CHECK(st.normals_estimated > st.points_total * 9 / 10);

  std::size_t interior = 0, exact = 0, covered = 0, interior_covered = 0;
  double err_sum = 0.0;
  for (std::size_t i = 0; i < s.points.size(); ++i) {
    const Rect& q = s.rects[static_cast<std::size_t>(s.owner[i])];
    if (c.coverage()[i] != ColorCoverage::kNone) ++covered;
    if (!is_interior(s, i, 0.2)) continue;
    ++interior;
    if (c.coverage()[i] == ColorCoverage::kNone) continue;
    ++interior_covered;
    const int dr = std::abs(static_cast<int>(s.points[i].r) - q.rgb[0]);
    const int dg = std::abs(static_cast<int>(s.points[i].g) - q.rgb[1]);
    const int db = std::abs(static_cast<int>(s.points[i].b) - q.rgb[2]);
    err_sum += (dr + dg + db) / 3.0;
    if (dr == 0 && dg == 0 && db == 0) ++exact;
  }
  REQUIRE(interior > 2500);
  const double exact_frac = static_cast<double>(exact) / static_cast<double>(interior_covered);
  const double mean_err = err_sum / static_cast<double>(interior_covered);
  MESSAGE("interior points ", interior, " of which covered ", interior_covered, "; exact colour ",
          100.0 * exact_frac, " %, mean channel error ", mean_err, "; whole-cloud coverage ",
          100.0 * static_cast<double>(covered) / static_cast<double>(s.points.size()), " %");
  CHECK(exact_frac > 0.995);
  CHECK(mean_err < 1.0);
  CHECK(interior_covered > interior * 99 / 100);
  CHECK(covered >= s.points.size() * 96 / 100);
}

TEST_CASE("color/the_z_buffer_keeps_an_occluder_off_the_wall_behind_it") {
  // A red wall, a white panel hanging in front of it, and three cameras: one
  // head-on (which cannot see the wall behind the panel at all) and two off to
  // the sides (which can, past the panel's edges). Without the occlusion test
  // the head-on camera paints the panel's colour onto the wall behind it,
  // which is exactly the artefact §3.5's z-buffer exists to prevent.
  Rect wall;
  {
    const double o[3] = {-2, 2, 0}, e1[3] = {4, 0, 0}, e2[3] = {0, 0, 2.5};
    wall = make_rect(o, e1, e2, 220, 40, 40);
  }
  Rect panel;
  {
    const double o[3] = {-0.5, 0.8, 0.9}, e1[3] = {1.0, 0, 0}, e2[3] = {0, 0, 0.8};
    panel = make_rect(o, e1, e2, 250, 250, 250);
  }
  std::vector<Rect> rects{wall, panel};

  Scene s;
  s.rects = rects;
  sample_rect(wall, 0.05, &s.points, &s.owner, 0, &s.along);
  const std::size_t first_panel = s.points.size();
  // The panel is sampled finer than the wall: the depth buffer is rendered
  // FROM THE CLOUD, so an occluder has to be dense enough in it to cover the
  // pixels it hides (docs/A11-color.md §6).
  sample_rect(panel, 0.02, &s.points, &s.owner, 1, &s.along);

  const CameraIntrinsics in = test_intrinsics();
  const double eyes[3][3] = {{0.0, -0.6, 1.3}, {-1.6, -0.6, 1.3}, {1.6, -0.6, 1.3}};
  MemoryImageSource images;
  for (int k = 0; k < 3; ++k) {
    Keyframe kf;
    kf.t_mono_ns = 1'000'000'000LL + 200'000'000LL * k;
    char name[64];
    std::snprintf(name, sizeof(name), "streams/frames/occ_%d.jpg", k);
    kf.image_path = name;
    kf.intrinsics = in;
    const double target[3] = {0.0, 2.0, 1.3};
    double R[9];
    look_at(eyes[k], target, R);
    for (int i = 0; i < 3; ++i) kf.pose.position[i] = eyes[k][i];
    se3::matrix_to_quat(R, kf.pose.orientation);
    kf.pose.t_mono_ns = kf.t_mono_ns;
    kf.pose.quality = PoseQuality::kGood;
    kf.flags = kKeyframeFlagMotionValid;
    kf.angular_rate_rad_s = 0.05f;
    s.keyframes.push_back(kf);
    const double* eye = eyes[k];
    images.add(kf.image_path, render(rects, in, [&](int, double Rr[9], double e[3]) {
                 std::memcpy(Rr, R, sizeof(double) * 9);
                 for (int i = 0; i < 3; ++i) e[i] = eye[i];
               }));
  }

  ColorizeConfig cfg = room_config();
  cfg.normal_radius_m = 0.09f;
  PointColorizer c(cfg);
  for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
  c.set_image_source(&images);
  REQUIRE(c.colorize_points(Span<PointVertex>(s.points.data(), s.points.size())).ok());
  CHECK(c.stats().rejected_occluded > 0);

  std::size_t wall_white = 0, panel_white = 0, panel_total = 0, shadowed = 0, shadowed_red = 0;
  for (std::size_t i = 0; i < s.points.size(); ++i) {
    const PointVertex& p = s.points[i];
    const bool white = p.r > 200 && p.g > 200 && p.b > 200;
    const bool red = p.r > 180 && p.g < 90 && p.b < 90;
    if (i >= first_panel) {
      ++panel_total;
      if (white) ++panel_white;
      continue;
    }
    if (white) ++wall_white;
    // Wall points inside the panel's shadow, as seen from the head-on camera.
    if (std::fabs(p.x) < 0.4 && p.z > 1.1 && p.z < 1.6) {
      ++shadowed;
      if (red) ++shadowed_red;
    }
  }
  MESSAGE("panel points ", panel_total, " of which white ", panel_white,
          "; wall points wearing the panel's colour: ", wall_white, "; shadowed wall points ",
          shadowed, " of which red ", shadowed_red);
  REQUIRE(panel_total > 500);
  CHECK(panel_white > panel_total * 95 / 100);  // the panel itself IS white
  CHECK(wall_white == 0);                       // and nothing behind it is
  REQUIRE(shadowed > 50);
  // ...and the side cameras, which CAN see behind the panel, colour them red.
  CHECK(shadowed_red == shadowed);

  SUBCASE("with the occlusion test off, the panel bleeds onto the wall") {
    ColorizeConfig off = cfg;
    off.occlusion_test = false;
    std::vector<PointVertex> work = s.points;
    for (std::size_t i = 0; i < work.size(); ++i) work[i].r = work[i].g = work[i].b = 128;
    PointColorizer c2(off);
    for (const Keyframe& kf : s.keyframes) REQUIRE(c2.add_keyframe(kf).ok());
    c2.set_image_source(&images);
    REQUIRE(c2.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    std::size_t bleed = 0;
    for (std::size_t i = 0; i < first_panel; ++i) {
      if (work[i].r > 200 && work[i].g > 200 && work[i].b > 200) ++bleed;
    }
    MESSAGE("occlusion test off: ", bleed, " wall points wearing the panel's colour");
    CHECK(bleed > 100);
  }
}

TEST_CASE("color/the_rolling_shutter_correction_measurably_reduces_the_error") {
  // S6 §7.1 item 2: "model rolling shutter as a per-row time offset in the
  // colorization projection — −6.8 px, free. Currently unmodelled and
  // silently spending a third of the budget."
  //
  // A camera rotating at 30 °/s (S6's budget case) with a 20 ms readout, in
  // front of a wall carrying a triangle-wave colour ramp whose period is
  // 1.0 m: one colour level is 1.96 mm of wall, so a colour error IS a
  // position error, measured in millimetres.
  const double omega = 30.0 * se3::kDegToRad;  // rad/s about world +Z
  const double readout_s = 0.020;
  const CameraIntrinsics base = test_intrinsics();
  const std::int64_t t0 = 2'000'000'000LL;

  Rect wall;
  {
    const double o[3] = {-4, 3, 0}, e1[3] = {8, 0, 0}, e2[3] = {0, 0, 2.5};
    wall = make_rect(o, e1, e2, 0, 0, 0);
    wall.gradient = true;
    wall.gradient_period_m = 1.0;
  }
  const std::vector<Rect> rects{wall};

  std::vector<PointVertex> truth_pts;
  std::vector<int> owner;
  std::vector<double> along;
  sample_rect(wall, 0.08, &truth_pts, &owner, 0, &along);
  // Only the part of the wall the camera actually sees head-on.
  std::vector<PointVertex> pts;
  std::vector<std::uint8_t> want;
  for (std::size_t i = 0; i < truth_pts.size(); ++i) {
    if (std::fabs(truth_pts[i].x) > 1.2 || truth_pts[i].z < 0.6 || truth_pts[i].z > 1.8) continue;
    pts.push_back(truth_pts[i]);
    std::uint8_t rgb[3];
    rect_color(wall, along[i], rgb);
    want.push_back(rgb[0]);
  }
  REQUIRE(pts.size() > 300);

  // The camera: at the origin, looking north, yawing at `omega`.
  auto pose_at_time = [&](double t_s, double R[9], double eye[3]) {
    const double yaw = omega * t_s;
    const double target[3] = {10.0 * std::sin(yaw), 10.0 * std::cos(yaw), 1.2};
    eye[0] = 0.0;
    eye[1] = -0.5;
    eye[2] = 1.2;
    look_at(eye, target, R);
  };

  // Two keyframes 200 ms apart, each rendered ROW BY ROW at the pose of that
  // row's exposure — a real rolling shutter, not a model of one.
  std::vector<Keyframe> kfs;
  MemoryImageSource images;
  for (int k = 0; k < 2; ++k) {
    const double t_k = 0.2 * k;
    Keyframe kf;
    kf.t_mono_ns = t0 + static_cast<std::int64_t>(std::llround(t_k * 1e9));
    char name[64];
    std::snprintf(name, sizeof(name), "streams/frames/rs_%d.jpg", k);
    kf.image_path = name;
    kf.intrinsics = base;
    kf.intrinsics.rolling_shutter_row_time_ns =
        static_cast<float>(readout_s * 1e9 / base.height);
    double R[9], eye[3];
    pose_at_time(t_k, R, eye);
    for (int i = 0; i < 3; ++i) kf.pose.position[i] = eye[i];
    se3::matrix_to_quat(R, kf.pose.orientation);
    kf.pose.t_mono_ns = kf.t_mono_ns;
    kf.pose.quality = PoseQuality::kGood;
    kf.flags = kKeyframeFlagMotionValid;
    kf.angular_rate_rad_s = static_cast<float>(omega);
    kfs.push_back(kf);
    images.add(kf.image_path,
               render(rects, base, [&](int row, double Rr[9], double e[3]) {
                 pose_at_time(t_k + row * readout_s / base.height, Rr, e);
               }));
  }

  auto mean_error = [&](bool correct, bool use_pose_fn) {
    std::vector<PointVertex> work = pts;
    ColorizeConfig cfg;
    cfg.sync_quality = SyncQuality::kGood;
    cfg.rolling_shutter = correct;
    cfg.estimate_normals = false;  // the wall's normal is not what is measured
    cfg.max_incidence_deg = 89.f;
    PointColorizer c(cfg);
    for (const Keyframe& kf : kfs) REQUIRE(c.add_keyframe(kf).ok());
    c.set_image_source(&images);
    if (use_pose_fn) {
      c.set_pose_fn([&](std::int64_t t_ns, double wfc[16]) {
        double R[9], eye[3];
        pose_at_time(static_cast<double>(t_ns - t0) * 1e-9, R, eye);
        se3::mat4_from_rt(R, eye, wfc);
        return true;
      });
    }
    REQUIRE(c.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < work.size(); ++i) {
      if (c.coverage()[i] == ColorCoverage::kNone) continue;
      sum += std::fabs(static_cast<double>(work[i].r) - want[i]);
      ++n;
    }
    REQUIRE(n > 250);
    return sum / static_cast<double>(n);
  };

  const double levels_to_mm = 1000.0 * wall.gradient_period_m / (2.0 * 255.0);
  const double off = mean_error(false, true);
  const double on_exact = mean_error(true, true);
  const double on_estimated = mean_error(true, false);
  MESSAGE("rolling shutter, mean colour error: uncorrected ", off, " levels (", off * levels_to_mm,
          " mm), corrected with the trajectory ", on_exact, " levels (", on_exact * levels_to_mm,
          " mm), corrected from the keyframes alone ", on_estimated, " levels (",
          on_estimated * levels_to_mm, " mm)");
  CHECK(off > 3.0 * on_exact);
  // The fallback — constant velocity differenced from the neighbouring
  // keyframes, no trajectory at all — has to earn its place too.
  CHECK(off > 3.0 * on_estimated);
}

TEST_CASE("color/the_motion_gate_prefers_the_slow_keyframe") {
  // S6 §6.3, the cheapest fix in the whole budget: two keyframes see the same
  // wall, and the FAST one is geometrically the better view. Without the gate
  // it wins; with it, the slow one does.
  Rect wall;
  {
    const double o[3] = {-3, 3, 0}, e1[3] = {6, 0, 0}, e2[3] = {0, 0, 2.5};
    wall = make_rect(o, e1, e2, 200, 200, 200);
  }
  std::vector<PointVertex> pts;
  std::vector<int> owner;
  std::vector<double> along;
  sample_rect(wall, 0.1, &pts, &owner, 0, &along);

  const CameraIntrinsics in = test_intrinsics();
  MemoryImageSource images;
  std::vector<Keyframe> kfs;
  const std::uint8_t tints[2][3] = {{250, 20, 20}, {20, 20, 250}};  // fast=red, slow=blue
  const double rates[2] = {40.0, 4.0};                              // deg/s
  // Nearly co-located, so the two see the same wall and the ONLY thing
  // separating them geometrically is that the fast one is 10 cm closer.
  const double eyes[2][3] = {{0.0, 0.3, 1.2}, {0.0, 0.2, 1.2}};
  for (int k = 0; k < 2; ++k) {
    Rect tinted = wall;
    std::memcpy(tinted.rgb, tints[k], 3);
    const std::vector<Rect> rects{tinted};
    Keyframe kf;
    kf.t_mono_ns = 1'000'000'000LL + 200'000'000LL * k;
    char name[64];
    std::snprintf(name, sizeof(name), "streams/frames/mg_%d.jpg", k);
    kf.image_path = name;
    kf.intrinsics = in;
    const double target[3] = {eyes[k][0], eyes[k][1] + 10.0, 1.2};
    double R[9];
    look_at(eyes[k], target, R);
    for (int i = 0; i < 3; ++i) kf.pose.position[i] = eyes[k][i];
    se3::matrix_to_quat(R, kf.pose.orientation);
    kf.pose.t_mono_ns = kf.t_mono_ns;
    kf.pose.quality = PoseQuality::kGood;
    kf.flags = kKeyframeFlagMotionValid;
    kf.angular_rate_rad_s = static_cast<float>(rates[k] * se3::kDegToRad);
    kfs.push_back(kf);
    images.add(kf.image_path, render(rects, in, [&](int, double Rr[9], double e[3]) {
                 std::memcpy(Rr, R, sizeof(double) * 9);
                 for (int i = 0; i < 3; ++i) e[i] = eyes[k][i];
               }));
  }

  auto count_red = [&](const ColorizeConfig& cfg, ColorizeStats* out_stats,
                       std::vector<ColorCoverage>* out_cov) {
    std::vector<PointVertex> work = pts;
    PointColorizer c(cfg);
    for (const Keyframe& kf : kfs) REQUIRE(c.add_keyframe(kf).ok());
    c.set_image_source(&images);
    REQUIRE(c.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    if (out_stats != nullptr) *out_stats = c.stats();
    if (out_cov != nullptr) {
      out_cov->assign(c.coverage().begin(), c.coverage().end());
    }
    std::size_t red = 0, blue = 0;
    for (const PointVertex& p : work) {
      if (p.r > 150 && p.b < 100) ++red;
      if (p.b > 150 && p.r < 100) ++blue;
    }
    return std::pair<std::size_t, std::size_t>(red, blue);
  };

  ColorizeConfig ungated = room_config();
  ungated.motion_gate_deg_s = 1000.f;  // the gate switched off
  ungated.motion_reject_deg_s = 1000.f;
  // Normals off in BOTH runs, so the only geometric difference between the
  // two views is the 10 cm of range: with the incidence term in play the
  // FARTHER camera is the more head-on one at the wall's edges, and the
  // control would be measuring that trade-off instead of the gate.
  ungated.estimate_normals = false;
  const auto no_gate = count_red(ungated, nullptr, nullptr);

  ColorizeConfig gated = room_config();
  gated.estimate_normals = false;
  gated.sync_quality = SyncQuality::kGated;  // S6's 15 ms case → a 15 °/s gate
  ColorizeStats st;
  std::vector<ColorCoverage> cov;
  const auto with_gate = count_red(gated, &st, &cov);

  MESSAGE("ungated: ", no_gate.first, " points from the fast frame, ", no_gate.second,
          " from the slow one; gated: ", with_gate.first, " / ", with_gate.second);
  // Without the gate the closer (fast) frame wins nearly everything...
  CHECK(no_gate.first > no_gate.second * 5);
  // ...and with it, the slow frame does.
  CHECK(with_gate.second > with_gate.first * 5);
  CHECK(st.points_low_confidence == 0);  // the winner is inside the gate

  SUBCASE("a frame above the gate still colours, but flagged low-confidence") {
    // S6 §6.3: "points that can only be seen from fast-turn frames should be
    // coloured anyway but flagged low-confidence".
    std::vector<Keyframe> only_fast{kfs[0]};
    std::vector<PointVertex> work = pts;
    PointColorizer c(gated);
    REQUIRE(c.add_keyframe(only_fast[0]).ok());
    c.set_image_source(&images);
    REQUIRE(c.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    CHECK(c.stats().points_colorized > 0);
    CHECK(c.stats().points_low_confidence == c.stats().points_colorized);
    for (std::size_t i = 0; i < work.size(); ++i) {
      if (c.coverage()[i] == ColorCoverage::kNone) continue;
      CHECK(c.coverage()[i] == ColorCoverage::kLowConfidence);
    }
  }

  SUBCASE("a frame above the REJECT threshold is not used at all") {
    ColorizeConfig strict = gated;
    strict.motion_reject_deg_s = 20.f;
    std::vector<PointVertex> work = pts;
    PointColorizer c(strict);
    REQUIRE(c.add_keyframe(kfs[0]).ok());  // 40 deg/s
    c.set_image_source(&images);
    REQUIRE(c.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    CHECK(c.stats().keyframes_rejected_motion == 1);
    CHECK(c.stats().points_colorized == 0);
    CHECK(work[0].r == 128);  // still wearing its intensity colour
  }

  SUBCASE("the caller's angular-rate function outranks the recorded value") {
    // A4's ImuIngest::angular_rate_at is the production source; the recorded
    // per-keyframe rate is the fallback when there is no IMU to ask.
    std::vector<PointVertex> work = pts;
    PointColorizer c(gated);
    for (const Keyframe& kf : kfs) REQUIRE(c.add_keyframe(kf).ok());
    c.set_image_source(&images);
    // Claim the FAST frame was actually slow and the slow one fast: the
    // colours must swap.
    c.set_angular_rate_fn([&](std::int64_t t_ns, double* rad_s) {
      *rad_s = (t_ns <= kfs[0].t_mono_ns) ? 4.0 * se3::kDegToRad : 40.0 * se3::kDegToRad;
      return true;
    });
    REQUIRE(c.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    std::size_t red = 0;
    for (const PointVertex& p : work) {
      if (p.r > 150 && p.b < 100) ++red;
    }
    CHECK(red > work.size() / 2);
  }
}

TEST_CASE("color/points_no_camera_saw_keep_their_colour_and_are_flagged") {
  Scene s = build_room(8);
  // A patch OUTSIDE the room, behind the north wall: in no camera's view, and
  // occluded by the wall even where it would project.
  const double o[3] = {-1, 3.5, 0.5}, e1[3] = {2, 0, 0}, e2[3] = {0, 0, 1};
  const int hidden = static_cast<int>(s.rects.size());
  s.rects.push_back(make_rect(o, e1, e2, 10, 10, 10));
  const std::size_t first_hidden = s.points.size();
  sample_rect(s.rects.back(), 0.1, &s.points, &s.owner, hidden, &s.along);

  PointColorizer c(room_config());
  for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
  c.set_image_source(&s.images);
  REQUIRE(c.colorize_points(Span<PointVertex>(s.points.data(), s.points.size())).ok());

  std::size_t hidden_total = 0;
  for (std::size_t i = first_hidden; i < s.points.size(); ++i) {
    ++hidden_total;
    CHECK(c.coverage()[i] == ColorCoverage::kNone);
    // The intensity-derived colour is untouched — §3.5's "keep what you had".
    CHECK(static_cast<int>(s.points[i].r) == 128);
    CHECK(static_cast<int>(s.points[i].g) == 128);
    CHECK(static_cast<int>(s.points[i].b) == 128);
    CHECK(static_cast<int>(s.points[i].a) == 255);
  }
  REQUIRE(hidden_total > 100);
  CHECK(c.stats().points_uncovered >= hidden_total);
  CHECK(c.stats().points_colorized + c.stats().points_uncovered == s.points.size());

  SUBCASE("and the alpha channel can be used to mark them, opt-in") {
    ColorizeConfig cfg = room_config();
    cfg.uncovered_alpha = 64;
    cfg.low_confidence_alpha = 96;
    std::vector<PointVertex> work = s.points;
    for (PointVertex& p : work) p.a = 255;
    PointColorizer c2(cfg);
    for (const Keyframe& kf : s.keyframes) REQUIRE(c2.add_keyframe(kf).ok());
    c2.set_image_source(&s.images);
    REQUIRE(c2.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    CHECK(static_cast<int>(work[first_hidden].a) == 64);
    std::size_t a_colorized = 0;
    for (std::size_t i = 0; i < first_hidden; ++i) {
      if (c2.coverage()[i] == ColorCoverage::kColorized) {
        a_colorized = i;
        break;
      }
    }
    CHECK(static_cast<int>(work[a_colorized].a) == 255);
  }
}

TEST_CASE("color/refuses_a_non_rigid_extrinsic_and_composes_a_lidar_body_trajectory") {
  Scene s = build_room(4);
  PointColorizer c(room_config());

  // A8 §4.4's column-major-across-JNI trap.
  double column_major[16];
  {
    double R[9];
    const double axis[3] = {0.3, 0.4, 0.86602540378};
    double w[3] = {axis[0] * 0.7, axis[1] * 0.7, axis[2] * 0.7};
    se3::so3_exp(w, R);
    const double t[3] = {0.1, 0.2, 0.3};
    double row_major[16];
    se3::mat4_from_rt(R, t, row_major);
    for (int r = 0; r < 4; ++r) {
      for (int col = 0; col < 4; ++col) column_major[col * 4 + r] = row_major[r * 4 + col];
    }
  }
  CHECK(c.set_extrinsics(column_major).error() == ScanError::kInvalidArgument);
  double scaled[16];
  se3::mat4_identity(scaled);
  scaled[0] = 2.0;
  CHECK(c.set_extrinsics(scaled).error() == ScanError::kInvalidArgument);

  // With poses in the LIDAR BODY frame, the colorizer must compose the mount
  // extrinsic itself and land on exactly the same colours as the camera-frame
  // path. camera_from_lidar here is a 110 mm baseline with a 15° tilt — S6's
  // Mid-360 mount (b).
  std::vector<PointVertex> ref = s.points;
  PointColorizer a(room_config());
  for (const Keyframe& kf : s.keyframes) REQUIRE(a.add_keyframe(kf).ok());
  a.set_image_source(&s.images);
  REQUIRE(a.colorize_points(Span<PointVertex>(ref.data(), ref.size())).ok());

  double camera_from_lidar[16];
  {
    double w[3] = {15.0 * se3::kDegToRad, 0.0, 0.0};
    double R[9];
    se3::so3_exp(w, R);
    const double t[3] = {0.0, -0.11, 0.02};
    se3::mat4_from_rt(R, t, camera_from_lidar);
  }
  double lidar_from_camera[16];
  se3::mat4_inverse_rigid(camera_from_lidar, lidar_from_camera);

  ColorizeConfig cfg = room_config();
  cfg.pose_frame = KeyframePoseFrame::kLidarBody;
  PointColorizer b(cfg);
  REQUIRE(b.set_extrinsics(camera_from_lidar).ok());
  for (const Keyframe& kf : s.keyframes) {
    // world_from_body = world_from_camera * camera_from_lidar
    Keyframe body = kf;
    double wfc[16], wfb[16];
    se3::mat4_from_quat_pos(kf.pose.orientation, kf.pose.position, wfc);
    se3::mat4_mul(wfc, camera_from_lidar, wfb);
    double R[9], t[3];
    se3::mat4_get_rt(wfb, R, t);
    se3::matrix_to_quat(R, body.pose.orientation);
    for (int i = 0; i < 3; ++i) body.pose.position[i] = t[i];
    REQUIRE(b.add_keyframe(body).ok());
  }
  b.set_image_source(&s.images);
  std::vector<PointVertex> got = s.points;
  REQUIRE(b.colorize_points(Span<PointVertex>(got.data(), got.size())).ok());
  std::size_t same = 0;
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (got[i].r == ref[i].r && got[i].g == ref[i].g && got[i].b == ref[i].b) ++same;
  }
  MESSAGE("lidar-body poses + extrinsic reproduce ",
          100.0 * static_cast<double>(same) / static_cast<double>(got.size()),
          " % of the camera-frame colours");
  CHECK(same > got.size() * 999 / 1000);
}

// ===========================================================================
// gate/* — plumbing: cancel, progress, determinism, the PageStore seam
// ===========================================================================

TEST_CASE("gate/colorize_is_deterministic") {
  Scene s = build_room(6);
  auto run = [&]() {
    std::vector<PointVertex> work = s.points;
    PointColorizer c(room_config());
    for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
    c.set_image_source(&s.images);
    REQUIRE(c.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    return work;
  };
  const auto a = run();
  const auto b = run();
  REQUIRE(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(PointVertex)) == 0);
}

TEST_CASE("gate/progress_is_monotone_and_cancel_unwinds") {
  Scene s = build_room(6);

  std::vector<float> fractions;
  std::vector<ColorStage> stages;
  {
    std::vector<PointVertex> work = s.points;
    PointColorizer c(room_config());
    for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
    c.set_image_source(&s.images);
    c.set_progress_callback([&](const ColorProgress& p) {
      fractions.push_back(p.fraction);
      stages.push_back(p.stage);
      CHECK(p.label != nullptr);
    });
    REQUIRE(c.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    CHECK(c.stage() == ColorStage::kDone);
    CHECK(c.progress() == doctest::Approx(1.f));
  }
  REQUIRE(fractions.size() > 5);
  for (std::size_t i = 1; i < fractions.size(); ++i) CHECK(fractions[i] >= fractions[i - 1]);
  CHECK(fractions.back() == doctest::Approx(1.f));
  CHECK(stages.back() == ColorStage::kDone);

  SUBCASE("an external token cancels mid-run") {
    post::CancelToken token;
    std::vector<PointVertex> work = s.points;
    PointColorizer c(room_config());
    for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
    c.set_image_source(&s.images);
    c.set_cancel_token(&token);
    // A15 owns the token; the pipeline only reads it (A7's contract).
    c.set_progress_callback([&](const ColorProgress& p) {
      if (p.fraction > 0.5f) token.cancel();
    });
    CHECK(c.colorize_points(Span<PointVertex>(work.data(), work.size())).error() ==
          ScanError::kCancelled);
    CHECK(c.stage() == ColorStage::kCancelled);
  }

  SUBCASE("the colorizer's own cancel() is sticky") {
    std::vector<PointVertex> work = s.points;
    PointColorizer c(room_config());
    for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
    c.set_image_source(&s.images);
    c.cancel();
    CHECK(c.colorize_points(Span<PointVertex>(work.data(), work.size())).error() ==
          ScanError::kCancelled);
  }
}

TEST_CASE("gate/colorizes_a_page_store_in_place") {
  Scene s = build_room(8);
  PageStoreConfig pcfg;
  pcfg.page_capacity = 2048;  // several pages, so the chunked path is exercised
  pcfg.max_pages = 64;
  PageStore store(pcfg);
  std::uint32_t appended = 0;
  REQUIRE(store
              .append(StreamId::kSlamMap, Span<const PointVertex>(s.points.data(), s.points.size()),
                      1'000'000'000LL, &appended)
              .ok());
  REQUIRE(appended == s.points.size());
  REQUIRE(store.page_count() > 1);

  PointColorizer c(room_config());
  for (const Keyframe& kf : s.keyframes) REQUIRE(c.add_keyframe(kf).ok());
  c.set_image_source(&s.images);
  REQUIRE(c.colorize(&store).ok());
  CHECK(c.stats().points_total == s.points.size());
  CHECK(c.stats().points_colorized >= s.points.size() * 96 / 100);

  // The points in the store now carry the colours, in the same order.
  std::size_t gi = 0, exact = 0, interior = 0;
  for (const PageId id : store.page_ids()) {
    const PageView pv = store.page_view(id);
    for (std::uint32_t i = 0; i < pv.count; ++i, ++gi) {
      // Position is untouched: colorization only ever writes r/g/b.
      CHECK(pv.data[i].x == s.points[gi].x);
      if (!is_interior(s, gi, 0.2)) continue;
      ++interior;
      const Rect& q = s.rects[static_cast<std::size_t>(s.owner[gi])];
      if (pv.data[i].r == q.rgb[0] && pv.data[i].g == q.rgb[1] && pv.data[i].b == q.rgb[2]) ++exact;
    }
  }
  CHECK(gi == s.points.size());
  REQUIRE(interior > 1000);
  CHECK(static_cast<double>(exact) / static_cast<double>(interior) > 0.995);

  SUBCASE("an empty store and a null store are refused") {
    PageStore empty;
    PointColorizer c2(room_config());
    for (const Keyframe& kf : s.keyframes) REQUIRE(c2.add_keyframe(kf).ok());
    c2.set_image_source(&s.images);
    CHECK(c2.colorize(&empty).error() == ScanError::kInvalidArgument);
    CHECK(c2.colorize(nullptr).error() == ScanError::kInvalidArgument);
  }
}

TEST_CASE("gate/end_to_end_from_a_real_lscan_directory") {
  // The shipping path: frames.idx on disk, JPEGs beside it, keyframes loaded
  // by the colorizer itself. The image is the tiny fixture JPEG (a solid-ish
  // two-tone), so what is asserted here is the PLUMBING — index → file →
  // decoder → sampler — not the colour accuracy the room scene measures.
  const std::string dir = temp_dir("e2e");
  std::error_code ec;
  fs::create_directories(fs::path(dir) / "streams" / "frames", ec);
  std::vector<std::uint8_t> jpeg(kTinyJpeg, kTinyJpeg + sizeof(kTinyJpeg));
  write_file(dir + "/streams/frames/f0.jpg", jpeg);

  Keyframe kf;
  kf.t_mono_ns = 5'000'000'000LL;
  kf.image_path = "streams/frames/f0.jpg";
  kf.intrinsics = test_intrinsics(8, 8, 6.f);
  kf.pose.position[0] = 0.0;
  kf.pose.position[1] = 0.0;
  kf.pose.position[2] = 0.0;
  {
    double R[9];
    const double eye[3] = {0, 0, 0};
    const double target[3] = {0, 2, 0};
    look_at(eye, target, R);
    se3::matrix_to_quat(R, kf.pose.orientation);
  }
  kf.pose.quality = PoseQuality::kGood;
  kf.image_bytes = static_cast<std::uint32_t>(jpeg.size());
  kf.flags = kKeyframeFlagMotionValid;
  kf.angular_rate_rad_s = 0.02f;

  KeyframeIndexWriter w;
  REQUIRE(w.open(dir).ok());
  REQUIRE(w.add(kf).ok());
  REQUIRE(w.close().ok());

  // Two points either side of the optical axis, 2 m away: one lands in the
  // left (red) half of the frame, one in the right (blue) half.
  std::vector<PointVertex> pts(2);
  pts[0] = PointVertex{-0.3f, 2.f, 0.f, 128, 128, 128, 255};
  pts[1] = PointVertex{0.3f, 2.f, 0.f, 128, 128, 128, 255};

  ColorizeConfig cfg;
  cfg.sync_quality = SyncQuality::kGood;
  cfg.estimate_normals = false;
  cfg.occlusion_test = false;
  cfg.edge_margin_px = 0;
  PointColorizer c(cfg);
  FrameIndexStats st;
  REQUIRE(c.load_keyframes(dir, &st).ok());
  CHECK(st.records == 1);
  CHECK(c.keyframes().size() == 1);
  REQUIRE(c.colorize_points(Span<PointVertex>(pts.data(), pts.size())).ok());
  CHECK(c.stats().points_colorized == 2);
  CHECK(pts[0].r > 150);  // left half: red
  CHECK(pts[0].b < 90);
  CHECK(pts[1].b > 150);  // right half: blue
  CHECK(pts[1].r < 90);

  SUBCASE("a missing image is skipped, not fatal") {
    fs::remove(fs::path(dir) / "streams" / "frames" / "f0.jpg", ec);
    std::vector<PointVertex> work = pts;
    PointColorizer c2(cfg);
    REQUIRE(c2.load_keyframes(dir, nullptr).ok());
    REQUIRE(c2.colorize_points(Span<PointVertex>(work.data(), work.size())).ok());
    CHECK(c2.stats().keyframes_image_failed == 1);
    CHECK(c2.stats().points_uncovered == 2);
  }
  fs::remove_all(dir, ec);
}
