// mid360_jni.cpp — B3's JNI surface for the Livox Mid-360.
//
// Three groups of entry points, and the split between them is the whole
// design of this task's native side:
//
//  1. `nativeAddMid360Device` — the CAPTURE path, over the C ABI
//     (`scan_engine_add_device` with kind = SCAN_DEVICE_MID360). This adds
//     the device to the SAME `scan_engine*` B2's RealEngineBridge already
//     owns, which is what puts Mid-360 points into the session's PageStore,
//     the session's `.lscan` recorder and live SLAM. It carries exactly what
//     `scan_device_config` carries — `lidar_ip` and `host_ip`, nothing else.
//
//  2. `nativeMid360Probe*` — the WIZARD path, over mid360_probe.h's
//     standalone C++ Engine. This is the only way to reach `Mid360Config`'s
//     backend selector, its ports and `UdpConfig::prebound_fd`, none of
//     which `scan_device_config` exposes. See mid360_probe.h for the full
//     statement of that gap.
//
//  3. `nativeSetTempDir` — an Android runtime fix, not a feature. See below.
//
// THE TMPDIR PROBLEM (found by reading the SDK2 backend against bionic, and
// the reason group 3 exists). `LivoxLidarSdkInit()` takes a config-file
// PATH, so `mid360_sdk2.cpp`'s `write_config()` synthesises one:
//
//     std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
//     if (ec) dir = std::filesystem::path(".");
//
// On Android neither branch is writable. libc++'s `temp_directory_path()`
// consults TMPDIR/TMP/TEMP/TEMPDIR and falls back to `/tmp`, and an Android
// device has none of those set and no `/tmp` directory at all — so `ec` is
// set — and the fallback `"."` is the process CWD, which for an app is `/`,
// also not writable. The generated SDK config would fail to write and the
// Mid-360 would never start, with `kFileError` and a path that makes no
// sense to anyone reading the log.
//
// The fix is one `setenv("TMPDIR", …)` with the app's own `cacheDir`, called
// once before any Mid-360 device is added. It is deliberately done HERE
// rather than by adding `sdk_config_path` to the C ABI: it needs no ABI
// change, it fixes every other `temp_directory_path()` caller in the engine
// at the same time, and `setenv` is the mechanism libc++ documents for this.
// The engine-side alternative (exposing `Mid360Config::sdk_config_path`
// through `scan_device_config`) is written up in android/NOTES.md as the
// cleaner long-term fix.
#include <jni.h>
#include <stdlib.h>

#include <cstring>
#include <string>

#include <android/log.h>

#include "jni_shared.h"
#include "mid360_probe.h"
#include "scanengine_c.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "mid360_jni", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "mid360_jni", __VA_ARGS__)

using lidarscan_jni::g_mid360_probe_class;
using lidarscan_jni::g_mid360_probe_ctor;
using lidarscan_jni::Mid360Probe;
using lidarscan_jni::Mid360ProbeSnapshot;

namespace {

// Small RAII for a jstring -> UTF-8 std::string, mirroring the helper
// scanengine_jni.cpp already uses. Returns an empty string for a null
// jstring, which is what the "field not set" case marshals to.
std::string to_utf8(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  if (chars == nullptr) return {};
  std::string out(chars);
  env->ReleaseStringUTFChars(s, chars);
  return out;
}

}  // namespace

extern "C" {

// --- group 3: the TMPDIR fix -------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeSetTempDir(JNIEnv* env, jobject /*thiz*/,
                                                                jstring path) {
  const std::string dir = to_utf8(env, path);
  if (dir.empty()) return JNI_FALSE;
  // overwrite = 1: idempotent, and a second call with a different cacheDir
  // (there is only one, but a test harness could) must win rather than be
  // silently ignored.
  if (::setenv("TMPDIR", dir.c_str(), 1) != 0) {
    LOGE("setenv(TMPDIR, %s) failed", dir.c_str());
    return JNI_FALSE;
  }
  LOGI("TMPDIR set to %s (SDK2 writes its generated config there)", dir.c_str());
  return JNI_TRUE;
}

// --- group 1: the capture path, over the C ABI -------------------------------

// Returns the device id (>= 0) or -1; the caller reads nativeLastError().
// Mirrors nativeAddD6Device's contract exactly.
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeAddMid360Device(JNIEnv* env, jobject /*thiz*/,
                                                                     jlong handle, jstring lidar_ip,
                                                                     jstring host_ip) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  if (engine == nullptr) return -1;

  const std::string lidar = to_utf8(env, lidar_ip);
  const std::string host = to_utf8(env, host_ip);
  // Both are REQUIRED (A3 §3, "Explicit IP is mandatory, everywhere"). The
  // driver rejects empties too, but failing here keeps the message in the
  // wizard's vocabulary and avoids creating a device the caller then has to
  // remove.
  if (lidar.empty() || host.empty()) return -1;

  scan_device_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.kind = SCAN_DEVICE_MID360;
  cfg.lidar_ip = lidar.c_str();
  cfg.host_ip = host.c_str();

  uint32_t device_id = 0;
  const scan_error_t err = scan_engine_add_device(engine, &cfg, &device_id);
  if (err != SCAN_OK) {
    LOGE("scan_engine_add_device(Mid-360 %s -> host %s) failed: %s (%s)", lidar.c_str(),
         host.c_str(), scan_error_str(err), scan_engine_last_error());
    return -1;
  }
  LOGI("Mid-360 %s -> host %s added as device %u", lidar.c_str(), host.c_str(), device_id);
  return static_cast<jint>(device_id);
}

// --- group 2: the wizard's probe ---------------------------------------------

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMid360ProbeCreate(JNIEnv* /*env*/,
                                                                       jobject /*thiz*/) {
  return reinterpret_cast<jlong>(new Mid360Probe());
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMid360ProbeDestroy(JNIEnv* /*env*/,
                                                                         jobject /*thiz*/,
                                                                         jlong handle) {
  auto* probe = reinterpret_cast<Mid360Probe*>(handle);
  delete probe;  // ~Mid360Probe stops the session and closes any owned fd
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMid360ProbeStart(
    JNIEnv* env, jobject /*thiz*/, jlong handle, jstring lidar_ip, jstring host_ip, jint backend,
    jint device_point_port, jint device_imu_port, jint device_cmd_port, jint host_point_port,
    jint host_imu_port, jint host_cmd_port, jint prebound_point_fd, jboolean publish_imu) {
  auto* probe = reinterpret_cast<Mid360Probe*>(handle);
  if (probe == nullptr) return JNI_FALSE;

  Mid360Probe::Params p;
  p.lidar_ip = to_utf8(env, lidar_ip);
  p.host_ip = to_utf8(env, host_ip);
  p.backend = backend;
  p.device_point_port = static_cast<std::uint16_t>(device_point_port);
  p.device_imu_port = static_cast<std::uint16_t>(device_imu_port);
  p.device_cmd_port = static_cast<std::uint16_t>(device_cmd_port);
  p.host_point_port = static_cast<std::uint16_t>(host_point_port);
  p.host_imu_port = static_cast<std::uint16_t>(host_imu_port);
  p.host_cmd_port = static_cast<std::uint16_t>(host_cmd_port);
  p.prebound_point_fd = prebound_point_fd;
  p.publish_imu = (publish_imu == JNI_TRUE);
  return probe->start(p) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMid360ProbeStop(JNIEnv* /*env*/,
                                                                      jobject /*thiz*/,
                                                                      jlong handle) {
  auto* probe = reinterpret_cast<Mid360Probe*>(handle);
  if (probe != nullptr) probe->stop();
}

JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMid360ProbeLastError(JNIEnv* env,
                                                                          jobject /*thiz*/,
                                                                          jlong handle) {
  auto* probe = reinterpret_cast<Mid360Probe*>(handle);
  return env->NewStringUTF(probe == nullptr ? "" : probe->last_error().c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMid360Sdk2Active(JNIEnv* /*env*/,
                                                                       jobject /*thiz*/) {
  return Mid360Probe::is_sdk2_active() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeMid360ProbeSnapshot(JNIEnv* env,
                                                                          jobject /*thiz*/,
                                                                          jlong handle) {
  auto* probe = reinterpret_cast<Mid360Probe*>(handle);
  if (probe == nullptr) return nullptr;
  const Mid360ProbeSnapshot s = probe->snapshot();

  // Field order MUST match NativeMid360Probe.kt's constructor exactly; the
  // descriptor cached in JNI_OnLoad is "(IIJJJJJDDDJJJJJJIJZI)V".
  return env->NewObject(
      g_mid360_probe_class, g_mid360_probe_ctor,
      static_cast<jint>(s.device_state), static_cast<jint>(s.last_error),
      static_cast<jlong>(s.packets_ok), static_cast<jlong>(s.packets_bad),
      static_cast<jlong>(s.points_out), static_cast<jlong>(s.drops),
      static_cast<jlong>(s.bytes_in), static_cast<jdouble>(s.points_per_sec),
      static_cast<jdouble>(s.imu_hz), static_cast<jdouble>(s.loss_pct),
      static_cast<jlong>(s.t_last_data_ns), static_cast<jlong>(s.datagrams_point),
      static_cast<jlong>(s.datagrams_imu), static_cast<jlong>(s.datagram_bytes),
      static_cast<jlong>(s.t_first_datagram_ns), static_cast<jlong>(s.t_last_datagram_ns),
      static_cast<jint>(s.link_state), static_cast<jlong>(s.elapsed_since_start_ns),
      s.running ? JNI_TRUE : JNI_FALSE, static_cast<jint>(s.backend));
}

}  // extern "C"
