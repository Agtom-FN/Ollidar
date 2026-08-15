// gnss_jni.cpp — B9's GNSS/RTK bindings (Tech Spec §3.4, engine/docs/A10-gnss.md).
//
// UNLIKE processing_jni.cpp, EVERY CALL HERE GOES THROUGH THE C ABI.
// A10 §9.2 listed exactly what B9 would need; INT-29 landed all of it at
// SCAN_ABI_VERSION 3 and it is still there at 4 (checked against
// engine/capi/scanengine_c.h at task start). So there is no reason to link
// gnss/ directly the way B3's Mid-360 probe and B6's job queue have to — the C
// ABI expresses the whole surface, and going through it keeps the rover on the
// SAME `scan_engine*` the capture session owns, which is what makes
// record-always work: `scan_engine_push_nmea`'s own contract says the bytes hit
// the .lscan as kGnssNmea chunks BEFORE they are parsed.
//
// MARSHALLING. `scan_gnss_fix`, `scan_gnss_stats` and `scan_ntrip_stats` are
// three wide structs of plain numbers and no strings, so each crosses as ONE
// flat `double[]` with a layout documented at its function and asserted by a
// `:core` unit test on the Kotlin side. Three more hand-typed constructor
// descriptors would have been three more chances at the "compiles on both
// sides, dies at JNI_OnLoad" failure B2/B4/B7 all flagged, in exchange for
// nothing — there is no string to carry.
//
// A double holds every field exactly: the widest are uint64 counters, and a
// double is exact to 2^53, which at 1 MB/s of NMEA is 285 years of bytes_in.
// The one field that genuinely does not fit is `utc_unix_ns` (~1.8e18 now), so
// it crosses as **unix MILLIseconds** — exact to 2^53 ms, i.e. 285,000 years —
// and the Kotlin side names it `utcUnixMillis` so nobody reads it as nanos.
#include <jni.h>

#include <cstring>
#include <string>
#include <vector>

#include "jni_shared.h"
#include "scanengine_c.h"

namespace {

struct RtcmSinkCtx {
  jobject sink_global = nullptr;  // ScanEngineNative.RtcmSink
  jmethodID write = nullptr;
};
RtcmSinkCtx g_rtcm;

std::string ToStdString(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c != nullptr ? c : "";
  if (c != nullptr) env->ReleaseStringUTFChars(s, c);
  return out;
}

jdoubleArray NewDoubleArray(JNIEnv* env, const std::vector<double>& v) {
  jdoubleArray a = env->NewDoubleArray(static_cast<jsize>(v.size()));
  if (a == nullptr) return nullptr;
  if (!v.empty()) env->SetDoubleArrayRegion(a, 0, static_cast<jsize>(v.size()), v.data());
  return a;
}

// RTCM3 out, straight to the rover's Bluetooth socket. Runs on the NTRIP
// receive thread — a thread the JVM never created — with NO client lock held
// (A10 §8), which is what makes a slow Bluetooth write safe here: it cannot
// deadlock the client. It must still be quick and must not re-enter the client.
void rtcm_trampoline(const uint8_t* data, size_t len, void* /*user*/) {
  if (g_rtcm.sink_global == nullptr || g_rtcm.write == nullptr) return;
  bool did_attach = false;
  JNIEnv* env = lidarscan_jni::AttachCurrentThreadOrGet(&did_attach);
  if (env == nullptr) return;
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(len));
  if (arr != nullptr) {
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(data));
    env->CallVoidMethod(g_rtcm.sink_global, g_rtcm.write, arr);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    env->DeleteLocalRef(arr);
  }
  if (did_attach) lidarscan_jni::g_jvm->DetachCurrentThread();
}

}  // namespace

extern "C" {

// --- the rover device --------------------------------------------------------

/**
 * Adds a SCAN_DEVICE_RTK_ROVER to the live capture engine. Returns the device
 * id (>= 0) or -1. `scan_device_config` carries no rover-specific fields —
 * A10 wires ONE engine-lifetime GnssSource (`EngineConfig::gnss`), because an
 * operator pairs the rover and waits for Fixed before any session exists.
 */
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeAddRtkRoverDevice(JNIEnv*, jclass,
                                                                       jlong handle) {
  if (handle == 0) return -1;
  scan_device_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.kind = SCAN_DEVICE_RTK_ROVER;
  uint32_t id = 0;
  if (scan_engine_add_device(reinterpret_cast<scan_engine*>(handle), &cfg, &id) != SCAN_OK) {
    return -1;
  }
  return static_cast<jint>(id);
}

/**
 * Rover bytes in. Any chunk is fine — the framer handles arbitrary chunking,
 * which is exactly what Bluetooth SPP's 20–990-byte MTU fragments require, and
 * is why this takes a direct ByteBuffer + length rather than a sentence.
 */
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativePushNmea(JNIEnv* env, jclass, jlong handle,
                                                              jint device_id, jobject buffer,
                                                              jint len, jlong t_mono_ns) {
  if (handle == 0 || buffer == nullptr) return SCAN_ERR_INVALID_ARGUMENT;
  auto* p = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buffer));
  if (p == nullptr) return SCAN_ERR_INVALID_ARGUMENT;  // not a direct ByteBuffer
  return scan_engine_push_nmea(reinterpret_cast<scan_engine*>(handle),
                               static_cast<uint32_t>(device_id), p, static_cast<size_t>(len),
                               t_mono_ns);
}

/*
 * `scan_gnss_fix` as 22 doubles:
 *   [0] fix (SCAN_FIX_*)      [1] satellites        [2] quality_raw
 *   [3] fix_dimension         [4] station_id
 *   [5] lat_deg               [6] lon_deg           [7] alt_m (ORTHOMETRIC)
 *   [8] geoid_sep_m           [9] height_ellipsoid_m
 *   [10] has_geoid_sep
 *   [11] hdop  [12] pdop  [13] vdop
 *   [14] correction_age_s (the ROVER's, GGA field 13)
 *   [15] sigma_horizontal_m   [16] sigma_from_gst
 *   [17] speed_mps            [18] course_deg       [19] has_course
 *   [20] utc unix MILLIseconds (0 until an RMC has supplied a date)
 *   [21] has_fix — 1 once at least one epoch has closed
 */
JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeLastFix(JNIEnv* env, jclass, jlong handle) {
  if (handle == 0) return nullptr;
  scan_gnss_fix f;
  std::memset(&f, 0, sizeof(f));
  if (scan_engine_last_fix(reinterpret_cast<scan_engine*>(handle), &f) != SCAN_OK) return nullptr;
  std::vector<double> v(22, 0.0);
  v[0] = f.fix;
  v[1] = f.satellites;
  v[2] = f.quality_raw;
  v[3] = f.fix_dimension;
  v[4] = f.station_id;
  v[5] = f.lat_deg;
  v[6] = f.lon_deg;
  v[7] = f.alt_m;
  v[8] = f.geoid_sep_m;
  v[9] = f.height_ellipsoid_m;
  v[10] = f.has_geoid_sep;
  v[11] = f.hdop;
  v[12] = f.pdop;
  v[13] = f.vdop;
  v[14] = f.correction_age_s;
  v[15] = f.sigma_horizontal_m;
  v[16] = f.sigma_from_gst;
  v[17] = f.speed_mps;
  v[18] = f.course_deg;
  v[19] = f.has_course;
  v[20] = static_cast<double>(f.utc_unix_ns / 1000000);
  // "Zero-filled with fix == SCAN_FIX_NONE before the first one — never an
  // error, because 'no fix yet' is a normal state a status strip has to
  // render." t_mono_ns is what distinguishes never-published from no-fix.
  v[21] = (f.t_mono_ns != 0) ? 1.0 : 0.0;
  return NewDoubleArray(env, v);
}

/*
 * `scan_gnss_stats` as 19 doubles:
 *   [0] bytes_in  [1] sentences_ok  [2] checksum_failed  [3] malformed
 *   [4] checksum_pass_rate
 *   [5] epochs    [6] fixes_published  [7] poses_published
 *   [8] epochs_no_position  [9] epochs_below_gate  [10] gst_epochs
 *   [11..15] by_fix[0..4] — the §3.4 fix-quality TIMELINE itself
 *   [16] time_converged (A4: false for the first ~16 s of a 1 Hz stream)
 *   [17] has_origin
 *   [18] time_uncertainty_ns
 */
JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeGnssStats(JNIEnv* env, jclass, jlong handle) {
  if (handle == 0) return nullptr;
  scan_gnss_stats s;
  std::memset(&s, 0, sizeof(s));
  if (scan_engine_gnss_stats(reinterpret_cast<scan_engine*>(handle), &s) != SCAN_OK) return nullptr;
  std::vector<double> v(19, 0.0);
  v[0] = static_cast<double>(s.bytes_in);
  v[1] = static_cast<double>(s.sentences_ok);
  v[2] = static_cast<double>(s.checksum_failed);
  v[3] = static_cast<double>(s.malformed);
  v[4] = s.checksum_pass_rate;
  v[5] = static_cast<double>(s.epochs);
  v[6] = static_cast<double>(s.fixes_published);
  v[7] = static_cast<double>(s.poses_published);
  v[8] = static_cast<double>(s.epochs_no_position);
  v[9] = static_cast<double>(s.epochs_below_gate);
  v[10] = static_cast<double>(s.gst_epochs);
  for (int i = 0; i < 5; ++i) v[11 + i] = static_cast<double>(s.by_fix[i]);
  v[16] = s.time_converged;
  v[17] = s.has_origin;
  v[18] = static_cast<double>(s.time_uncertainty_ns);
  return NewDoubleArray(env, v);
}

/*
 * `scan_georef_solution` as 27 doubles:
 *   [0] converged  [1] epsg  [2] yaw_deg
 *   [3..18] global_from_local, ROW-MAJOR
 *   [19] horizontal_sigma_m  [20] vertical_sigma_m  [21] cep95_m
 *   [22] samples  [23] inliers  [24] residual_rms_m  [25] span_m
 *   [26] dominant_fix (SCAN_FIX_*)
 *
 * The blocker string comes back from nativeGeorefBlocker() separately —
 * `scan_georef_solution.blocker` is the one non-numeric field, and a
 * not-converged solution's numbers are meaningless anyway, so the two are never
 * read together in a way an inconsistent snapshot could hurt.
 */
JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeGeorefSolution(JNIEnv* env, jclass,
                                                                    jlong handle) {
  if (handle == 0) return nullptr;
  scan_georef_solution g;
  std::memset(&g, 0, sizeof(g));
  if (scan_engine_georef_solution(reinterpret_cast<scan_engine*>(handle), &g) != SCAN_OK) {
    return nullptr;
  }
  std::vector<double> v(27, 0.0);
  v[0] = g.converged;
  v[1] = g.epsg;
  v[2] = g.yaw_deg;
  for (int i = 0; i < 16; ++i) v[3 + i] = g.global_from_local[i];
  v[19] = g.horizontal_sigma_m;
  v[20] = g.vertical_sigma_m;
  v[21] = g.cep95_m;
  v[22] = static_cast<double>(g.samples);
  v[23] = static_cast<double>(g.inliers);
  v[24] = g.residual_rms_m;
  v[25] = g.span_m;
  v[26] = g.dominant_fix;
  return NewDoubleArray(env, v);
}

JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeGeorefBlocker(JNIEnv* env, jclass,
                                                                   jlong handle) {
  if (handle == 0) return env->NewStringUTF("");
  scan_georef_solution g;
  std::memset(&g, 0, sizeof(g));
  if (scan_engine_georef_solution(reinterpret_cast<scan_engine*>(handle), &g) != SCAN_OK) {
    return env->NewStringUTF("");
  }
  g.blocker[sizeof(g.blocker) - 1] = '\0';
  return env->NewStringUTF(g.blocker);
}

/**
 * The A9 export seam. **Empty until the georef transform converges**, and that
 * is stricter than "the UTM zone is known" on purpose: until then the cloud is
 * still in the local frame, and labelling it with a real CRS produces a file
 * that opens fine and lands in the wrong place. Empty is A9's documented
 * "embed the local-frame placeholder" input.
 *
 * Copied immediately: the C ABI's string convention is "valid until the next
 * engine call ON THE SAME THREAD", and NewStringUTF does exactly that copy.
 */
JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeCrsWkt(JNIEnv* env, jclass, jlong handle) {
  if (handle == 0) return env->NewStringUTF("");
  const char* s = scan_engine_crs_wkt(reinterpret_cast<scan_engine*>(handle));
  return env->NewStringUTF(s != nullptr ? s : "");
}

JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeCrsEpsg(JNIEnv* env, jclass, jlong handle) {
  if (handle == 0) return env->NewStringUTF("");
  const char* s = scan_engine_crs_epsg(reinterpret_cast<scan_engine*>(handle));
  return env->NewStringUTF(s != nullptr ? s : "");
}

// --- NTRIP -------------------------------------------------------------------

/**
 * `engine != 0` BORROWS the engine's own client — the one whose GGA upload is
 * the rover's own last sentence and whose forwarded frames are recorded. That
 * is what an app wants, so the app always passes its engine handle; passing 0
 * makes a standalone client, which is only useful for a mountpoint picker with
 * no engine.
 */
JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeNtripCreate(JNIEnv*, jclass,
                                                                 jlong engine_handle) {
  scan_ntrip* c = nullptr;
  if (scan_ntrip_create(reinterpret_cast<scan_engine*>(engine_handle), &c) != SCAN_OK) return 0;
  return reinterpret_cast<jlong>(c);
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeNtripDestroy(JNIEnv*, jclass, jlong handle) {
  if (handle != 0) scan_ntrip_destroy(reinterpret_cast<scan_ntrip*>(handle));
}

/**
 * Performs the FIRST HANDSHAKE SYNCHRONOUSLY, so a wrong password comes back as
 * SCAN_ERR_PERMISSION_DENIED and an unknown mountpoint as SCAN_ERR_NOT_FOUND
 * from this very call — not as an infinite reconnect loop under a
 * "connecting…" spinner. Call it off the main thread.
 */
JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeNtripConnect(
    JNIEnv* env, jclass, jlong handle, jstring host, jint port, jstring mountpoint,
    jstring username, jstring password, jint ntrip_version, jboolean allow_v1_fallback,
    jint gga_interval_ms, jboolean auto_reconnect) {
  if (handle == 0) return SCAN_ERR_INVALID_ARGUMENT;
  const std::string h = ToStdString(env, host);
  const std::string m = ToStdString(env, mountpoint);
  const std::string u = ToStdString(env, username);
  const std::string p = ToStdString(env, password);

  scan_ntrip_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.host = h.c_str();
  cfg.port = static_cast<uint16_t>(port);
  cfg.mountpoint = m.c_str();
  cfg.username = u.empty() ? nullptr : u.c_str();
  cfg.password = p.empty() ? nullptr : p.c_str();
  cfg.ntrip_version = ntrip_version;
  cfg.allow_v1_fallback_set = 1;
  cfg.allow_v1_fallback = allow_v1_fallback == JNI_TRUE ? 1 : 0;
  cfg.gga_interval_ms = gga_interval_ms;
  cfg.auto_reconnect_set = 1;
  cfg.auto_reconnect = auto_reconnect == JNI_TRUE ? 1 : 0;
  return scan_ntrip_connect(reinterpret_cast<scan_ntrip*>(handle), &cfg);
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeNtripDisconnect(JNIEnv*, jclass, jlong handle) {
  if (handle == 0) return SCAN_ERR_INVALID_ARGUMENT;
  return scan_ntrip_disconnect(reinterpret_cast<scan_ntrip*>(handle));
}

/*
 * `scan_ntrip_stats` as 18 doubles:
 *   [0] state (SCAN_NTRIP_*)  [1] receiving  [2] correction_age_s (-1 = none yet)
 *   [3] bytes_received  [4] frames_ok  [5] frames_crc_failed  [6] rtcm_bytes
 *   [7] gga_sent  [8] connect_attempts  [9] connects_ok  [10] disconnects
 *   [11] reconnects  [12] stalls  [13] handshake_failures
 *   [14] backoff_ms  [15] http_status  [16] ntrip_version_used  [17] last_error
 */
JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeNtripStats(JNIEnv* env, jclass, jlong handle) {
  if (handle == 0) return nullptr;
  scan_ntrip_stats s;
  std::memset(&s, 0, sizeof(s));
  if (scan_ntrip_get_stats(reinterpret_cast<scan_ntrip*>(handle), &s) != SCAN_OK) return nullptr;
  std::vector<double> v(18, 0.0);
  v[0] = s.state;
  v[1] = s.receiving;
  v[2] = s.correction_age_s;
  v[3] = static_cast<double>(s.bytes_received);
  v[4] = static_cast<double>(s.frames_ok);
  v[5] = static_cast<double>(s.frames_crc_failed);
  v[6] = static_cast<double>(s.rtcm_bytes);
  v[7] = static_cast<double>(s.gga_sent);
  v[8] = static_cast<double>(s.connect_attempts);
  v[9] = static_cast<double>(s.connects_ok);
  v[10] = static_cast<double>(s.disconnects);
  v[11] = static_cast<double>(s.reconnects);
  v[12] = static_cast<double>(s.stalls);
  v[13] = static_cast<double>(s.handshake_failures);
  v[14] = s.backoff_ms;
  v[15] = s.http_status;
  v[16] = s.ntrip_version_used;
  v[17] = s.last_error;
  return NewDoubleArray(env, v);
}

/** Installs (or clears, with a null sink) the RTCM3 → rover write callback. */
JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeNtripSetRtcmSink(JNIEnv* env, jclass,
                                                                      jlong handle, jobject sink) {
  if (handle == 0) return JNI_FALSE;
  if (g_rtcm.sink_global != nullptr) {
    env->DeleteGlobalRef(g_rtcm.sink_global);
    g_rtcm.sink_global = nullptr;
  }
  if (sink == nullptr) {
    return scan_ntrip_set_rtcm_callback(reinterpret_cast<scan_ntrip*>(handle), nullptr, nullptr) ==
                   SCAN_OK
               ? JNI_TRUE
               : JNI_FALSE;
  }
  jclass cls = env->GetObjectClass(sink);
  g_rtcm.write = env->GetMethodID(cls, "write", "([B)V");
  env->DeleteLocalRef(cls);
  if (g_rtcm.write == nullptr) return JNI_FALSE;
  g_rtcm.sink_global = env->NewGlobalRef(sink);
  return scan_ntrip_set_rtcm_callback(reinterpret_cast<scan_ntrip*>(handle), &rtcm_trampoline,
                                      nullptr) == SCAN_OK
             ? JNI_TRUE
             : JNI_FALSE;
}

/*
 * The mountpoint picker. A separate short-lived connection, so it works before
 * connect() and while streaming.
 *
 * Numbers come back as one flat double[] (stride 6: lat, lon, needs_gga, fee,
 * carrier, solution) and the four strings as one String[] (stride 4:
 * mountpoint, identifier, format, country) — a String[] cannot carry the
 * numbers and a double[] cannot carry the strings, and two arrays built from
 * ONE fetch cannot disagree.
 */
JNIEXPORT jobjectArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeNtripSourcetableText(
    JNIEnv* env, jclass, jstring host, jint port, jstring username, jstring password,
    jint capacity) {
  const std::string h = ToStdString(env, host);
  const std::string u = ToStdString(env, username);
  const std::string p = ToStdString(env, password);
  scan_ntrip_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.host = h.c_str();
  cfg.port = static_cast<uint16_t>(port);
  cfg.username = u.empty() ? nullptr : u.c_str();
  cfg.password = p.empty() ? nullptr : p.c_str();

  const uint32_t cap = capacity > 0 ? static_cast<uint32_t>(capacity) : 128;
  std::vector<scan_ntrip_source> out(cap);
  uint32_t count = 0;
  const scan_error_t rc = scan_ntrip_fetch_sourcetable(&cfg, out.data(), cap, &count);
  // SCAN_ERR_CAPACITY_EXCEEDED means truncated, not failed — the caster's true
  // total is still in `count`, so a caller could retry bigger. A truncated list
  // is still a usable picker, so it is returned rather than discarded.
  if (rc != SCAN_OK && rc != SCAN_ERR_CAPACITY_EXCEEDED) return nullptr;
  const uint32_t n = count < cap ? count : cap;

  jclass str_class = env->FindClass("java/lang/String");
  jobjectArray arr = env->NewObjectArray(static_cast<jsize>(n) * 4, str_class, nullptr);
  for (uint32_t i = 0; i < n; ++i) {
    out[i].mountpoint[sizeof(out[i].mountpoint) - 1] = '\0';
    out[i].identifier[sizeof(out[i].identifier) - 1] = '\0';
    out[i].format[sizeof(out[i].format) - 1] = '\0';
    out[i].country[sizeof(out[i].country) - 1] = '\0';
    const char* fields[4] = {out[i].mountpoint, out[i].identifier, out[i].format, out[i].country};
    for (int k = 0; k < 4; ++k) {
      jstring s = env->NewStringUTF(fields[k]);
      env->SetObjectArrayElement(arr, static_cast<jsize>(i) * 4 + k, s);
      env->DeleteLocalRef(s);
    }
  }
  env->DeleteLocalRef(str_class);
  return arr;
}

JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeNtripSourcetableNumbers(
    JNIEnv* env, jclass, jstring host, jint port, jstring username, jstring password,
    jint capacity) {
  const std::string h = ToStdString(env, host);
  const std::string u = ToStdString(env, username);
  const std::string p = ToStdString(env, password);
  scan_ntrip_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.host = h.c_str();
  cfg.port = static_cast<uint16_t>(port);
  cfg.username = u.empty() ? nullptr : u.c_str();
  cfg.password = p.empty() ? nullptr : p.c_str();

  const uint32_t cap = capacity > 0 ? static_cast<uint32_t>(capacity) : 128;
  std::vector<scan_ntrip_source> out(cap);
  uint32_t count = 0;
  const scan_error_t rc = scan_ntrip_fetch_sourcetable(&cfg, out.data(), cap, &count);
  if (rc != SCAN_OK && rc != SCAN_ERR_CAPACITY_EXCEEDED) return nullptr;
  const uint32_t n = count < cap ? count : cap;

  std::vector<double> v;
  v.reserve(static_cast<std::size_t>(n) * 6);
  for (uint32_t i = 0; i < n; ++i) {
    v.push_back(out[i].lat_deg);
    v.push_back(out[i].lon_deg);
    v.push_back(out[i].needs_gga);
    v.push_back(out[i].fee);
    v.push_back(out[i].carrier);
    v.push_back(out[i].solution);
  }
  return NewDoubleArray(env, v);
}

}  // extern "C"
