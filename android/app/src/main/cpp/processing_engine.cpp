#include "processing_engine.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "scanengine/core/error.h"
#include "scanengine/core/event.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/export/exporter.h"
#include "scanengine/gnss/crs.h"
#include "scanengine/gnss/georef.h"
#include "scanengine/merge/align.h"
#include "scanengine/merge/session.h"
#include "scanengine/plan/occupancy.h"
#include "scanengine/plan/plan_writers.h"

namespace lidarscan_jni {

using scanengine::PageStore;
using scanengine::ScanError;
using scanengine::Status;
using scanengine::StreamId;

namespace {

const char* err_name(ScanError e) { return scanengine::error_str(e); }

// A no-op deleter: the store belongs to the Engine, and jobs/ takes a
// shared_ptr. The Engine outlives every job by construction — ~Engine destroys
// (and joins) the JobQueue first, before the page store — so an aliasing
// shared_ptr here is a lifetime statement, not a leak.
std::shared_ptr<PageStore> borrow(PageStore& s) {
  return std::shared_ptr<PageStore>(&s, [](PageStore*) {});
}

}  // namespace

ProcessingEngine::ProcessingEngine() {
  scanengine::EngineConfig cfg;
  cfg.app_name = "lidarscan-processing";
  auto e = scanengine::Engine::create(cfg);
  if (!e.ok()) {
    last_error_ = std::string("could not create the processing engine: ") + err_name(e.error());
    return;
  }
  engine_ = std::move(e.value());
  store_ = borrow(engine_->points());

  // kJobProgress in CALLBACK mode. Queued mode would need a pump thread of our
  // own for something that is already delivered on the queue's worker thread;
  // the bus's own contract for a callback subscriber (quick, no publish, no
  // subscribe/unsubscribe, no re-entry) is exactly what forwarding one float to
  // Kotlin does.
  scanengine::SubscriptionOptions so;
  so.callback = [](const scanengine::Event& ev, void* user) {
    auto* self = static_cast<ProcessingEngine*>(user);
    if (ev.type != scanengine::EventType::kJobProgress) return;
    ProgressFn fn;
    {
      std::lock_guard<std::mutex> lock(self->m_);
      fn = self->progress_;
    }
    if (fn) fn(ev.payload.job.job_id, ev.payload.job.progress, static_cast<int>(ev.payload.job.state));
  };
  so.user_data = this;
  auto sub = engine_->events().subscribe(so);
  if (sub.ok()) sub_ = sub.value();
}

ProcessingEngine::~ProcessingEngine() {
  plan_cancel_.request_cancel();
  merge_cancel_.cancel();
  if (engine_ && sub_ != scanengine::kInvalidSubscription) {
    // unsubscribe() blocks until any in-flight callback returns, which is what
    // makes `this` safe to destroy afterwards.
    (void)engine_->events().unsubscribe(sub_);
  }
  {
    std::lock_guard<std::mutex> lock(m_);
    progress_ = nullptr;
  }
  // Order matters: the Engine's destructor joins the JobQueue worker, so no
  // job can still be dereferencing a colorizer when the map is cleared.
  engine_.reset();
  colorizers_.clear();
  merged_store_.reset();
  merge_inputs_.clear();
}

void ProcessingEngine::set_error(std::string msg) {
  std::lock_guard<std::mutex> lock(m_);
  last_error_ = std::move(msg);
}

std::string ProcessingEngine::last_error() {
  std::lock_guard<std::mutex> lock(m_);
  return last_error_;
}

void ProcessingEngine::set_progress_callback(ProgressFn fn) {
  std::lock_guard<std::mutex> lock(m_);
  progress_ = std::move(fn);
}

PageStore* ProcessingEngine::points() { return engine_ ? &engine_->points() : nullptr; }

std::uint64_t ProcessingEngine::total_points() const {
  return engine_ ? engine_->points().total_points() : 0;
}

bool ProcessingEngine::has_cloud() const { return total_points() > 0; }

// --- jobs -------------------------------------------------------------------

std::uint64_t ProcessingEngine::submit_post_process(const std::string& lscan_dir) {
  if (!engine_) return 0;
  scanengine::jobs::JobSpec spec;
  spec.kind = scanengine::jobs::JobKind::kPostProcess;
  spec.post.lscan_dir = lscan_dir;
  // Publish into THIS engine's store rather than one the pipeline owns: A15
  // routes a post job "at an externally-owned PageStore ... so the store
  // outlives the pipeline object", and here that store is also the one the
  // renderer is already reading pages out of, so the processed cloud appears
  // in the viewer with no extra plumbing.
  spec.post.store = store_;
  auto id = engine_->jobs().submit(std::move(spec));
  if (!id.ok()) {
    set_error(std::string("post-process was refused: ") + err_name(id.error()));
    return 0;
  }
  return id.value();
}

std::uint64_t ProcessingEngine::submit_colorize(std::uint64_t chain_from,
                                                const std::string& lscan_dir,
                                                const double camera_from_lidar[16],
                                                int sync_quality, bool allow_poor_sync,
                                                std::int64_t camera_clock_offset_ns) {
  if (!engine_) return 0;

  scanengine::color::ColorizeConfig ccfg;
  // SCAN_SYNC_* is a value-for-value mirror of A4's SyncQuality, and 0
  // (kUnknown) is what an unconverged estimator reports — the colorizer
  // refuses it before decoding a single image. Clamping an out-of-range int to
  // kUnknown keeps that failure closed rather than reinterpreting garbage as
  // kGood.
  ccfg.sync_quality = (sync_quality >= 0 && sync_quality <= 3)
                          ? static_cast<scanengine::SyncQuality>(sync_quality)
                          : scanengine::SyncQuality::kUnknown;
  ccfg.allow_poor_sync = allow_poor_sync;
  ccfg.camera_clock_offset_ns = camera_clock_offset_ns;
  // B8 records world_from_CAMERA poses (scanengine_c.h: "SCAN_KEYFRAME_POSE_CAMERA
  // = what B8 records"), so the extrinsic is validated but not applied.
  ccfg.pose_frame = scanengine::color::KeyframePoseFrame::kCamera;

  auto colorizer = std::make_unique<scanengine::color::PointColorizer>(ccfg);
  const Status ex = colorizer->set_extrinsics(camera_from_lidar);
  if (!ex.ok()) {
    // The A8 §4.4 trap: a column-major matrix across JNI produces a
    // plausible-looking mirrored result nobody notices until export. Named
    // here rather than relayed as a bare kInvalidArgument.
    set_error(std::string("the mount extrinsic was rejected (") + err_name(ex.error()) +
              ") — it must be a ROW-MAJOR rigid 4x4; a column-major matrix is refused on purpose");
    return 0;
  }

  scanengine::jobs::JobSpec spec;
  spec.kind = scanengine::jobs::JobKind::kColorize;
  spec.colorize.colorizer = colorizer.get();
  spec.colorize.lscan_dir = lscan_dir;  // load_keyframes() + FileImageSource
  std::memcpy(spec.colorize.camera_from_lidar, camera_from_lidar, sizeof(double) * 16);
  if (chain_from != 0) {
    spec.colorize.chain_from = chain_from;
  } else {
    spec.colorize.store = store_;
  }

  auto id = engine_->jobs().submit(std::move(spec));
  if (!id.ok()) {
    set_error(std::string("colorize was refused: ") + err_name(id.error()));
    return 0;
  }
  std::lock_guard<std::mutex> lock(m_);
  colorizers_.emplace(id.value(), std::move(colorizer));
  return id.value();
}

std::uint64_t ProcessingEngine::submit_export(std::uint64_t chain_from, int format,
                                              const std::string& output_path,
                                              const std::string& crs_wkt,
                                              const std::string& crs_epsg) {
  if (!engine_) return 0;
  scanengine::jobs::JobSpec spec;
  spec.kind = scanengine::jobs::JobKind::kExportPoints;
  spec.export_points.format = static_cast<scanengine::ExportFormat>(format);
  spec.export_points.output_path = output_path;
  spec.export_points.options.format = spec.export_points.format;
  spec.export_points.options.output_path = output_path;
  // A9's CRS seam: empty is not a mistake, it is the documented input for "the
  // cloud is still in the local frame", which makes LAS embed the ENGCRS
  // placeholder instead of mislabelling the file with a real CRS.
  spec.export_points.options.crs_wkt = crs_wkt;
  spec.export_points.options.crs_epsg = crs_epsg;
  if (chain_from != 0) {
    spec.export_points.chain_from = chain_from;
  } else {
    spec.export_points.store = store_;
  }
  auto id = engine_->jobs().submit(std::move(spec));
  if (!id.ok()) {
    set_error(std::string("export was refused: ") + err_name(id.error()));
    return 0;
  }
  return id.value();
}

std::uint64_t ProcessingEngine::submit_transfer_export(const std::string& project_dir,
                                                       const std::string& zip_path,
                                                       bool include_results) {
  if (!engine_) return 0;
  scanengine::jobs::JobSpec spec;
  spec.kind = scanengine::jobs::JobKind::kTransferExport;
  spec.transfer.project_dir = project_dir;
  spec.transfer.zip_path = zip_path;
  spec.transfer.include_results = include_results;
  auto id = engine_->jobs().submit(std::move(spec));
  if (!id.ok()) {
    set_error(std::string("transfer export was refused: ") + err_name(id.error()));
    return 0;
  }
  return id.value();
}

bool ProcessingEngine::cancel(std::uint64_t job_id) {
  if (!engine_) return false;
  const Status s = engine_->jobs().cancel(job_id);
  if (!s.ok()) {
    set_error(std::string("cancel failed: ") + err_name(s.error()));
    return false;
  }
  return true;
}

namespace {
JobSnapshot to_snapshot(const scanengine::jobs::Job& j) {
  JobSnapshot s;
  s.id = j.id;
  s.kind = static_cast<int>(j.kind);
  s.state = static_cast<int>(j.state);
  s.progress = j.progress;
  s.error = static_cast<std::int32_t>(j.error);
  s.stage = j.stage;
  s.message = j.message;
  return s;
}
}  // namespace

std::vector<JobSnapshot> ProcessingEngine::list_jobs() const {
  std::vector<JobSnapshot> out;
  if (!engine_) return out;
  for (const auto& j : engine_->jobs().list()) out.push_back(to_snapshot(j));
  return out;
}

JobSnapshot ProcessingEngine::job_status(std::uint64_t job_id) const {
  if (!engine_) return JobSnapshot{};
  return to_snapshot(engine_->jobs().status(job_id));
}

// --- A12 floor plan ---------------------------------------------------------

bool ProcessingEngine::run_plan(const scanengine::plan::PlanOptions& opts,
                                scanengine::plan::PlanModel* out) {
  if (!engine_ || out == nullptr) {
    set_error("no processing engine");
    return false;
  }
  if (engine_->points().total_points() == 0) {
    set_error("there is no cloud to slice — post-process the capture first");
    return false;
  }
  plan_cancel_.reset();
  plan_progress_.store(0.f);

  scanengine::plan::PlanInput in;
  in.store = &engine_->points();
  // Every page regardless of stream: a processed cloud lands on kSlamMap, but
  // a record-only D6 project that was never post-processed has only its raw
  // stream, and refusing to slice that would be a worse answer than slicing it.
  in.up = scanengine::plan::UpAxis::kZ;

  scanengine::plan::PlanModel model;
  const Status s = scanengine::plan::extract_floor_plan(
      in, opts, &model,
      [](float f, void* user) { static_cast<ProcessingEngine*>(user)->plan_progress_.store(f); },
      this, &plan_cancel_);
  if (!s.ok()) {
    set_error(std::string("floor-plan extraction failed: ") + err_name(s.error()));
    return false;
  }
  plan_progress_.store(1.f);
  {
    std::lock_guard<std::mutex> lock(m_);
    plan_ = std::make_unique<scanengine::plan::PlanModel>(model);
  }
  *out = std::move(model);
  return true;
}

void ProcessingEngine::cancel_plan() { plan_cancel_.request_cancel(); }
float ProcessingEngine::plan_progress() const { return plan_progress_.load(); }

bool ProcessingEngine::has_plan() const {
  std::lock_guard<std::mutex> lock(m_);
  return plan_ != nullptr;
}

bool ProcessingEngine::write_plan_dxf(const std::string& path) {
  std::lock_guard<std::mutex> lock(m_);
  if (!plan_) {
    last_error_ = "no floor plan has been extracted yet";
    return false;
  }
  scanengine::plan::DxfOptions o;
  const Status s = scanengine::plan::write_dxf(*plan_, o, path);
  if (!s.ok()) {
    last_error_ = std::string("DXF write failed: ") + err_name(s.error());
    return false;
  }
  return true;
}

bool ProcessingEngine::write_plan_pdf(const std::string& path, const std::string& title,
                                      const std::string& project, const std::string& date) {
  std::lock_guard<std::mutex> lock(m_);
  if (!plan_) {
    last_error_ = "no floor plan has been extracted yet";
    return false;
  }
  scanengine::plan::PdfOptions o;
  o.title = title.empty() ? std::string("Floor plan") : title;
  o.project = project;
  // NOTHING is derived from the clock here: A12's writer is deterministic on
  // purpose and takes the date from the caller, so the app passes the project's
  // own date rather than "now".
  o.date = date;
  const Status s = scanengine::plan::write_pdf(*plan_, o, path);
  if (!s.ok()) {
    last_error_ = std::string("PDF write failed: ") + err_name(s.error());
    return false;
  }
  return true;
}

// --- A13 merge --------------------------------------------------------------

scanengine::PageStore* ProcessingEngine::merged_points() { return merged_store_.get(); }
void ProcessingEngine::cancel_merge() { merge_cancel_.cancel(); }

MergeSummary ProcessingEngine::run_merge(const std::vector<MergeSessionInput>& sessions,
                                         const std::string& out_ply_path,
                                         const std::function<void(float, const char*)>& progress) {
  MergeSummary sum;
  if (!engine_) {
    sum.message = "no processing engine";
    return sum;
  }
  if (sessions.size() < 2) {
    sum.message = "a merge needs at least two sessions";
    return sum;
  }
  merge_cancel_.reset();

  const int georeferenced =
      static_cast<int>(std::count_if(sessions.begin(), sessions.end(), [](const MergeSessionInput& s) {
        return s.georef_valid && s.georef_converged;
      }));
  if (georeferenced < 2) {
    // The polite refusal Tech Spec §3.10 asks for: "Android offers
    // georeferenced auto-merge only" — the manual 3-point/drag path is the
    // desktop merge workbench's (C6), and pretending otherwise here would put
    // two clouds on top of each other at the identity, which A13 itself calls
    // "the worst possible failure mode — it looks like data".
    sum.blocker = "not georeferenced";
    sum.message =
        "Only " + std::to_string(georeferenced) +
        " of the selected sessions has a converged georeference. Automatic merge places sessions by "
        "composing through their shared CRS, so at least two need one. Manual 3-point alignment is "
        "the desktop app's merge workbench.";
    return sum;
  }

  merge_inputs_.clear();
  scanengine::merge::MergeProject project;

  float base = 0.f;
  const float per_session = 0.5f / static_cast<float>(sessions.size());

  for (std::size_t i = 0; i < sessions.size(); ++i) {
    const MergeSessionInput& in = sessions[i];
    if (merge_cancel_.cancelled()) {
      sum.message = "cancelled";
      return sum;
    }
    if (progress) progress(base, ("preparing " + in.provenance_id).c_str());

    std::shared_ptr<PageStore> store;
    if (in.chain_from_job != 0) {
      store = engine_->jobs().produced_store(in.chain_from_job);
      if (!store) {
        sum.message = "session \"" + in.provenance_id +
                      "\" pointed at job " + std::to_string(in.chain_from_job) +
                      ", which has not finished successfully";
        return sum;
      }
    } else {
      // Post-process it now. This is the expensive half of an Android merge
      // and the UI says so: A13 cannot read a cloud out of a `.lscan` (nothing
      // writes a processed cloud into one — merge/merge.h's own note), so
      // "open two finished projects and merge them" necessarily means re-running
      // the pipeline for each.
      auto own = std::make_shared<PageStore>(scanengine::PageStoreConfig{});
      scanengine::post::PostConfig pcfg;
      pcfg.store = own.get();
      scanengine::post::PostSlamPipeline pipe(pcfg);
      pipe.set_cancel_token(&merge_cancel_);
      const float slot_base = base;
      pipe.set_progress_callback([&](const scanengine::post::PostProgress& p) {
        if (progress) progress(slot_base + per_session * p.fraction, p.label);
      });
      const Status s = pipe.run(in.lscan_dir);
      if (!s.ok()) {
        sum.message = "post-processing \"" + in.provenance_id + "\" failed: " + err_name(s.error());
        return sum;
      }
      store = own;
    }
    merge_inputs_.push_back(store);

    scanengine::merge::SessionInput si;
    si.provenance_id = in.provenance_id;
    si.lscan_dir = in.lscan_dir;
    // Every page regardless of stream — a post-processed session's cloud is on
    // kSlamMap, and taking "every page" also carries a pushbroom-assembled D6
    // cloud, which lands on the same stream.
    const Status cs =
        scanengine::merge::collect_pages(*store, StreamId::kUnknown, &si.cloud);
    if (!cs.ok() || si.cloud.empty()) {
      sum.message = "session \"" + in.provenance_id + "\" produced no points to merge";
      return sum;
    }

    if (in.georef_valid) {
      si.georef.valid = true;
      si.georef.epsg = in.epsg;
      si.georef.solution.converged = in.georef_converged;
      std::memcpy(si.georef.solution.global_from_local, in.global_from_local, sizeof(double) * 16);
      si.georef.solution.horizontal_sigma_m = in.horizontal_sigma_m;
      scanengine::crs::Geodetic origin;
      origin.lat_deg = in.enu_origin_lat_deg;
      origin.lon_deg = in.enu_origin_lon_deg;
      origin.height_m = in.enu_origin_height_m;
      // merge/session.h: "THE ENU FRAME IS NOT OPTIONAL AND IS NOT SHARED" —
      // two sessions captured on two days anchor two different ENU origins, so
      // "same CRS" does not mean "same frame"; align_georeferenced() composes
      // ENU_a -> ECEF -> ENU_b, and it needs this to do it.
      si.georef.enu = scanengine::crs::make_enu_frame(origin);
    }

    auto added = project.add_session(si);
    if (!added.ok()) {
      sum.message = "could not add \"" + in.provenance_id + "\": " + err_name(added.error());
      return sum;
    }
    sum.input_points += si.cloud.point_count();
    base += per_session;
  }

  if (progress) progress(0.55f, "aligning through the shared CRS");
  scanengine::merge::MergeProject::GeorefAlignReport align;
  const Status as = project.align_georeferenced(&align);
  if (!as.ok()) {
    sum.message = std::string("georeferenced alignment failed: ") + err_name(as.error());
    if (align.blocker != nullptr && align.blocker[0] != '\0') sum.blocker = align.blocker;
    return sum;
  }
  sum.sessions_aligned = align.aligned;
  sum.sessions_skipped = align.skipped;
  sum.epsg_mismatch = align.epsg_mismatch;

  if (progress) progress(0.65f, "refining with ICP");
  scanengine::merge::RefineConfig rcfg;
  const Status rs = project.refine(rcfg, &merge_cancel_);
  if (!rs.ok()) {
    sum.message = std::string("ICP refinement failed: ") + err_name(rs.error());
    return sum;
  }
  const auto& report = project.report();
  sum.pairs_refined = report.pairs_refined;
  sum.pairs_converged = report.pairs_converged;
  sum.pairs_low_overlap = report.pairs_low_overlap;
  sum.worst_rms_m = report.worst_rms_m;
  sum.worst_overlap = report.worst_overlap;

  if (progress) progress(0.85f, "building the merged cloud");
  scanengine::merge::MergeOutputConfig ocfg;
  scanengine::merge::MergeResult result;
  const Status bs = project.build(ocfg, &result, &merge_cancel_);
  if (!bs.ok()) {
    sum.message = std::string("merge build failed: ") + err_name(bs.error());
    return sum;
  }
  sum.merged_points = static_cast<std::uint64_t>(result.cloud.size());

  merged_store_ = std::make_unique<PageStore>(scanengine::PageStoreConfig{});
  const Status ps = project.publish(&result, merged_store_.get());
  if (!ps.ok()) {
    sum.message = std::string("publishing the merged cloud failed: ") + err_name(ps.error());
    return sum;
  }

  if (!out_ply_path.empty()) {
    if (progress) progress(0.95f, "writing the merged cloud");
    scanengine::ExportOptions eo;
    eo.format = scanengine::ExportFormat::kPlyBinary;
    eo.output_path = out_ply_path;
    const Status es = scanengine::export_points(
        *merged_store_, scanengine::Span<const StreamId>{}, scanengine::ExportFormat::kPlyBinary,
        out_ply_path, eo);
    if (!es.ok()) {
      sum.message = std::string("merged export failed: ") + err_name(es.error());
      return sum;
    }
  }

  if (progress) progress(1.0f, "done");
  sum.ok = true;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "%u of %zu sessions aligned through the shared CRS; %u pair(s) refined, worst RMS %.3f m",
                align.aligned, sessions.size(), report.pairs_refined,
                static_cast<double>(report.worst_rms_m));
  sum.message = buf;
  return sum;
}

}  // namespace lidarscan_jni
