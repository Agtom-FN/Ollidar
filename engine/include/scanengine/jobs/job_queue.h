// job_queue.h — one worker thread, FIFO-with-priorities, cancellation,
// completion callbacks. The concrete engine behind Tech Spec §3.8's
// "processing-mode chooser" and jobs/job.h's JobRunner seam.
//
// THREADING (DESIGN.md §2: "every thread gets introduced by the task that
// needs it, and must be documented"). JobQueue owns exactly one worker
// thread, started in the constructor and joined in the destructor —
// "(A15) job workers" in DESIGN.md's thread table, one instance. submit(),
// cancel(), status(), list(), on_completion() are safe from any thread.
// Jobs run strictly one at a time, FIFO within a priority level, highest
// priority first — no job ever starts before an equal-or-higher-priority
// job already queued ahead of it. A Colorize job additionally spawns one
// short-lived progress-sampling thread of its own (see local_runner.h's
// comment on why); it joins before that job's run() call returns, so it
// never overlaps the next job.
//
// PROGRESS. Every progress update updates the in-memory Job snapshot
// (status()/list()) AND, if constructed with an EventBus, publishes
// EventType::kJobProgress with JobProgressPayload{job_id, progress, state}
// (core/event.h — A1 already added this event type; jobs/job.h's header
// comment is what documents the republishing contract). This is the "two-
// line republishing lambda" docs/A7-post.md §8 item 3 asks A15 for, done
// once here for every job kind rather than per-pipeline.
//
// CANCELLATION. cancel() on a Queued job removes it and finalizes it
// immediately (kFailed / ScanError::kCancelled) without ever running.
// cancel() on a Running job sets state kCancelling and signals whatever
// cooperative-cancel mechanism that kind's runner is using: post::CancelToken
// for kPostProcess and (when the concrete color::PointColorizer is behind
// the Colorizer* seam — see local_runner.h) kColorize too, ExportCancelToken
// for kExportPoints, a plain atomic polled between chunks/polls for
// kCloudSubmit; the job still finalizes from the worker thread once its
// run() call unwinds. kTransferExport has a documented cancellation gap —
// see transfer.h — because zip_export()/zip_import() expose no cancel hook
// today. A kColorize job driven by a non-PointColorizer Colorizer* (e.g. a
// test double against the bare abstract seam) has the same gap, for the
// same reason.
//
// CHAINING. A JobSpec's ColorizeParams/ExportPointsParams/CloudSubmitParams
// may set `chain_from` to a prior job's id instead of supplying its input
// directly; JobQueue resolves it against produced_store()/produced_zip_path()
// right before running, and fails the job with ScanError::kInvalidState if
// the source job is not a finished, successful producer of the right kind.
//
// Owner: A15.
#ifndef SCANENGINE_JOBS_JOB_QUEUE_H
#define SCANENGINE_JOBS_JOB_QUEUE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/jobs/job_types.h"

namespace scanengine {
namespace jobs {

using CompletionFn = std::function<void(const Job&)>;
using CompletionSubscriptionId = std::uint32_t;
inline constexpr CompletionSubscriptionId kInvalidCompletionSubscription = 0;

class JobQueue {
 public:
  // `event_bus` is optional and not owned; null means progress lives only
  // in status()/list() (the shape every unit test in tests/test_jobs.cpp
  // that does not care about the event bus uses).
  explicit JobQueue(EventBus* event_bus = nullptr);
  ~JobQueue();

  JobQueue(const JobQueue&) = delete;
  JobQueue& operator=(const JobQueue&) = delete;

  // Fails fast (before ever touching the worker thread) for a spec whose
  // required-but-unresolvable-at-submit-time fields are already wrong
  // (empty lscan_dir/output_path/etc — ScanError::kInvalidArgument).
  // `chain_from` targets are resolved later, when the job actually runs,
  // because the source job may not have finished yet.
  Result<std::uint64_t> submit(JobSpec spec);

  Status cancel(std::uint64_t job_id);

  Job status(std::uint64_t job_id) const;
  std::vector<Job> list() const;

  // Fires once per job, when it reaches kDone or kFailed, on the worker
  // thread (quick, no re-entry into the queue — the same rule EventBus
  // callbacks and PageStore subscribers follow, DESIGN.md §2).
  CompletionSubscriptionId on_completion(CompletionFn cb);
  Status remove_completion_listener(CompletionSubscriptionId id);

  // Valid once the given job id has reached kDone: a kPostProcess job's
  // PageStore (for a chained kColorize/kExportPoints), or a kColorize job's
  // (same store, now colorized) if `chain_from`'d instead of started fresh.
  // Null if the job id is unknown, not done, failed, or of another kind.
  std::shared_ptr<PageStore> produced_store(std::uint64_t job_id) const;

  // Valid once the given kTransferExport job id has reached kDone: the zip
  // path it produced, for a chained kCloudSubmit. Empty otherwise.
  std::string produced_zip_path(std::uint64_t job_id) const;

  // Stops accepting new work from the worker: the currently-running job (if
  // any) finishes normally, every job still Queued is finalized as
  // kFailed/kCancelled without running, and the worker thread is joined.
  // Idempotent; the destructor calls it. Exposed so a test can assert on
  // queue state after a clean shutdown without waiting on the destructor.
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace jobs
}  // namespace scanengine

#endif  // SCANENGINE_JOBS_JOB_QUEUE_H
