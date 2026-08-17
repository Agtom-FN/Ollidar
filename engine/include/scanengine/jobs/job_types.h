// job_types.h — the concrete job model that drives jobs/job_queue.h.
//
// jobs/job.h is A1's SEAM: JobMode (local/cloud/extract-for-transfer, §3.8's
// "where"), JobRequest/JobStatus/JobRunner. This header is A15's concrete
// answer to "what actually runs" — five job KINDS, each a real local
// pipeline invocation:
//
//   kPostProcess    drives A7's PostSlamPipeline on a .lscan directory.
//   kColorize       drives A11's Colorizer — SEAM: see ColorizeParams below,
//                    A11 has landed only the interface (color/colorize.h),
//                    not an implementation, so a Colorize job needs a
//                    caller-supplied Colorizer* until it does.
//   kExportPoints   drives A9's export_points().
//   kTransferExport drives A5's zip_export() (§3.8 "extract for transfer").
//   kCloudSubmit    drives jobs/cloud_submit.h's HttpTransport-based client
//                    (§3.8 "cloud").
//
// A JobSpec is submitted; a Job is what JobQueue reports back (id, kind,
// state, progress + stage label, error) — see jobs/job_queue.h. Progress
// updates are ALSO republished as EventType::kJobProgress on the engine
// event bus when a queue is constructed with one (jobs/job.h's contract);
// this header only defines the vocabulary, not the publishing.
//
// Chaining ("jobs may chain exports", docs/A9-export.md): kExportPoints,
// kColorize and kCloudSubmit can each either take a ready-made input
// directly (a PageStore / a zip path) or a `chain_from` job id, resolved by
// JobQueue against a prior job's produced artifact (a PostProcess job's
// PageStore, or a TransferExport job's zip). See job_queue.h's
// "produced_store()" / "produced_zip_path()".
//
// Owner: A15.
#ifndef SCANENGINE_JOBS_JOB_TYPES_H
#define SCANENGINE_JOBS_JOB_TYPES_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/color/colorize.h"
#include "scanengine/core/error.h"
#include "scanengine/core/types.h"
#include "scanengine/export/exporter.h"
#include "scanengine/jobs/cloud_submit.h"
#include "scanengine/jobs/http_transport.h"
#include "scanengine/slam/post/post_pipeline.h"

namespace scanengine {
namespace jobs {

enum class JobKind : std::uint8_t {
  kPostProcess = 0,
  kColorize = 1,
  kExportPoints = 2,
  kTransferExport = 3,
  kCloudSubmit = 4,
};

const char* to_string(JobKind k) noexcept;

// Deliberately the five states the task calls for, no more. "Cancelled" is
// not a sixth state: a cancelled job settles into kFailed with
// error == ScanError::kCancelled, exactly the convention SCAN_TRY/Status
// already use everywhere else in the engine (core/error.h) — a UI does not
// need a second code path to notice a cancellation, it reads the error.
enum class JobState : std::uint8_t {
  kQueued = 0,
  kRunning = 1,
  kCancelling = 2,  // cancel() was called while Running; cooperative unwind in flight
  kDone = 3,
  kFailed = 4,
};

const char* to_string(JobState s) noexcept;

// --- per-kind parameters -----------------------------------------------

struct PostProcessParams {
  std::string lscan_dir;
  post::PostConfig config;  // config.store is ignored — the job owns/creates
                             // the PageStore below (or uses `store` if set)
                             // so a chained export can still reach it after
                             // the PostSlamPipeline itself is destroyed.
  std::shared_ptr<PageStore> store;  // optional: publish into a caller-owned
                                      // store (e.g. the live Engine's, per
                                      // docs/A7-post.md §7); null = the job
                                      // creates one, kept alive for chaining.

  // --- ROUND 8, additive: the D6 route ------------------------------------
  //
  // A `.lscan` holding kD6Raw and no Mid-360 chunks runs post::D6ResolvePipeline
  // instead of A7's PostSlamPipeline — see jobs/local_runner.cpp, and
  // slam/post/d6_resolve.h for why the two are different pipelines and not one
  // with a branch. `config` above is ignored on that route (there is no
  // odometry to configure) and these two fields are ignored on A7's.
  //
  // The extrinsic is OPTIONAL: unset, the pipeline reads `"mountCalibration"`
  // out of the container's own manifest. Set, it wins — the Android app holds
  // the operator's persisted mount re-zero and that is fresher than a manifest
  // written before the re-zero was taken.
  bool d6_mount_valid = false;
  double d6_mount[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // row-major phone_from_lidar
};

// `colorizer` is caller-injected (a `color::PointColorizer*` in production;
// a test double is enough to drive the abstract `Colorizer` seam on its
// own). A null colorizer makes the job fail fast with
// ScanError::kUnimplemented rather than silently no-op'ing.
//
// jobs/local_runner.h's run_colorize() special-cases a `color::PointColorizer`
// (dynamic_cast) to get real cancellation and fine-grained progress out of
// A11's concrete implementation, and to call its own `load_keyframes()`
// rather than require the caller to hand-populate `keyframes`; a plain
// `Colorizer*` (any other implementation, or a test fake) still works
// through the abstract seam, with the degraded progress/cancellation
// documented in local_runner.h.
struct ColorizeParams {
  Colorizer* colorizer = nullptr;
  double camera_from_lidar[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // The .lscan directory to load keyframes from — used only when `colorizer`
  // is a color::PointColorizer AND `keyframes` is empty (its own
  // load_keyframes(), which also installs a FileImageSource). Ignored
  // otherwise.
  std::string lscan_dir;
  std::vector<Keyframe> keyframes;  // scanengine::Keyframe, color/colorize.h;
                                     // manual override, or required for a
                                     // plain (non-PointColorizer) Colorizer.
  std::shared_ptr<PageStore> store;
  std::uint64_t chain_from = 0;  // 0 = use `store`; else resolve from that
                                  // job's PageStore (must be a finished
                                  // kPostProcess job).
};

struct ExportPointsParams {
  std::shared_ptr<PageStore> store;
  std::uint64_t chain_from = 0;  // 0 = use `store`; else resolve from that
                                  // job's PageStore (kPostProcess/kColorize).
  std::vector<StreamId> streams;  // empty = every page, per export_points()
  ExportFormat format = ExportFormat::kPlyBinary;
  std::string output_path;
  ExportOptions options;
};

struct TransferExportParams {
  std::string project_dir;  // the .lscan directory to package
  std::string zip_path;     // destination .lscan.zip
  // false: stage only manifest.json + streams/ into a temp dir before
  // zipping (a "raw capture" transfer with no processed results attached).
  // A5's zip_export() has no include/exclude filter (record/zip.h is not
  // this task's file), so the filtering happens by staging a copy first —
  // see jobs/transfer.h.
  bool include_results = true;
};

struct CloudSubmitParams {
  HttpTransport* transport = nullptr;  // required; see jobs/http_transport.h
  CloudSubmitConfig cloud_config;
  std::string local_zip_path;  // already-built .lscan.zip; empty + chain_from
                                // != 0 resolves it from a TransferExport job
  std::uint64_t chain_from = 0;
  std::string result_dir;  // where the downloaded result is written
};

struct JobSpec {
  JobKind kind = JobKind::kPostProcess;
  int priority = 0;  // higher runs first; FIFO among equal priorities

  PostProcessParams post;
  ColorizeParams colorize;
  ExportPointsParams export_points;
  TransferExportParams transfer;
  CloudSubmitParams cloud;
};

// What JobQueue::status()/list()/on_completion() hand back.
struct Job {
  std::uint64_t id = 0;
  JobKind kind = JobKind::kPostProcess;
  JobState state = JobState::kQueued;
  int priority = 0;
  float progress = 0.f;      // 0..1, monotone non-decreasing within a run
  std::string stage;         // stable label, e.g. a PostStage::to_string()
  ScanError error = ScanError::kOk;
  std::string message;       // human detail; "" unless error != kOk
};

}  // namespace jobs
}  // namespace scanengine

#endif  // SCANENGINE_JOBS_JOB_TYPES_H
