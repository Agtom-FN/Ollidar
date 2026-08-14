// job.h — processing modes: local · cloud · extract-for-transfer (§3.8).
//
// SEAM ONLY. Owner: A15 (local runner, cloud submit client, transfer bundle
// export/import), with D1/D2/D3 on the service side.
//
// Cloud MVP boundaries are contractual (§3.8): single-tenant, token auth, no
// payments or quotas, one worker, hard upload-size cap. A15 must not design
// past them.
//
// Progress and completion are reported ONLY through EventType::kJobProgress
// on the engine's event bus — the Android foreground service and the Qt
// processing queue are both just subscribers.
#ifndef SCANENGINE_JOBS_JOB_H
#define SCANENGINE_JOBS_JOB_H

#include <cstdint>
#include <string>

#include "scanengine/core/error.h"

namespace scanengine {

enum class JobMode : std::uint8_t {
  kLocal = 0,
  kCloud = 1,
  kExtractForTransfer = 2,
};

enum class JobState : std::uint8_t {
  kQueued = 0,
  kUploading = 1,
  kRunning = 2,
  kDownloading = 3,
  kDone = 4,
  kFailed = 5,
  kCancelled = 6,
};

struct JobRequest {
  JobMode mode = JobMode::kLocal;
  std::string lscan_dir;
  std::string output_dir;
  std::string pipeline;   // "post", "colorize", "plan", "merge", "export"
};

struct JobStatus {
  std::uint64_t id = 0;
  JobState state = JobState::kQueued;
  float progress = 0.f;
  ScanError error = ScanError::kOk;
  std::string message;
};

class JobRunner {
 public:
  virtual ~JobRunner() = default;
  virtual Result<std::uint64_t> submit(const JobRequest& req) = 0;
  virtual Status cancel(std::uint64_t job_id) = 0;
  virtual JobStatus status(std::uint64_t job_id) const = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_JOBS_JOB_H
