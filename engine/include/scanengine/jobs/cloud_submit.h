// cloud_submit.h — the cloud-mode client (Tech Spec §3.8's "Cloud" row).
//
//   .lscan zip (resumable upload) -> job service -> Linux worker runs
//   engine CLI on it -> results downloaded into project
//
// The REST contract this client speaks is documented in full in
// docs/A15-jobs.md ("Cloud REST contract"); the short version:
//
//   POST   {base}/jobs                 create a job, get an upload URL
//   PUT    {base}{upload_url}           one call per chunk, Content-Range
//   GET    {base}/jobs/{id}             poll state/progress
//   GET    {base}/jobs/{id}/result      download the result bundle
//
// Every call carries `Authorization: Bearer <token>` (single-tenant MVP,
// Tech Spec §3.8: "token auth, no payments/quotas, one worker instance").
// `CloudSubmitConfig::max_upload_bytes` is the MVP's hard upload-size cap,
// checked before the first byte moves.
//
// This client owns NO socket. It is written and tested entirely against
// `HttpTransport` (http_transport.h) — see tests/test_jobs.cpp's scripted
// fake for the happy path, a token reject, a mid-upload disconnect +
// resume, a job poll to completion, and a result download. The real
// transport is D3's.
//
// Owner: A15.
#ifndef SCANENGINE_JOBS_CLOUD_SUBMIT_H
#define SCANENGINE_JOBS_CLOUD_SUBMIT_H

#include <cstdint>
#include <functional>
#include <string>

#include "scanengine/core/error.h"
#include "scanengine/jobs/http_transport.h"

namespace scanengine {
namespace jobs {

struct CloudSubmitConfig {
  std::string base_url = "https://cloud.lidarscan.example/v1";
  std::string auth_token;

  std::uint64_t chunk_bytes = 8u * 1024u * 1024u;  // 8 MiB per upload chunk
  // Tech Spec §3.8 MVP: "hard upload-size cap". Checked against the local
  // file's size before POST /jobs is even sent.
  std::uint64_t max_upload_bytes = 2ull * 1024 * 1024 * 1024;  // 2 GiB

  int max_retries = 5;               // per chunk, before a resume-offset query
  std::uint32_t backoff_initial_ms = 500;
  double backoff_multiplier = 2.0;
  std::uint32_t backoff_max_ms = 30000;

  std::uint32_t poll_interval_ms = 2000;
  // 0 = unbounded (bounded only by `cancelled`/the caller's own timeout).
  std::uint32_t max_poll_attempts = 0;
};

enum class CloudJobState : std::uint8_t {
  kUnknown = 0,
  kQueued = 1,
  kUploading = 2,
  kProcessing = 3,
  kDone = 4,
  kFailed = 5,
};

const char* to_string(CloudJobState s) noexcept;

struct CloudJobStatus {
  std::string cloud_job_id;
  CloudJobState state = CloudJobState::kUnknown;
  float progress = 0.f;
  std::string message;
};

// `progress` in [0, 1], `cancelled` polled between chunks/polls — same
// cooperative-cancellation shape as every other long-running seam in the
// engine (post::CancelToken, ExportCancelToken).
using CloudProgressFn = std::function<void(float progress)>;
using CloudCancelledFn = std::function<bool()>;
using CloudStatusFn = std::function<void(const CloudJobStatus&)>;

class CloudSubmitClient {
 public:
  CloudSubmitClient(HttpTransport& transport, CloudSubmitConfig config);

  // Creates a cloud job (POST /jobs) and uploads `local_zip_path` in
  // Content-Range chunks. On a chunk failure that survives `max_retries`
  // retries with exponential backoff, issues ONE resume-offset query
  // (Content-Range "bytes */total", empty body) and continues from the
  // server's authoritative offset instead of failing outright — the
  // "disconnect mid-upload, then resume" path. Returns the cloud job id.
  Result<std::string> submit(const std::string& local_zip_path, CloudProgressFn progress_cb = nullptr,
                              CloudCancelledFn cancelled = nullptr);

  Result<CloudJobStatus> poll(const std::string& cloud_job_id);

  // Blocks (subject to `cancelled` and `config.max_poll_attempts`), polling
  // at `config.poll_interval_ms`, until the cloud job reaches kDone/kFailed.
  Result<CloudJobStatus> wait_until_terminal(const std::string& cloud_job_id,
                                              CloudStatusFn status_cb = nullptr,
                                              CloudCancelledFn cancelled = nullptr);

  // Writes the result bundle's bytes to `dest_path`. kNotFound if the job
  // is not yet done; kPermissionDenied on a token reject.
  Status download_result(const std::string& cloud_job_id, const std::string& dest_path);

 private:
  HttpTransport& transport_;
  CloudSubmitConfig cfg_;

  HttpResponse send_with_retry(const HttpRequest& req, CloudCancelledFn cancelled, bool* cancelled_out);
  std::string auth_header_value() const;
};

}  // namespace jobs
}  // namespace scanengine

#endif  // SCANENGINE_JOBS_CLOUD_SUBMIT_H
