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

  // Prefer A11's concrete implementation: real cancellation and
  // fine-grained progress, plus its own frames.idx loader.
  if (auto* pc = dynamic_cast<color::PointColorizer*>(params.colorizer)) {
    if (cancel_token != nullptr) pc->set_cancel_token(cancel_token);
    if (progress_cb) {
      pc->set_progress_callback(
          [progress_cb](const color::ColorProgress& p) { progress_cb(p.fraction); });
    }
    SCAN_TRY(pc->set_extrinsics(params.camera_from_lidar));
    if (!params.keyframes.empty()) {
      for (const Keyframe& kf : params.keyframes) SCAN_TRY(pc->add_keyframe(kf));
    } else if (!params.lscan_dir.empty()) {
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
    return pc->colorize(params.store.get());
  }

  // Generic fallback for any other Colorizer (a test double, or a future
  // second implementation): the plain abstract-seam sequence. No
  // cancellation — see local_runner.h's comment — and only the two progress
  // ticks already reported above and below.
  SCAN_TRY(params.colorizer->set_extrinsics(params.camera_from_lidar));
  for (const Keyframe& kf : params.keyframes) {
    SCAN_TRY(params.colorizer->add_keyframe(kf));
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
