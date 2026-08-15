// arcore_jni.cpp — B7/B8's JNI surface: the ARCore pose stream, the D6
// pushbroom, the mount-calibration solver, and the `frames.idx` keyframe
// writer.
//
// Split out of scanengine_jni.cpp (B2/B4) purely for file size; it is the
// same .so, the same conventions, and it reuses the same JNI_OnLoad-cached
// classes via jni_shared.h. Everything here except the keyframe writer is a
// thin transliteration of `engine/capi/scanengine_c.h` at SCAN_ABI_VERSION 3
// — the version this task is pinned against, checked at runtime in
// scanengine_jni.cpp's nativeCreateEngine.
//
// Three groups:
//
//  1. POSES IN (A8 §3, capi "poses in"): nativePushPose. Called once per
//     ARCore frame from the AR thread, concurrently with the D6 reader
//     thread pushing serial bytes — safe, per scanengine_c.h's own note
//     ("Safe from the AR thread while points are decoded on another") and
//     A8 §3.8 (ExternalPoseSource *is* thread-safe; the assembler is the
//     part that is not, and the app never touches it directly).
//
//  2. PUSHBROOM + MOUNT CALIBRATION (A8 §4): set_mount_extrinsics /
//     pushbroom_enable / flush / stats, and the standalone
//     scan_mount_calib_* solver handle the wizard drives. The solver needs
//     NO engine and no session — the observations come from the app's own
//     checkerboard detection (see com.lidarscan.core.calib), which is why
//     A8 put it behind its own handle.
//
//  3. KEYFRAMES (A11 §3): the frames.idx writer. See keyframe_writer.h for
//     why this is a C++ helper rather than a `scan_engine_record_keyframe`
//     C-ABI call — in short, that entry point does not exist at ABI 3 and
//     engine/ is read-only here.
//
// MATRIX CONVENTION, stated once because getting it wrong is the documented
// field failure: every double[16] crossing this boundary is ROW-MAJOR.
// scan_engine_set_mount_extrinsics rejects a column-major matrix outright
// (SCAN_ERR_INVALID_ARGUMENT) rather than produce "a plausible-looking
// mirrored cloud nobody notices until export" (scanengine_c.h). The Kotlin
// side builds row-major throughout (com.lidarscan.core.calib.Mat4).

#include <jni.h>
#include <android/log.h>

#include <cstring>
#include <string>
#include <vector>

#include "jni_shared.h"
#include "keyframe_writer.h"
#include "scanengine_c.h"

#define LOG_TAG "arcore_jni"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using lidarscan_jni::g_mount_calib_result_class;
using lidarscan_jni::g_mount_calib_result_ctor;
using lidarscan_jni::g_pushbroom_stats_class;
using lidarscan_jni::g_pushbroom_stats_ctor;

namespace {

// Copies a jdoubleArray of exactly 16 into a row-major double[16]. Returns
// false (and leaves `out` untouched) on a wrong length, so a Kotlin-side
// mistake surfaces as SCAN_ERR_INVALID_ARGUMENT instead of reading past the
// end of the array.
bool copy_mat4(JNIEnv* env, jdoubleArray in, double out[16]) {
  if (in == nullptr || env->GetArrayLength(in) != 16) return false;
  env->GetDoubleArrayRegion(in, 0, 16, out);
  return true;
}

}  // namespace

extern "C" {

// --- 1. poses in -----------------------------------------------------------

// One ARCore frame's pose. `confidence` < 0 means "derive it from
// quality/tracking_lost" (scanengine_c.h) — which is what the Kotlin side
// passes, because ARCore reports a tracking STATE and a failure REASON, not
// a scalar confidence, and inventing one here would be fabricating a number
// the engine already knows how to derive.
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePushPose(
    JNIEnv* env, jclass, jlong handle, jlong t_mono_ns, jdouble px, jdouble py, jdouble pz,
    jdouble qx, jdouble qy, jdouble qz, jdouble qw, jfloat position_sigma_m,
    jfloat orientation_sigma_deg, jint quality, jboolean tracking_lost, jfloat confidence) {
  (void)env;
  auto* engine = reinterpret_cast<scan_engine*>(handle);

  scan_pose pose{};
  pose.t_mono_ns = static_cast<int64_t>(t_mono_ns);
  pose.position[0] = px;
  pose.position[1] = py;
  pose.position[2] = pz;
  pose.orientation[0] = qx;
  pose.orientation[1] = qy;
  pose.orientation[2] = qz;
  pose.orientation[3] = qw;
  pose.position_sigma_m = position_sigma_m;
  pose.orientation_sigma_deg = orientation_sigma_deg;
  pose.source = SCAN_STREAM_POSE_AR;
  pose.quality = static_cast<uint8_t>(quality);
  pose.tracking_lost = tracking_lost ? 1 : 0;

  return static_cast<jint>(scan_engine_push_pose(engine, &pose, confidence));
}

// Interpolated lookup. Returns the GATE (SCAN_POSE_GATE_*), which is always
// written even on failure — the five-outcome distinction §3.3 asks for is
// exactly what a caller wants here, so it is the return value rather than
// the error code. -1 means "the call itself failed" (bad handle).
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePoseGateAt(JNIEnv*, jclass, jlong handle,
                                                                  jlong t_mono_ns) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  uint8_t gate = 0;
  scan_error_t err = scan_engine_pose_at(engine, static_cast<int64_t>(t_mono_ns), nullptr, &gate);
  if (err != SCAN_OK && err != SCAN_ERR_AGAIN && err != SCAN_ERR_NOT_FOUND) return -1;
  return static_cast<jint>(gate);
}

// --- 2. pushbroom + mount extrinsics ---------------------------------------

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeSetMountExtrinsics(JNIEnv* env, jclass,
                                                                          jlong handle,
                                                                          jdoubleArray m) {
  double phone_from_lidar[16];
  if (!copy_mat4(env, m, phone_from_lidar)) return SCAN_ERR_INVALID_ARGUMENT;
  return static_cast<jint>(
      scan_engine_set_mount_extrinsics(reinterpret_cast<scan_engine*>(handle), phone_from_lidar));
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePushbroomEnable(JNIEnv*, jclass, jlong handle,
                                                                       jboolean on) {
  return static_cast<jint>(
      scan_engine_pushbroom_enable(reinterpret_cast<scan_engine*>(handle), on ? 1 : 0));
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePushbroomFlush(JNIEnv*, jclass, jlong handle) {
  return static_cast<jint>(scan_engine_pushbroom_flush(reinterpret_cast<scan_engine*>(handle)));
}

JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePushbroomStats(JNIEnv* env, jclass,
                                                                      jlong handle) {
  scan_pushbroom_stats s{};
  if (scan_engine_pushbroom_stats(reinterpret_cast<scan_engine*>(handle), &s) != SCAN_OK) {
    return nullptr;
  }
  return env->NewObject(
      g_pushbroom_stats_class, g_pushbroom_stats_ctor, static_cast<jlong>(s.points_in),
      static_cast<jlong>(s.points_out), static_cast<jlong>(s.points_pending),
      static_cast<jlong>(s.dropped_range), static_cast<jlong>(s.dropped_no_pose),
      static_cast<jlong>(s.dropped_overflow), static_cast<jlong>(s.dropped_page_full),
      static_cast<jlong>(s.flagged_tracking_lost), static_cast<jlong>(s.flagged_stale_pose),
      static_cast<jlong>(s.flagged_low_confidence), static_cast<jlong>(s.flagged_emitted),
      static_cast<jlong>(s.t_first_ns), static_cast<jlong>(s.t_last_ns));
}

// --- the mount-calibration solver handle -----------------------------------

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMountCalibCreate(JNIEnv*, jclass) {
  scan_mount_calib* calib = nullptr;
  if (scan_mount_calib_create(&calib) != SCAN_OK) {
    LOGE("scan_mount_calib_create failed: %s", scan_engine_last_error());
    return 0;
  }
  return reinterpret_cast<jlong>(calib);
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMountCalibDestroy(JNIEnv*, jclass,
                                                                         jlong handle) {
  scan_mount_calib_destroy(reinterpret_cast<scan_mount_calib*>(handle));
}

// One wizard pose. `normal`/`d` are the board plane AS THE CAMERA MEASURED IT
// (A8 §4.1), in the camera frame, from the app's checkerboard homography;
// `points` are the lidar returns segmented onto the board, in the SENSOR
// frame, marshalled as a flat float[3n] (x,y,z per point) rather than as
// scan_point_vertex[] — the colour bytes of a PointVertex mean nothing to
// the residual, and a float[] is the cheapest thing to build on the Kotlin
// side from a page read.
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMountCalibAddObservation(
    JNIEnv* env, jclass, jlong handle, jdouble nx, jdouble ny, jdouble nz, jdouble d,
    jfloatArray xyz, jdouble sigma_m) {
  auto* calib = reinterpret_cast<scan_mount_calib*>(handle);
  if (calib == nullptr || xyz == nullptr) return SCAN_ERR_INVALID_ARGUMENT;

  const jsize len = env->GetArrayLength(xyz);
  if (len <= 0 || (len % 3) != 0) return SCAN_ERR_INVALID_ARGUMENT;
  const uint32_t n = static_cast<uint32_t>(len / 3);

  jfloat* raw = env->GetFloatArrayElements(xyz, nullptr);
  if (raw == nullptr) return SCAN_ERR_OUT_OF_MEMORY;

  // scan_point_vertex is the 16-byte GPU layout (x,y,z + rgba); the solver
  // reads only the position, but the ABI takes the whole struct, so this
  // builds them here. Colour is left at 0 deliberately — it is never read,
  // and inventing a value would suggest otherwise.
  std::vector<scan_point_vertex> pts(n);
  for (uint32_t i = 0; i < n; ++i) {
    pts[i].x = raw[3 * i + 0];
    pts[i].y = raw[3 * i + 1];
    pts[i].z = raw[3 * i + 2];
    pts[i].r = 0;
    pts[i].g = 0;
    pts[i].b = 0;
    pts[i].a = 0;
  }
  env->ReleaseFloatArrayElements(xyz, raw, JNI_ABORT);

  const double normal[3] = {nx, ny, nz};
  return static_cast<jint>(
      scan_mount_calib_add_observation(calib, normal, d, pts.data(), n, sigma_m));
}

// Solves from the bracket's CAD nominal (row-major phone_from_lidar). Returns
// the full scan_mount_calib_result, or null on failure — including the
// documented refusals (< 3 observations is kInvalidArgument, "undetermined,
// not merely ill-conditioned").
JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMountCalibSolve(JNIEnv* env, jclass,
                                                                       jlong handle,
                                                                       jdoubleArray cad) {
  auto* calib = reinterpret_cast<scan_mount_calib*>(handle);
  double cad_nominal[16];
  if (calib == nullptr || !copy_mat4(env, cad, cad_nominal)) return nullptr;

  scan_mount_calib_result r{};
  scan_error_t err = scan_mount_calib_solve(calib, cad_nominal, &r);
  if (err != SCAN_OK) {
    LOGE("scan_mount_calib_solve failed: %d (%s)", err, scan_engine_last_error());
    return nullptr;
  }

  jdoubleArray m = env->NewDoubleArray(16);
  if (m == nullptr) return nullptr;
  env->SetDoubleArrayRegion(m, 0, 16, r.camera_from_lidar);

  return env->NewObject(g_mount_calib_result_class, g_mount_calib_result_ctor, m,
                        r.converged != 0 ? JNI_TRUE : JNI_FALSE,
                        r.degenerate != 0 ? JNI_TRUE : JNI_FALSE,
                        static_cast<jint>(r.iterations_l2), static_cast<jint>(r.iterations_robust),
                        static_cast<jlong>(r.observations), static_cast<jlong>(r.residuals),
                        static_cast<jdouble>(r.rms_residual_m), static_cast<jdouble>(r.final_cost),
                        static_cast<jdouble>(r.split_half_px), static_cast<jdouble>(r.gate_range_m),
                        static_cast<jint>(r.gate), static_cast<jdouble>(r.sigma_rot_deg),
                        static_cast<jdouble>(r.sigma_trans_mm),
                        static_cast<jdouble>(r.condition_number));
}

// --- 3. keyframes (frames.idx) ---------------------------------------------

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeKeyframeWriterOpen(JNIEnv* env, jclass,
                                                                          jstring lscan_dir) {
  if (lscan_dir == nullptr) return 0;
  const char* dir = env->GetStringUTFChars(lscan_dir, nullptr);
  auto* w = new lidarscan_jni::KeyframeWriter();
  std::string error;
  bool ok = w->open(dir, &error);
  env->ReleaseStringUTFChars(lscan_dir, dir);
  if (!ok) {
    LOGE("keyframe writer open failed: %s", error.c_str());
    delete w;
    return 0;
  }
  return reinterpret_cast<jlong>(w);
}

// Returns an error string, or null on success. A string (not an error code)
// because every failure here is a VALIDATION failure whose useful content is
// the field name validate_keyframe() put in last_error() — collapsing that to
// SCAN_ERR_INVALID_ARGUMENT would throw away the only part worth logging.
JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeKeyframeWriterAdd(
    JNIEnv* env, jclass, jlong handle, jlong t_engine_ns, jlong exposure_ns, jdouble px, jdouble py,
    jdouble pz, jdouble qx, jdouble qy, jdouble qz, jdouble qw, jfloat fx, jfloat fy, jfloat cx,
    jfloat cy, jfloatArray distortion, jint width, jint height, jfloat row_time_ns,
    jfloat position_sigma_m, jfloat orientation_sigma_deg, jint pose_quality,
    jboolean tracking_lost, jint pose_source, jint flags, jfloat iso, jfloat angular_rate_rad_s,
    jfloat linear_speed_m_s, jint image_bytes, jstring image_name) {
  auto* w = reinterpret_cast<lidarscan_jni::KeyframeWriter*>(handle);
  if (w == nullptr) return env->NewStringUTF("keyframe writer is not open");

  lidarscan_jni::KeyframeInput in;
  in.t_engine_ns = t_engine_ns;
  in.exposure_ns = exposure_ns;
  in.position[0] = px;
  in.position[1] = py;
  in.position[2] = pz;
  in.orientation[0] = qx;
  in.orientation[1] = qy;
  in.orientation[2] = qz;
  in.orientation[3] = qw;
  in.fx = fx;
  in.fy = fy;
  in.cx = cx;
  in.cy = cy;
  if (distortion != nullptr && env->GetArrayLength(distortion) == 5) {
    env->GetFloatArrayRegion(distortion, 0, 5, in.distortion);
  }
  in.width = static_cast<uint32_t>(width);
  in.height = static_cast<uint32_t>(height);
  in.row_time_ns = row_time_ns;
  in.position_sigma_m = position_sigma_m;
  in.orientation_sigma_deg = orientation_sigma_deg;
  in.pose_quality = static_cast<uint8_t>(pose_quality);
  in.tracking_lost = tracking_lost ? 1 : 0;
  in.pose_source = static_cast<uint8_t>(pose_source);
  in.flags = static_cast<uint32_t>(flags);
  in.iso = iso;
  in.angular_rate_rad_s = angular_rate_rad_s;
  in.linear_speed_m_s = linear_speed_m_s;
  in.image_bytes = static_cast<uint32_t>(image_bytes);

  if (image_name != nullptr) {
    const char* name = env->GetStringUTFChars(image_name, nullptr);
    in.image_name = name;
    env->ReleaseStringUTFChars(image_name, name);
  }

  std::string error;
  if (!w->add(in, &error)) return env->NewStringUTF(error.c_str());
  return nullptr;
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeKeyframeWriterRecords(JNIEnv*, jclass,
                                                                             jlong handle) {
  auto* w = reinterpret_cast<lidarscan_jni::KeyframeWriter*>(handle);
  return w == nullptr ? 0 : static_cast<jint>(w->records());
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeKeyframeWriterFlush(JNIEnv*, jclass,
                                                                           jlong handle) {
  auto* w = reinterpret_cast<lidarscan_jni::KeyframeWriter*>(handle);
  if (w == nullptr) return;
  std::string error;
  if (!w->flush(&error)) LOGE("keyframe writer flush failed: %s", error.c_str());
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeKeyframeWriterClose(JNIEnv*, jclass,
                                                                           jlong handle) {
  auto* w = reinterpret_cast<lidarscan_jni::KeyframeWriter*>(handle);
  if (w == nullptr) return;
  std::string error;
  if (!w->close(&error)) LOGE("keyframe writer close failed: %s", error.c_str());
  delete w;
}

}  // extern "C"
