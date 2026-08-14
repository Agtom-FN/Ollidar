// replay_jni.cpp — JNI bindings for B4's synthetic-capture replay path
// (see replay_engine.h's header comment for the "why a second Engine"
// rationale). Bound to Kotlin's ScanEngineNative exactly like
// scanengine_jni.cpp's live-engine functions — same object, same handle
// convention (an opaque jlong pointer) — just a distinct C++ type
// underneath (`lidarscan_jni::ReplayEngine*` instead of `scan_engine*`),
// since the replay engine talks to engine/'s C++ API directly rather than
// through capi/scanengine_c.h.
//
// Surface exposed (all `external fun`s on ScanEngineNative):
//   nativeReplayCreate/Destroy, nativeReplayStart/Stop, nativeReplayIsRunning,
//   nativeReplayLastError, nativeReplayStats, nativeReplayDeviceHealth,
//   nativeReplayPageCount/PageIdAt/GetPointPage/TotalPoints.
//
// The last four mirror scanengine_jni.cpp's live nativePageCount/PageIdAt/
// GetPointPage/TotalPoints field-for-field and byte-for-byte (both hand back
// a direct ByteBuffer over 16-byte PointVertex/scan_point_vertex records),
// so PointCloudRenderer.kt (the Filament side) does not need to know which
// source it is reading from — see PointCloudSource.kt's shared interface.
#include <jni.h>

#include "jni_shared.h"
#include "replay_engine.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/types.h"

using lidarscan_jni::g_health_class;
using lidarscan_jni::g_health_ctor;
using lidarscan_jni::g_point_page_class;
using lidarscan_jni::g_point_page_ctor;
using lidarscan_jni::g_replay_stats_class;
using lidarscan_jni::g_replay_stats_ctor;
using lidarscan_jni::ReplayEngine;

namespace {
inline ReplayEngine* HandleOf(jlong handle) { return reinterpret_cast<ReplayEngine*>(handle); }
}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayCreate(JNIEnv*, jclass) {
  auto* engine = new ReplayEngine();
  if (!engine->create()) {
    delete engine;
    return 0;
  }
  return reinterpret_cast<jlong>(engine);
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayDestroy(JNIEnv*, jclass, jlong handle) {
  delete HandleOf(handle);
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayStart(JNIEnv* env, jclass, jlong handle,
                                                                    jstring lscan_dir,
                                                                    jdouble speed) {
  auto* replay = HandleOf(handle);
  if (replay == nullptr || lscan_dir == nullptr) return JNI_FALSE;
  const char* dir_utf = env->GetStringUTFChars(lscan_dir, nullptr);
  bool ok = replay->start(dir_utf, static_cast<double>(speed));
  env->ReleaseStringUTFChars(lscan_dir, dir_utf);
  return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayStop(JNIEnv*, jclass, jlong handle) {
  auto* replay = HandleOf(handle);
  if (replay != nullptr) replay->stop();
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayIsRunning(JNIEnv*, jclass,
                                                                        jlong handle) {
  auto* replay = HandleOf(handle);
  return (replay != nullptr && replay->is_running()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayLastError(JNIEnv* env, jclass,
                                                                        jlong handle) {
  auto* replay = HandleOf(handle);
  return env->NewStringUTF(replay != nullptr ? replay->last_error().c_str() : "no replay handle");
}

JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayStats(JNIEnv* env, jclass,
                                                                    jlong handle) {
  auto* replay = HandleOf(handle);
  if (replay == nullptr) return nullptr;
  const lidarscan_jni::ReplayStatsSnapshot s = replay->stats();
  return env->NewObject(g_replay_stats_class, g_replay_stats_ctor,
                         static_cast<jlong>(s.chunks_replayed), static_cast<jlong>(s.bytes_replayed),
                         static_cast<jint>(s.truncated_tail_chunks),
                         static_cast<jint>(s.crc_mismatch_chunks), s.running ? JNI_TRUE : JNI_FALSE,
                         s.done ? JNI_TRUE : JNI_FALSE, static_cast<jint>(s.result_error));
}

// Field-for-field the same conversion as scanengine_jni.cpp's live
// nativeDeviceHealth, just reading scanengine::DeviceHealth (the C++ struct)
// instead of scan_device_health (the C ABI mirror) — this replay Engine has
// no C-ABI handle to poll through.
JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayDeviceHealth(JNIEnv* env, jclass,
                                                                           jlong handle) {
  auto* replay = HandleOf(handle);
  if (replay == nullptr || replay->engine() == nullptr) return nullptr;
  auto result = replay->engine()->device_health(replay->device_id());
  if (!result.ok()) return nullptr;
  const scanengine::DeviceHealth& h = result.value();

  return env->NewObject(
      g_health_class, g_health_ctor, static_cast<jint>(h.id),
      static_cast<jint>(h.kind), static_cast<jint>(h.state), static_cast<jint>(h.last_error),
      static_cast<jlong>(h.bytes_in), static_cast<jlong>(h.packets_ok),
      static_cast<jlong>(h.packets_bad), static_cast<jlong>(h.points_out),
      static_cast<jlong>(h.drops), static_cast<jdouble>(h.points_per_sec),
      static_cast<jdouble>(h.rotation_hz), static_cast<jdouble>(h.checksum_pass_rate),
      static_cast<jlong>(h.t_last_data_ns));
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayPageCount(JNIEnv*, jclass,
                                                                        jlong handle) {
  auto* replay = HandleOf(handle);
  if (replay == nullptr || replay->engine() == nullptr) return 0;
  return static_cast<jint>(replay->engine()->points().page_count());
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayPageIdAt(JNIEnv*, jclass, jlong handle,
                                                                       jint index) {
  auto* replay = HandleOf(handle);
  if (replay == nullptr || replay->engine() == nullptr) return -1;
  const auto ids = replay->engine()->points().page_ids();
  if (index < 0 || static_cast<size_t>(index) >= ids.size()) return -1;
  return static_cast<jint>(ids[static_cast<size_t>(index)]);
}

JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayGetPointPage(JNIEnv* env, jclass,
                                                                           jlong handle,
                                                                           jint page_id) {
  auto* replay = HandleOf(handle);
  if (replay == nullptr || replay->engine() == nullptr) return nullptr;
  const scanengine::PageView view = replay->engine()->points().page_view(
      static_cast<scanengine::PageId>(page_id));
  if (!view.valid()) return nullptr;

  jobject buffer = env->NewDirectByteBuffer(
      const_cast<void*>(static_cast<const void*>(view.data)),
      static_cast<jlong>(view.count) * sizeof(scanengine::PointVertex));
  if (buffer == nullptr) return nullptr;

  return env->NewObject(g_point_page_class, g_point_page_ctor, static_cast<jint>(view.id),
                         static_cast<jint>(view.stream), static_cast<jint>(view.count),
                         static_cast<jint>(view.capacity), static_cast<jlong>(view.t_first_ns),
                         static_cast<jlong>(view.t_last_ns),
                         static_cast<jfloat>(view.bounds_min[0]),
                         static_cast<jfloat>(view.bounds_min[1]),
                         static_cast<jfloat>(view.bounds_min[2]),
                         static_cast<jfloat>(view.bounds_max[0]),
                         static_cast<jfloat>(view.bounds_max[1]),
                         static_cast<jfloat>(view.bounds_max[2]), buffer);
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeReplayTotalPoints(JNIEnv*, jclass,
                                                                          jlong handle) {
  auto* replay = HandleOf(handle);
  if (replay == nullptr || replay->engine() == nullptr) return 0;
  return static_cast<jlong>(replay->engine()->points().total_points());
}

}  // extern "C"
