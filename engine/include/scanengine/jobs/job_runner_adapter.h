// job_runner_adapter.h — A1's `JobRunner` seam, in front of A15's `JobQueue`.
//
// INT-34, closing docs/A15-jobs.md §7.3.
//
// TWO JOB VOCABULARIES, AND WHY BOTH SURVIVE
//
//   jobs/job.h (A1)      JobMode {local, cloud, extract-for-transfer} +
//                        JobRequest {lscan_dir, output_dir, pipeline} +
//                        JobStatus {id, JobState, progress, error}.
//                        This is what §3.8's "processing-mode chooser" UI —
//                        B6's Android queue screen, the Qt processing panel —
//                        naturally codes against: three radio buttons and a
//                        directory.
//
//   jobs/job_types.h     JobKind {post, colorize, export, transfer, cloud} +
//   (A15)                JobSpec with five per-kind parameter structs,
//                        priorities, and `chain_from`. This is what actually
//                        runs.
//
// A15 left the translation undone on purpose (§7.3): `JobRequest` cannot
// express `chain_from`, priority, an `ExportFormat`, a `Colorizer*` or a
// `CloudSubmitConfig`, so a lossy shim inside `jobs/` would have been a worse
// API than either side. What was missing was not the translation but the
// place to PUT the missing choices. `JobRunnerOptions` below is that place:
// the app configures it once (it already knows its format, its transport and
// its colorizer), and from then on the UI submits plain `JobRequest`s.
//
// THE CHAINS ARE THE POINT. `JobRequest` names a `.lscan` directory, never a
// `PageStore`, so "colorize this session" and "export this session" are not
// single jobs — they are a `kPostProcess` job with a `kColorize` /
// `kExportPoints` job chained off its produced store. The adapter builds
// those chains, and `status()` reports the chain as ONE job with one monotone
// progress, which is what the UI asked for in the first place.
//
// THREADING. Every method is safe from any thread; the adapter holds one
// mutex around its own id bookkeeping and does not hold it across a
// `JobQueue` call. It borrows the queue and never owns it.
//
// Owner: INT-34 (the adapter) over A15's queue and A1's seam.
#ifndef SCANENGINE_JOBS_JOB_RUNNER_ADAPTER_H
#define SCANENGINE_JOBS_JOB_RUNNER_ADAPTER_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/export/exporter.h"
#include "scanengine/jobs/cloud_submit.h"
#include "scanengine/jobs/http_transport.h"
#include "scanengine/jobs/job.h"
#include "scanengine/jobs/job_queue.h"
#include "scanengine/jobs/job_types.h"
#include "scanengine/slam/post/post_pipeline.h"

namespace scanengine {
namespace jobs {

// Everything a JobRequest cannot say. Set once, by the layer that knows.
struct JobRunnerOptions {
  // Applied to every JobSpec this adapter submits.
  int priority = 0;

  // mode = kLocal, pipeline = "post" (and the head of every other kLocal
  // chain, since a .lscan has to become a PageStore before anything can
  // colour or export it).
  post::PostConfig post_config;
  // Optional: publish the post pipeline's final cloud into a store the CALLER
  // owns — the live Engine's, or one a headless worker wants to read counts
  // and bounds off after the chain finishes. Null lets the job create and
  // keep one, which is enough for chaining but unreachable from outside.
  std::shared_ptr<PageStore> store;

  // mode = kLocal, pipeline = "colorize". Null makes that pipeline fail with
  // kUnimplemented at submit() rather than at run time — a mode chooser
  // should grey the button out, and this is the answer it greys it out on.
  Colorizer* colorizer = nullptr;  // not owned
  double camera_from_lidar[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  // mode = kLocal, pipeline = "export". The file lands in
  // `JobRequest::output_dir` as `export_basename` + the format's extension.
  ExportFormat export_format = ExportFormat::kPlyBinary;
  ExportOptions export_options;
  std::string export_basename = "cloud";
  std::vector<StreamId> export_streams;  // empty = every page

  // mode = kExtractForTransfer and the first half of mode = kCloud. The
  // bundle lands in `output_dir` as `transfer_basename` + ".lscan.zip".
  bool transfer_include_results = true;
  std::string transfer_basename = "bundle";

  // mode = kCloud. Null transport fails at submit(), same reason as
  // `colorizer` above.
  HttpTransport* cloud_transport = nullptr;  // not owned
  CloudSubmitConfig cloud_config;
};

// The adapter. Construct one per queue; a null queue is a programming error
// and every call then returns kInvalidState rather than dereferencing it.
class QueueJobRunner final : public JobRunner {
 public:
  explicit QueueJobRunner(JobQueue* queue, const JobRunnerOptions& options = JobRunnerOptions());
  ~QueueJobRunner() override;

  QueueJobRunner(const QueueJobRunner&) = delete;
  QueueJobRunner& operator=(const QueueJobRunner&) = delete;

  // Translates one JobRequest into one or two JobSpecs and submits them.
  // Returns the id of the LAST spec in the chain — the one whose completion
  // means the request is done, and the one `cancel()`/`status()` take.
  //
  //   mode = kLocal
  //     pipeline ""/"post"  → kPostProcess
  //     pipeline "colorize" → kPostProcess → kColorize (chained)
  //     pipeline "export"   → kPostProcess → kExportPoints (chained)
  //     pipeline "colorize-export"
  //                         → kPostProcess → kColorize → kExportPoints
  //                            (INT-FINAL: the cloud worker's whole job —
  //                             `engine_cli --post --colorize --out`. Two
  //                             separate requests cannot express it, because
  //                             chaining is by job id and the second request
  //                             would re-run the post pipeline.)
  //     pipeline "plan"/"merge" → kUnimplemented: A12/A13 have no job kind,
  //                            and inventing one here would hide that.
  //   mode = kExtractForTransfer → kTransferExport
  //   mode = kCloud              → kTransferExport → kCloudSubmit (chained)
  //
  // kInvalidArgument for an empty lscan_dir (or an empty output_dir where one
  // is needed); kUnimplemented for a pipeline with no job kind behind it or a
  // missing required option.
  Result<std::uint64_t> submit(const JobRequest& req) override;

  // Cancels the whole chain, not just the tail: cancelling "export this
  // session" while its post-process stage is still running has to stop the
  // post-process. Unknown id → kNotFound.
  Status cancel(std::uint64_t job_id) override;

  // The chain as ONE job. `progress` is (finished stages + the running
  // stage's own progress) / stage count, so it is monotone across the whole
  // chain rather than resetting at each hand-off. `state` collapses A15's
  // five states onto A1's seven:
  //
  //   jobs::kQueued            → kQueued
  //   jobs::kRunning           → kRunning, except a kCloudSubmit stage, whose
  //                              own stage label distinguishes kUploading and
  //                              kDownloading — the two states A1's enum has
  //                              and A15's deliberately does not
  //   jobs::kCancelling        → kRunning (the unwind has not landed yet)
  //   jobs::kDone              → kDone
  //   jobs::kFailed            → kCancelled when the error is kCancelled,
  //                              else kFailed — A15's five-state model folds
  //                              cancellation into kFailed + kCancelled, and
  //                              this is where it comes back apart
  //
  // An unknown id is reported, not thrown: kFailed with kNotFound.
  JobStatus status(std::uint64_t job_id) const override;

  // Mutable so an app can change its export format or install a colorizer
  // between submissions without rebuilding the runner. Not synchronized
  // against a concurrent submit() — configure from the control thread.
  JobRunnerOptions& options();
  const JobRunnerOptions& options() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace jobs
}  // namespace scanengine

#endif  // SCANENGINE_JOBS_JOB_RUNNER_ADAPTER_H
