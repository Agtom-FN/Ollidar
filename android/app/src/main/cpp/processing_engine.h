// processing_engine.h — B6/B11/B12's engine-side helper: A15's job queue,
// A12's floor-plan extraction and A13's georeferenced merge, behind one small
// C++ object the JNI layer drives.
//
// WHY THIS LINKS THE ENGINE'S C++ API AND NOT capi/scanengine_c.h
//
// A15's queue has NO C surface, deliberately, and the header says so in as
// many words: scanengine_c.h's kJobProgress comment ends "an app that wants a
// queue should drive a Colorize job through the C++ jobs::JobQueue instead
// (there is no C surface for the queue at ABI 4)", and capi/scanengine_c.cpp's
// own test-seam comment repeats it ("ABI 4 deliberately gives A15's job queue
// no C entry point — an app drives it through Engine::jobs()"). The same is
// true of plan/ (A12) and merge/ (A13): neither has a single symbol in the C
// ABI. So this file follows the pattern B4 established for
// `lscan::ReplaySource` (replay_engine.h), B8 for `color::KeyframeIndexWriter`
// (keyframe_writer.h) and B3 for `Mid360Config` (mid360_probe.h) — link
// `scanengine` directly, which is the same static library the C-ABI path
// already links.
//
// ONE STANDALONE scanengine::Engine, NOT THE CAPTURE ONE
//
// The capture engine lives behind an opaque `scan_engine*` whose wrapping
// `EngineHandle` is file-local to scanengine_c.cpp, so there is no route from
// it to the `scanengine::Engine&` that owns `jobs()`. That is the same wall B4
// and B3 hit. A separate Engine is also the RIGHT shape here rather than a
// workaround: processing runs from a `.lscan` on disk (A7: "the input is a
// .lscan directory... the post run is not a refinement of the live result — it
// is a second, better run from the same bytes"), so it needs no devices, no
// session and no relationship to whatever the capture engine is currently
// doing. An operator can post-process yesterday's project while today's is
// still recording.
//
// Using a full Engine rather than a bare JobQueue buys three things: its
// PageStore is where every produced cloud lands (so B4's renderer draws the
// processed result through the same page-read path it already uses for live
// capture), its EventBus is what JobQueue publishes kJobProgress on, and
// `Engine::jobs()` constructs the queue lazily and joins its worker thread
// before the store it appends into is destroyed.
//
// THREADING. Every public method is safe from any thread (JobQueue's own
// methods are, and the rest is guarded by `m_`). The progress callback fires
// on the JobQueue worker thread; `run_plan()`/`run_merge()` BLOCK on the
// calling thread (they are driven from a Kotlin coroutine, not the UI thread).
//
// Owner: B6/B11/B12. Deliberately free of any JNI type, like keyframe_writer.h,
// so it stays host-compilable.
#ifndef LIDARSCAN_JNI_PROCESSING_ENGINE_H
#define LIDARSCAN_JNI_PROCESSING_ENGINE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/color/colorizer.h"
#include "scanengine/core/engine.h"
#include "scanengine/jobs/job_queue.h"
#include "scanengine/jobs/job_types.h"
#include "scanengine/merge/merge.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/slam/post/post_pipeline.h"

namespace lidarscan_jni {

// A flattened jobs::Job, so the JNI marshalling has no engine types in it.
struct JobSnapshot {
  std::uint64_t id = 0;
  int kind = 0;
  int state = 0;
  float progress = 0.f;
  std::int32_t error = 0;
  std::string stage;
  std::string message;
};

// What run_merge() reports back. Deliberately a summary and not A13's whole
// MergeReport: the Android flow is §3.10's "Android offers georeferenced
// auto-merge only", so the pair-by-pair residual table belongs to C6's merge
// workbench, not here.
struct MergeSummary {
  bool ok = false;
  std::uint32_t sessions_aligned = 0;
  std::uint32_t sessions_skipped = 0;
  std::uint32_t pairs_refined = 0;
  std::uint32_t pairs_converged = 0;
  std::uint32_t pairs_low_overlap = 0;
  float worst_rms_m = 0.f;
  float worst_overlap = 0.f;
  std::uint64_t input_points = 0;
  std::uint64_t merged_points = 0;
  bool epsg_mismatch = false;
  std::string blocker;   // empty when nothing went wrong
  std::string message;   // human detail, always set
};

// One session handed to run_merge().
struct MergeSessionInput {
  std::string lscan_dir;
  std::string provenance_id;
  // 0 = post-process this session now; non-zero = reuse the PageStore a
  // finished kPostProcess job in THIS ProcessingEngine already produced.
  std::uint64_t chain_from_job = 0;

  // A10's answer for this session, read back out of the project manifest.
  bool georef_valid = false;
  bool georef_converged = false;
  int epsg = 0;
  double global_from_local[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  double enu_origin_lat_deg = 0.0;
  double enu_origin_lon_deg = 0.0;
  double enu_origin_height_m = 0.0;
  double horizontal_sigma_m = 0.0;
};

class ProcessingEngine {
 public:
  ProcessingEngine();
  ~ProcessingEngine();

  ProcessingEngine(const ProcessingEngine&) = delete;
  ProcessingEngine& operator=(const ProcessingEngine&) = delete;

  bool ok() const { return engine_ != nullptr; }
  std::string last_error();

  // --- A15 jobs ------------------------------------------------------------
  //
  // Each returns the job id, or 0 on a submit-time refusal (last_error() says
  // why). kCloudSubmit is deliberately absent — see the class comment in
  // processing_jni.cpp and android/NOTES.md: the cloud client is Kotlin.
  std::uint64_t submit_post_process(const std::string& lscan_dir);

  // `chain_from` must be a finished kPostProcess job. `sync_quality` is
  // SCAN_SYNC_* (A4's SyncQuality) and FAILS CLOSED at 0.
  std::uint64_t submit_colorize(std::uint64_t chain_from, const std::string& lscan_dir,
                                const double camera_from_lidar[16], int sync_quality,
                                bool allow_poor_sync, std::int64_t camera_clock_offset_ns);

  // `format` is ExportFormat's underlying value (0 ply, 1 las, 2 pcd).
  std::uint64_t submit_export(std::uint64_t chain_from, int format, const std::string& output_path,
                              const std::string& crs_wkt, const std::string& crs_epsg);

  std::uint64_t submit_transfer_export(const std::string& project_dir, const std::string& zip_path,
                                       bool include_results);

  bool cancel(std::uint64_t job_id);
  std::vector<JobSnapshot> list_jobs() const;
  JobSnapshot job_status(std::uint64_t job_id) const;

  // Fired from the JobQueue worker thread on every progress tick, via the
  // Engine's own EventBus (EventType::kJobProgress — the payload A15 §7.1
  // asked for and INT-34 landed). Quick, no re-entry.
  using ProgressFn = std::function<void(std::uint64_t id, float progress, int state)>;
  void set_progress_callback(ProgressFn fn);

  // --- the produced cloud --------------------------------------------------
  //
  // Every job publishes into this Engine's own PageStore, which is what makes
  // the processed cloud renderable through the page-read path B4 already has.
  scanengine::PageStore* points();
  std::uint64_t total_points() const;
  bool has_cloud() const;

  // --- A12 floor plan ------------------------------------------------------
  //
  // BLOCKING, on the calling thread. Returns false and sets last_error() when
  // there is no cloud or the extraction fails; an EMPTY plan from a real cloud
  // is a success (the Kotlin side has the diagnosis copy for that case).
  bool run_plan(const scanengine::plan::PlanOptions& opts, scanengine::plan::PlanModel* out);
  void cancel_plan();
  float plan_progress() const;

  // Writes the LAST extracted plan. Kept separate from run_plan() so the
  // viewer can be looked at before anything is written, and so DXF and PDF
  // come from the same model rather than two extractions (A12: "There is no
  // 'DXF plan' and 'PDF plan'").
  bool write_plan_dxf(const std::string& path);
  bool write_plan_pdf(const std::string& path, const std::string& title,
                      const std::string& project, const std::string& date);
  bool has_plan() const;

  // --- A13 georeferenced merge --------------------------------------------
  //
  // BLOCKING. Post-processes any session without a `chain_from_job`, aligns
  // every converged one through the shared CRS, refines, builds and publishes
  // the merged cloud into `merged_store` (a store of its own — A13: "a merged
  // product normally gets its own store"). `out_ply_path` may be empty.
  MergeSummary run_merge(const std::vector<MergeSessionInput>& sessions,
                         const std::string& out_ply_path,
                         const std::function<void(float, const char*)>& progress);
  void cancel_merge();

  // The merged cloud's own store, for rendering. Null until run_merge() succeeds.
  scanengine::PageStore* merged_points();

 private:
  void set_error(std::string msg);

  mutable std::mutex m_;
  std::unique_ptr<scanengine::Engine> engine_;
  std::shared_ptr<scanengine::PageStore> store_;  // aliasing: engine_->points()
  std::string last_error_;

  // Colorizers outlive their submit() call: ColorizeParams::colorizer is a
  // BORROWED pointer the queue dereferences on the worker thread.
  std::map<std::uint64_t, std::unique_ptr<scanengine::color::PointColorizer>> colorizers_;

  ProgressFn progress_;
  scanengine::SubscriptionId sub_ = 0;

  std::unique_ptr<scanengine::plan::PlanModel> plan_;
  scanengine::plan::PlanCancelToken plan_cancel_;
  std::atomic<float> plan_progress_{0.f};

  scanengine::post::CancelToken merge_cancel_;
  std::unique_ptr<scanengine::PageStore> merged_store_;
  // The merge sessions' clouds are NON-OWNING spans into these stores
  // (merge/session.h: "The caller must keep that memory alive for as long as
  // the MergeProject is used"), so they are held for the object's lifetime,
  // not the call's.
  std::vector<std::shared_ptr<scanengine::PageStore>> merge_inputs_;
};

}  // namespace lidarscan_jni

#endif  // LIDARSCAN_JNI_PROCESSING_ENGINE_H
