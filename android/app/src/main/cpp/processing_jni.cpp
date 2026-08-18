// processing_jni.cpp — JNI bindings for processing_engine.h (B6 jobs, B11
// floor plan, B12 merge).
//
// MARSHALLING POLICY, and why it differs from B3/B7's
//
// B2, B4, B7 and B3 all noted the same latent hazard: a constructor descriptor
// typed by hand ("(IIIIJJJJJDDDJ)V") compiles on both sides and only fails at
// JNI_OnLoad. This file therefore uses a marshalling CLASS only where a record
// genuinely mixes numbers and strings — `NativeJob` and `NativeMergeSummary` —
// and returns everything else as a **flat primitive array with a documented
// index layout**. The floor-plan model and the GNSS snapshots (gnss_jni.cpp)
// carry no strings, so they need no class, no descriptor, and no FindClass at
// all; a layout mismatch there is a wrong number, which a unit test catches,
// rather than a load-time abort.
//
// The one thing a flat array cannot do is give two related arrays a consistent
// snapshot, so every multi-array read here is served from a model the native
// side already holds still (`ProcessingEngine::run_plan()` stores the result;
// the getters read that stored copy), never from a live structure.
//
// NO kCloudSubmit JOB KIND IS EXPOSED. A15 has one and it works, but the
// Android cloud client is Kotlin (`com.lidarscan.core.cloud`) — see
// android/NOTES.md for the reasoning. Submitting the same upload from two
// implementations would give the app two retry policies and two size caps.
#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "jni_shared.h"
#include "processing_engine.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/types.h"
#include "scanengine_c.h"

using lidarscan_jni::g_job_class;
using lidarscan_jni::g_job_ctor;
using lidarscan_jni::g_merge_summary_class;
using lidarscan_jni::g_merge_summary_ctor;
using lidarscan_jni::g_point_page_class;
using lidarscan_jni::g_point_page_ctor;
using lidarscan_jni::ProcessingEngine;

namespace {

ProcessingEngine* HandleOf(jlong h) { return reinterpret_cast<ProcessingEngine*>(h); }

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

jintArray NewIntArray(JNIEnv* env, const std::vector<jint>& v) {
  jintArray a = env->NewIntArray(static_cast<jsize>(v.size()));
  if (a == nullptr) return nullptr;
  if (!v.empty()) env->SetIntArrayRegion(a, 0, static_cast<jsize>(v.size()), v.data());
  return a;
}

// The Kotlin-visible progress bridge (ScanEngineNative.JobProgressListener).
struct ProgressCtx {
  jobject listener_global = nullptr;
  jmethodID method = nullptr;
};
ProgressCtx g_progress;

jobject MakePointPage(JNIEnv* env, const scanengine::PageView& view) {
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

// The last extracted plan, held here rather than re-fetched from the engine on
// every getter: a getter set that read a live model could hand out a wall array
// and an opening array from two different extractions.
scanengine::plan::PlanModel g_plan;
bool g_have_plan = false;

}  // namespace

extern "C" {

// --- lifecycle ---------------------------------------------------------------

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcCreate(JNIEnv*, jclass) {
  auto* p = new (std::nothrow) ProcessingEngine();
  if (p == nullptr) return 0;
  if (!p->ok()) {
    delete p;
    return 0;
  }
  return reinterpret_cast<jlong>(p);
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcDestroy(JNIEnv* env, jclass, jlong handle) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return;
  p->set_progress_callback(nullptr);
  delete p;
  if (g_progress.listener_global != nullptr) {
    env->DeleteGlobalRef(g_progress.listener_global);
    g_progress.listener_global = nullptr;
  }
  g_have_plan = false;
}

JNIEXPORT jstring JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcLastError(JNIEnv* env, jclass,
                                                                   jlong handle) {
  auto* p = HandleOf(handle);
  return env->NewStringUTF(p == nullptr ? "no processing engine" : p->last_error().c_str());
}

// --- jobs --------------------------------------------------------------------

// ROUND 8: `mount_phone_from_lidar` may be null (or the wrong length), which
// means "read the extrinsic out of the container's own manifest". A D6 project
// resolved through the wrong extrinsic produces a confidently wrong room, so
// the length is checked rather than assumed — a 16-double contract crossing
// JNI is exactly where a silent truncation would go unnoticed.
JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcSubmitPostProcess(
    JNIEnv* env, jclass, jlong handle, jstring lscan_dir, jdoubleArray mount_phone_from_lidar) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return 0;
  const std::string dir = ToStdString(env, lscan_dir);
  if (mount_phone_from_lidar == nullptr || env->GetArrayLength(mount_phone_from_lidar) != 16) {
    return static_cast<jlong>(p->submit_post_process(dir, nullptr));
  }
  double m[16];
  env->GetDoubleArrayRegion(mount_phone_from_lidar, 0, 16, m);
  return static_cast<jlong>(p->submit_post_process(dir, m));
}

// ROUND 8 (owner item 27c). Returned as a long bitfield plus counts rather than
// as an object, for the same reason MakePointPage exists on the other side of
// this file: one JNI call and no per-field FindClass/GetFieldID, on a path the
// Review screen hits every time a project is opened.
//
//   bit 0  opened            bit 3  has recorded map
//   bit 1  is a D6 project   bit 4  manifest carries the mount extrinsic
//   bit 2  has kPoseAr poses
JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcProbeProject(JNIEnv* env, jclass,
                                                                       jlong handle,
                                                                       jstring lscan_dir) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return 0;
  const auto probe = p->probe_project(ToStdString(env, lscan_dir));
  jlong flags = 0;
  if (probe.opened) flags |= 1;
  if (probe.is_d6) flags |= 2;
  if (probe.has_poses) flags |= 4;
  if (probe.has_recorded_map) flags |= 8;
  if (probe.has_mount) flags |= 16;
  return flags;
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcOpenRecordedCloud(JNIEnv* env, jclass,
                                                                           jlong handle,
                                                                           jstring lscan_dir) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return 0;
  return static_cast<jlong>(p->open_recorded_cloud(ToStdString(env, lscan_dir)));
}

// ── ROUND 13: "Process this scan" ─────────────────────────────────────────
//
// Returns a double[16] rather than a struct, for the same reason every other
// numeric result here does: a jdoubleArray needs no class lookup, no field
// IDs and no ProGuard rule, and the Kotlin side names the slots once.
//
//   0 ran            4 seams_refined   8  vertical_after   12 mount_verdict
//   1 map_written    5 points          9  end_gap_before   13 mount_impossible_fraction
//   2 sections       6 moved_m         10 end_gap_after    14 poses
//   3 seams          7 vertical_before 11 moved_deg        15 poses_untracked
//
// ROUND 15 item 57 APPENDS the ROUND 12 self-consistency ruler, which the
// engine now computes inside the same resolve for free:
//
//   16 selfcheck_measurable   18 selfcheck_floor_m     20 selfcheck_seconds
//   17 selfcheck_offset_m     19 selfcheck_windows     21 selfcheck_p90_m
//
// Appended rather than inserted, and the Kotlin decoder still accepts a
// 16-long array, so an older StitchResult.fromNative reading a newer array
// (or the reverse, in a partially-updated build) reads the same numbers in
// the same slots and simply has no self-check.
//
// It runs the WHOLE resolve, so it blocks for tens of seconds — the Kotlin
// side calls it on Dispatchers.IO and drives a progress bar from the callback
// below. Progress crosses back as a plain jfloat through a global ref to the
// callback object, attached to the calling thread (the pipeline calls the
// progress function on the thread inside run(), which IS this thread).
JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcReprocessD6(
    JNIEnv* env, jclass, jstring lscan_dir, jboolean refine, jobject progress) {
  jdoubleArray out = env->NewDoubleArray(22);
  if (out == nullptr) return nullptr;
  jdouble v[22] = {0};

  scan_reprocess_options opts{};
  opts.stitch_sections = 1;
  opts.densify_with_phone_imu = 1;
  opts.refine_seams = refine ? 1 : 0;
  opts.close_loops = 0;

  struct Ctx {
    JNIEnv* env;
    jobject cb;
    jmethodID mid;
  } ctx{env, nullptr, nullptr};
  if (progress != nullptr) {
    jclass cls = env->GetObjectClass(progress);
    ctx.mid = env->GetMethodID(cls, "onProgress", "(F)Z");
    env->DeleteLocalRef(cls);
    if (ctx.mid != nullptr) ctx.cb = progress;
  }

  scan_reprocess_result r{};
  scan_selfcheck_result sc{};
  const scan_error_t e = scan_lscan_reprocess_d6_ex(
      ToStdString(env, lscan_dir).c_str(), &opts, &r, &sc,
      ctx.cb == nullptr ? nullptr
                        : +[](float fraction, void* user) -> int32_t {
                            auto* c = static_cast<Ctx*>(user);
                            // CallBooleanMethodA, NOT the varargs form. C
                            // varargs promote a `float` to `double`, so
                            // `CallBooleanMethod(obj, mid, fraction)` hands a
                            // (F)Z method eight bytes where it expects four —
                            // the call then throws, this wrapper reads the
                            // exception as "the callback said stop", and the
                            // whole reprocess silently CANCELS. That is
                            // exactly how it failed on the emulator: `ran = 0`
                            // with no error anywhere, and only on the code
                            // path that passes a progress callback.
                            jvalue arg;
                            arg.f = fraction;
                            const jboolean go =
                                c->env->CallBooleanMethodA(c->cb, c->mid, &arg);
                            // A Kotlin callback that throws must not be
                            // swallowed into "keep going" — clear it and stop.
                            if (c->env->ExceptionCheck()) {
                              c->env->ExceptionClear();
                              return 0;
                            }
                            return go == JNI_TRUE ? 1 : 0;
                          },
      &ctx);

  if (e == SCAN_OK) {
    v[0] = r.ran ? 1 : 0;
    v[1] = r.map_written ? 1 : 0;
    v[2] = static_cast<jdouble>(r.sections);
    v[3] = static_cast<jdouble>(r.seams);
    v[4] = static_cast<jdouble>(r.seams_refined);
    v[5] = static_cast<jdouble>(r.points);
    v[6] = r.first_section_moved_m;
    v[7] = r.vertical_extent_before_m;
    v[8] = r.vertical_extent_after_m;
    v[9] = r.end_gap_before_m;
    v[10] = r.end_gap_after_m;
    v[11] = r.first_section_moved_deg;
    v[12] = static_cast<jdouble>(r.mount_verdict);
    v[13] = r.mount_impossible_fraction;
    v[14] = static_cast<jdouble>(r.poses);
    v[15] = static_cast<jdouble>(r.poses_untracked);
    v[16] = sc.measurable ? 1 : 0;
    v[17] = sc.nearest_offset_m;
    v[18] = sc.self_floor_m;
    v[19] = static_cast<jdouble>(sc.windows);
    v[20] = static_cast<jdouble>(sc.nearest_separation) * sc.window_seconds;
    v[21] = sc.p90_offset_m;
  }
  env->SetDoubleArrayRegion(out, 0, 22, v);
  return out;
}

// ── ROUND 15 item 56: the floor plan ──────────────────────────────────────
//
// Handle-less and static, like the reprocess above and for the same reason:
// it works on a DIRECTORY, owns its own PageStore inside the engine, and must
// not publish into the process-wide ProcessingEngine that Review is reading
// from. (That sharing is exactly what makes the existing nativeProcRunPlan
// path fragile — it slices whatever cloud happens to be loaded, with no
// project scoping at all.)
//
// Returns a double[24] of numbers; the three output paths come back through
// nativeProcPlanFilePaths so the strings do not have to be marshalled twice.
//
//   0 ran                  8 walls            16 largest_room_area_m2
//   1 mode                 9 walls_paired     17 extent_x_m
//   2 walls_from_floor_map 10 openings        18 extent_y_m
//   3 no_room_closed       11 doors           19 png_px_per_m
//   4 cloud_points         12 windows         20 png_scale_bar_m
//   5 band_points          13 rooms           21 png_w
//   6 map_points           14 wall_length_m   22 png_h
//   7 occupied_cells       15 room_area_m2    23 map_cells
namespace {
scan_plan_result g_plan_result{};
bool g_have_plan_result = false;
}  // namespace

JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcFloorPlan(
    JNIEnv* env, jclass, jstring lscan_dir, jdouble slice_min_m, jdouble slice_max_m,
    jdouble grid_res_m, jint png_max_px, jstring out_dir, jstring base_name, jstring title) {
  const std::string dir = ToStdString(env, lscan_dir);
  const std::string odir = out_dir == nullptr ? std::string() : ToStdString(env, out_dir);
  const std::string base = base_name == nullptr ? std::string() : ToStdString(env, base_name);
  const std::string ttl = title == nullptr ? std::string() : ToStdString(env, title);

  scan_plan_options opts{};
  opts.slice_min_m = slice_min_m;
  opts.slice_max_m = slice_max_m;
  opts.grid_res_m = grid_res_m;
  // ARCore's world is +Y up. The default is stated here as well as in the
  // engine because getting it wrong produces a plausible-looking drawing of a
  // VERTICAL slab, which is what the app shipped through nativeProcRunPlan.
  opts.up_axis = 1;
  opts.write_dxf = 1;
  opts.write_pdf = 1;
  opts.write_png = 1;
  opts.png_max_px = static_cast<uint32_t>(png_max_px);
  opts.out_dir = odir.empty() ? nullptr : odir.c_str();
  opts.base_name = base.empty() ? nullptr : base.c_str();
  opts.title = ttl.empty() ? nullptr : ttl.c_str();

  scan_plan_result r{};
  const scan_error_t e = scan_lscan_floor_plan(dir.c_str(), &opts, &r);
  g_plan_result = r;
  g_have_plan_result = (e == SCAN_OK);

  jdoubleArray out = env->NewDoubleArray(24);
  if (out == nullptr) return nullptr;
  jdouble v[24] = {0};
  if (e == SCAN_OK) {
    v[0] = r.ran ? 1 : 0;
    v[1] = static_cast<jdouble>(r.mode);
    v[2] = r.walls_from_floor_map ? 1 : 0;
    v[3] = r.no_room_closed ? 1 : 0;
    v[4] = static_cast<jdouble>(r.cloud_points);
    v[5] = static_cast<jdouble>(r.band_points);
    v[6] = static_cast<jdouble>(r.map_points);
    v[7] = static_cast<jdouble>(r.occupied_cells);
    v[8] = static_cast<jdouble>(r.walls);
    v[9] = static_cast<jdouble>(r.walls_paired);
    v[10] = static_cast<jdouble>(r.openings);
    v[11] = static_cast<jdouble>(r.doors);
    v[12] = static_cast<jdouble>(r.windows);
    v[13] = static_cast<jdouble>(r.rooms);
    v[14] = r.total_wall_length_m;
    v[15] = r.total_room_area_m2;
    v[16] = r.largest_room_area_m2;
    v[17] = r.extent_x_m;
    v[18] = r.extent_y_m;
    v[19] = r.png_px_per_m;
    v[20] = r.png_scale_bar_m;
    v[21] = static_cast<jdouble>(r.png_w);
    v[22] = static_cast<jdouble>(r.png_h);
    v[23] = static_cast<jdouble>(r.map_cells);
  }
  env->SetDoubleArrayRegion(out, 0, 24, v);
  return out;
}

// [png, pdf, dxf, cloudSource]. An entry is "" when that file was not
// written — a density-mode plan has no DXF and no PDF, deliberately.
JNIEXPORT jobjectArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanFilePaths(JNIEnv* env, jclass) {
  jclass str_cls = env->FindClass("java/lang/String");
  if (str_cls == nullptr) return nullptr;
  jobjectArray out = env->NewObjectArray(4, str_cls, nullptr);
  if (out == nullptr) return nullptr;
  const char* empty = "";
  const char* vals[4] = {
      g_have_plan_result ? g_plan_result.png_path : empty,
      g_have_plan_result ? g_plan_result.pdf_path : empty,
      g_have_plan_result ? g_plan_result.dxf_path : empty,
      g_have_plan_result ? g_plan_result.cloud_source : empty,
  };
  for (jsize i = 0; i < 4; ++i) {
    jstring s = env->NewStringUTF(vals[i]);
    env->SetObjectArrayElement(out, i, s);
    env->DeleteLocalRef(s);
  }
  return out;
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcHasStitchedCloud(JNIEnv* env, jclass,
                                                                          jstring lscan_dir) {
  return scan_lscan_has_stitched_cloud(ToStdString(env, lscan_dir).c_str()) != 0 ? JNI_TRUE
                                                                                : JNI_FALSE;
}

// ROUND 13 item 48. Returns [verdict, revolutions, points, extent_m,
// impossible_fraction, median_range_m].
JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcMountCheck(JNIEnv* env, jclass,
                                                                    jstring lscan_dir,
                                                                    jdouble window_seconds) {
  jdoubleArray out = env->NewDoubleArray(6);
  if (out == nullptr) return nullptr;
  jdouble v[6] = {static_cast<jdouble>(SCAN_MOUNT_NOT_MEASURABLE), 0, 0, 0, 0, 0};
  scan_mount_check_result m{};
  if (scan_lscan_mount_check(ToStdString(env, lscan_dir).c_str(), window_seconds, &m) == SCAN_OK) {
    v[0] = static_cast<jdouble>(m.verdict);
    v[1] = static_cast<jdouble>(m.revolutions);
    v[2] = static_cast<jdouble>(m.points);
    v[3] = m.median_revolution_extent_m;
    v[4] = m.impossible_fraction;
    v[5] = m.median_range_m;
  }
  env->SetDoubleArrayRegion(out, 0, 6, v);
  return out;
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcClearCloud(JNIEnv*, jclass, jlong handle) {
  auto* p = HandleOf(handle);
  if (p != nullptr) p->clear_cloud();
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcSubmitColorize(
    JNIEnv* env, jclass, jlong handle, jlong chain_from, jstring lscan_dir,
    jdoubleArray camera_from_lidar, jint sync_quality, jboolean allow_poor_sync,
    jlong clock_offset_ns) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return 0;
  double m[16];
  if (camera_from_lidar == nullptr || env->GetArrayLength(camera_from_lidar) != 16) return 0;
  env->GetDoubleArrayRegion(camera_from_lidar, 0, 16, m);
  return static_cast<jlong>(p->submit_colorize(static_cast<std::uint64_t>(chain_from),
                                               ToStdString(env, lscan_dir), m, sync_quality,
                                               allow_poor_sync == JNI_TRUE,
                                               static_cast<std::int64_t>(clock_offset_ns)));
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcSubmitExport(JNIEnv* env, jclass,
                                                                      jlong handle,
                                                                      jlong chain_from, jint format,
                                                                      jstring output_path,
                                                                      jstring crs_wkt,
                                                                      jstring crs_epsg) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return 0;
  return static_cast<jlong>(p->submit_export(static_cast<std::uint64_t>(chain_from), format,
                                             ToStdString(env, output_path),
                                             ToStdString(env, crs_wkt), ToStdString(env, crs_epsg)));
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcSubmitTransferExport(
    JNIEnv* env, jclass, jlong handle, jstring project_dir, jstring zip_path,
    jboolean include_results) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return 0;
  return static_cast<jlong>(p->submit_transfer_export(ToStdString(env, project_dir),
                                                      ToStdString(env, zip_path),
                                                      include_results == JNI_TRUE));
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcCancelJob(JNIEnv*, jclass, jlong handle,
                                                                   jlong job_id) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return JNI_FALSE;
  return p->cancel(static_cast<std::uint64_t>(job_id)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcJobs(JNIEnv* env, jclass, jlong handle) {
  auto* p = HandleOf(handle);
  if (p == nullptr || g_job_class == nullptr) return nullptr;
  const auto jobs = p->list_jobs();
  jobjectArray arr = env->NewObjectArray(static_cast<jsize>(jobs.size()), g_job_class, nullptr);
  if (arr == nullptr) return nullptr;
  for (jsize i = 0; i < static_cast<jsize>(jobs.size()); ++i) {
    const auto& j = jobs[static_cast<std::size_t>(i)];
    jstring stage = env->NewStringUTF(j.stage.c_str());
    jstring message = env->NewStringUTF(j.message.c_str());
    jobject obj = env->NewObject(g_job_class, g_job_ctor, static_cast<jlong>(j.id),
                                 static_cast<jint>(j.kind), static_cast<jint>(j.state),
                                 static_cast<jfloat>(j.progress), static_cast<jint>(j.error), stage,
                                 message);
    env->SetObjectArrayElement(arr, i, obj);
    env->DeleteLocalRef(stage);
    env->DeleteLocalRef(message);
    if (obj != nullptr) env->DeleteLocalRef(obj);
  }
  return arr;
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcSetJobProgressListener(JNIEnv* env, jclass,
                                                                                jlong handle,
                                                                                jobject listener) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return JNI_FALSE;
  if (g_progress.listener_global != nullptr) {
    env->DeleteGlobalRef(g_progress.listener_global);
    g_progress.listener_global = nullptr;
  }
  if (listener == nullptr) {
    p->set_progress_callback(nullptr);
    return JNI_TRUE;
  }
  jclass cls = env->GetObjectClass(listener);
  g_progress.method = env->GetMethodID(cls, "onJobProgress", "(JFI)V");
  env->DeleteLocalRef(cls);
  if (g_progress.method == nullptr) return JNI_FALSE;
  g_progress.listener_global = env->NewGlobalRef(listener);

  // Runs on the JobQueue worker thread — a thread the JVM never created — so
  // it must attach before touching anything Java, exactly like B2's event pump.
  p->set_progress_callback([](std::uint64_t id, float progress, int state) {
    if (g_progress.listener_global == nullptr || g_progress.method == nullptr) return;
    bool did_attach = false;
    JNIEnv* e = lidarscan_jni::AttachCurrentThreadOrGet(&did_attach);
    if (e == nullptr) return;
    e->CallVoidMethod(g_progress.listener_global, g_progress.method, static_cast<jlong>(id),
                      static_cast<jfloat>(progress), static_cast<jint>(state));
    if (e->ExceptionCheck()) {
      e->ExceptionDescribe();
      e->ExceptionClear();
    }
    if (did_attach) lidarscan_jni::g_jvm->DetachCurrentThread();
  });
  return JNI_TRUE;
}

// --- the produced cloud ------------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPageCount(JNIEnv*, jclass, jlong handle) {
  auto* p = HandleOf(handle);
  if (p == nullptr || p->points() == nullptr) return 0;
  return static_cast<jint>(p->points()->page_ids().size());
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPageIdAt(JNIEnv*, jclass, jlong handle,
                                                                  jint index) {
  auto* p = HandleOf(handle);
  if (p == nullptr || p->points() == nullptr) return -1;
  const auto ids = p->points()->page_ids();
  if (index < 0 || static_cast<std::size_t>(index) >= ids.size()) return -1;
  return static_cast<jint>(ids[static_cast<std::size_t>(index)]);
}

JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcGetPointPage(JNIEnv* env, jclass,
                                                                      jlong handle, jint page_id) {
  auto* p = HandleOf(handle);
  if (p == nullptr || p->points() == nullptr) return nullptr;
  return MakePointPage(env, p->points()->page_view(static_cast<scanengine::PageId>(page_id)));
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcTotalPoints(JNIEnv*, jclass,
                                                                     jlong handle) {
  auto* p = HandleOf(handle);
  return p == nullptr ? 0 : static_cast<jlong>(p->total_points());
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcMergedPageCount(JNIEnv*, jclass,
                                                                         jlong handle) {
  auto* p = HandleOf(handle);
  if (p == nullptr || p->merged_points() == nullptr) return 0;
  return static_cast<jint>(p->merged_points()->page_ids().size());
}

JNIEXPORT jint JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcMergedPageIdAt(JNIEnv*, jclass,
                                                                        jlong handle, jint index) {
  auto* p = HandleOf(handle);
  if (p == nullptr || p->merged_points() == nullptr) return -1;
  const auto ids = p->merged_points()->page_ids();
  if (index < 0 || static_cast<std::size_t>(index) >= ids.size()) return -1;
  return static_cast<jint>(ids[static_cast<std::size_t>(index)]);
}

JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcMergedGetPointPage(JNIEnv* env, jclass,
                                                                            jlong handle,
                                                                            jint page_id) {
  auto* p = HandleOf(handle);
  if (p == nullptr || p->merged_points() == nullptr) return nullptr;
  return MakePointPage(env,
                       p->merged_points()->page_view(static_cast<scanengine::PageId>(page_id)));
}

JNIEXPORT jlong JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcMergedTotalPoints(JNIEnv*, jclass,
                                                                           jlong handle) {
  auto* p = HandleOf(handle);
  if (p == nullptr || p->merged_points() == nullptr) return 0;
  return static_cast<jlong>(p->merged_points()->total_points());
}

// --- A12 floor plan ----------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcRunPlan(
    JNIEnv*, jclass, jlong handle, jfloat z_min, jfloat z_max, jfloat grid_res,
    jboolean snap_orthogonal, jfloat snap_tol_deg, jint min_cell_points, jboolean sill_check,
    jboolean detect_rooms, jboolean detect_openings) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return JNI_FALSE;
  scanengine::plan::PlanOptions opts;
  opts.slice.z_min_m = z_min;
  opts.slice.z_max_m = z_max;
  opts.slice.grid_res_m = grid_res;
  opts.slice.snap_orthogonal = snap_orthogonal == JNI_TRUE;
  opts.slice.snap_tolerance_deg = snap_tol_deg;
  opts.slice.min_cell_points = static_cast<std::uint32_t>(min_cell_points);
  opts.slice.window_sill_check = sill_check == JNI_TRUE;
  opts.rooms.enabled = detect_rooms == JNI_TRUE;
  opts.openings.enabled = detect_openings == JNI_TRUE;

  scanengine::plan::PlanModel model;
  const bool ok = p->run_plan(opts, &model);
  if (!ok) {
    g_have_plan = false;
    return JNI_FALSE;
  }
  g_plan = std::move(model);
  g_have_plan = true;
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcCancelPlan(JNIEnv*, jclass, jlong handle) {
  auto* p = HandleOf(handle);
  if (p != nullptr) p->cancel_plan();
}

JNIEXPORT jfloat JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanProgress(JNIEnv*, jclass,
                                                                      jlong handle) {
  auto* p = HandleOf(handle);
  return p == nullptr ? 0.f : p->plan_progress();
}

/*
 * The flat-array layouts. Each is documented HERE and mirrored in
 * NativePlanArrays.kt; the Kotlin side asserts the per-wall/opening/room stride
 * so a layout drift is a test failure, not a silently transposed plan.
 *
 *   walls   doubles, stride 8: ax, ay, bx, by, thickness_m, rms_residual_m,
 *                              coverage, confidence
 *           ints,    stride 4: id, evidence, support_cells, snapped
 *   openings doubles, stride 5: ax, ay, bx, by, width_m
 *           ints,    stride 4: id, wall_id, kind, sill
 *           (confidence rides in a separate doubles array to keep the stride a
 *            round number — see below)
 *   rooms   doubles, stride 5: area_m2, perimeter_m, cx, cy, confidence
 *           ints,    stride 3: id, fully_measured, polygon_vertex_count
 *           polygon doubles: every room's vertices concatenated, x,y — walked
 *                            with the per-room vertex counts above.
 */

JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanWallsD(JNIEnv* env, jclass, jlong) {
  std::vector<double> v;
  if (g_have_plan) {
    v.reserve(g_plan.walls.size() * 8);
    for (const auto& w : g_plan.walls) {
      v.push_back(w.a.x);
      v.push_back(w.a.y);
      v.push_back(w.b.x);
      v.push_back(w.b.y);
      v.push_back(w.thickness_m);
      v.push_back(w.rms_residual_m);
      v.push_back(w.coverage);
      v.push_back(static_cast<double>(w.confidence));
    }
  }
  return NewDoubleArray(env, v);
}

JNIEXPORT jintArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanWallsI(JNIEnv* env, jclass, jlong) {
  std::vector<jint> v;
  if (g_have_plan) {
    v.reserve(g_plan.walls.size() * 4);
    for (const auto& w : g_plan.walls) {
      v.push_back(static_cast<jint>(w.id));
      v.push_back(static_cast<jint>(w.evidence));
      v.push_back(static_cast<jint>(w.support_cells));
      v.push_back(w.snapped ? 1 : 0);
    }
  }
  return NewIntArray(env, v);
}

JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanOpeningsD(JNIEnv* env, jclass, jlong) {
  std::vector<double> v;
  if (g_have_plan) {
    v.reserve(g_plan.openings.size() * 6);
    for (const auto& o : g_plan.openings) {
      v.push_back(o.a.x);
      v.push_back(o.a.y);
      v.push_back(o.b.x);
      v.push_back(o.b.y);
      v.push_back(o.width_m);
      v.push_back(static_cast<double>(o.confidence));
    }
  }
  return NewDoubleArray(env, v);
}

JNIEXPORT jintArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanOpeningsI(JNIEnv* env, jclass, jlong) {
  std::vector<jint> v;
  if (g_have_plan) {
    v.reserve(g_plan.openings.size() * 4);
    for (const auto& o : g_plan.openings) {
      v.push_back(static_cast<jint>(o.id));
      v.push_back(static_cast<jint>(o.wall_id));
      v.push_back(static_cast<jint>(o.kind));
      v.push_back(static_cast<jint>(o.sill));
    }
  }
  return NewIntArray(env, v);
}

JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanRoomsD(JNIEnv* env, jclass, jlong) {
  std::vector<double> v;
  if (g_have_plan) {
    v.reserve(g_plan.rooms.size() * 5);
    for (const auto& r : g_plan.rooms) {
      v.push_back(r.area_m2);
      v.push_back(r.perimeter_m);
      v.push_back(r.centroid.x);
      v.push_back(r.centroid.y);
      v.push_back(static_cast<double>(r.confidence));
    }
  }
  return NewDoubleArray(env, v);
}

JNIEXPORT jintArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanRoomsI(JNIEnv* env, jclass, jlong) {
  std::vector<jint> v;
  if (g_have_plan) {
    v.reserve(g_plan.rooms.size() * 3);
    for (const auto& r : g_plan.rooms) {
      v.push_back(static_cast<jint>(r.id));
      v.push_back(r.fully_measured ? 1 : 0);
      v.push_back(static_cast<jint>(r.polygon.size()));
    }
  }
  return NewIntArray(env, v);
}

JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanRoomPolygons(JNIEnv* env, jclass,
                                                                          jlong) {
  std::vector<double> v;
  if (g_have_plan) {
    for (const auto& r : g_plan.rooms) {
      for (const auto& p : r.polygon) {
        v.push_back(p.x);
        v.push_back(p.y);
      }
    }
  }
  return NewDoubleArray(env, v);
}

JNIEXPORT jobjectArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanRoomLabels(JNIEnv* env, jclass, jlong) {
  jclass str_class = env->FindClass("java/lang/String");
  const jsize n = g_have_plan ? static_cast<jsize>(g_plan.rooms.size()) : 0;
  jobjectArray arr = env->NewObjectArray(n, str_class, nullptr);
  for (jsize i = 0; i < n; ++i) {
    jstring s = env->NewStringUTF(g_plan.rooms[static_cast<std::size_t>(i)].label.c_str());
    env->SetObjectArrayElement(arr, i, s);
    env->DeleteLocalRef(s);
  }
  env->DeleteLocalRef(str_class);
  return arr;
}

/*
 * Summary layout, doubles:
 *   [0..3]  bounds min_x, min_y, max_x, max_y
 *   [4]     bounds.valid (0/1)
 *   [5]     slice_z_min_m      [6] slice_z_max_m      [7] grid_res_m
 *   [8]     stats.points_considered   [9]  stats.points_in_band
 *   [10]    stats.grid_w              [11] stats.grid_h
 *   [12]    stats.occupied_cells      [13] stats.ransac_lines
 *   [14]    stats.snapped_walls       [15] stats.paired_walls
 *   [16]    stats.dominant_angle_rad  [17] stats.total_wall_length_m
 *   [18]    stats.total_room_area_m2
 *
 * The counts ride as doubles rather than in a second array on purpose: every
 * one of them is an exact integer well below 2^53 (a phone would need to see
 * 9x10^15 points), so there is no precision to lose, and one array cannot
 * disagree with itself.
 */
JNIEXPORT jdoubleArray JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcPlanSummary(JNIEnv* env, jclass, jlong) {
  std::vector<double> v(19, 0.0);
  if (g_have_plan) {
    v[0] = g_plan.bounds.min_x;
    v[1] = g_plan.bounds.min_y;
    v[2] = g_plan.bounds.max_x;
    v[3] = g_plan.bounds.max_y;
    v[4] = g_plan.bounds.valid ? 1.0 : 0.0;
    v[5] = g_plan.slice_z_min_m;
    v[6] = g_plan.slice_z_max_m;
    v[7] = g_plan.grid_res_m;
    v[8] = static_cast<double>(g_plan.stats.points_considered);
    v[9] = static_cast<double>(g_plan.stats.points_in_band);
    v[10] = static_cast<double>(g_plan.stats.grid_w);
    v[11] = static_cast<double>(g_plan.stats.grid_h);
    v[12] = static_cast<double>(g_plan.stats.occupied_cells);
    v[13] = static_cast<double>(g_plan.stats.ransac_lines);
    v[14] = static_cast<double>(g_plan.stats.snapped_walls);
    v[15] = static_cast<double>(g_plan.stats.paired_walls);
    v[16] = g_plan.stats.dominant_angle_rad;
    v[17] = g_plan.stats.total_wall_length_m;
    v[18] = g_plan.stats.total_room_area_m2;
  }
  return NewDoubleArray(env, v);
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcWritePlanDxf(JNIEnv* env, jclass,
                                                                      jlong handle, jstring path) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return JNI_FALSE;
  return p->write_plan_dxf(ToStdString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcWritePlanPdf(JNIEnv* env, jclass,
                                                                      jlong handle, jstring path,
                                                                      jstring title,
                                                                      jstring project,
                                                                      jstring date) {
  auto* p = HandleOf(handle);
  if (p == nullptr) return JNI_FALSE;
  return p->write_plan_pdf(ToStdString(env, path), ToStdString(env, title),
                           ToStdString(env, project), ToStdString(env, date))
             ? JNI_TRUE
             : JNI_FALSE;
}

// --- A13 merge ---------------------------------------------------------------

/*
 * `georef` layout, doubles, stride 23 per session:
 *   [0]      valid (0/1)
 *   [1]      converged (0/1)
 *   [2]      epsg
 *   [3..18]  global_from_local, ROW-MAJOR
 *   [19]     enu origin lat_deg   [20] lon_deg   [21] height_m (ELLIPSOIDAL)
 *   [22]     horizontal_sigma_m
 */
JNIEXPORT jobject JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcRunMerge(
    JNIEnv* env, jclass, jlong handle, jobjectArray lscan_dirs, jobjectArray provenance_ids,
    jlongArray chain_from_jobs, jdoubleArray georef, jstring out_ply_path, jobject listener) {
  auto* p = HandleOf(handle);
  if (p == nullptr || g_merge_summary_class == nullptr) return nullptr;

  const jsize n = env->GetArrayLength(lscan_dirs);
  std::vector<lidarscan_jni::MergeSessionInput> inputs;
  inputs.reserve(static_cast<std::size_t>(n));

  std::vector<jlong> chains(static_cast<std::size_t>(n), 0);
  if (chain_from_jobs != nullptr && env->GetArrayLength(chain_from_jobs) == n) {
    env->GetLongArrayRegion(chain_from_jobs, 0, n, chains.data());
  }
  std::vector<double> g(static_cast<std::size_t>(n) * 23, 0.0);
  if (georef != nullptr && env->GetArrayLength(georef) == n * 23) {
    env->GetDoubleArrayRegion(georef, 0, n * 23, g.data());
  }

  for (jsize i = 0; i < n; ++i) {
    lidarscan_jni::MergeSessionInput in;
    auto dir = static_cast<jstring>(env->GetObjectArrayElement(lscan_dirs, i));
    auto pid = static_cast<jstring>(env->GetObjectArrayElement(provenance_ids, i));
    in.lscan_dir = ToStdString(env, dir);
    in.provenance_id = ToStdString(env, pid);
    if (dir != nullptr) env->DeleteLocalRef(dir);
    if (pid != nullptr) env->DeleteLocalRef(pid);
    in.chain_from_job = static_cast<std::uint64_t>(chains[static_cast<std::size_t>(i)]);
    const double* row = &g[static_cast<std::size_t>(i) * 23];
    in.georef_valid = row[0] != 0.0;
    in.georef_converged = row[1] != 0.0;
    in.epsg = static_cast<int>(row[2]);
    for (int k = 0; k < 16; ++k) in.global_from_local[k] = row[3 + k];
    in.enu_origin_lat_deg = row[19];
    in.enu_origin_lon_deg = row[20];
    in.enu_origin_height_m = row[21];
    in.horizontal_sigma_m = row[22];
    inputs.push_back(std::move(in));
  }

  jmethodID on_progress = nullptr;
  if (listener != nullptr) {
    jclass cls = env->GetObjectClass(listener);
    on_progress = env->GetMethodID(cls, "onMergeProgress", "(FLjava/lang/String;)V");
    env->DeleteLocalRef(cls);
  }

  // run_merge() blocks on THIS thread — which is a Kotlin coroutine's worker,
  // already attached to the JVM — so the progress callback can use `env`
  // directly with no attach dance.
  auto summary = p->run_merge(
      inputs, ToStdString(env, out_ply_path),
      [&](float f, const char* label) {
        if (listener == nullptr || on_progress == nullptr) return;
        jstring s = env->NewStringUTF(label != nullptr ? label : "");
        env->CallVoidMethod(listener, on_progress, static_cast<jfloat>(f), s);
        if (env->ExceptionCheck()) {
          env->ExceptionDescribe();
          env->ExceptionClear();
        }
        env->DeleteLocalRef(s);
      });

  jstring blocker = env->NewStringUTF(summary.blocker.c_str());
  jstring message = env->NewStringUTF(summary.message.c_str());
  jobject obj = env->NewObject(
      g_merge_summary_class, g_merge_summary_ctor, summary.ok ? JNI_TRUE : JNI_FALSE,
      static_cast<jint>(summary.sessions_aligned), static_cast<jint>(summary.sessions_skipped),
      static_cast<jint>(summary.pairs_refined), static_cast<jint>(summary.pairs_converged),
      static_cast<jint>(summary.pairs_low_overlap), static_cast<jfloat>(summary.worst_rms_m),
      static_cast<jfloat>(summary.worst_overlap), static_cast<jlong>(summary.input_points),
      static_cast<jlong>(summary.merged_points), summary.epsg_mismatch ? JNI_TRUE : JNI_FALSE,
      blocker, message);
  env->DeleteLocalRef(blocker);
  env->DeleteLocalRef(message);
  return obj;
}

JNIEXPORT void JNICALL
Java_com_lidarscan_app_engine_ScanEngineNative_nativeProcCancelMerge(JNIEnv*, jclass, jlong handle) {
  auto* p = HandleOf(handle);
  if (p != nullptr) p->cancel_merge();
}

}  // extern "C"
