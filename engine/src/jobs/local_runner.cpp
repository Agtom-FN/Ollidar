#include "scanengine/jobs/local_runner.h"

#include "scanengine/color/colorizer.h"
#include "scanengine/export/exporter.h"
#include "scanengine/slam/post/post_pipeline.h"

namespace scanengine {
namespace jobs {

Status run_post_process(const PostProcessParams& params, post::CancelToken* cancel_token,
                         post::PostProgressFn progress_cb, std::shared_ptr<PageStore>* out_store) {
  if (params.lscan_dir.empty()) {
    return set_last_error(ScanError::kInvalidArgument, "jobs/local_runner: PostProcess: lscan_dir is empty");
  }

  // Always route the pipeline at an EXTERNALLY-owned PageStore (never the
  // one PostSlamPipeline would otherwise create and hold internally) so the
  // store outlives the pipeline object below — required for a chained
  // kColorize/kExportPoints job to reach it after this call returns
  // (docs/A7-post.md §7: "STREAM CHOICE ... a config field precisely so
  // this does not become a code change").
  std::shared_ptr<PageStore> store = params.store ? params.store : std::make_shared<PageStore>();

  post::PostConfig cfg = params.config;
  cfg.store = store.get();
  cfg.publish_to_store = true;

  post::PostSlamPipeline pipeline(cfg);
  if (progress_cb) pipeline.set_progress_callback(std::move(progress_cb));
  if (cancel_token != nullptr) pipeline.set_cancel_token(cancel_token);

  const Status st = pipeline.run(params.lscan_dir);
  if (out_store != nullptr) *out_store = store;
  return st;
}

Status run_colorize(const ColorizeParams& params, std::function<void(float)> progress_cb,
                     post::CancelToken* cancel_token) {
  if (params.colorizer == nullptr) {
    return set_last_error(ScanError::kUnimplemented,
                           "jobs/local_runner: Colorize: no Colorizer implementation configured");
  }
  if (!params.store) {
    return set_last_error(ScanError::kInvalidArgument, "jobs/local_runner: Colorize: no PageStore given");
  }

  if (progress_cb) progress_cb(0.f);

  // INT-34 (docs/A15-jobs.md §7.6): cancellation and progress are now on the
  // ABSTRACT Colorizer seam, so they are wired for EVERY implementation
  // rather than only for the one this function could dynamic_cast to. Both
  // hooks default to no-ops on the interface, so an implementation that does
  // not override them behaves exactly as it did before — it simply still
  // cannot be interrupted.
  params.colorizer->set_cancel_token(cancel_token);
  if (progress_cb) params.colorizer->set_progress_fn(progress_cb);

  SCAN_TRY(params.colorizer->set_extrinsics(params.camera_from_lidar));

  if (!params.keyframes.empty()) {
    for (const Keyframe& kf : params.keyframes) SCAN_TRY(params.colorizer->add_keyframe(kf));
  } else if (auto* pc = dynamic_cast<color::PointColorizer*>(params.colorizer)) {
    // The one thing still specific to A11's implementation, and it is a
    // CONVENIENCE rather than a capability: PointColorizer knows how to load
    // its own keyframes (and install a FileImageSource) from a .lscan
    // directory. The abstract seam has no such notion — a second
    // implementation would source keyframes its own way — so this stays a
    // dynamic_cast instead of becoming a virtual nobody else can answer.
    if (!params.lscan_dir.empty()) {
      const Status ks = pc->load_keyframes(params.lscan_dir);
      if (!ks.ok()) {
        if (ks.error() == ScanError::kNotFound) {
          // Tech Spec §3.5: "Desktop-captured sessions have no camera ->
          // colorization gracefully unavailable" — not a job failure.
          if (progress_cb) progress_cb(1.f);
          return kOkStatus;
        }
        return ks;
      }
    }
  }

  SCAN_TRY(params.colorizer->colorize(params.store.get()));
  if (progress_cb) progress_cb(1.f);
  return kOkStatus;
}

Status run_export_points(const ExportPointsParams& params, ExportCancelToken* cancel_token,
                          ExportProgressCallback progress_cb, void* progress_user_data) {
  if (!params.store) {
    return set_last_error(ScanError::kInvalidArgument, "jobs/local_runner: ExportPoints: no PageStore given");
  }
  if (params.output_path.empty()) {
    return set_last_error(ScanError::kInvalidArgument,
                           "jobs/local_runner: ExportPoints: output_path is empty");
  }
  return export_points(*params.store, Span<const StreamId>(params.streams.data(), params.streams.size()),
                        params.format, params.output_path, params.options, progress_cb, progress_user_data,
                        cancel_token);
}

}  // namespace jobs
}  // namespace scanengine
