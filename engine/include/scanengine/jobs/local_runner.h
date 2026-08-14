// local_runner.h — Tech Spec §3.8's "Local" row: run the real pipelines
// in-process, on this device.
//
// Each function here drives exactly one existing seam/implementation and
// nothing else — no retry, no chaining, no event-bus publishing. JobQueue
// (job_queue.h) is the orchestrator: it resolves chained inputs, wraps these
// calls with a per-job progress/cancel bridge, and republishes progress as
// EventType::kJobProgress. Kept separate so each of these is independently
// unit-testable without a queue or a worker thread in sight — the same
// reason PostSlamPipeline and export_points() themselves take no Engine.
//
// Owner: A15.
#ifndef SCANENGINE_JOBS_LOCAL_RUNNER_H
#define SCANENGINE_JOBS_LOCAL_RUNNER_H

#include <functional>
#include <memory>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/jobs/job_types.h"

namespace scanengine {
namespace jobs {

// Drives A7's PostSlamPipeline (docs/A7-post.md). `progress_cb` receives the
// pipeline's own PostProgress records unmodified — JobQueue is what turns
// `.fraction`/`.label` into a Job's progress/stage. On success `*out_store`
// holds the PageStore the pipeline published into (params.store if set,
// else one the pipeline created) so a chained kColorize/kExportPoints job
// can reach it after this call returns and the PostSlamPipeline itself has
// been destroyed.
Status run_post_process(const PostProcessParams& params, post::CancelToken* cancel_token,
                         post::PostProgressFn progress_cb, std::shared_ptr<PageStore>* out_store);

// Drives A11's Colorizer interface (color/colorize.h). Returns
// ScanError::kUnimplemented immediately if `params.colorizer` is null —
// never silently no-ops.
//
// Two paths, chosen by a dynamic_cast to A11's concrete implementation:
//
//   color::PointColorizer (color/colorizer.h, landed alongside this task):
//     real cancellation via `cancel_token` (post::CancelToken — the same
//     type A7's pipeline uses) and fine-grained progress via
//     PointColorizer::set_progress_callback(), plus load_keyframes(lscan_dir)
//     when the caller left `params.keyframes` empty. A kNotFound from
//     load_keyframes() (no camera for this session — Tech Spec §3.5
//     "gracefully unavailable") is NOT treated as a job failure: the job
//     completes with progress 1.0 and no points touched.
//
//   any other Colorizer (a test double, or a future second implementation):
//     the plain abstract-seam sequence — set_extrinsics(), add_keyframe()
//     per params.keyframes, colorize() — with only two progress ticks (0.0
//     then 1.0) and NO cancellation: the abstract interface exposes only a
//     poll-style progress() and no cancel hook, so there is no safe way to
//     interrupt a blocking colorize() call through it alone.
Status run_colorize(const ColorizeParams& params, std::function<void(float)> progress_cb,
                     post::CancelToken* cancel_token);

// Drives A9's export_points() (docs/A9-export.md — "the one entry point
// jobs/ (A15) calls").
Status run_export_points(const ExportPointsParams& params, ExportCancelToken* cancel_token,
                          ExportProgressCallback progress_cb, void* progress_user_data);

}  // namespace jobs
}  // namespace scanengine

#endif  // SCANENGINE_JOBS_LOCAL_RUNNER_H
