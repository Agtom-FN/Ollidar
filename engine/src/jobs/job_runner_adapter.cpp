#include "scanengine/jobs/job_runner_adapter.h"

#include <algorithm>
#include <cstring>

#include "scanengine/core/log.h"

namespace scanengine {
namespace jobs {

namespace {

constexpr const char* kMod = "jobs";

const char* extension_for(ExportFormat f) {
  switch (f) {
    case ExportFormat::kPlyBinary: return ".ply";
    case ExportFormat::kLas14: return ".las";
    case ExportFormat::kPcd: return ".pcd";
    case ExportFormat::kDxf: return ".dxf";
    case ExportFormat::kPdf: return ".pdf";
  }
  return ".bin";
}

std::string join_path(const std::string& dir, const std::string& leaf) {
  if (dir.empty()) return leaf;
  const char back = dir.back();
  if (back == '/' || back == '\\') return dir + leaf;
  return dir + "/" + leaf;
}

// Inside `namespace jobs`, the unqualified name `JobState` is A15's
// five-value one; A1's seven-value seam enum needs the alias. Spelling both
// out is also the clearest possible reminder that these are two enums.
using SeamState = ::scanengine::JobState;

// A15's five states onto A1's seven. The two A15 does not have — kUploading
// and kDownloading — are recovered from the kCloudSubmit stage label, which
// job_queue.cpp sets to "uploading" while streaming chunks and to the
// server's own state string while polling/downloading. Nothing else can tell
// them apart: A15 folded them into one kRunning on purpose.
SeamState seam_state_of(const Job& j) {
  switch (j.state) {
    case JobState::kQueued:
      return SeamState::kQueued;
    case JobState::kRunning:
    case JobState::kCancelling:
      if (j.kind == JobKind::kCloudSubmit) {
        if (j.stage.find("upload") != std::string::npos) return SeamState::kUploading;
        if (j.stage.find("download") != std::string::npos) return SeamState::kDownloading;
      }
      return SeamState::kRunning;
    case JobState::kDone:
      return SeamState::kDone;
    case JobState::kFailed:
      // A15 has five states, not six: a cancelled job settles into kFailed
      // with kCancelled (docs/A15-jobs.md §2). A1's enum keeps them apart, so
      // this is where they come back apart.
      return j.error == ScanError::kCancelled ? SeamState::kCancelled : SeamState::kFailed;
  }
  return SeamState::kFailed;
}

}  // namespace

struct QueueJobRunner::Impl {
  JobQueue* queue = nullptr;
  JobRunnerOptions opts;

  mutable std::mutex m;
  // Tail id -> every id in that chain, in execution order. One entry per
  // submit(); the tail is always chain.back().
  std::map<std::uint64_t, std::vector<std::uint64_t>> chains;

  std::vector<std::uint64_t> chain_of(std::uint64_t tail) const {
    std::lock_guard<std::mutex> lock(m);
    auto it = chains.find(tail);
    if (it == chains.end()) return {};
    return it->second;
  }
};

QueueJobRunner::QueueJobRunner(JobQueue* queue, const JobRunnerOptions& options)
    : impl_(new Impl) {
  impl_->queue = queue;
  impl_->opts = options;
}

QueueJobRunner::~QueueJobRunner() = default;

JobRunnerOptions& QueueJobRunner::options() { return impl_->opts; }
const JobRunnerOptions& QueueJobRunner::options() const { return impl_->opts; }

Result<std::uint64_t> QueueJobRunner::submit(const JobRequest& req) {
  if (impl_->queue == nullptr) {
    return set_last_error(ScanError::kInvalidState, "jobs/runner: no JobQueue");
  }
  if (req.lscan_dir.empty()) {
    return set_last_error(ScanError::kInvalidArgument, "jobs/runner: lscan_dir is empty");
  }

  std::vector<std::uint64_t> chain;
  // Anything already submitted has to be cleaned up if a later spec in the
  // same chain is rejected — a half-built chain would run its head, produce a
  // store nobody consumes, and report kDone for a request that never
  // completed.
  const auto unwind = [&chain, this](ScanError e, const char* what) -> Result<std::uint64_t> {
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) (void)impl_->queue->cancel(*it);
    return set_last_error(e, "%s", what);
  };

  const auto submit_spec = [&](JobSpec spec) -> bool {
    spec.priority = impl_->opts.priority;
    auto id = impl_->queue->submit(std::move(spec));
    if (!id.ok()) return false;
    chain.push_back(id.value());
    return true;
  };

  switch (req.mode) {
    case JobMode::kLocal: {
      const std::string& p = req.pipeline;
      if (!(p.empty() || p == "post" || p == "colorize" || p == "export")) {
        return set_last_error(ScanError::kUnimplemented,
                              "jobs/runner: pipeline '%s' has no job kind (A12 plan / A13 merge "
                              "are not job kinds; see jobs/job_types.h)",
                              p.c_str());
      }
      // Every local pipeline starts by turning the .lscan into a PageStore:
      // JobRequest names a directory, and neither colorize nor export can
      // consume one.
      JobSpec post;
      post.kind = JobKind::kPostProcess;
      post.post.lscan_dir = req.lscan_dir;
      post.post.config = impl_->opts.post_config;
      post.post.store = impl_->opts.store;
      if (!submit_spec(std::move(post))) {
        return set_last_error(ScanError::kInvalidArgument,
                              "jobs/runner: PostProcess spec rejected: %s", last_error_message());
      }
      if (p.empty() || p == "post") break;

      if (p == "colorize") {
        if (impl_->opts.colorizer == nullptr) {
          return unwind(ScanError::kUnimplemented,
                        "jobs/runner: pipeline 'colorize' needs JobRunnerOptions::colorizer");
        }
        JobSpec col;
        col.kind = JobKind::kColorize;
        col.colorize.colorizer = impl_->opts.colorizer;
        col.colorize.lscan_dir = req.lscan_dir;
        col.colorize.chain_from = chain.back();
        std::memcpy(col.colorize.camera_from_lidar, impl_->opts.camera_from_lidar,
                    sizeof(col.colorize.camera_from_lidar));
        if (!submit_spec(std::move(col))) {
          return unwind(ScanError::kInvalidArgument, "jobs/runner: Colorize spec rejected");
        }
        break;
      }

      // p == "export"
      if (req.output_dir.empty()) {
        return unwind(ScanError::kInvalidArgument,
                      "jobs/runner: pipeline 'export' needs JobRequest::output_dir");
      }
      JobSpec ex;
      ex.kind = JobKind::kExportPoints;
      ex.export_points.chain_from = chain.back();
      ex.export_points.format = impl_->opts.export_format;
      ex.export_points.options = impl_->opts.export_options;
      ex.export_points.streams = impl_->opts.export_streams;
      ex.export_points.output_path =
          join_path(req.output_dir,
                    impl_->opts.export_basename + extension_for(impl_->opts.export_format));
      ex.export_points.options.output_path = ex.export_points.output_path;
      if (!submit_spec(std::move(ex))) {
        return unwind(ScanError::kInvalidArgument, "jobs/runner: ExportPoints spec rejected");
      }
      break;
    }

    case JobMode::kExtractForTransfer:
    case JobMode::kCloud: {
      if (req.output_dir.empty()) {
        return set_last_error(ScanError::kInvalidArgument,
                              "jobs/runner: output_dir is required for transfer/cloud");
      }
      JobSpec tx;
      tx.kind = JobKind::kTransferExport;
      tx.transfer.project_dir = req.lscan_dir;
      tx.transfer.zip_path =
          join_path(req.output_dir, impl_->opts.transfer_basename + ".lscan.zip");
      tx.transfer.include_results = impl_->opts.transfer_include_results;
      if (!submit_spec(std::move(tx))) {
        return set_last_error(ScanError::kInvalidArgument,
                              "jobs/runner: TransferExport spec rejected: %s", last_error_message());
      }
      if (req.mode == JobMode::kExtractForTransfer) break;

      if (impl_->opts.cloud_transport == nullptr) {
        return unwind(ScanError::kUnimplemented,
                      "jobs/runner: mode kCloud needs JobRunnerOptions::cloud_transport "
                      "(D3 implements HttpTransport over a real socket/TLS stack)");
      }
      JobSpec cs;
      cs.kind = JobKind::kCloudSubmit;
      cs.cloud.transport = impl_->opts.cloud_transport;
      cs.cloud.cloud_config = impl_->opts.cloud_config;
      cs.cloud.chain_from = chain.back();
      cs.cloud.result_dir = req.output_dir;
      if (!submit_spec(std::move(cs))) {
        return unwind(ScanError::kInvalidArgument, "jobs/runner: CloudSubmit spec rejected");
      }
      break;
    }
  }

  const std::uint64_t tail = chain.back();
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->chains[tail] = chain;
  }
  SCAN_LOG_INFO(kMod, "jobs/runner: request(mode=%d, pipeline='%s') -> %zu job(s), tail %llu",
                static_cast<int>(req.mode), req.pipeline.c_str(), chain.size(),
                static_cast<unsigned long long>(tail));
  return tail;
}

Status QueueJobRunner::cancel(std::uint64_t job_id) {
  if (impl_->queue == nullptr) {
    return set_last_error(ScanError::kInvalidState, "jobs/runner: no JobQueue");
  }
  const std::vector<std::uint64_t> chain = impl_->chain_of(job_id);
  if (chain.empty()) {
    return set_last_error(ScanError::kNotFound, "jobs/runner: unknown job %llu",
                          static_cast<unsigned long long>(job_id));
  }
  // Tail first: cancelling the head can let the queue start the tail before
  // the tail's own cancel lands, and a chained job whose source was cancelled
  // would then fail with kInvalidState instead of kCancelled.
  Status first_error = kOkStatus;
  bool any_ok = false;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const Status s = impl_->queue->cancel(*it);
    if (s.ok()) {
      any_ok = true;
    } else if (first_error.ok()) {
      first_error = s;
    }
  }
  // Every stage already finished is an "already finished" error from the
  // queue, which is not a failure of the request-level cancel — only a chain
  // where nothing at all could be cancelled is.
  return any_ok ? kOkStatus : first_error;
}

JobStatus QueueJobRunner::status(std::uint64_t job_id) const {
  JobStatus out;
  out.id = job_id;
  if (impl_->queue == nullptr) {
    out.state = SeamState::kFailed;
    out.error = ScanError::kInvalidState;
    out.message = "no JobQueue";
    return out;
  }
  const std::vector<std::uint64_t> chain = impl_->chain_of(job_id);
  if (chain.empty()) {
    out.state = SeamState::kFailed;
    out.error = ScanError::kNotFound;
    out.message = "unknown job";
    return out;
  }

  const double n = static_cast<double>(chain.size());
  double finished = 0.0;
  std::vector<Job> stages;
  stages.reserve(chain.size());
  for (const std::uint64_t id : chain) stages.push_back(impl_->queue->status(id));

  // A failure anywhere is the request's outcome, whatever the later stages
  // say (a chained job whose source failed reports kInvalidState, which is a
  // consequence and not the cause — report the cause).
  for (const Job& j : stages) {
    if (j.state == JobState::kFailed) {
      out.state = seam_state_of(j);
      out.error = j.error;
      out.message = j.message.empty() ? j.stage : j.message;
      out.progress = static_cast<float>(finished / n);
      return out;
    }
    if (j.state == JobState::kDone) {
      finished += 1.0;
      continue;
    }
    // The first stage that is not finished is the one the request is on.
    out.state = seam_state_of(j);
    out.progress = static_cast<float>(
        std::min(1.0, (finished + static_cast<double>(j.progress)) / n));
    out.message = j.stage;
    return out;
  }

  out.state = SeamState::kDone;
  out.progress = 1.f;
  out.message = stages.empty() ? "" : stages.back().stage;
  return out;
}

}  // namespace jobs
}  // namespace scanengine
