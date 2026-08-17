// d6_resolve.cpp — see d6_resolve.h for why this is not "A7 with a D6 branch".
//
// The whole pipeline is one pass over the container:
//
//   kOpening        FileRecordReader::open + the mount extrinsic + sizing
//   kReintegration  kD6Raw -> D6Driver -> assembler; kPoseAr -> pose source
//   kPublishing     assembler flush()
//
// Only three of A7's stages appear, and that is honest rather than lazy: a D6
// has no odometry to run, no loops to detect and no graph to optimize (the
// trajectory was measured, not estimated). Reusing kReintegration as the label
// for the resolve pass is deliberate too — it is exactly what that stage means
// in A7: "points through the trajectory".
#include "scanengine/slam/post/d6_resolve.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "scanengine/core/event_bus.h"
#include "scanengine/core/log.h"
#include "scanengine/drivers/d6/d6_driver.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/record/lscan.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "d6resolve";

// Stage weights, fixed for the same reason A7 fixes its own: a bar that runs
// backwards is worse than one that is slightly wrong. Opening is a whole
// validation pass over every stream file, which on a 400 KB capture is most of
// the wall time, so it is not the rounding error it looks like.
constexpr float kOpenFraction = 0.05f;
constexpr float kResolveFraction = 0.93f;

}  // namespace

// --- manifest mount extrinsic -----------------------------------------------
//
// A hand-rolled field read rather than a JSON parser, following A5's own
// precedent in lscan.cpp (`looks_like_valid_json_object` is a brace counter,
// not a schema validator) and the dependency-light rule for workstream A. The
// shape is fixed and written by one function ten lines away in the same
// codebase, so the risk this trades away is small and the alternative is a new
// third-party dependency in the engine's core.
bool read_manifest_mount(const std::string& lscan_dir, double phone_from_lidar[16]) {
  if (phone_from_lidar == nullptr) return false;
  std::ifstream in(lscan_dir + "/" + lscan::kManifestFile, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string j = ss.str();

  const std::size_t key = j.find("\"phoneFromLidar\"");
  if (key == std::string::npos) return false;
  const std::size_t open = j.find('[', key);
  if (open == std::string::npos) return false;
  const std::size_t close = j.find(']', open);
  if (close == std::string::npos) return false;

  double m[16];
  int n = 0;
  const char* p = j.c_str() + open + 1;
  const char* end = j.c_str() + close;
  while (p < end && n < 16) {
    char* stop = nullptr;
    const double v = std::strtod(p, &stop);
    if (stop == p) {  // not a number here — skip one char and keep looking
      ++p;
      continue;
    }
    if (!std::isfinite(v)) return false;
    m[n++] = v;
    p = stop;
    while (p < end && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p;
  }
  // Exactly 16, never "the first 16 of more": a longer array means the writer
  // and this reader disagree about the shape, and guessing which 16 to take is
  // how a cloud ends up mirrored.
  if (n != 16) return false;
  for (int i = 0; i < 16; ++i) phone_from_lidar[i] = m[i];
  return true;
}

Status lscan_is_d6_project(const std::string& lscan_dir, bool* is_d6) {
  if (is_d6 == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "d6resolve: is_d6 is null");
  }
  *is_d6 = false;
  lscan::FileRecordReader reader;
  SCAN_TRY(reader.open(lscan_dir));
  bool has_d6 = false;
  bool has_mid360 = false;
  for (const auto& s : reader.stream_summaries()) {
    if (s.chunk_count == 0) continue;
    if (s.stream == StreamId::kLidarD6) has_d6 = true;
    if (s.stream == StreamId::kLidarMid360 || s.stream == StreamId::kImu) has_mid360 = true;
  }
  (void)reader.close();
  // A container holding both is a rig this product does not ship, and A7's
  // pipeline is the better answer for it (the Mid-360 carries its own IMU and
  // does not need the phone). So "D6 project" means D6 AND NOT Mid-360, and
  // the ambiguous case routes to A7 rather than to a guess.
  *is_d6 = has_d6 && !has_mid360;
  return kOkStatus;
}

Status load_recorded_cloud(const std::string& lscan_dir, PageStore* store, StreamId out_stream,
                           std::uint64_t* out_points) {
  if (store == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "d6resolve: no PageStore to load into");
  }
  if (out_points != nullptr) *out_points = 0;

  static_assert(sizeof(PointVertex) == lscan::kPointVertexBytes,
                "the kPointsXyzRgba payload IS a PointVertex array; if PointVertex changes size "
                "the format changes with it and needs a new chunk type, not a silent reinterpret");

  lscan::FileRecordReader reader;
  SCAN_TRY(reader.open(lscan_dir));

  std::uint64_t loaded = 0;
  for (;;) {
    lscan::ChunkHeader h{};
    std::vector<std::uint8_t> payload;
    const Status st = reader.next_chunk(&h, &payload);
    if (st.error() == ScanError::kAgain) break;
    if (!st.ok()) {
      (void)reader.close();
      return st;
    }
    if (h.type != lscan::ChunkType::kPointsXyzRgba) continue;
    // A payload that is not a whole number of vertices is a torn chunk that
    // somehow passed CRC, which should be impossible; skipping it is cheaper
    // than trusting it, and the re-resolve path can rebuild what it skipped.
    if (payload.empty() || (payload.size() % lscan::kPointVertexBytes) != 0) continue;
    const std::size_t n = payload.size() / lscan::kPointVertexBytes;
    std::vector<PointVertex> pts(n);
    std::memcpy(pts.data(), payload.data(), payload.size());
    const Status ap =
        store->append(out_stream, Span<const PointVertex>(pts.data(), pts.size()), h.t_mono_ns);
    if (!ap.ok()) {
      // PageStore backpressure (the store is full) is a real answer, not a
      // corruption: report what was loaded and let the caller decide.
      SCAN_LOG_WARN(kMod, "recorded cloud: store refused %zu points at %lld ns: %s", n,
                    static_cast<long long>(h.t_mono_ns), error_str(ap.error()));
      break;
    }
    loaded += n;
  }
  (void)reader.close();
  if (out_points != nullptr) *out_points = loaded;
  SCAN_LOG_INFO(kMod, "recorded cloud: %llu points loaded from '%s'",
                static_cast<unsigned long long>(loaded), lscan_dir.c_str());
  return kOkStatus;
}

// --- the pipeline -----------------------------------------------------------

struct D6ResolvePipeline::Impl {
  D6ResolveConfig cfg;
  PostProgressFn progress;
  CancelToken* cancel = nullptr;
  D6ResolveStats stats{};

  // Owned for the run. The assembler holds a raw pointer to the pose source,
  // and the driver's profile sink holds a raw pointer to this Impl, so both
  // must outlive the decode loop — which they do, being members.
  std::unique_ptr<ExternalPoseSource> poses;
  std::unique_ptr<D6PushbroomAssembler> assembler;

  void report(PostStage stage, float fraction, float stage_fraction, std::uint64_t done,
              std::uint64_t total) {
    if (!progress) return;
    PostProgress p{};
    p.stage = stage;
    p.label = to_string(stage);
    p.fraction = fraction;
    p.stage_fraction = stage_fraction;
    p.done = done;
    p.total = total;
    progress(p);
  }

  // The D6 driver's per-point sink. This is the SAME callback shape
  // Engine::Impl::on_d6_profile installs for a live capture, and it does the
  // same one thing: hand the point to the assembler with the point's OWN
  // timestamp. ROUND 7's whole walls-are-straight fix lives on the other side
  // of this boundary (D6Driver::feed_time_sliced back-dates each byte slice),
  // so an offline resolve inherits it for free — and would lose it instantly
  // if this file decoded packets itself instead.
  static void on_profile(float angle_deg, float range_m, std::uint8_t intensity,
                         std::uint8_t high_reflectivity, std::int64_t t_engine_ns, void* user) {
    auto* self = static_cast<Impl*>(user);
    ProfilePoint p{};
    p.t_mono_ns = t_engine_ns;
    p.angle_deg = angle_deg;
    p.range_m = range_m;
    p.intensity = intensity;
    p.high_reflectivity = high_reflectivity;
    (void)self->assembler->push_profile(Span<const ProfilePoint>(&p, 1));
  }
};

D6ResolvePipeline::D6ResolvePipeline(const D6ResolveConfig& cfg) : impl_(new Impl()) {
  impl_->cfg = cfg;
}
D6ResolvePipeline::~D6ResolvePipeline() = default;

void D6ResolvePipeline::set_progress_callback(PostProgressFn fn) {
  impl_->progress = std::move(fn);
}
void D6ResolvePipeline::set_cancel_token(CancelToken* token) { impl_->cancel = token; }
const D6ResolveStats& D6ResolvePipeline::stats() const { return impl_->stats; }

Status D6ResolvePipeline::run(const std::string& lscan_dir) {
  Impl& s = *impl_;
  s.stats = D6ResolveStats{};

  if (s.cfg.store == nullptr) {
    return set_last_error(ScanError::kInvalidArgument,
                          "d6resolve: no PageStore to publish into");
  }

  s.report(PostStage::kOpening, 0.f, 0.f, 0, 0);

  // --- the extrinsic, before anything else ---------------------------------
  //
  // Resolved first because failing here costs nothing, and failing after a
  // full decode pass costs the operator a minute for a refusal that was
  // knowable up front.
  double mount[16];
  if (s.cfg.have_mount) {
    for (int i = 0; i < 16; ++i) mount[i] = s.cfg.mount_phone_from_lidar[i];
  } else if (read_manifest_mount(lscan_dir, mount)) {
    s.stats.mount_from_manifest = true;
  } else {
    return set_last_error(
        ScanError::kNotFound,
        "d6resolve: '%s' has no mount extrinsic — its manifest predates ROUND 8's "
        "\"mountCalibration\" and the caller supplied none. A COIN-D6's returns are in the "
        "lidar's own frame; without phone_from_lidar there is no way to say where that frame "
        "sat on the rig, and resolving through identity would produce a confidently wrong room.",
        lscan_dir.c_str());
  }

  lscan::FileRecordReader reader;
  SCAN_TRY(reader.open(lscan_dir));

  std::uint64_t total_chunks = 0;
  bool saw_d6 = false;
  bool saw_poses = false;
  for (const auto& sum : reader.stream_summaries()) {
    total_chunks += sum.chunk_count;
    if (sum.stream == StreamId::kLidarD6 && sum.chunk_count > 0) saw_d6 = true;
    if (sum.stream == StreamId::kPoseAr && sum.chunk_count > 0) saw_poses = true;
  }

  if (!saw_d6) {
    (void)reader.close();
    return set_last_error(ScanError::kNotFound,
                          "d6resolve: '%s' holds no COIN-D6 chunks", lscan_dir.c_str());
  }
  if (!saw_poses) {
    // THE pre-0.5.0 case, and the message is written for the person who will
    // read it in a UI rather than for a log grep. `poses_read == 0` is the
    // machine-readable half.
    (void)reader.close();
    return set_last_error(
        ScanError::kNotFound,
        "d6resolve: '%s' holds no trajectory. A COIN-D6 is a 2D lidar — its third dimension "
        "is entirely the phone's motion — so a recording without poses cannot be rebuilt into "
        "a 3D cloud by anything, ever. This capture was recorded before LidarScan 0.5.0 stored "
        "the trajectory alongside the returns; scans taken from 0.5.0 on re-resolve normally.",
        lscan_dir.c_str());
  }

  // --- the assembler and the pose source, configured exactly as live -------
  ExternalPoseConfig pc;
  pc.stream = StreamId::kPoseAr;
  pc.capacity = s.cfg.pose_capacity;
  // No TimeSync: the recorded stamps are already engine time. A4 maps kPoseAr
  // with the identity (offset_estimator.cpp: "ARCore is already
  // CLOCK_BOOTTIME"), so applying a mapping here would be a second identity at
  // best and a drift at worst.
  s.poses = std::make_unique<ExternalPoseSource>(pc);
  SCAN_TRY(s.poses->start());

  PushbroomConfig bc = s.cfg.pushbroom;
  bc.out_stream = s.cfg.out_stream;
  s.assembler = std::make_unique<D6PushbroomAssembler>(s.cfg.store, bc);
  s.assembler->set_pose_source(s.poses.get());
  {
    const Status st = s.assembler->set_mount_extrinsics(mount);
    if (!st.ok()) {
      (void)reader.close();
      return st;
    }
  }

  // --- the driver ----------------------------------------------------------
  //
  // Receive-only: no serial write function, so no start/stop command bytes are
  // synthesized at a transport that does not exist. `points = nullptr` is not
  // an option (DriverContext::valid() requires it) and would be wrong anyway —
  // the driver's raw sensor-frame preview must NOT land in the caller's store,
  // or the resolved cloud would come back with a flat 2D fan mixed into it.
  // (That is not hypothetical: it is precisely the bug ROUND 8 found in the
  // Android preview writer, from the other end.) So it gets a scratch store of
  // its own, which is discarded with this function's stack frame.
  EventBus scratch_bus;
  PageStore scratch_points;
  DriverContext ctx;
  ctx.bus = &scratch_bus;
  ctx.points = &scratch_points;
  ctx.timesync = nullptr;

  D6Config dcfg{};
  dcfg.send_start_stop_commands = false;
  dcfg.require_start_ack = false;
  dcfg.profile_sink = &Impl::on_profile;
  dcfg.profile_sink_user_data = &s;
  D6Driver driver(kInvalidDeviceId, dcfg, ctx);
  SCAN_TRY(driver.start());

  // --- one pass ------------------------------------------------------------
  std::uint64_t seen = 0;
  for (;;) {
    if (cancelled(s.cancel)) {
      (void)reader.close();
      s.report(PostStage::kCancelled, 0.f, 0.f, seen, total_chunks);
      return set_last_error(ScanError::kCancelled, "d6resolve: cancelled");
    }

    lscan::ChunkHeader h{};
    std::vector<std::uint8_t> payload;
    const Status st = reader.next_chunk(&h, &payload);
    if (st.error() == ScanError::kAgain) break;
    if (!st.ok()) {
      (void)reader.close();
      return st;
    }
    ++seen;

    if (h.type == lscan::ChunkType::kPoseAr) {
      lscan::PoseChunkRecord rec{};
      if (lscan::decode_pose_chunk(ByteSpan(payload.data(), payload.size()), &rec)) {
        ++s.stats.poses_read;
        Pose p{};
        p.t_mono_ns = h.t_mono_ns;
        for (int i = 0; i < 3; ++i) p.position[i] = rec.position[i];
        for (int i = 0; i < 4; ++i) p.orientation[i] = rec.orientation[i];
        p.position_sigma_m = rec.position_sigma_m;
        p.orientation_sigma_deg = rec.orientation_sigma_deg;
        p.source = static_cast<StreamId>(rec.source);
        p.quality = static_cast<PoseQuality>(rec.quality);
        p.tracking_lost = rec.tracking_lost;
        if (s.poses->push_pose(p).ok()) ++s.stats.poses_accepted;
      }
    } else if (h.type == lscan::ChunkType::kD6Raw) {
      ++s.stats.lidar_chunks;
      s.stats.lidar_bytes += payload.size();
      // The chunk's recorded stamp, exactly as the live path passed it — which
      // is what makes ROUND 7's byte-position back-dating reproduce the same
      // per-point times it produced during the capture.
      (void)driver.push_bytes(ByteSpan(payload.data(), payload.size()),
                              TimePoint{h.t_mono_ns});
    }
    // Everything else (kPointsXyzRgba from a previous resolve, GNSS, camera
    // frames) is skipped by length, per the format's forward-compatibility
    // rule. Notably the recorded map is NOT read back here: this is the path
    // that RE-DERIVES it, and reading the cache would defeat the point.

    if ((seen & 0x3F) == 0 && total_chunks > 0) {
      const float f = static_cast<float>(seen) / static_cast<float>(total_chunks);
      s.report(PostStage::kReintegration, kOpenFraction + kResolveFraction * f, f, seen,
               total_chunks);
    }
  }

  (void)reader.close();
  (void)driver.stop();

  s.report(PostStage::kPublishing, kOpenFraction + kResolveFraction, 1.f, seen, total_chunks);
  SCAN_TRY(s.assembler->flush());

  s.stats.pushbroom = s.assembler->stats();
  s.stats.points_out = s.stats.pushbroom.points_out;

  SCAN_LOG_INFO(kMod,
                "resolved '%s': %llu D6 chunks / %llu bytes, %llu poses (%llu accepted) -> "
                "%llu world points (mount from %s)",
                lscan_dir.c_str(), static_cast<unsigned long long>(s.stats.lidar_chunks),
                static_cast<unsigned long long>(s.stats.lidar_bytes),
                static_cast<unsigned long long>(s.stats.poses_read),
                static_cast<unsigned long long>(s.stats.poses_accepted),
                static_cast<unsigned long long>(s.stats.points_out),
                s.stats.mount_from_manifest ? "manifest" : "caller");

  if (s.stats.points_out == 0) {
    // A resolve that produced nothing is a failure even though every step
    // returned kOk — ROUND 7 §3's rule, that a zero-point result must never be
    // reported as success, applied to the offline path.
    return set_last_error(
        ScanError::kNotFound,
        "d6resolve: '%s' resolved to 0 points from %llu returns — %llu were dropped for want of "
        "a pose covering their timestamp and %llu were outside the D6's range window",
        lscan_dir.c_str(), static_cast<unsigned long long>(s.stats.pushbroom.points_in),
        static_cast<unsigned long long>(s.stats.pushbroom.dropped_no_pose),
        static_cast<unsigned long long>(s.stats.pushbroom.dropped_range));
  }

  s.report(PostStage::kDone, 1.f, 1.f, seen, total_chunks);
  return kOkStatus;
}

}  // namespace post
}  // namespace scanengine
