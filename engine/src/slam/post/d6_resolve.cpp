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

#include <algorithm>
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
#include "scanengine/drivers/d6/d6_fan.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/poses/imu_densified_pose.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"
#include "scanengine/slam/post/trajectory_loop.h"

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
// ROUND 9: the same hand-rolled read for the phone-IMU extrinsic. Kept as a
// separate function rather than a templated one because the arity is the whole
// safety property here — "exactly N, never the first N of more" is what stops a
// writer/reader disagreement from silently producing a plausible wrong frame.
bool read_manifest_array(const std::string& lscan_dir, const char* key_name,
                         double* out, int want) {
  if (out == nullptr) return false;
  std::ifstream in(lscan_dir + "/" + lscan::kManifestFile, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string j = ss.str();

  const std::size_t key = j.find(key_name);
  if (key == std::string::npos) return false;
  const std::size_t open = j.find('[', key);
  if (open == std::string::npos) return false;
  const std::size_t close = j.find(']', open);
  if (close == std::string::npos) return false;

  std::vector<double> m;
  const char* p = j.c_str() + open + 1;
  const char* end = j.c_str() + close;
  while (p < end && static_cast<int>(m.size()) < want) {
    char* stop = nullptr;
    const double v = std::strtod(p, &stop);
    if (stop == p) { ++p; continue; }
    if (!std::isfinite(v)) return false;
    m.push_back(v);
    p = stop;
    while (p < end && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p;
  }
  if (static_cast<int>(m.size()) != want) return false;
  for (int i = 0; i < want; ++i) out[i] = m[i];
  return true;
}

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

namespace {

// Read ONE chunk file (stream header + framed chunks) directly, rather than
// through FileRecordReader — that class walks a hard-coded list of
// `streams/*.bin` (ROUND 9 named this) and a derived product in `processed/`
// is deliberately not on it.
Status load_chunk_file_(const std::string& path, PageStore* store, StreamId out_stream,
                        std::uint64_t* out_points) {
  if (out_points != nullptr) *out_points = 0;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return ScanError::kNotFound;
  std::uint8_t sh[lscan::kStreamHeaderBytes];
  if (std::fread(sh, 1, sizeof(sh), f) != sizeof(sh)) {
    std::fclose(f);
    return ScanError::kIoError;
  }
  lscan::StreamFileHeader hdr{};
  if (!lscan::decode_stream_header(ByteSpan(sh, sizeof(sh)), &hdr)) {
    std::fclose(f);
    return ScanError::kInvalidArgument;
  }
  std::uint64_t loaded = 0;
  std::vector<std::uint8_t> payload;
  for (;;) {
    std::uint8_t ch[lscan::kChunkHeaderBytes];
    if (std::fread(ch, 1, sizeof(ch), f) != sizeof(ch)) break;  // clean EOF
    lscan::ChunkHeader h{};
    if (!lscan::decode_chunk_header(ByteSpan(ch, sizeof(ch)), &h)) break;
    payload.resize(h.payload_len);
    if (h.payload_len > 0 && std::fread(payload.data(), 1, h.payload_len, f) != h.payload_len) {
      break;
    }
    std::uint8_t trailer[lscan::kChunkTrailerBytes];
    if (std::fread(trailer, 1, sizeof(trailer), f) != sizeof(trailer)) break;
    const std::uint32_t want = static_cast<std::uint32_t>(trailer[0]) |
                               (static_cast<std::uint32_t>(trailer[1]) << 8) |
                               (static_cast<std::uint32_t>(trailer[2]) << 16) |
                               (static_cast<std::uint32_t>(trailer[3]) << 24);
    // A derived file is regenerable, so a bad CRC is a reason to fall back to
    // the sealed cache rather than to fail: stop here and report what loaded.
    if (lscan::chunk_crc(h, ByteSpan(payload.data(), payload.size())) != want) break;
    if (h.type != lscan::ChunkType::kPointsXyzRgba) continue;
    if (payload.empty() || (payload.size() % lscan::kPointVertexBytes) != 0) continue;
    const std::size_t n = payload.size() / lscan::kPointVertexBytes;
    std::vector<PointVertex> pts(n);
    std::memcpy(pts.data(), payload.data(), payload.size());
    if (!store->append(out_stream, Span<const PointVertex>(pts.data(), pts.size()), h.t_mono_ns)
             .ok()) {
      break;
    }
    loaded += n;
  }
  std::fclose(f);
  if (out_points != nullptr) *out_points = loaded;
  return loaded > 0 ? kOkStatus : Status(ScanError::kNotFound);
}

}  // namespace

Status load_recorded_cloud(const std::string& lscan_dir, PageStore* store, StreamId out_stream,
                           std::uint64_t* out_points) {
  if (store == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "d6resolve: no PageStore to load into");
  }
  if (out_points != nullptr) *out_points = 0;

  static_assert(sizeof(PointVertex) == lscan::kPointVertexBytes,
                "the kPointsXyzRgba payload IS a PointVertex array; if PointVertex changes size "
                "the format changes with it and needs a new chunk type, not a silent reinterpret");

  // ROUND 13: a container that has been Processed carries a CORRECTED cloud in
  // `processed/map_stitched.bin`, and that is what the viewer must draw — the
  // whole point of the button is that the room stops being in five pieces.
  // Preferred here, at the one function every reader already goes through,
  // rather than at each caller: Review, the thumbnail, Export and Colorize all
  // reach the cloud this way and every one of them wants the corrected answer.
  // Deleting the file returns the container to exactly what the phone sealed.
  {
    std::uint64_t n = 0;
    const Status stitched = load_chunk_file_(lscan_dir + "/processed/map_stitched.bin", store,
                                             out_stream, &n);
    if (stitched.ok() && n > 0) {
      if (out_points != nullptr) *out_points = n;
      SCAN_LOG_INFO(kMod, "recorded cloud: %llu points loaded from the STITCHED map of '%s'",
                    static_cast<unsigned long long>(n), lscan_dir.c_str());
      return kOkStatus;
    }
  }

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
  // ROUND 9: wraps `poses`, and is what the assembler resolves against when
  // D6ResolveConfig::densify_with_phone_imu is on. Declared after `poses` so it
  // is destroyed first — it holds a raw pointer to it.
  std::unique_ptr<ImuDensifiedPoseSource> densified;
  std::unique_ptr<D6PushbroomAssembler> assembler;

  // ROUND 17 item 63. `densified`'s ring is 8 s deep by design — it exists to
  // bridge 33 ms brackets, and a ring that held a whole capture would be a
  // megabyte of cache miss on the hot path. Bridging a TRACKING GAP asks the
  // same integrator a question about a moment that may be minutes in the past,
  // so the gap bridge gets its own source over a kept copy of the stream,
  // built once after the decode and thrown away with the run. Kept only when
  // stitching will actually ask (12.8 KB per second of capture).
  std::vector<PhoneImuSample> imu_kept;
  std::unique_ptr<ImuDensifiedPoseSource> gap_gyro;
  // The config `densified` was built with, so the gap bridge can be built with
  // the same extrinsic and the same hole tolerance.
  ImuDensifyConfig densified_cfg{};

  // ROUND 19 item 74. Every decoded return, kept ONLY when the caller asked
  // for loss-window recovery: 4 k samples/s at 24 bytes is ~15 MB for a
  // three-minute capture, which an offline pass can afford and the live path
  // is never asked to. Kept rather than re-read because the driver's ROUND-7
  // byte-position back-dating happens during THIS decode — a second pass over
  // the raw chunks would have to reproduce it exactly to stamp the same
  // times, and one decode with one stamping is how that stays true.
  bool keep_profiles = false;
  std::vector<ProfilePoint> profile_kept;

  // ROUND 11 item 41. Owned when the caller did not supply its own sinks, so
  // that `close_loops` works without the caller having to know it needs them.
  std::vector<TrajPose> traj_own;
  std::vector<std::int64_t> times_own;
  std::vector<TrajPose>* traj = nullptr;
  std::vector<std::int64_t>* times = nullptr;

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
    if (self->keep_profiles) self->profile_kept.push_back(p);
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
  s.imu_kept.clear();
  s.profile_kept.clear();
  s.gap_gyro.reset();
  s.keep_profiles = s.cfg.recover_gap_points;

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

  // --- ROUND 9: the same densifier the live pass uses ----------------------
  //
  // BACKWARD-ONLY, which is what makes this safe under `drain_on_push`. That
  // flag is on here (see D6ResolveConfig::pushbroom), so a return is resolved
  // the instant the chunk carrying it is decoded — there is no second pass to
  // fix up a query that guessed. The densifier never needs one: for a query at
  // `t` it integrates the gyro over the bracket [ta, tb] the wrapped source
  // ALREADY chose, and that bracket exists only once the pose at tb has been
  // pushed. FileRecordReader hands chunks back in non-decreasing t_mono_ns
  // order across every stream, so by the time the kPoseAr chunk at tb arrives,
  // every kPhoneImu chunk stamped at or before tb has arrived too — the ring
  // covers [ta, tb] completely, and nothing is ever read from the future.
  //
  // When it does NOT cover it (a container whose IMU stream is short, sparse
  // or absent) the densifier's own coverage check fails and the query falls
  // back to the plain interpolation. So the failure mode of getting this wrong
  // is "ROUND 8's answer", never a wrong one.
  ImuDensifyConfig icfg;
  icfg.capacity = s.cfg.imu_capacity > 0 ? s.cfg.imu_capacity : 1;
  // Caller first, then the container's own manifest. ROUND 9 added
  // `"imuCalibration": {"cameraFromImu": [...]}` for exactly this: a container
  // carrying a kPhoneImu stream but no IMU extrinsic is self-contained only by
  // accident, because the gyro is in the Android sensor frame while ARCore's
  // pose is in the camera frame and on a real phone those differ by the
  // camera's SENSOR_ORIENTATION. Getting it wrong does not fail loudly — it
  // distorts the densified path between pose knots, which is the one thing the
  // gyro was added to improve.
  if (s.cfg.have_imu_extrinsics) {
    for (int i = 0; i < 4; ++i) icfg.camera_from_imu[i] = s.cfg.imu_camera_from_imu[i];
  } else {
    double q[4];
    if (read_manifest_array(lscan_dir, "\"cameraFromImu\"", q, 4)) {
      for (int i = 0; i < 4; ++i) icfg.camera_from_imu[i] = q[i];
      s.stats.imu_extrinsics_from_manifest = true;
    }
  }
  s.densified_cfg = icfg;
  s.densified = std::make_unique<ImuDensifiedPoseSource>(s.poses.get(), icfg);

  // --- ROUND 11 item 41: the sinks loop closure needs -----------------------
  //
  // Chosen BEFORE the assembler is built because `out_point_times` is part of
  // its config, and a sink attached afterwards would miss the first batch.
  s.traj = s.cfg.out_trajectory;
  s.times = s.cfg.out_point_times;
  if (s.cfg.close_loops || s.cfg.stitch_sections) {
    if (s.traj == nullptr) s.traj = &s.traj_own;
    if (s.times == nullptr) s.times = &s.times_own;
  }

  PushbroomConfig bc = s.cfg.pushbroom;
  bc.out_stream = s.cfg.out_stream;
  bc.out_point_times = s.times;
  s.assembler = std::make_unique<D6PushbroomAssembler>(s.cfg.store, bc);
  s.assembler->set_pose_source(s.cfg.densify_with_phone_imu
                                   ? static_cast<const PoseInterpolator*>(s.densified.get())
                                   : static_cast<const PoseInterpolator*>(s.poses.get()));
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
        if (s.poses->push_pose(p).ok()) {
          ++s.stats.poses_accepted;
          if (s.traj != nullptr) {
            // The ACCEPTED poses only, and in the order the interpolator took
            // them: loop closure's arc-length parameterisation has to be built
            // over the same trajectory the points were resolved against, not
            // over everything the file happened to contain.
            TrajPose tp;
            tp.t_ns = p.t_mono_ns;
            for (int i = 0; i < 4; ++i) tp.q[i] = p.orientation[i];
            for (int i = 0; i < 3; ++i) tp.p[i] = p.position[i];
            // ROUND 13: carry the tracker's own verdict through, so a
            // consumer can tell "the phone was here" from "the phone did not
            // know where it was".
            tp.quality = static_cast<std::uint8_t>(rec.quality);
            tp.tracking_lost = rec.tracking_lost;
            s.traj->push_back(tp);
          }
        }
      }
    } else if (h.type == lscan::ChunkType::kPhoneImu) {
      // Decoded even when densification is off, so `imu_read` always reports
      // what the container HOLDS rather than what this run chose to use — a
      // caller comparing the A and B arms needs the same denominator for both.
      lscan::PhoneImuChunkRecord rec{};
      if (lscan::decode_phone_imu_chunk(ByteSpan(payload.data(), payload.size()), &rec)) {
        ++s.stats.imu_read;
        PhoneImuSample smp;
        smp.t_mono_ns = h.t_mono_ns;
        for (int i = 0; i < 3; ++i) {
          smp.gyro_rad_s[i] = rec.gyro_rad_s[i];
          smp.accel_m_s2[i] = rec.accel_m_s2[i];
        }
        if (s.densified->push_imu(smp)) ++s.stats.imu_accepted;
        const bool wants_gap_gyro = (s.cfg.stitch_sections && s.cfg.sections.bridge_long_gaps) ||
                                    s.cfg.rescue_gaps || s.cfg.recover_gap_points;
        if (wants_gap_gyro) s.imu_kept.push_back(smp);
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
  // ROUND 19 yield audit: the returns that never reach the assembler. The
  // parser counts every decoded sample; the driver drops range-0 before the
  // profile sink, so "no return" is only knowable here.
  s.stats.d6_no_returns = driver.parser_stats().points_zero_range;
  (void)driver.stop();

  s.report(PostStage::kPublishing, kOpenFraction + kResolveFraction, 1.f, seen, total_chunks);
  SCAN_TRY(s.assembler->flush());

  s.stats.pushbroom = s.assembler->stats();
  s.stats.points_out = s.stats.pushbroom.points_out;
  {
    const ImuDensifyStats ist = s.densified->stats();
    s.stats.imu_densified = ist.densified;
    s.stats.imu_fallbacks = ist.fallbacks;
    s.stats.imu = ist;
  }

  // ROUND 17 item 63 (and ROUND 19 items 73/74, which ask the same witness):
  // the whole-stream gyro, over the kept copy of the recorded IMU, seeded
  // with the bias the resolve just measured — that estimate is the only
  // calibration this capture will ever get. Built once, on first demand;
  // `densified`'s own ring is 8 s deep by design and cannot answer a
  // question about a moment minutes in the past.
  const auto ensure_gap_gyro = [&s]() -> const reanchor::GyroBridge* {
    if (s.gap_gyro != nullptr) return s.gap_gyro.get();
    if (s.imu_kept.empty()) return nullptr;
    ImuDensifyConfig gcfg = s.densified_cfg;
    gcfg.capacity = s.imu_kept.size();
    const ImuDensifyStats ist = s.densified->stats();
    for (int i = 0; i < 3; ++i) gcfg.initial_bias_rad_s[i] = ist.bias_rad_s[i];
    s.gap_gyro = std::make_unique<ImuDensifiedPoseSource>(nullptr, gcfg);
    for (const PhoneImuSample& smp : s.imu_kept) (void)s.gap_gyro->push_imu(smp);
    return s.gap_gyro.get();
  };

  // --- ROUND 13: section stitching -----------------------------------------
  //
  // BEFORE loop closure, and that order is the whole point. A capture with
  // five sections is five maps in five world frames; a revisit detector run
  // over it is comparing places that are not where the trajectory says they
  // are. Stitching first makes the walk one walk, which is the state ROUND 11
  // and ROUND 12's machinery was built to assume.
  //
  // Both the cloud AND `*s.traj` are corrected, so anything downstream — the
  // closer, a caller's trajectory export, the CLI's rulers — sees one frame.
  if (s.cfg.stitch_sections && s.traj != nullptr && s.times != nullptr) {
    std::vector<PointVertex> cloud;
    cloud.reserve(static_cast<std::size_t>(s.cfg.store->total_points()));
    const std::vector<PageId> ids = s.cfg.store->page_ids();
    for (const PageId id : ids) {
      const PageView v = s.cfg.store->page_view(id);
      if (!v.valid()) continue;
      for (std::uint32_t k = 0; k < v.count; ++k) cloud.push_back(v.data[k]);
    }
    if (cloud.size() != s.times->size()) {
      SCAN_LOG_WARN(kMod,
                    "section stitching skipped: %zu points in the store but %zu point times",
                    cloud.size(), s.times->size());
      s.stats.sections.summary = "the cloud and its per-point timestamps disagree in length";
    } else {
      // ROUND 17 item 63: the gyro, over the WHOLE stream, so a gap in the
      // first minute can still be asked about in the last.
      SectionStitchConfig scfg = s.cfg.sections;
      if (scfg.bridge_long_gaps && scfg.gyro == nullptr) scfg.gyro = ensure_gap_gyro();
      SectionCorrection corr;
      s.stats.sections = stitch_sections(
          *s.traj, Span<const PointVertex>(cloud.data(), cloud.size()),
          Span<const std::int64_t>(s.times->data(), s.times->size()), scfg, &corr);
      if (corr.active()) {
        std::size_t k = 0;
        for (const PageId id : ids) {
          const PageView v = s.cfg.store->page_view(id);
          if (!v.valid()) continue;
          PointVertex* w = s.cfg.store->page_data_mutable(id);
          if (w == nullptr) {
            k += v.count;
            continue;
          }
          for (std::uint32_t i = 0; i < v.count; ++i, ++k) {
            float xyz[3] = {w[i].x, w[i].y, w[i].z};
            corr.apply_point((*s.times)[k], xyz);
            w[i].x = xyz[0];
            w[i].y = xyz[1];
            w[i].z = xyz[2];
          }
          (void)s.cfg.store->notify_recoloured(id, 0, v.count);
        }
        for (TrajPose& tp : *s.traj) corr.apply_pose(tp.t_ns, tp.q, tp.p);
        s.stats.sections_stitched = true;
      }
    }
    SCAN_LOG_INFO(kMod, "section stitching: %zu sections — %s", s.stats.sections.sections,
                  s.stats.sections.summary);
  }

  // --- ROUND 19 item 73: gyro-constrained gap rescue ------------------------
  //
  // AFTER stitching — the pass above just decided which gaps are refused, and
  // everything it could fix is already in one frame — and BEFORE the loop-end
  // closure, which needs the walk to be one walk before "the end" means
  // anything. Every gap the stitch pass examined and refused is retried as a
  // registration problem (gap_rescue.h): rotation locked to the gyro that
  // witnessed the blind window, translation solved from the walls the two
  // sides share, the ruler voting last. A rescue that survives is applied
  // exactly the way a section seam is — a world-frame rigid transform on
  // everything strictly before the gap's re-acquisition — and every attempt,
  // refusals included, lands in `stats.rescues` with its numbers.
  if (s.cfg.rescue_gaps && s.traj != nullptr && s.times != nullptr) {
    std::vector<SectionSeam> candidates;
    for (const SectionSeam& g : s.stats.sections.gaps_examined) {
      switch (g.decision) {
        case SeamDecision::kGapRefusedDisagree:
        case SeamDecision::kGapRefusedNoGyro:
        case SeamDecision::kGapRefusedTooLong:
        case SeamDecision::kThinSubmap:
        case SeamDecision::kMapGotWorse:
          candidates.push_back(g);
          break;
        default:
          // Negligible gaps have nothing to rescue; kept bridges already
          // moved what needed moving.
          break;
      }
    }
    // gaps_examined interleaves the detection scan with the bridged-gap
    // review, so impose time order before corrections compose.
    std::sort(candidates.begin(), candidates.end(),
              [](const SectionSeam& a, const SectionSeam& b) { return a.t_ns < b.t_ns; });
    if (!candidates.empty()) {
      std::vector<PointVertex> cloud;
      cloud.reserve(static_cast<std::size_t>(s.cfg.store->total_points()));
      const std::vector<PageId> ids = s.cfg.store->page_ids();
      for (const PageId id : ids) {
        const PageView v = s.cfg.store->page_view(id);
        if (!v.valid()) continue;
        for (std::uint32_t k = 0; k < v.count; ++k) cloud.push_back(v.data[k]);
      }
      if (cloud.size() != s.times->size()) {
        SCAN_LOG_WARN(kMod, "gap rescue skipped: %zu points in the store but %zu point times",
                      cloud.size(), s.times->size());
      } else {
        GapRescueConfig rcfg = s.cfg.rescue;
        if (rcfg.gyro == nullptr) rcfg.gyro = ensure_gap_gyro();
        for (const SectionSeam& g : candidates) {
          GapRescueReport rr = rescue_gap(
              *s.traj, Span<const PointVertex>(cloud.data(), cloud.size()),
              Span<const std::int64_t>(s.times->data(), s.times->size()), g.pose_before,
              g.pose_after, rcfg);
          s.stats.rescues.push_back(rr);
          if (rr.decision != GapRescueDecision::kRescued) {
            SCAN_LOG_INFO(kMod, "gap of %.3f s not rescued: %s — %s", rr.gap_s,
                          to_string(rr.decision), rr.reason);
            continue;
          }
          ++s.stats.gaps_rescued;
          // A point stamped exactly at the seam belongs to the LATER frame —
          // the same rule SectionCorrection::section_of enforces, for the
          // same reason.
          const std::int64_t cut = rr.t_after_ns;
          for (std::size_t k = 0; k < cloud.size(); ++k) {
            if ((*s.times)[k] >= cut) continue;
            const double in[3] = {cloud[k].x, cloud[k].y, cloud[k].z};
            double o[3];
            se3::mat4_apply(rr.correction, in, o);
            cloud[k].x = static_cast<float>(o[0]);
            cloud[k].y = static_cast<float>(o[1]);
            cloud[k].z = static_cast<float>(o[2]);
          }
          std::size_t k = 0;
          for (const PageId id : ids) {
            const PageView v = s.cfg.store->page_view(id);
            if (!v.valid()) continue;
            PointVertex* w = s.cfg.store->page_data_mutable(id);
            if (w == nullptr) {
              k += v.count;
              continue;
            }
            for (std::uint32_t i = 0; i < v.count; ++i, ++k) {
              w[i].x = cloud[k].x;
              w[i].y = cloud[k].y;
              w[i].z = cloud[k].z;
            }
            (void)s.cfg.store->notify_recoloured(id, 0, v.count);
          }
          double cr[9], ct[3], cq[4];
          se3::mat4_get_rt(rr.correction, cr, ct);
          se3::matrix_to_quat(cr, cq);
          for (TrajPose& tp : *s.traj) {
            if (tp.t_ns >= cut) continue;
            double nq[4];
            se3::quat_mul(cq, tp.q, nq);
            se3::quat_normalize(nq);
            double np[3];
            se3::mat4_apply(rr.correction, tp.p, np);
            for (int i = 0; i < 4; ++i) tp.q[i] = nq[i];
            for (int i = 0; i < 3; ++i) tp.p[i] = np[i];
          }
          SCAN_LOG_INFO(kMod,
                        "gap of %.3f s RESCUED: %.2f deg locked from the gyro, %.3f m solved "
                        "from the walls",
                        rr.gap_s, rr.rotation_applied_deg, rr.translation_m);
        }
      }
    }
  }

  // --- ROUND 11 item 41: loop closure --------------------------------------
  //
  // After the resolve, never during it. The detector needs the WHOLE
  // trajectory (a revisit is a statement about two moments that may be three
  // minutes apart) and the whole cloud, so there is nothing to gain from
  // interleaving and a great deal of clarity to lose.
  if (s.cfg.close_loops && s.traj != nullptr && s.times != nullptr) {
    // Flatten the store in page order. PageStore appends pages in creation
    // order and never reorders them, and points inside a page are in append
    // order, so this is exactly the emit order `out_point_times` was written
    // in — which is the pairing the whole correction depends on.
    std::vector<PointVertex> cloud;
    cloud.reserve(static_cast<std::size_t>(s.cfg.store->total_points()));
    const std::vector<PageId> ids = s.cfg.store->page_ids();
    for (const PageId id : ids) {
      const PageView v = s.cfg.store->page_view(id);
      if (!v.valid()) continue;
      for (std::uint32_t k = 0; k < v.count; ++k) cloud.push_back(v.data[k]);
    }
    if (cloud.size() != s.times->size()) {
      // Not fatal, and it must not be silent: it would mean the store took
      // points this pipeline did not publish (a shared store) or dropped some
      // without saying so. Either way the pairing is void and closing on a
      // void pairing would smear the cloud.
      SCAN_LOG_WARN(kMod,
                    "loop closure skipped: %zu points in the store but %zu point times — the "
                    "pairing is not trustworthy",
                    cloud.size(), s.times->size());
      s.stats.loop.decision = LoopDecision::kNoTrajectory;
      s.stats.loop.reason = "loop: the cloud and its per-point timestamps disagree in length";
    } else {
      TrajectoryCorrection corr;
      s.stats.loop = close_trajectory_loop(
          *s.traj, Span<const PointVertex>(cloud.data(), cloud.size()),
          Span<const std::int64_t>(s.times->data(), s.times->size()), s.cfg.loop, &corr);
      if (corr.active()) {
        std::size_t k = 0;
        for (const PageId id : ids) {
          const PageView v = s.cfg.store->page_view(id);
          if (!v.valid()) continue;
          PointVertex* w = s.cfg.store->page_data_mutable(id);
          if (w == nullptr) {
            k += v.count;
            continue;
          }
          for (std::uint32_t i = 0; i < v.count; ++i, ++k) {
            float xyz[3] = {w[i].x, w[i].y, w[i].z};
            corr.apply_point((*s.times)[k], xyz);
            w[i].x = xyz[0];
            w[i].y = xyz[1];
            w[i].z = xyz[2];
          }
          // INT-34's seam is named for colour because colour was its first
          // user; what it actually means to a subscriber is "re-read this
          // page, it changed under you", which is exactly what a moved point
          // needs. Offline there are usually no subscribers at all.
          (void)s.cfg.store->notify_recoloured(id, 0, v.count);
        }
        s.stats.loop_applied = true;
      }
    }
    SCAN_LOG_INFO(kMod, "loop closure: %s — %s", to_string(s.stats.loop.decision),
                  s.stats.loop.reason);
  }

  // --- ROUND 16 item 60: loop-end closure ----------------------------------
  //
  // LAST, and the order is the argument. Stitching has already put a broken
  // capture into one frame, so "the walk ended near where it started" is a
  // statement about one trajectory rather than about two unrelated ones.
  // ROUND 11's six-DoF closer, when it fires at all, has already removed this
  // drift — so if it did, this finds nothing left to close and says so, which
  // is the correct answer and not a conflict.
  //
  // Same flatten-apply-notify shape as the block above, deliberately: the
  // correction type is the same `TrajectoryCorrection` (with its rotation
  // half exactly zero), so there is one way points move in this file.
  if (s.cfg.close_loop_end && s.traj != nullptr && s.times != nullptr) {
    std::vector<PointVertex> cloud;
    cloud.reserve(static_cast<std::size_t>(s.cfg.store->total_points()));
    const std::vector<PageId> ids = s.cfg.store->page_ids();
    for (const PageId id : ids) {
      const PageView v = s.cfg.store->page_view(id);
      if (!v.valid()) continue;
      for (std::uint32_t k = 0; k < v.count; ++k) cloud.push_back(v.data[k]);
    }
    if (cloud.size() != s.times->size()) {
      SCAN_LOG_WARN(kMod,
                    "loop-end closure skipped: %zu points in the store but %zu point times — "
                    "the pairing is not trustworthy",
                    cloud.size(), s.times->size());
      s.stats.loop_end.decision = LoopEndDecision::kNoTrajectory;
      s.stats.loop_end.reason =
          "loop end: the cloud and its per-point timestamps disagree in length";
    } else {
      TrajectoryCorrection corr;
      s.stats.loop_end = close_loop_end(
          *s.traj, Span<const PointVertex>(cloud.data(), cloud.size()),
          Span<const std::int64_t>(s.times->data(), s.times->size()), s.cfg.loop_end, &corr);
      if (corr.active()) {
        std::size_t k = 0;
        for (const PageId id : ids) {
          const PageView v = s.cfg.store->page_view(id);
          if (!v.valid()) continue;
          PointVertex* w = s.cfg.store->page_data_mutable(id);
          if (w == nullptr) {
            k += v.count;
            continue;
          }
          for (std::uint32_t i = 0; i < v.count; ++i, ++k) {
            float xyz[3] = {w[i].x, w[i].y, w[i].z};
            corr.apply_point((*s.times)[k], xyz);
            w[i].x = xyz[0];
            w[i].y = xyz[1];
            w[i].z = xyz[2];
          }
          (void)s.cfg.store->notify_recoloured(id, 0, v.count);
        }
        // The TRAJECTORY too, and for the same reason section stitching
        // corrects it: anything downstream — the ruler, the floor plan, the
        // path the app is about to draw inside the cloud — must see one frame.
        for (TrajPose& tp : *s.traj) corr.apply_pose(tp.t_ns, tp.q, tp.p);
        s.stats.loop_end_applied = true;
      }
    }
    SCAN_LOG_INFO(kMod, "loop-end closure: %s — %s", to_string(s.stats.loop_end.decision),
                  s.stats.loop_end.reason);
  }

  // --- ROUND 19 item 74: recover the loss-window returns --------------------
  //
  // LAST, after every correction, because the interpolation is anchored on
  // the CORRECTED endpoint poses: a gap is recoverable exactly when its two
  // ends are finally in one trusted frame — bridged by the stitch pass, or
  // rescued above. Orientation across the window comes from the gyro with the
  // closing error distributed linearly in time (the densifier's own model,
  // imu_densified_pose.h step 3); position is linearly interpolated between
  // the endpoints, which is the honest statement of what is known about it.
  // The ruler votes per gap, on the number the summary card prints, and a
  // recovery it vetoes is dropped and named. Admitted points carry
  // `flagged_alpha` — recovered, and marked as such.
  if (s.cfg.recover_gap_points && s.traj != nullptr && s.times != nullptr &&
      !s.profile_kept.empty()) {
    struct GapRef {
      std::size_t jb = 0;
      std::size_t ja = 0;
    };
    std::vector<GapRef> gaps;
    for (const SectionSeam& g : s.stats.sections.gaps_examined) {
      if (g.decision == SeamDecision::kBridged) gaps.push_back({g.pose_before, g.pose_after});
    }
    for (const GapRescueReport& r : s.stats.rescues) {
      if (r.decision == GapRescueDecision::kRescued) {
        gaps.push_back({r.pose_before, r.pose_after});
      }
    }
    std::sort(gaps.begin(), gaps.end(),
              [](const GapRef& a, const GapRef& b) { return a.jb < b.jb; });
    const reanchor::GyroBridge* gyro = ensure_gap_gyro();
    if (!gaps.empty() && gyro != nullptr) {
      std::vector<PointVertex> cloud;
      cloud.reserve(static_cast<std::size_t>(s.cfg.store->total_points()));
      for (const PageId id : s.cfg.store->page_ids()) {
        const PageView v = s.cfg.store->page_view(id);
        if (!v.valid()) continue;
        for (std::uint32_t k = 0; k < v.count; ++k) cloud.push_back(v.data[k]);
      }
      if (cloud.size() != s.times->size()) {
        SCAN_LOG_WARN(kMod,
                      "loss-window recovery skipped: %zu points in the store but %zu point "
                      "times",
                      cloud.size(), s.times->size());
      } else {
        for (const GapRef& g : gaps) {
          GapRecovery rec;
          if (g.jb >= s.traj->size() || g.ja >= s.traj->size()) continue;
          const TrajPose& pb = (*s.traj)[g.jb];
          const TrajPose& pa = (*s.traj)[g.ja];
          rec.t_before_ns = pb.t_ns;
          rec.t_after_ns = pa.t_ns;
          if (pa.t_ns <= pb.t_ns) continue;
          double qg[4] = {0, 0, 0, 1};
          double peak = 0.0;
          bool hole = false;
          if (!gyro->relative_rotation(pb.t_ns, pa.t_ns, qg, &peak, &hole) || hole) {
            rec.reason = "the gyro does not cover the window continuously";
            s.stats.recoveries.push_back(rec);
            continue;
          }
          double qb[4] = {pb.q[0], pb.q[1], pb.q[2], pb.q[3]};
          double qa[4] = {pa.q[0], pa.q[1], pa.q[2], pa.q[3]};
          if (!se3::quat_normalize(qb) || !se3::quat_normalize(qa) ||
              !se3::quat_normalize(qg)) {
            rec.reason = "a rotation at the window's edge is degenerate";
            s.stats.recoveries.push_back(rec);
            continue;
          }
          // The closing error: everything the gyro's account fails to meet
          // the corrected after-pose by, distributed linearly in time so the
          // path lands EXACTLY on both trusted endpoints.
          double q_pred_end[4];
          se3::quat_mul(qb, qg, q_pred_end);
          se3::quat_normalize(q_pred_end);
          double e[4];
          {
            double c[4];
            se3::quat_conj(q_pred_end, c);
            se3::quat_mul(c, qa, e);
            se3::quat_normalize(e);
          }
          const double span_ns = static_cast<double>(pa.t_ns - pb.t_ns);
          std::vector<PointVertex> add;
          std::vector<std::int64_t> add_t;
          for (const ProfilePoint& pp : s.profile_kept) {
            if (pp.t_mono_ns <= pb.t_ns || pp.t_mono_ns >= pa.t_ns) continue;
            if (!(pp.range_m >= s.cfg.pushbroom.min_range_m) ||
                !(pp.range_m <= s.cfg.pushbroom.max_range_m)) {
              continue;
            }
            ++rec.candidates;
            double qr[4] = {0, 0, 0, 1};
            double pk2 = 0.0;
            bool h2 = false;
            if (!gyro->relative_rotation(pb.t_ns, pp.t_mono_ns, qr, &pk2, &h2) || h2) {
              ++rec.no_gyro;
              continue;
            }
            const double u = static_cast<double>(pp.t_mono_ns - pb.t_ns) / span_ns;
            double qi[4];
            se3::quat_mul(qb, qr, qi);
            se3::quat_normalize(qi);
            const double id4[4] = {0, 0, 0, 1};
            double eu[4];
            se3::quat_slerp(id4, e, u, eu);
            double qt[4];
            se3::quat_mul(qi, eu, qt);
            se3::quat_normalize(qt);
            const double pt[3] = {pb.p[0] + u * (pa.p[0] - pb.p[0]),
                                  pb.p[1] + u * (pa.p[1] - pb.p[1]),
                                  pb.p[2] + u * (pa.p[2] - pb.p[2])};
            double wfp[16];
            se3::mat4_from_quat_pos(qt, pt, wfp);
            double wfl[16];
            se3::mat4_mul(wfp, mount, wfl);
            double pl[3];
            d6::fan_point(static_cast<double>(pp.angle_deg), static_cast<double>(pp.range_m),
                          pl);
            double pw[3];
            se3::mat4_apply(wfl, pl, pw);
            PointVertex v{};
            v.x = static_cast<float>(pw[0]);
            v.y = static_cast<float>(pw[1]);
            v.z = static_cast<float>(pw[2]);
            v.r = pp.intensity;
            v.g = pp.high_reflectivity != 0 ? std::uint8_t{255} : pp.intensity;
            v.b = pp.intensity;
            v.a = s.cfg.pushbroom.flagged_alpha;
            add.push_back(v);
            add_t.push_back(pp.t_mono_ns);
          }
          if (add.empty()) {
            rec.reason = rec.candidates == 0
                             ? "no in-range returns inside the window"
                             : "the gyro covered none of the window's returns";
            s.stats.recoveries.push_back(rec);
            continue;
          }
          // The ruler votes on the map WITH the recovery in it. Appended,
          // measured, and rolled back if it reads worse — measured on the
          // points, so admitting them cannot flatter it.
          const MapConsistencyReport rb =
              measure_map_consistency(cloud, *s.times, s.cfg.rescue.consistency);
          cloud.insert(cloud.end(), add.begin(), add.end());
          s.times->insert(s.times->end(), add_t.begin(), add_t.end());
          bool admit = true;
          if (rb.measurable) {
            const MapConsistencyReport ra =
                measure_map_consistency(cloud, *s.times, s.cfg.rescue.consistency);
            rec.self_check_before_m = rb.nearest_offset_m;
            rec.self_check_after_m = ra.measurable ? ra.nearest_offset_m : rb.nearest_offset_m;
            if (!ra.measurable ||
                ra.nearest_offset_m >
                    rb.nearest_offset_m + s.cfg.rescue.self_consistency_tolerance_m) {
              admit = false;
            }
          }
          if (!admit) {
            cloud.resize(cloud.size() - add.size());
            s.times->resize(s.times->size() - add_t.size());
            rec.ruler_vetoed = true;
            rec.reason = "admitting the recovered points made the map agree with itself less "
                         "— they stay excluded for this gap";
            s.stats.recoveries.push_back(rec);
            SCAN_LOG_WARN(kMod,
                          "recovery vetoed for the %.1f s window: self-check %.2f -> %.2f cm",
                          span_ns * 1e-9, rec.self_check_before_m * 100.0,
                          rec.self_check_after_m * 100.0);
            continue;
          }
          rec.admitted = add.size();
          rec.reason = "re-resolved against the bridged/rescued trajectory and kept";
          s.stats.recovered_points += add.size();
          // Publish in the recorder's own batch size, so a reader's per-chunk
          // costs stay the ones it was written for.
          for (std::size_t i = 0; i < add.size(); i += 4096) {
            const std::size_t n = std::min<std::size_t>(4096, add.size() - i);
            const Status ap = s.cfg.store->append(
                s.cfg.out_stream, Span<const PointVertex>(add.data() + i, n), add_t[i]);
            if (!ap.ok()) {
              SCAN_LOG_WARN(kMod, "recovery: store refused %zu points: %s", n,
                            error_str(ap.error()));
              break;
            }
          }
          s.stats.recoveries.push_back(rec);
          SCAN_LOG_INFO(kMod,
                        "recovered %llu of %llu loss-window returns across the %.1f s gap "
                        "(%llu had no gyro cover); self-check %.2f -> %.2f cm",
                        static_cast<unsigned long long>(rec.admitted),
                        static_cast<unsigned long long>(rec.candidates), span_ns * 1e-9,
                        static_cast<unsigned long long>(rec.no_gyro),
                        rec.self_check_before_m * 100.0, rec.self_check_after_m * 100.0);
        }
      }
    }
  }

  SCAN_LOG_INFO(kMod,
                "resolved '%s': %llu D6 chunks / %llu bytes, %llu poses (%llu accepted), "
                "%llu phone-IMU samples (%llu accepted; %llu returns densified, %llu fell "
                "back) -> %llu world points (mount from %s)",
                lscan_dir.c_str(), static_cast<unsigned long long>(s.stats.lidar_chunks),
                static_cast<unsigned long long>(s.stats.lidar_bytes),
                static_cast<unsigned long long>(s.stats.poses_read),
                static_cast<unsigned long long>(s.stats.poses_accepted),
                static_cast<unsigned long long>(s.stats.imu_read),
                static_cast<unsigned long long>(s.stats.imu_accepted),
                static_cast<unsigned long long>(s.stats.imu_densified),
                static_cast<unsigned long long>(s.stats.imu_fallbacks),
                static_cast<unsigned long long>(s.stats.points_out),
                s.stats.mount_from_manifest ? "manifest" : "caller");

  if (s.stats.points_out == 0) {
    // A resolve that produced nothing is a failure even though every step
    // returned kOk — ROUND 7 §3's rule, that a zero-point result must never be
    // reported as success, applied to the offline path.
    // ROUND 19: name the flagged-excluded returns too. The owner's scan-045
    // resolved to 0 points from 34,436 returns and the old message accounted
    // for 223 of them — the other 34,213 were painted while EVERY pose was
    // disowned (the double-start's reset world) and vanished without a word.
    return set_last_error(
        ScanError::kNotFound,
        "d6resolve: '%s' resolved to 0 points from %llu returns — %llu had no pose covering "
        "their timestamp, %llu were painted during tracking loss and excluded, and %llu were "
        "outside the D6's range window. A capture whose every pose is disowned has no anchor "
        "frame anywhere, and no rescue can register two sides that were never resolved.",
        lscan_dir.c_str(), static_cast<unsigned long long>(s.stats.pushbroom.points_in),
        static_cast<unsigned long long>(s.stats.pushbroom.dropped_no_pose),
        static_cast<unsigned long long>(s.stats.pushbroom.flagged_total() -
                                        s.stats.pushbroom.flagged_emitted),
        static_cast<unsigned long long>(s.stats.pushbroom.dropped_range));
  }

  s.report(PostStage::kDone, 1.f, 1.f, seen, total_chunks);
  return kOkStatus;
}

}  // namespace post
}  // namespace scanengine
