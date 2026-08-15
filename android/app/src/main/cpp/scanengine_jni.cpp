// scanengine_jni.cpp — B2's JNI shim over engine/capi/scanengine_c.h.
//
// Binds the flat C ABI (NOT the C++ headers — see scanengine_c.h's own
// header comment: "WHO USES THIS: Android/JNI"), pinned against
// SCAN_ABI_VERSION as observed when B2 started (checked once in
// nativeCreateEngine below; see the ABI-version comment there for what
// happens if it drifts).
//
// Surface exposed to Kotlin (com.lidarscan.app.engine.ScanEngineNative):
//   - engine create/destroy, session start/stop/state
//   - add/remove D6 device (serial_write routed back into a Kotlin
//     SerialWriter so usb-serial-for-android does the actual USB I/O)
//   - push_serial_bytes, zero-copy via a direct ByteBuffer
//     (GetDirectBufferAddress hands the engine a raw pointer straight into
//     the JVM-owned direct buffer — no JNI array pinning/copying)
//   - device health polling (scan_engine_device_health -> NativeDeviceHealth)
//   - a dedicated event-pump thread (scan_engine_wait_event in a loop,
//     JNI-attached once for the thread's lifetime) delivering events to a
//     Kotlin EngineEventListener
//   - last_error / error_str passthrough
//
// THREADING (mirrors scanengine_c.h's own contract): every scan_engine_*
// call here is safe from any thread. The one thread *we* create (the event
// pump) attaches to the JVM once at start and detaches once at stop, per
// the header's instruction not to touch the JVM without attaching first.
// serial_write callbacks arrive synchronously on whatever thread called
// into the engine (per A2's docs/A2-d6-driver.md, the engine owns no
// threads of its own in A1/A2) — never on the event-pump thread — so they
// reuse whichever JNIEnv is already attached to that calling thread,
// attaching it if this is the first native call on it.

#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "scanengine_c.h"
#include "jni_shared.h"

#define LOG_TAG "scanengine_jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// --- symbols shared with replay_jni.cpp (see jni_shared.h) -----------------
// Defined here (not in an anonymous namespace) because JNI_OnLoad, the only
// place that resolves them, lives in this file — replay_jni.cpp only reads
// them through jni_shared.h's extern declarations.
namespace lidarscan_jni {
JavaVM* g_jvm = nullptr;
jclass g_health_class = nullptr;
jmethodID g_health_ctor = nullptr;
jclass g_point_page_class = nullptr;
jmethodID g_point_page_ctor = nullptr;
jclass g_replay_stats_class = nullptr;
jmethodID g_replay_stats_ctor = nullptr;
jclass g_mount_calib_result_class = nullptr;
jmethodID g_mount_calib_result_ctor = nullptr;
jclass g_pushbroom_stats_class = nullptr;
jmethodID g_pushbroom_stats_ctor = nullptr;
jclass g_mid360_probe_class = nullptr;
jmethodID g_mid360_probe_ctor = nullptr;

JNIEnv* AttachCurrentThreadOrGet(bool* did_attach) {
  JNIEnv* env = nullptr;
  jint rc = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (rc == JNI_OK) {
    if (did_attach) *did_attach = false;
    return env;
  }
  if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
    __android_log_print(ANDROID_LOG_ERROR, "scanengine_jni", "AttachCurrentThread failed");
    return nullptr;
  }
  if (did_attach) *did_attach = true;
  return env;
}
}  // namespace lidarscan_jni

using lidarscan_jni::AttachCurrentThreadOrGet;
using lidarscan_jni::g_health_class;
using lidarscan_jni::g_health_ctor;
using lidarscan_jni::g_jvm;
using lidarscan_jni::g_point_page_class;
using lidarscan_jni::g_point_page_ctor;
using lidarscan_jni::g_mid360_probe_class;
using lidarscan_jni::g_mid360_probe_ctor;
using lidarscan_jni::g_mount_calib_result_class;
using lidarscan_jni::g_mount_calib_result_ctor;
using lidarscan_jni::g_pushbroom_stats_class;
using lidarscan_jni::g_pushbroom_stats_ctor;
using lidarscan_jni::g_replay_stats_class;
using lidarscan_jni::g_replay_stats_ctor;

namespace {

jclass g_serial_writer_class = nullptr;
jmethodID g_serial_writer_write = nullptr;
jclass g_event_listener_class = nullptr;
jmethodID g_event_listener_on_event = nullptr;

// --- serial write callback: JNI shim -> Kotlin SerialWriter -> USB I/O ----
struct SerialWriterCtx {
  jobject writer_global_ref;  // com.lidarscan.app.engine.ScanEngineNative.SerialWriter
};

scan_error_t serial_write_trampoline(const uint8_t* data, size_t len, void* user_data) {
  auto* ctx = static_cast<SerialWriterCtx*>(user_data);
  if (ctx == nullptr || ctx->writer_global_ref == nullptr) return SCAN_ERR_INVALID_STATE;

  bool did_attach = false;
  JNIEnv* env = AttachCurrentThreadOrGet(&did_attach);
  if (env == nullptr) return SCAN_ERR_UNKNOWN;

  jbyteArray arr = env->NewByteArray(static_cast<jsize>(len));
  if (arr == nullptr) {
    if (did_attach) g_jvm->DetachCurrentThread();
    return SCAN_ERR_OUT_OF_MEMORY;
  }
  if (len > 0) {
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(data));
  }

  jint result = env->CallIntMethod(ctx->writer_global_ref, g_serial_writer_write, arr);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    result = SCAN_ERR_IO;
  }
  env->DeleteLocalRef(arr);

  // Never detach a thread we didn't attach — it may be a long-lived thread
  // (e.g. the reader thread) that will make more calls into the engine.
  if (did_attach) g_jvm->DetachCurrentThread();
  return static_cast<scan_error_t>(result);
}

// --- event pump: one native thread per live engine handle ------------------
struct PumpState {
  scan_engine* engine = nullptr;
  jobject listener_global_ref = nullptr;
  std::atomic<bool> running{false};
  std::thread thread;
};

std::mutex g_pumps_mutex;
std::unordered_map<jlong, PumpState*> g_pumps;

// Keyed by (engine_handle << 32 | device_id); guarded by g_pumps_mutex too
// (small map, not worth a second lock). Freed by nativeRemoveDevice.
std::unordered_map<jlong, SerialWriterCtx*> g_writer_ctxs;

jlong WriterCtxKey(jlong engine_handle, jint device_id) {
  return (engine_handle << 32) | (static_cast<jlong>(device_id) & 0xffffffffLL);
}

void event_pump_loop(PumpState* state) {
  bool did_attach = false;
  JNIEnv* env = AttachCurrentThreadOrGet(&did_attach);
  if (env == nullptr) {
    LOGE("event pump: failed to attach JVM thread");
    return;
  }

  scan_event ev;
  while (state->running.load(std::memory_order_relaxed)) {
    // 200ms wait: short enough that stopping the pump (running=false) is
    // observed promptly, long enough to not spin the core.
    scan_error_t err = scan_engine_wait_event(state->engine, &ev, 200);
    if (err == SCAN_ERR_TIMEOUT || err == SCAN_ERR_AGAIN) {
      continue;
    }
    if (err != SCAN_OK) {
      // Engine handle went away out from under us, or another fatal error;
      // stop rather than spin.
      LOGW("event pump: wait_event returned %d (%s), stopping", err, scan_error_str(err));
      break;
    }

    jlong i0 = 0, i1 = 0, i2 = 0, i3 = 0, i4 = 0;
    jdouble d0 = 0.0;
    switch (ev.type) {
      case SCAN_EVENT_EVENTS_DROPPED:
        i0 = static_cast<jlong>(ev.payload.dropped.count);
        i1 = static_cast<jlong>(ev.payload.dropped.total);
        break;
      case SCAN_EVENT_ENGINE_STATE:
        i0 = ev.payload.engine_state.state;
        i1 = ev.payload.engine_state.previous;
        break;
      case SCAN_EVENT_SESSION_STATE:
        i0 = ev.payload.session.recording;
        i1 = static_cast<jlong>(ev.payload.session.session_id);
        i2 = static_cast<jlong>(ev.payload.session.bytes_written);
        break;
      case SCAN_EVENT_DEVICE_STATE:
        i0 = ev.payload.device.device;
        i1 = ev.payload.device.kind;
        i2 = ev.payload.device.state;
        i3 = ev.payload.device.previous;
        i4 = ev.payload.device.error;
        break;
      case SCAN_EVENT_POINTS_AVAILABLE:
        i0 = ev.payload.points.page;
        i1 = ev.payload.points.first;
        i2 = ev.payload.points.count;
        i3 = ev.payload.points.stream;
        i4 = ev.payload.points.page_created;
        break;
      case SCAN_EVENT_ROTATION:
        i0 = ev.payload.rotation.device;
        i1 = static_cast<jlong>(ev.payload.rotation.rotation_index);
        i2 = ev.payload.rotation.points_in_rotation;
        d0 = ev.payload.rotation.rotation_hz;
        break;
      case SCAN_EVENT_ERROR:
        i0 = ev.payload.error.error;
        i1 = ev.payload.error.device;
        i2 = ev.payload.error.stream;
        break;
      case SCAN_EVENT_DEVICE_HEALTH:
      case SCAN_EVENT_POSE_UPDATE:
      case SCAN_EVENT_GNSS_FIX:
      case SCAN_EVENT_JOB_PROGRESS:
        // C-ABI GAP (see android/NOTES.md "C ABI gaps"): scanengine_c.cpp's
        // convert_event() has no case for these types yet, so the payload
        // arrives as opaque raw bytes at the C boundary. Interpreting raw
        // bytes here would mean guessing the still-unmirrored C++ struct
        // layout, which is exactly what that file's own comment warns
        // against. Forward type/sequence/timestamp only; zero payload.
        // Device health specifically is *not* needed via this path anyway
        // — poll scan_engine_device_health() instead (nativeDeviceHealth),
        // which is fully specified today and is what this shim uses.
        break;
      default:
        break;
    }

    env->CallVoidMethod(state->listener_global_ref, g_event_listener_on_event,
                         static_cast<jint>(ev.type), static_cast<jint>(ev.sequence),
                         static_cast<jlong>(ev.t_mono_ns), i0, i1, i2, i3, i4, d0);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  }

  if (did_attach) g_jvm->DetachCurrentThread();
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  g_jvm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  jclass health_local = env->FindClass("com/lidarscan/app/engine/NativeDeviceHealth");
  if (health_local == nullptr) {
    LOGE("JNI_OnLoad: NativeDeviceHealth class not found");
    return JNI_ERR;
  }
  g_health_class = static_cast<jclass>(env->NewGlobalRef(health_local));
  g_health_ctor = env->GetMethodID(g_health_class, "<init>", "(IIIIJJJJJDDDJ)V");
  if (g_health_ctor == nullptr) {
    LOGE("JNI_OnLoad: NativeDeviceHealth constructor not found");
    return JNI_ERR;
  }

  jclass writer_local = env->FindClass("com/lidarscan/app/engine/ScanEngineNative$SerialWriter");
  if (writer_local == nullptr) {
    LOGE("JNI_OnLoad: SerialWriter interface not found");
    return JNI_ERR;
  }
  g_serial_writer_class = static_cast<jclass>(env->NewGlobalRef(writer_local));
  g_serial_writer_write = env->GetMethodID(g_serial_writer_class, "write", "([B)I");
  if (g_serial_writer_write == nullptr) {
    LOGE("JNI_OnLoad: SerialWriter.write not found");
    return JNI_ERR;
  }

  jclass listener_local = env->FindClass("com/lidarscan/app/engine/ScanEngineNative$EngineEventListener");
  if (listener_local == nullptr) {
    LOGE("JNI_OnLoad: EngineEventListener interface not found");
    return JNI_ERR;
  }
  g_event_listener_class = static_cast<jclass>(env->NewGlobalRef(listener_local));
  g_event_listener_on_event = env->GetMethodID(g_event_listener_class, "onEvent", "(IIJJJJJJD)V");
  if (g_event_listener_on_event == nullptr) {
    LOGE("JNI_OnLoad: EngineEventListener.onEvent not found");
    return JNI_ERR;
  }

  // B4: point-page reads (both the live scan_engine* path below and
  // replay_jni.cpp's standalone replay Engine share this one cached class).
  jclass page_local = env->FindClass("com/lidarscan/app/engine/NativePointPage");
  if (page_local == nullptr) {
    LOGE("JNI_OnLoad: NativePointPage class not found");
    return JNI_ERR;
  }
  g_point_page_class = static_cast<jclass>(env->NewGlobalRef(page_local));
  g_point_page_ctor = env->GetMethodID(g_point_page_class, "<init>",
                                        "(IIIIJJFFFFFFLjava/nio/ByteBuffer;)V");
  if (g_point_page_ctor == nullptr) {
    LOGE("JNI_OnLoad: NativePointPage constructor not found");
    return JNI_ERR;
  }

  // B4: replay_jni.cpp's stats accessor. Cached here (not lazily in
  // replay_jni.cpp) for the same reason as the two classes above: every
  // *_native* entry point in this .so is invoked from a proper Java-created
  // thread (a JNI call always is), so a lazy FindClass in replay_jni.cpp
  // would in fact be safe too, but centralizing every class lookup in this
  // one already-audited JNI_OnLoad keeps the "which thread may FindClass"
  // reasoning in exactly one place.
  jclass replay_stats_local = env->FindClass("com/lidarscan/app/engine/NativeReplayStats");
  if (replay_stats_local == nullptr) {
    LOGE("JNI_OnLoad: NativeReplayStats class not found");
    return JNI_ERR;
  }
  g_replay_stats_class = static_cast<jclass>(env->NewGlobalRef(replay_stats_local));
  g_replay_stats_ctor = env->GetMethodID(g_replay_stats_class, "<init>", "(JJIIZZI)V");
  if (g_replay_stats_ctor == nullptr) {
    LOGE("JNI_OnLoad: NativeReplayStats constructor not found");
    return JNI_ERR;
  }

  // B7/B8 (arcore_jni.cpp): the mount-calibration result and pushbroom
  // stats carriers. Same reasoning as NativeReplayStats above for why they
  // are resolved here and not lazily in the file that uses them.
  jclass calib_local = env->FindClass("com/lidarscan/app/engine/NativeMountCalibResult");
  if (calib_local == nullptr) {
    LOGE("JNI_OnLoad: NativeMountCalibResult class not found");
    return JNI_ERR;
  }
  g_mount_calib_result_class = static_cast<jclass>(env->NewGlobalRef(calib_local));
  g_mount_calib_result_ctor =
      env->GetMethodID(g_mount_calib_result_class, "<init>", "([DZZIIJJDDDDIDDD)V");
  if (g_mount_calib_result_ctor == nullptr) {
    LOGE("JNI_OnLoad: NativeMountCalibResult constructor not found");
    return JNI_ERR;
  }

  jclass pb_local = env->FindClass("com/lidarscan/app/engine/NativePushbroomStats");
  if (pb_local == nullptr) {
    LOGE("JNI_OnLoad: NativePushbroomStats class not found");
    return JNI_ERR;
  }
  g_pushbroom_stats_class = static_cast<jclass>(env->NewGlobalRef(pb_local));
  g_pushbroom_stats_ctor = env->GetMethodID(g_pushbroom_stats_class, "<init>", "(JJJJJJJJJJJJJ)V");
  if (g_pushbroom_stats_ctor == nullptr) {
    LOGE("JNI_OnLoad: NativePushbroomStats constructor not found");
    return JNI_ERR;
  }

  // B3 (mid360_jni.cpp): the Mid-360 connect wizard's transport snapshot.
  jclass mid360_local = env->FindClass("com/lidarscan/app/engine/NativeMid360Probe");
  if (mid360_local == nullptr) {
    LOGE("JNI_OnLoad: NativeMid360Probe class not found");
    return JNI_ERR;
  }
  g_mid360_probe_class = static_cast<jclass>(env->NewGlobalRef(mid360_local));
  g_mid360_probe_ctor =
      env->GetMethodID(g_mid360_probe_class, "<init>", "(IIJJJJJDDDJJJJJJIJZI)V");
  if (g_mid360_probe_ctor == nullptr) {
    LOGE("JNI_OnLoad: NativeMid360Probe constructor not found");
    return JNI_ERR;
  }

  LOGI("scanengine_jni loaded; engine ABI version %u (%s)", scan_engine_abi_version(),
       scan_engine_version_string());
  return JNI_VERSION_1_6;
}

extern "C" {

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeAbiVersion(JNIEnv*, jclass) {
  return static_cast<jint>(scan_engine_abi_version());
}

JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeVersionString(JNIEnv* env, jclass) {
  return env->NewStringUTF(scan_engine_version_string());
}

JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeLastError(JNIEnv* env, jclass) {
  return env->NewStringUTF(scan_engine_last_error());
}

JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeErrorStr(JNIEnv* env, jclass, jint code) {
  return env->NewStringUTF(scan_error_str(static_cast<scan_error_t>(code)));
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeCreateEngine(
    JNIEnv* env, jclass, jstring app_name, jint log_level, jint page_capacity, jint max_pages,
    jint event_queue_capacity) {
  // ABI-version pin (scanengine_c.h convention #7): checked once here. This
  // shim was written against SCAN_ABI_VERSION 1. A mismatch means the
  // engine/capi/ this .so was linked against moved out from under B2's
  // pin (integration #24's concurrent capi/ edits) — refuse to proceed
  // rather than mis-marshal an event/health struct whose layout changed.
  uint32_t abi = scan_engine_abi_version();
  if (abi != SCAN_ABI_VERSION) {
    LOGE("engine ABI version mismatch: linked against %u, shim compiled for %u", abi,
         SCAN_ABI_VERSION);
    return 0;
  }

  const char* app_name_utf = app_name != nullptr ? env->GetStringUTFChars(app_name, nullptr) : nullptr;

  scan_engine_config cfg{};
  cfg.app_name = app_name_utf;
  cfg.log_level = log_level;
  cfg.page_capacity = static_cast<uint32_t>(page_capacity);
  cfg.max_pages = static_cast<uint32_t>(max_pages);
  cfg.event_queue_capacity = static_cast<uint32_t>(event_queue_capacity);

  scan_engine* engine = nullptr;
  scan_error_t err = scan_engine_create(&cfg, &engine);

  if (app_name_utf != nullptr) env->ReleaseStringUTFChars(app_name, app_name_utf);

  if (err != SCAN_OK) {
    LOGE("scan_engine_create failed: %d (%s)", err, scan_engine_last_error());
    return 0;
  }
  return reinterpret_cast<jlong>(engine);
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeDestroyEngine(JNIEnv*, jclass, jlong handle) {
  scan_engine_destroy(reinterpret_cast<scan_engine*>(handle));
}

// B4: `live_slam` binds scan_session_config.live_slam (Tech Spec §3.1's
// Live-SLAM/Record-only toggle), added to the C ABI in the INT-24 ABI bump
// (SCAN_ABI_VERSION 2 — see scanengine_c.h's revision note at the top). B2
// pinned against ABI 1, where this field did not exist yet, and documented
// the gap in NOTES.md; B4 is pinned against ABI 2, where it does, so this is
// that gap closing rather than a new workaround.
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeStartSession(
    JNIEnv* env, jclass, jlong handle, jstring lscan_dir, jstring profile, jboolean record,
    jboolean live_slam) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  const char* dir_utf = lscan_dir != nullptr ? env->GetStringUTFChars(lscan_dir, nullptr) : nullptr;
  const char* profile_utf = profile != nullptr ? env->GetStringUTFChars(profile, nullptr) : nullptr;

  scan_session_config cfg{};
  cfg.lscan_dir = dir_utf;
  cfg.profile = profile_utf;
  cfg.record = record ? 1 : 0;
  cfg.live_slam = live_slam ? 1 : 0;

  scan_error_t err = scan_engine_start(engine, &cfg);

  if (dir_utf != nullptr) env->ReleaseStringUTFChars(lscan_dir, dir_utf);
  if (profile_utf != nullptr) env->ReleaseStringUTFChars(profile, profile_utf);
  return static_cast<jint>(err);
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeStopSession(JNIEnv*, jclass, jlong handle) {
  return static_cast<jint>(scan_engine_stop(reinterpret_cast<scan_engine*>(handle)));
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeEngineState(JNIEnv*, jclass, jlong handle) {
  int32_t state = -1;
  scan_engine_state(reinterpret_cast<scan_engine*>(handle), &state);
  return static_cast<jint>(state);
}

// Returns the device id (>= 0) on success, -1 on failure (call
// nativeLastError() for detail). `writer` may be null (device.write == NULL
// => "engine sends no commands", per scanengine_c.h) for a receive-only
// connect. On success the shim holds a JVM global ref to `writer` until
// nativeRemoveDevice frees it.
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeAddD6Device(
    JNIEnv* env, jclass, jlong handle, jstring serial_port_name, jint baud,
    jboolean send_start_stop, jobject writer) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  const char* port_utf =
      serial_port_name != nullptr ? env->GetStringUTFChars(serial_port_name, nullptr) : nullptr;

  scan_device_config cfg{};
  cfg.kind = SCAN_DEVICE_D6;
  cfg.serial_port_name = port_utf;
  cfg.serial_baud = static_cast<uint32_t>(baud);
  cfg.send_start_stop_commands = send_start_stop ? 1 : 0;

  SerialWriterCtx* ctx = nullptr;
  if (writer != nullptr) {
    ctx = new SerialWriterCtx{env->NewGlobalRef(writer)};
    cfg.serial_write = &serial_write_trampoline;
    cfg.serial_write_user_data = ctx;
  }

  uint32_t device_id = 0;
  scan_error_t err = scan_engine_add_device(engine, &cfg, &device_id);

  if (port_utf != nullptr) env->ReleaseStringUTFChars(serial_port_name, port_utf);

  if (err != SCAN_OK) {
    LOGE("scan_engine_add_device (D6) failed: %d (%s)", err, scan_engine_last_error());
    if (ctx != nullptr) {
      env->DeleteGlobalRef(ctx->writer_global_ref);
      delete ctx;
    }
    return -1;
  }

  // Stash the ctx pointer keyed by (engine, device_id) so nativeRemoveDevice
  // can free it and drop the global ref.
  if (ctx != nullptr) {
    std::lock_guard<std::mutex> lock(g_pumps_mutex);
    g_writer_ctxs[WriterCtxKey(handle, static_cast<jint>(device_id))] = ctx;
  }

  return static_cast<jint>(device_id);
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeRemoveDevice(JNIEnv* env, jclass, jlong handle,
                                                                   jint device_id) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  scan_error_t err = scan_engine_remove_device(engine, static_cast<uint32_t>(device_id));

  // Cleanup of the writer global ref, if this device had one (Mid-360
  // devices in a future B3 build won't). Freed regardless of `err` — the
  // device is gone from the engine's perspective either way once removed
  // is attempted, and leaking the global ref on a retry path is worse.
  SerialWriterCtx* ctx = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_pumps_mutex);
    auto it = g_writer_ctxs.find(WriterCtxKey(handle, device_id));
    if (it != g_writer_ctxs.end()) {
      ctx = it->second;
      g_writer_ctxs.erase(it);
    }
  }
  if (ctx != nullptr) {
    env->DeleteGlobalRef(ctx->writer_global_ref);
    delete ctx;
  }
  return static_cast<jint>(err);
}

JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeDeviceHealth(JNIEnv* env, jclass, jlong handle,
                                                                    jint device_id) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  scan_device_health h{};
  scan_error_t err = scan_engine_device_health(engine, static_cast<uint32_t>(device_id), &h);
  if (err != SCAN_OK) return nullptr;

  return env->NewObject(g_health_class, g_health_ctor, static_cast<jint>(h.id),
                         static_cast<jint>(h.kind), static_cast<jint>(h.state),
                         static_cast<jint>(h.last_error), static_cast<jlong>(h.bytes_in),
                         static_cast<jlong>(h.packets_ok), static_cast<jlong>(h.packets_bad),
                         static_cast<jlong>(h.points_out), static_cast<jlong>(h.drops),
                         static_cast<jdouble>(h.points_per_sec), static_cast<jdouble>(h.rotation_hz),
                         static_cast<jdouble>(h.checksum_pass_rate),
                         static_cast<jlong>(h.t_last_data_ns));
}

// Zero-copy: `buffer` must be a direct ByteBuffer (java.nio.ByteBuffer.
// allocateDirect). GetDirectBufferAddress hands the engine a pointer
// straight into it; no array copy happens at this JNI call.
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePushSerialBytes(
    JNIEnv* env, jclass, jlong handle, jint device_id, jobject buffer, jint len, jlong t_mono_ns) {
  void* addr = env->GetDirectBufferAddress(buffer);
  if (addr == nullptr) {
    LOGE("nativePushSerialBytes: buffer is not a direct ByteBuffer");
    return SCAN_ERR_INVALID_ARGUMENT;
  }
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  scan_error_t err =
      scan_engine_push_serial_bytes(engine, static_cast<uint32_t>(device_id),
                                     static_cast<const uint8_t*>(addr), static_cast<size_t>(len),
                                     static_cast<int64_t>(t_mono_ns));
  return static_cast<jint>(err);
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeStartEventPump(JNIEnv* env, jclass,
                                                                      jlong handle,
                                                                      jobject listener) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  std::lock_guard<std::mutex> lock(g_pumps_mutex);
  if (g_pumps.find(handle) != g_pumps.end()) {
    LOGW("nativeStartEventPump: pump already running for this handle");
    return JNI_FALSE;
  }
  auto* state = new PumpState();
  state->engine = engine;
  state->listener_global_ref = env->NewGlobalRef(listener);
  state->running.store(true, std::memory_order_relaxed);
  state->thread = std::thread(event_pump_loop, state);
  g_pumps[handle] = state;
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeStopEventPump(JNIEnv* env, jclass,
                                                                     jlong handle) {
  PumpState* state = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_pumps_mutex);
    auto it = g_pumps.find(handle);
    if (it == g_pumps.end()) return;
    state = it->second;
    g_pumps.erase(it);
  }
  state->running.store(false, std::memory_order_relaxed);
  if (state->thread.joinable()) state->thread.join();
  env->DeleteGlobalRef(state->listener_global_ref);
  delete state;
}

// --- B4: point-page reads ---------------------------------------------------
//
// The minimal JNI this task needs to feed Filament's paged vertex buffers
// (PointCloudRenderer.kt): enumerate pages, then hand back each one as a
// DIRECT ByteBuffer over scan_point_page.data — the same zero-copy-across-
// JNI approach nativePushSerialBytes already uses in the other direction.
// scanengine_c.h's contract (`data` "stable for the page's lifetime") is
// what makes this safe: the buffer stays valid until the PageStore is
// cleared (a new session), which the Kotlin side never straddles a read
// across (PointCloudRenderer re-syncs pages once per frame from page ids
// enumerated fresh each time, mirroring desktop's PagedCloudRenderer::sync()
// comment on why polling needs no lock).

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePageCount(JNIEnv*, jclass, jlong handle) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  uint32_t count = 0;
  if (scan_engine_page_count(engine, &count) != SCAN_OK) return 0;
  return static_cast<jint>(count);
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePageIdAt(JNIEnv*, jclass, jlong handle,
                                                                 jint index) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  uint32_t id = 0;
  if (scan_engine_page_id_at(engine, static_cast<uint32_t>(index), &id) != SCAN_OK) return -1;
  return static_cast<jint>(id);
}

JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeGetPointPage(JNIEnv* env, jclass,
                                                                     jlong handle, jint page_id) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  scan_point_page page{};
  if (scan_engine_get_point_page(engine, static_cast<uint32_t>(page_id), &page) != SCAN_OK) {
    return nullptr;
  }
  // const_cast: NewDirectByteBuffer takes a non-const void*, but this shim
  // never writes through it — the GPU upload on the Kotlin side only reads.
  jobject buffer = env->NewDirectByteBuffer(
      const_cast<void*>(static_cast<const void*>(page.data)),
      static_cast<jlong>(page.count) * sizeof(scan_point_vertex));
  if (buffer == nullptr) return nullptr;

  return env->NewObject(g_point_page_class, g_point_page_ctor, static_cast<jint>(page.id),
                         static_cast<jint>(page.stream), static_cast<jint>(page.count),
                         static_cast<jint>(page.capacity), static_cast<jlong>(page.t_first_ns),
                         static_cast<jlong>(page.t_last_ns),
                         static_cast<jfloat>(page.bounds_min[0]),
                         static_cast<jfloat>(page.bounds_min[1]),
                         static_cast<jfloat>(page.bounds_min[2]),
                         static_cast<jfloat>(page.bounds_max[0]),
                         static_cast<jfloat>(page.bounds_max[1]),
                         static_cast<jfloat>(page.bounds_max[2]), buffer);
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeTotalPoints(JNIEnv*, jclass, jlong handle) {
  auto* engine = reinterpret_cast<scan_engine*>(handle);
  uint64_t total = 0;
  if (scan_engine_total_points(engine, &total) != SCAN_OK) return 0;
  return static_cast<jlong>(total);
}

}  // extern "C"
