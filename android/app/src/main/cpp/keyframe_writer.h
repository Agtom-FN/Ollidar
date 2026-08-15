// keyframe_writer.h — B8's `streams/frames/frames.idx` writer.
//
// WHY THIS EXISTS (and is not `scan_engine_record_keyframe`):
//
// A11 (`engine/docs/A11-color.md` §8.2) asks for a C-ABI entry point
// `scan_engine_record_keyframe(scan_engine*, const scan_keyframe*)` — "it is
// `encode_keyframe_record()` + `recorder.write_chunk()`, and it is what makes
// B8 a capture task rather than a format task". At the time B7/B8 were
// written that function did not exist in `engine/capi/scanengine_c.h`
// (SCAN_ABI_VERSION 3 — checked; the header has push_pose, the pushbroom and
// the mount-calibration solver, but nothing keyframe-shaped), and `engine/`
// is read-only to this task. So the shim links the engine's C++ API directly
// instead — exactly the pattern B4's `replay_engine.{h,cpp}` established for
// `lscan::ReplaySource`, which the C ABI also does not expose.
//
// WHICH WRITER, AND WHY IT IS NOT A SECOND `FileRecordWriter`:
//
// `color/frames_idx.h` offers two byte-identical paths:
//
//   1. `lscan::FileRecordWriter::write_chunk(kCameraFrameIndex, ...)` — what
//      A11 recommends "because it is already open". It is NOT already open to
//      us: the session's recorder lives inside the opaque `scan_engine*`
//      (`EngineHandle` is a file-local detail of `capi/scanengine_c.cpp`), and
//      opening a SECOND `FileRecordWriter` on the same `.lscan` directory is
//      actively wrong — `FileRecordWriter::open()` writes `manifest.json`, and
//      that file is already owned twice over (by the engine's own recorder and
//      by :core's `FileProjectStore`, which uses a different schema).
//
//   2. `color::KeyframeIndexWriter` — the standalone writer A11 ships "for the
//      tools that have no recorder", which `tests/test_color.cpp` asserts is
//      byte-for-byte identical to what the recorder produces. It creates
//      `streams/frames/` and, lazily on the first record, the index file
//      itself — and NOTHING else. That is what this wrapper uses.
//
// The two never collide: each `.lscan` stream is its own file, `frames.idx`
// is created lazily on the first record on both sides, and nothing in the
// engine publishes `StreamId::kCameraFrames` today (no camera data is pushed
// into the engine at all), so the recorder never opens that file.
//
// WHAT THIS ADDS ON TOP: nothing but marshalling. `KeyframeInput` is a flat,
// POD mirror of `color::Keyframe` + `CameraIntrinsics` shaped for a JNI call
// signature; `add()` fills the engine structs field for field and hands them
// to `KeyframeIndexWriter::add()`, which validates
// (`validate_keyframe()` — unit quaternion, positive fx/fy, principal point
// inside the image, relative image name with no `..`) and encodes
// (`encode_keyframe_record()`). An invalid record is REFUSED here, not
// silently written; the Kotlin side surfaces the failure.
//
// The JPEG file itself is written by the Kotlin side (it already holds the
// compressed bytes and a `java.io.File`) — this writer only records the
// index entry that names it. `image_bytes` is the size the Kotlin side
// observed on disk, which is exactly the cheap integrity check
// `frames_idx.h` documents that field for.
//
// THREADING: not internally synchronized (same contract as every engine
// module — DESIGN.md §2). One instance is pushed from one thread at a time;
// B8's `KeyframeRecorder` owns exactly one and drives it from its own
// single-threaded encoder dispatcher.
//
// Owner: android/ (B8).
#ifndef LIDARSCAN_KEYFRAME_WRITER_H
#define LIDARSCAN_KEYFRAME_WRITER_H

#include <cstdint>
#include <string>

#include "scanengine/color/frames_idx.h"

namespace lidarscan_jni {

// Flat mirror of `color::Keyframe` + `CameraIntrinsics`, in the field order
// of `frames.idx`'s 160-byte record (A11-color.md §3.2) so the two can be
// diffed by eye. Every field is documented there; the notes below are only
// the ones with an Android-specific origin.
struct KeyframeInput {
  // ARCore `Frame.getTimestamp()`, which is CLOCK_BOOTTIME — the engine's own
  // domain (A4 installs a passthrough estimator for kPoseAr, A8 §3.5) — and,
  // per Camera2's SENSOR_TIMESTAMP contract, the start of exposure of ROW 0,
  // which is exactly what A11-color.md §3.3 item 1 requires.
  std::int64_t t_engine_ns = 0;
  std::int64_t exposure_ns = 0;  // ImageMetadata.SENSOR_EXPOSURE_TIME

  double position[3] = {0, 0, 0};
  double orientation[4] = {0, 0, 0, 1};  // x, y, z, w — ARCore's own order

  float fx = 0, fy = 0, cx = 0, cy = 0;
  // ARCore's CameraIntrinsics exposes NO distortion (it hands out the
  // intrinsics of an already-rectified image); left all-zero unless the
  // caller pulled ImageMetadata.LENS_RADIAL_DISTORTION. See
  // ArCameraIntrinsics.kt.
  float distortion[5] = {0, 0, 0, 0, 0};
  std::uint32_t width = 0, height = 0;
  float row_time_ns = 0.f;  // 0 = global shutter (A11's convention)

  float position_sigma_m = 0.f;
  float orientation_sigma_deg = 0.f;
  std::uint8_t pose_quality = 0;  // SCAN_POSE_QUALITY_* / PoseQuality
  std::uint8_t tracking_lost = 0;
  std::uint8_t pose_source = 0;  // StreamId (kPoseAr == 4)

  std::uint32_t flags = 0;  // kKeyframeFlag*
  float iso = 0.f;
  float angular_rate_rad_s = 0.f;
  float linear_speed_m_s = 0.f;

  std::uint32_t image_bytes = 0;
  // Relative to streams/frames/, forward slashes, no "..", not absolute —
  // validate_keyframe() enforces all three (the same zip-slip class A5's
  // zip_import() defends against).
  std::string image_name;
};

class KeyframeWriter {
 public:
  KeyframeWriter() = default;

  // Creates `<lscan_dir>/streams/frames/`. The index FILE is created lazily,
  // on the first add() — so a session that captured no keyframes leaves no
  // frames.idx behind and `read_frame_index()` can return kNotFound and mean
  // it ("colorization gracefully unavailable", Tech Spec §3.5).
  bool open(const std::string& lscan_dir, std::string* error);
  bool add(const KeyframeInput& in, std::string* error);
  bool flush(std::string* error);
  bool close(std::string* error);

  bool is_open() const { return writer_.is_open(); }
  std::uint32_t records() const { return writer_.records(); }

 private:
  scanengine::color::KeyframeIndexWriter writer_;
};

}  // namespace lidarscan_jni

#endif  // LIDARSCAN_KEYFRAME_WRITER_H
