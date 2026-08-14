#include "scanengine/jobs/job_queue.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "scanengine/core/event.h"
#include "scanengine/jobs/local_runner.h"
#include "scanengine/jobs/transfer.h"

namespace scanengine {
namespace jobs {

struct JobQueue::Impl {
  EventBus* event_bus = nullptr;

  mutable std::mutex m;
  std::condition_variable cv;
  std::thread worker;
  bool stopping = false;

  std::uint64_t next_id = 1;
  // Highest priority first (std::greater<int>); FIFO within a priority via
  // the per-bucket deque. One worker thread, so "the job that runs next" is
  // exactly the front of ready.begin()->second whenever the worker is free.
  std::map<int, std::deque<std::uint64_t>, std::greater<int>> ready;

  struct Runtime {
    JobSpec spec;
    Job info;
    // Set only while the job is Running; flips whatever cooperative-cancel
    // mechanism that job kind is using. Empty for kinds/moments with no
    // cancel hook available (see job_queue.h's CANCELLATION section).
    std::function<void()> cancel_fn;
  };
  std::unordered_map<std::uint64_t, Runtime> jobs;  // every job ever submitted; never erased

  std::unordered_map<std::uint64_t, std::shared_ptr<PageStore>> produced_stores;
  std::unordered_map<std::uint64_t, std::string> produced_zips;

  std::uint32_t next_sub_id = 1;
  std::unordered_map<CompletionSubscriptionId, CompletionFn> completion_subs;

  void worker_main();
  void run_job(std::uint64_t id);
  Status run_kind_post_process(std::uint64_t id, JobSpec& spec);
  Status run_kind_colorize(std::uint64_t id, JobSpec& spec);
  Status run_kind_export(std::uint64_t id, JobSpec& spec);
  Status run_kind_transfer(std::uint64_t id, JobSpec& spec);
  Status run_kind_cloud(std::uint64_t id, JobSpec& spec);

  void report_progress(std::uint64_t id, float progress, const std::string& stage);
  void finalize(std::uint64_t id, Status st);

  // direct wins; otherwise chain_from must name a kDone job with a produced
  // PageStore. Locks `m` itself — call with no lock held.
  std::shared_ptr<PageStore> resolve_store(std::shared_ptr<PageStore> direct, std::uint64_t chain_from,
                                            Status* err);

  // Bridges A9's C-function-pointer ExportProgressCallback (export/exporter.h)
  // into report_progress(). A static member function has the same "no
  // captures" plain-pointer type as a free function, so &Impl::export_progress_trampoline
  // is usable directly as an ExportProgressCallback.
  static void export_progress_trampoline(float fraction, void* user);
};

std::shared_ptr<PageStore> JobQueue::Impl::resolve_store(std::shared_ptr<PageStore> direct,
                                                          std::uint64_t chain_from, Status* err) {
  if (direct) {
    *err = kOkStatus;
    return direct;
  }
  std::lock_guard<std::mutex> lk(m);
  auto it = jobs.find(chain_from);
  if (it == jobs.end() || it->second.info.state != JobState::kDone) {
    *err = set_last_error(ScanError::kInvalidState,
                           "jobs/job_queue: chain_from job %llu is not a finished, successful producer",
                           static_cast<unsigned long long>(chain_from));
    return nullptr;
  }
  auto sit = produced_stores.find(chain_from);
  if (sit == produced_stores.end()) {
    *err = set_last_error(ScanError::kNotFound, "jobs/job_queue: chain_from job %llu produced no PageStore",
                           static_cast<unsigned long long>(chain_from));
    return nullptr;
  }
  *err = kOkStatus;
  return sit->second;
}

void JobQueue::Impl::report_progress(std::uint64_t id, float progress, const std::string& stage) {
  Job snapshot;
  {
    std::lock_guard<std::mutex> lk(m);
    auto it = jobs.find(id);
    if (it == jobs.end()) return;
    it->second.info.progress = progress;
    it->second.info.stage = stage;
    snapshot = it->second.info;
  }
  if (event_bus != nullptr) {
    JobProgressPayload payload{};
    payload.job_id = snapshot.id;
    payload.progress = snapshot.progress;
    payload.state = static_cast<std::uint8_t>(snapshot.state);
    event_bus->publish(EventType::kJobProgress, payload);
  }
}

void JobQueue::Impl::finalize(std::uint64_t id, Status st) {
  Job snapshot;
  std::vector<CompletionFn> cbs;
  {
    std::lock_guard<std::mutex> lk(m);
    auto& rt = jobs.at(id);
    rt.cancel_fn = nullptr;
    rt.info.error = st.error();
    if (st.ok()) {
      rt.info.state = JobState::kDone;
      rt.info.progress = 1.f;
      rt.info.message.clear();
    } else {
      rt.info.state = JobState::kFailed;
      rt.info.message = error_str(st.error());
    }
    snapshot = rt.info;
    cbs.reserve(completion_subs.size());
    for (auto& kv : completion_subs) cbs.push_back(kv.second);
  }
  if (event_bus != nullptr) {
    JobProgressPayload payload{};
    payload.job_id = snapshot.id;
    payload.progress = snapshot.progress;
    payload.state = static_cast<std::uint8_t>(snapshot.state);
    event_bus->publish(EventType::kJobProgress, payload);
  }
  for (auto& cb : cbs) cb(snapshot);
}

Status JobQueue::Impl::run_kind_post_process(std::uint64_t id, JobSpec& spec) {
  post::CancelToken token;
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = [&token] { token.cancel(); };
  }
  auto progress_cb = [this, id](const post::PostProgress& p) {
    report_progress(id, p.fraction, p.label != nullptr ? p.label : "");
  };
  std::shared_ptr<PageStore> store;
  const Status st = run_post_process(spec.post, &token, progress_cb, &store);
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = nullptr;
  }
  if (store) {
    std::lock_guard<std::mutex> lk(m);
    produced_stores[id] = store;
  }
  return st;
}

Status JobQueue::Impl::run_kind_colorize(std::uint64_t id, JobSpec& spec) {
  ColorizeParams p = spec.colorize;
  if (!p.store) {
    Status err;
    p.store = resolve_store(nullptr, p.chain_from, &err);
    if (!p.store) return err;
  }
  // Registered regardless of which Colorizer implementation runs: it is
  // only ACTED ON when run_colorize() finds a color::PointColorizer (see
  // local_runner.h) — a generic Colorizer's cancel_fn is simply never
  // invoked-against by anything downstream of it, which is harmless.
  post::CancelToken token;
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = [&token] { token.cancel(); };
  }
  auto progress_cb = [this, id](float f) { report_progress(id, f, "colorizing"); };
  const Status st = run_colorize(p, progress_cb, &token);
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = nullptr;
  }
  if (st.ok()) {
    std::lock_guard<std::mutex> lk(m);
    produced_stores[id] = p.store;  // same store, now colorized in place
  }
  return st;
}

void JobQueue::Impl::export_progress_trampoline(float fraction, void* user) {
  auto* ctx = static_cast<std::pair<Impl*, std::uint64_t>*>(user);
  ctx->first->report_progress(ctx->second, fraction, "exporting");
}

Status JobQueue::Impl::run_kind_export(std::uint64_t id, JobSpec& spec) {
  ExportPointsParams p = spec.export_points;
  if (!p.store) {
    Status err;
    p.store = resolve_store(nullptr, p.chain_from, &err);
    if (!p.store) return err;
  }
  ExportCancelToken token;
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = [&token] { token.request_cancel(); };
  }
  std::pair<Impl*, std::uint64_t> ctx{this, id};
  const Status st = run_export_points(p, &token, &Impl::export_progress_trampoline, &ctx);
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = nullptr;
  }
  return st;
}

Status JobQueue::Impl::run_kind_transfer(std::uint64_t id, JobSpec& spec) {
  std::atomic<bool> cancel_flag{false};
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = [&cancel_flag] { cancel_flag.store(true); };
  }
  auto progress_cb = [this, id](float f) { report_progress(id, f, "transferring"); };
  auto cancelled_cb = [&cancel_flag] { return cancel_flag.load(); };
  const Status st = run_transfer_export(spec.transfer, progress_cb, cancelled_cb);
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = nullptr;
  }
  if (st.ok()) {
    std::lock_guard<std::mutex> lk(m);
    produced_zips[id] = spec.transfer.zip_path;
  }
  return st;
}

Status JobQueue::Impl::run_kind_cloud(std::uint64_t id, JobSpec& spec) {
  CloudSubmitParams p = spec.cloud;
  if (p.transport == nullptr) {
    return set_last_error(ScanError::kInvalidArgument,
                           "jobs/job_queue: CloudSubmit: no HttpTransport configured");
  }
  if (p.local_zip_path.empty()) {
    if (p.chain_from == 0) {
      return set_last_error(ScanError::kInvalidArgument,
                             "jobs/job_queue: CloudSubmit: no local_zip_path or chain_from");
    }
    std::lock_guard<std::mutex> lk(m);
    auto it = jobs.find(p.chain_from);
    if (it == jobs.end() || it->second.info.state != JobState::kDone) {
      return set_last_error(
          ScanError::kInvalidState,
          "jobs/job_queue: chain_from job %llu is not a finished, successful TransferExport",
          static_cast<unsigned long long>(p.chain_from));
    }
    auto zit = produced_zips.find(p.chain_from);
    if (zit == produced_zips.end()) {
      return set_last_error(ScanError::kNotFound, "jobs/job_queue: chain_from job %llu produced no zip",
                             static_cast<unsigned long long>(p.chain_from));
    }
    p.local_zip_path = zit->second;
  }

  std::atomic<bool> cancel_flag{false};
  {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = [&cancel_flag] { cancel_flag.store(true); };
  }
  auto cancelled_cb = [&cancel_flag] { return cancel_flag.load(); };
  auto clear_cancel_fn = [this, id] {
    std::lock_guard<std::mutex> lk(m);
    jobs.at(id).cancel_fn = nullptr;
  };

  CloudSubmitClient client(*p.transport, p.cloud_config);

  auto upload_progress = [this, id](float f) { report_progress(id, f * 0.6f, "uploading"); };
  const Result<std::string> submit_r = client.submit(p.local_zip_path, upload_progress, cancelled_cb);
  if (!submit_r.ok()) {
    clear_cancel_fn();
    return submit_r.status();
  }

  auto poll_progress = [this, id](const CloudJobStatus& cs) {
    report_progress(id, 0.6f + 0.3f * cs.progress, cs.message.empty() ? to_string(cs.state) : cs.message);
  };
  const Result<CloudJobStatus> wait_r =
      client.wait_until_terminal(submit_r.value(), poll_progress, cancelled_cb);
  if (!wait_r.ok()) {
    clear_cancel_fn();
    return wait_r.status();
  }
  if (wait_r.value().state != CloudJobState::kDone) {
    clear_cancel_fn();
    return set_last_error(ScanError::kUnknown, "jobs/job_queue: CloudSubmit: server job failed: %s",
                           wait_r.value().message.c_str());
  }

  Status dl_status = kOkStatus;
  if (!p.result_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(p.result_dir, ec);
    dl_status = client.download_result(submit_r.value(), p.result_dir + "/result.zip");
  }
  clear_cancel_fn();
  if (!dl_status.ok()) return dl_status;

  report_progress(id, 1.f, "done");
  return kOkStatus;
}

void JobQueue::Impl::run_job(std::uint64_t id) {
  JobSpec spec;
  {
    std::lock_guard<std::mutex> lk(m);
    spec = jobs.at(id).spec;
  }

  Status result;
  switch (spec.kind) {
    case JobKind::kPostProcess: result = run_kind_post_process(id, spec); break;
    case JobKind::kColorize: result = run_kind_colorize(id, spec); break;
    case JobKind::kExportPoints: result = run_kind_export(id, spec); break;
    case JobKind::kTransferExport: result = run_kind_transfer(id, spec); break;
    case JobKind::kCloudSubmit: result = run_kind_cloud(id, spec); break;
  }
  finalize(id, result);
}

void JobQueue::Impl::worker_main() {
  for (;;) {
    std::uint64_t id = 0;
    {
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [this] { return stopping || !ready.empty(); });
      if (stopping) return;
      auto it = ready.begin();
      id = it->second.front();
      it->second.pop_front();
      if (it->second.empty()) ready.erase(it);
      jobs.at(id).info.state = JobState::kRunning;
    }
    run_job(id);
  }
}

JobQueue::JobQueue(EventBus* event_bus) : impl_(std::make_unique<Impl>()) {
  impl_->event_bus = event_bus;
  impl_->worker = std::thread([this] { impl_->worker_main(); });
}

JobQueue::~JobQueue() { stop(); }

void JobQueue::stop() {
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->stopping = true;
  }
  impl_->cv.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();

  std::vector<Job> snapshots;
  std::vector<CompletionFn> cbs;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    for (auto& bucket : impl_->ready) {
      for (std::uint64_t id : bucket.second) {
        auto& rt = impl_->jobs.at(id);
        rt.info.state = JobState::kFailed;
        rt.info.error = ScanError::kCancelled;
        rt.info.message = error_str(ScanError::kCancelled);
        snapshots.push_back(rt.info);
      }
    }
    impl_->ready.clear();
    if (!snapshots.empty()) {
      cbs.reserve(impl_->completion_subs.size());
      for (auto& kv : impl_->completion_subs) cbs.push_back(kv.second);
    }
  }
  for (auto& snap : snapshots) {
    for (auto& cb : cbs) cb(snap);
  }
}

Result<std::uint64_t> JobQueue::submit(JobSpec spec) {
  switch (spec.kind) {
    case JobKind::kPostProcess:
      if (spec.post.lscan_dir.empty()) {
        return set_last_error(ScanError::kInvalidArgument, "jobs/job_queue: PostProcess needs lscan_dir");
      }
      break;
    case JobKind::kColorize:
      if (!spec.colorize.store && spec.colorize.chain_from == 0) {
        return set_last_error(ScanError::kInvalidArgument,
                               "jobs/job_queue: Colorize needs store or chain_from");
      }
      break;
    case JobKind::kExportPoints:
      if (!spec.export_points.store && spec.export_points.chain_from == 0) {
        return set_last_error(ScanError::kInvalidArgument,
                               "jobs/job_queue: ExportPoints needs store or chain_from");
      }
      if (spec.export_points.output_path.empty()) {
        return set_last_error(ScanError::kInvalidArgument, "jobs/job_queue: ExportPoints needs output_path");
      }
      break;
    case JobKind::kTransferExport:
      if (spec.transfer.project_dir.empty() || spec.transfer.zip_path.empty()) {
        return set_last_error(ScanError::kInvalidArgument,
                               "jobs/job_queue: TransferExport needs project_dir and zip_path");
      }
      break;
    case JobKind::kCloudSubmit:
      if (spec.cloud.transport == nullptr) {
        return set_last_error(ScanError::kInvalidArgument, "jobs/job_queue: CloudSubmit needs a transport");
      }
      if (spec.cloud.local_zip_path.empty() && spec.cloud.chain_from == 0) {
        return set_last_error(ScanError::kInvalidArgument,
                               "jobs/job_queue: CloudSubmit needs local_zip_path or chain_from");
      }
      break;
  }

  const int priority = spec.priority;
  const JobKind kind = spec.kind;
  std::uint64_t id;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    id = impl_->next_id++;
    Impl::Runtime rt;
    rt.info.id = id;
    rt.info.kind = kind;
    rt.info.state = JobState::kQueued;
    rt.info.priority = priority;
    rt.spec = std::move(spec);
    impl_->jobs.emplace(id, std::move(rt));
    impl_->ready[priority].push_back(id);
  }
  impl_->cv.notify_one();
  return id;
}

Status JobQueue::cancel(std::uint64_t job_id) {
  std::function<void()> cancel_fn_copy;
  Job snapshot;
  std::vector<CompletionFn> cbs;
  bool immediate_finalize = false;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    auto it = impl_->jobs.find(job_id);
    if (it == impl_->jobs.end()) {
      return set_last_error(ScanError::kNotFound, "jobs/job_queue: unknown job id %llu",
                             static_cast<unsigned long long>(job_id));
    }
    Impl::Runtime& rt = it->second;
    if (rt.info.state == JobState::kQueued) {
      auto rit = impl_->ready.find(rt.info.priority);
      if (rit != impl_->ready.end()) {
        auto& dq = rit->second;
        dq.erase(std::remove(dq.begin(), dq.end(), job_id), dq.end());
        if (dq.empty()) impl_->ready.erase(rit);
      }
      rt.info.state = JobState::kFailed;
      rt.info.error = ScanError::kCancelled;
      rt.info.message = error_str(ScanError::kCancelled);
      snapshot = rt.info;
      immediate_finalize = true;
      cbs.reserve(impl_->completion_subs.size());
      for (auto& kv : impl_->completion_subs) cbs.push_back(kv.second);
    } else if (rt.info.state == JobState::kRunning) {
      rt.info.state = JobState::kCancelling;
      cancel_fn_copy = rt.cancel_fn;
    } else if (rt.info.state == JobState::kCancelling) {
      // Already unwinding; nothing more to do.
    } else {
      return set_last_error(ScanError::kInvalidState, "jobs/job_queue: job %llu has already finished",
                             static_cast<unsigned long long>(job_id));
    }
  }
  if (cancel_fn_copy) cancel_fn_copy();
  if (immediate_finalize) {
    for (auto& cb : cbs) cb(snapshot);
  }
  return kOkStatus;
}

Job JobQueue::status(std::uint64_t job_id) const {
  std::lock_guard<std::mutex> lk(impl_->m);
  auto it = impl_->jobs.find(job_id);
  if (it == impl_->jobs.end()) return Job{};
  return it->second.info;
}

std::vector<Job> JobQueue::list() const {
  std::lock_guard<std::mutex> lk(impl_->m);
  std::vector<Job> out;
  out.reserve(impl_->jobs.size());
  for (auto& kv : impl_->jobs) out.push_back(kv.second.info);
  return out;
}

CompletionSubscriptionId JobQueue::on_completion(CompletionFn cb) {
  std::lock_guard<std::mutex> lk(impl_->m);
  const auto id = impl_->next_sub_id++;
  impl_->completion_subs[id] = std::move(cb);
  return id;
}

Status JobQueue::remove_completion_listener(CompletionSubscriptionId id) {
  std::lock_guard<std::mutex> lk(impl_->m);
  auto it = impl_->completion_subs.find(id);
  if (it == impl_->completion_subs.end()) {
    return set_last_error(ScanError::kNotFound, "jobs/job_queue: unknown completion subscription %u", id);
  }
  impl_->completion_subs.erase(it);
  return kOkStatus;
}

std::shared_ptr<PageStore> JobQueue::produced_store(std::uint64_t job_id) const {
  std::lock_guard<std::mutex> lk(impl_->m);
  auto it = impl_->produced_stores.find(job_id);
  return it == impl_->produced_stores.end() ? nullptr : it->second;
}

std::string JobQueue::produced_zip_path(std::uint64_t job_id) const {
  std::lock_guard<std::mutex> lk(impl_->m);
  auto it = impl_->produced_zips.find(job_id);
  return it == impl_->produced_zips.end() ? std::string() : it->second;
}

}  // namespace jobs
}  // namespace scanengine
