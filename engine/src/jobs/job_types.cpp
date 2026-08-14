#include "scanengine/jobs/job_types.h"

namespace scanengine {
namespace jobs {

const char* to_string(JobKind k) noexcept {
  switch (k) {
    case JobKind::kPostProcess: return "post-process";
    case JobKind::kColorize: return "colorize";
    case JobKind::kExportPoints: return "export-points";
    case JobKind::kTransferExport: return "transfer-export";
    case JobKind::kCloudSubmit: return "cloud-submit";
  }
  return "unknown-job-kind";
}

const char* to_string(JobState s) noexcept {
  switch (s) {
    case JobState::kQueued: return "queued";
    case JobState::kRunning: return "running";
    case JobState::kCancelling: return "cancelling";
    case JobState::kDone: return "done";
    case JobState::kFailed: return "failed";
  }
  return "unknown-job-state";
}

}  // namespace jobs
}  // namespace scanengine
