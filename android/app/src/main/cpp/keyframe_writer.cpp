#include "keyframe_writer.h"

#include <cstdio>

#include "scanengine/color/colorize.h"
#include "scanengine/core/error.h"

namespace lidarscan_jni {
namespace {

using scanengine::Keyframe;
using scanengine::Status;

// Composes the detail message the same way every engine-adjacent call site
// here does: the enum's stable string plus the thread-local detail A5/A11 set
// (`last_error_message()` names the offending FIELD for a validation
// failure, which is the whole reason to surface it rather than just "invalid
// argument").
void describe(const char* what, Status s, std::string* error) {
  if (error == nullptr) return;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%s: %s (%s)", what, s.message(),
                scanengine::last_error_message());
  *error = buf;
}

}  // namespace

bool KeyframeWriter::open(const std::string& lscan_dir, std::string* error) {
  Status s = writer_.open(lscan_dir);
  if (!s.ok()) {
    describe("KeyframeIndexWriter::open", s, error);
    return false;
  }
  return true;
}

bool KeyframeWriter::add(const KeyframeInput& in, std::string* error) {
  Keyframe kf;
  kf.t_mono_ns = in.t_engine_ns;
  // `Keyframe::image_path` is relative to the .lscan ROOT; the RECORD stores
  // it relative to streams/frames/ and frames_idx.cpp does that decomposition
  // itself (color/frames_idx.h, "composed on read and decomposed on write").
  // So the path handed in here carries the prefix and the 160-byte record
  // will not.
  kf.image_path = "streams/frames/" + in.image_name;

  kf.pose.t_mono_ns = in.t_engine_ns;
  kf.pose.position[0] = in.position[0];
  kf.pose.position[1] = in.position[1];
  kf.pose.position[2] = in.position[2];
  kf.pose.orientation[0] = in.orientation[0];
  kf.pose.orientation[1] = in.orientation[1];
  kf.pose.orientation[2] = in.orientation[2];
  kf.pose.orientation[3] = in.orientation[3];
  kf.pose.position_sigma_m = in.position_sigma_m;
  kf.pose.orientation_sigma_deg = in.orientation_sigma_deg;
  kf.pose.source = static_cast<scanengine::StreamId>(in.pose_source);
  kf.pose.quality = static_cast<scanengine::PoseQuality>(in.pose_quality);
  kf.pose.tracking_lost = in.tracking_lost;

  kf.intrinsics.fx = in.fx;
  kf.intrinsics.fy = in.fy;
  kf.intrinsics.cx = in.cx;
  kf.intrinsics.cy = in.cy;
  for (int i = 0; i < 5; ++i) kf.intrinsics.distortion[i] = in.distortion[i];
  kf.intrinsics.width = in.width;
  kf.intrinsics.height = in.height;
  kf.intrinsics.rolling_shutter_row_time_ns = in.row_time_ns;

  kf.flags = in.flags;
  kf.exposure_duration_ns = in.exposure_ns;
  kf.iso = in.iso;
  kf.angular_rate_rad_s = in.angular_rate_rad_s;
  kf.linear_speed_m_s = in.linear_speed_m_s;
  kf.image_bytes = in.image_bytes;

  Status s = writer_.add(kf);
  if (!s.ok()) {
    describe("KeyframeIndexWriter::add", s, error);
    return false;
  }
  return true;
}

bool KeyframeWriter::flush(std::string* error) {
  Status s = writer_.flush();
  if (!s.ok()) {
    describe("KeyframeIndexWriter::flush", s, error);
    return false;
  }
  return true;
}

bool KeyframeWriter::close(std::string* error) {
  Status s = writer_.close();
  if (!s.ok()) {
    describe("KeyframeIndexWriter::close", s, error);
    return false;
  }
  return true;
}

}  // namespace lidarscan_jni
