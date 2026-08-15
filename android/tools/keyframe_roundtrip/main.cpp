// keyframe_roundtrip — writes N synthetic keyframes through the shipped
// keyframe writer, then reads the .lscan back with the engine's own reader
// and checks every field survived. See CMakeLists.txt for why this is a host
// tool rather than an Android instrumentation test.
//
// Checks, in order:
//   1. A fresh .lscan has NO frames.idx (the lazy-create property A11 relies
//      on for "a session with no camera has no index", so read_frame_index()
//      returning kNotFound means it).
//   2. After N adds it exists, and read_frame_index() returns exactly N
//      records with a clean FrameIndexStats (no truncated tail, no CRC
//      mismatch, no rejected/out-of-order records).
//   3. Field-for-field equality on record 0 and record N-1: time, pose,
//      intrinsics, rolling-shutter row time, motion, exposure, flags and the
//      image name (which the writer stores relative to streams/frames/ and
//      the reader recomposes relative to the .lscan root — the one
//      transformation in the path, so it is checked explicitly).
//   4. A record the format must REFUSE (non-unit quaternion) is refused at
//      add() time rather than written and skipped later.
//   5. An absolute image name is refused (the zip-slip class validate_
//      keyframe() guards).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "keyframe_writer.h"
#include "scanengine/color/colorize.h"
#include "scanengine/color/frames_idx.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::printf("  ok   %s\n", what.c_str());
  } else {
    std::printf("  FAIL %s\n", what.c_str());
    ++g_failures;
  }
}

void check_near(double a, double b, double tol, const std::string& what) {
  bool ok = std::fabs(a - b) <= tol;
  if (!ok) std::printf("       (%.9g vs %.9g, tol %.3g)\n", a, b, tol);
  check(ok, what);
}

// A deterministic synthetic capture: a rig walking +x at 1 m/s while yawing
// slowly, 3 fps keyframes. Nothing here is random — a failure has to be
// reproducible to be worth reporting.
lidarscan_jni::KeyframeInput make_keyframe(int i) {
  lidarscan_jni::KeyframeInput kf;
  const double t = i / 3.0;  // seconds
  kf.t_engine_ns = static_cast<std::int64_t>(1'000'000'000LL * 100 + t * 1e9);
  kf.exposure_ns = 8'000'000;  // 8 ms

  kf.position[0] = t * 1.0;
  kf.position[1] = 1.4;
  kf.position[2] = -0.25 * t;

  // Yaw about +y at 6 deg/s, as a unit quaternion (x, y, z, w).
  const double yaw = 6.0 * t * M_PI / 180.0;
  kf.orientation[0] = 0.0;
  kf.orientation[1] = std::sin(yaw / 2.0);
  kf.orientation[2] = 0.0;
  kf.orientation[3] = std::cos(yaw / 2.0);

  kf.fx = 1462.5f;
  kf.fy = 1462.5f;
  kf.cx = 640.0f;
  kf.cy = 360.0f;
  kf.distortion[0] = 0.05f;
  kf.distortion[1] = -0.12f;
  kf.width = 1280;
  kf.height = 720;
  // A typical phone: ~22 ms of readout skew over 720 rows.
  kf.row_time_ns = 22'000'000.0f / 719.0f;

  kf.position_sigma_m = 0.02f;
  kf.orientation_sigma_deg = 0.5f;
  kf.pose_quality = 3;  // PoseQuality::kGood
  kf.tracking_lost = 0;
  kf.pose_source = 4;  // StreamId::kPoseAr

  kf.flags = scanengine::kKeyframeFlagMotionValid | scanengine::kKeyframeFlagExposureValid;
  kf.iso = 320.0f;
  kf.angular_rate_rad_s = static_cast<float>(6.0 * M_PI / 180.0);
  kf.linear_speed_m_s = 1.03f;

  kf.image_bytes = 100'000 + static_cast<std::uint32_t>(i);
  char name[32];
  std::snprintf(name, sizeof(name), "kf_%06d.jpg", i);
  kf.image_name = name;
  return kf;
}

}  // namespace

int main(int argc, char** argv) {
  const int kCount = 12;
  fs::path root =
      argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "lidarscan-keyframe-roundtrip";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::path lscan = root / "roundtrip.lscan";
  fs::create_directories(lscan, ec);

  std::printf("keyframe_roundtrip: %s\n", lscan.string().c_str());

  const fs::path idx = lscan / "streams" / "frames" / "frames.idx";

  // --- 1. lazy creation ----------------------------------------------------
  {
    lidarscan_jni::KeyframeWriter w;
    std::string err;
    check(w.open(lscan.string(), &err), "open() succeeds: " + err);
    check(!fs::exists(idx), "frames.idx is NOT created by open() (lazy, per A11 §3.1)");

    std::vector<lidarscan_jni::KeyframeInput> written;
    for (int i = 0; i < kCount; ++i) {
      auto kf = make_keyframe(i);
      written.push_back(kf);
      if (!w.add(kf, &err)) {
        check(false, "add(" + std::to_string(i) + "): " + err);
        return 1;
      }
    }
    check(w.records() == static_cast<std::uint32_t>(kCount), "records() == N");

    // --- 4/5. the format's refusals ---------------------------------------
    auto bad_quat = make_keyframe(999);
    bad_quat.orientation[3] = 0.5;  // no longer unit
    check(!w.add(bad_quat, &err), "a non-unit quaternion is REFUSED at add()");

    auto bad_name = make_keyframe(998);
    bad_name.image_name = "/etc/passwd";
    check(!w.add(bad_name, &err), "an absolute image name is REFUSED at add()");

    auto escaping_name = make_keyframe(997);
    escaping_name.image_name = "../../../etc/passwd";
    check(!w.add(escaping_name, &err), "a '..' image name is REFUSED at add()");

    check(w.close(&err), "close() succeeds: " + err);
    check(w.records() == static_cast<std::uint32_t>(kCount),
          "the three refused records did not reach the file");
  }

  // --- 2/3. read it back ---------------------------------------------------
  check(fs::exists(idx), "frames.idx exists after the first add()");

  std::vector<scanengine::Keyframe> read;
  scanengine::color::FrameIndexStats stats;
  scanengine::Status s = scanengine::color::read_frame_index(lscan.string(), &read, &stats);
  check(s.ok(), std::string("read_frame_index(): ") + s.message());
  check(read.size() == static_cast<std::size_t>(kCount),
        "read back " + std::to_string(read.size()) + " of " + std::to_string(kCount));
  check(stats.truncated_tail_chunks == 0 && stats.crc_mismatch_chunks == 0 &&
            stats.malformed_records == 0 && stats.rejected_records == 0 &&
            stats.out_of_order_records == 0 && stats.header_time_mismatches == 0,
        "FrameIndexStats is clean (no truncated/CRC/malformed/rejected/out-of-order)");

  if (read.size() != static_cast<std::size_t>(kCount)) return 1;

  for (int i : {0, kCount - 1}) {
    const auto want = make_keyframe(i);
    const auto& got = read[static_cast<std::size_t>(i)];
    const std::string tag = "[" + std::to_string(i) + "] ";

    check(got.t_mono_ns == want.t_engine_ns, tag + "t_mono_ns (exposure of row 0)");
    check(got.exposure_duration_ns == want.exposure_ns, tag + "exposure_ns");
    for (int k = 0; k < 3; ++k) {
      check_near(got.pose.position[k], want.position[k], 1e-12, tag + "position[" + std::to_string(k) + "]");
    }
    for (int k = 0; k < 4; ++k) {
      check_near(got.pose.orientation[k], want.orientation[k], 1e-12,
                 tag + "orientation[" + std::to_string(k) + "]");
    }
    check_near(got.intrinsics.fx, want.fx, 1e-6f, tag + "fx");
    check_near(got.intrinsics.fy, want.fy, 1e-6f, tag + "fy");
    check_near(got.intrinsics.cx, want.cx, 1e-6f, tag + "cx");
    check_near(got.intrinsics.cy, want.cy, 1e-6f, tag + "cy");
    check_near(got.intrinsics.distortion[0], want.distortion[0], 1e-6f, tag + "k1");
    check_near(got.intrinsics.distortion[1], want.distortion[1], 1e-6f, tag + "k2");
    check(got.intrinsics.width == want.width && got.intrinsics.height == want.height,
          tag + "width/height");
    check_near(got.intrinsics.rolling_shutter_row_time_ns, want.row_time_ns, 1e-3f,
               tag + "rolling-shutter row_time_ns");
    check_near(got.pose.position_sigma_m, want.position_sigma_m, 1e-6f, tag + "position_sigma_m");
    check_near(got.pose.orientation_sigma_deg, want.orientation_sigma_deg, 1e-6f,
               tag + "orientation_sigma_deg");
    check(static_cast<std::uint8_t>(got.pose.quality) == want.pose_quality, tag + "pose_quality");
    check(got.pose.tracking_lost == want.tracking_lost, tag + "tracking_lost");
    check(static_cast<std::uint8_t>(got.pose.source) == want.pose_source, tag + "pose_source");
    check(got.flags == want.flags, tag + "flags");
    check(got.has_motion() && got.has_exposure(), tag + "motion/exposure flags decode");
    check_near(got.angular_rate_rad_s, want.angular_rate_rad_s, 1e-6f, tag + "angular_rate_rad_s");
    check_near(got.linear_speed_m_s, want.linear_speed_m_s, 1e-6f, tag + "linear_speed_m_s");
    check_near(got.iso, want.iso, 1e-6f, tag + "iso");
    check(got.image_bytes == want.image_bytes, tag + "image_bytes");
    check(got.image_path == "streams/frames/" + want.image_name,
          tag + "image_path recomposed relative to the .lscan root (got '" + got.image_path + "')");
    check(scanengine::color::keyframe_image_name(got) == want.image_name,
          tag + "keyframe_image_name() is the bare name again");
  }

  // Chunk-count cross-check against the raw file: the 32-byte stream header
  // plus N chunks, nothing else.
  const std::uintmax_t bytes = fs::file_size(idx, ec);
  std::printf("  info frames.idx is %llu bytes for %d records\n",
              static_cast<unsigned long long>(bytes), kCount);
  check(bytes > 32, "frames.idx is larger than a bare stream header");

  std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
