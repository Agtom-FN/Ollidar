// post_pipeline.cpp — the A7 "Finish scan" pipeline.
//
// Stage order (Tech Spec §3.3), and what each one does here:
//
//   kOpening        FileRecordReader::open + sizing the work
//   kOdometry       decode pass 1 -> LioOdometry (full density) + keyframes
//   kLoopDetection  ScanContextDb -> local submap -> point-to-plane ICP
//   kOptimization   PoseGraph (odometry chain + accepted loop edges)
//   kReintegration  decode pass 2 -> corrected trajectory -> VoxelAccumulator
//   kFiltering      statistical outlier filter
//   kPublishing     PageStore
//
// TWO PASSES OVER THE RECORDING, NOT ONE. The corrections are not known until
// the graph has solved, so re-integration cannot ride along with the odometry.
// Buffering every raw point instead would cost ~5.7 GB on a 30-minute
// session; re-reading a sequential file costs a second decode pass. The file
// is the cheap resource here, and A5's reader is built for exactly this access
// pattern (docs/A5-lscan.md §3: "a .lscan is read start-to-finish far more
// often than seeked into").
#include "scanengine/slam/post/post_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"
#include "scanengine/timesync/imu_ingest.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {
namespace post {
namespace {

double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// Stage weights. Fixed rather than measured, so the bar never runs backwards
// (progress.h explains why that is the right trade).
struct StageSpan {
  float lo;
  float hi;
};
constexpr StageSpan kSpan[] = {
    {0.00f, 0.00f},  // kIdle
    {0.00f, 0.02f},  // kOpening
    {0.02f, 0.55f},  // kOdometry
    {0.55f, 0.70f},  // kLoopDetection
    {0.70f, 0.75f},  // kOptimization
    {0.75f, 0.92f},  // kReintegration
    {0.92f, 0.98f},  // kFiltering
    {0.98f, 1.00f},  // kPublishing
    {1.00f, 1.00f},  // kDone
    {0.00f, 1.00f},  // kCancelled
    {0.00f, 1.00f},  // kFailed
};

// Poses are (quaternion (x,y,z,w), position) throughout — the same pair
// poses/pose_source.h carries, so nothing is converted at a boundary.

void quat_mul(const double a[4], const double b[4], double out[4]) {
  const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

void quat_conj(const double a[4], double out[4]) {
  out[0] = -a[0];
  out[1] = -a[1];
  out[2] = -a[2];
  out[3] = a[3];
}

void quat_rotate(const double q[4], const double v[3], double out[3]) {
  double R[9];
  se3::quat_to_matrix(q, R);
  const double x = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
  const double y = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
  const double z = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

// c = a * b.
void pose_compose(const double qa[4], const double pa[3], const double qb[4], const double pb[3],
                  double qc[4], double pc[3]) {
  double rot[3];
  quat_rotate(qa, pb, rot);
  double q[4];
  quat_mul(qa, qb, q);
  se3::quat_normalize(q);
  const double px = pa[0] + rot[0];
  const double py = pa[1] + rot[1];
  const double pz = pa[2] + rot[2];
  for (int i = 0; i < 4; ++i) qc[i] = q[i];
  pc[0] = px;
  pc[1] = py;
  pc[2] = pz;
}

void pose_inverse(const double q[4], const double p[3], double qi[4], double pi[3]) {
  double c[4];
  quat_conj(q, c);
  double r[3];
  quat_rotate(c, p, r);
  for (int i = 0; i < 4; ++i) qi[i] = c[i];
  pi[0] = -r[0];
  pi[1] = -r[1];
  pi[2] = -r[2];
}

// r = a^-1 * b.
void pose_between(const double qa[4], const double pa[3], const double qb[4], const double pb[3],
                  double qr[4], double pr[3]) {
  double qai[4], pai[3];
  pose_inverse(qa, pa, qai, pai);
  pose_compose(qai, pai, qb, pb, qr, pr);
}

double rotation_angle_deg(const double qa[4], const double qb[4]) {
  double Ra[9], Rb[9];
  se3::quat_to_matrix(qa, Ra);
  se3::quat_to_matrix(qb, Rb);
  return se3::rot_angle_deg(Ra, Rb);
}

// Points outside the trajectory's time span by more than this are dropped
// rather than clamped. One scan period of slack covers the tail of a capture
// whose last partial scan produced no pose; anything further is not a timing
// edge case, it is a different problem.
constexpr std::int64_t kPoseClampSlackNs = 200'000'000LL;

}  // namespace

PostConfig::PostConfig() {
  // --- full density: the whole reason the post run exists ---------------
  lio.live_points_per_sec = 0;  // no decimation
  lio.max_points_per_scan = 0;  // and no burst ceiling
  lio.map_radius_m = 0.0;       // never forget
  lio.internal_thread = false;  // see PostSlamPipeline's class comment
  lio.map.max_points_per_voxel = 40;
  lio.map.max_voxels = 4000000;
  lio.publish_map_points_only = true;
  // The odometry's own reach. The FINAL cloud uses the same gate, so a point
  // that shaped the trajectory is a point that appears in the deliverable.
  lio.min_range_m = 0.3f;
  lio.max_range_m = 80.0f;

  point_filter.drop_no_return = true;
  outlier.enabled = true;
}

// ===========================================================================

struct PostSlamPipeline::Impl {
  PostConfig cfg;
  PostProgressFn progress_cb;
  CancelToken own_token;
  CancelToken* token = nullptr;

  std::atomic<float> fraction{0.f};
  std::atomic<int> stage_v{static_cast<int>(PostStage::kIdle)};

  PostStats stats;
  std::vector<Keyframe> keyframes;
  std::vector<LoopClosure> loops;
  PoseGraph graph;
  LioPoseSource corrected{10000000u, StreamId::kPoseLio};
  std::vector<PointVertex> final_cloud;
  std::unique_ptr<PageStore> own_store;

  // Odometry-pass state.
  LioOdometry* lio = nullptr;
  std::vector<Pose> odom_poses;
  std::vector<PointVertex> pending_pts;  // LIDAR frame, range-gated
  std::vector<std::int64_t> pending_t;   // per-point engine time
  std::size_t pending_consumed = 0;

  bool is_cancelled() const { return post::cancelled(token); }

  void report(PostStage s, double stage_fraction, std::uint64_t done, std::uint64_t total) {
    const StageSpan sp = kSpan[static_cast<int>(s)];
    double f = stage_fraction;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    const float overall = static_cast<float>(sp.lo + (sp.hi - sp.lo) * f);
    fraction.store(overall, std::memory_order_relaxed);
    stage_v.store(static_cast<int>(s), std::memory_order_relaxed);
    if (progress_cb) {
      PostProgress p;
      p.stage = s;
      p.label = to_string(s);
      p.fraction = overall;
      p.stage_fraction = static_cast<float>(f);
      p.done = done;
      p.total = total;
      progress_cb(p);
    }
  }

  void reset() {
    stats = PostStats();
    keyframes.clear();
    loops.clear();
    graph.clear();
    corrected.clear();
    final_cloud.clear();
    odom_poses.clear();
    pending_pts.clear();
    pending_t.clear();
    pending_consumed = 0;
    lio = nullptr;
    fraction.store(0.f, std::memory_order_relaxed);
  }

  PageStore& store() {
    if (cfg.store != nullptr) return *cfg.store;
    if (!own_store) own_store.reset(new PageStore());
    return *own_store;
  }

  // --- shared decode loop ------------------------------------------------
  template <typename PointsFn, typename ImuFn>
  Status decode_pass(const std::string& dir, PostStage stage_id, bool count_stats,
                     PointsFn on_points, ImuFn on_imu);

  void buffer_points(const std::vector<PointVertex>& batch, std::int64_t t_batch,
                     std::int64_t t_prev);
  void build_keyframe(std::int64_t t_kf, const Pose& kf_pose);
  void maybe_keyframe(bool force);
  Status run_loop_detection();
  Status run_optimization();
  void build_corrected_trajectory();
  Status publish(const std::vector<PointVertex>& cloud);
};

template <typename PointsFn, typename ImuFn>
Status PostSlamPipeline::Impl::decode_pass(const std::string& dir, PostStage stage_id,
                                          bool count_stats, PointsFn on_points, ImuFn on_imu) {
  lscan::FileRecordReader reader;
  SCAN_TRY(reader.open(dir));

  std::uint64_t total_chunks = 0;
  for (const lscan::StreamSummary& s : reader.stream_summaries()) {
    if (s.stream == StreamId::kLidarMid360 || s.stream == StreamId::kImu) {
      total_chunks += s.chunk_count;
    }
  }
  if (count_stats) {
    stats.truncated_tail_chunks = reader.warnings().truncated_tail_chunks;
    stats.crc_mismatch_chunks = reader.warnings().crc_mismatch_chunks;
  }
  if (total_chunks == 0) {
    return set_last_error(ScanError::kNotFound, "post: '%s' holds no Mid-360 point/IMU chunks",
                          dir.c_str());
  }

  // A4, wired as docs/A6-lio.md §7.2 prescribes — including its one deliberate
  // departure: the point stream and the IMU share the kLidarMid360 estimator,
  // because the Mid-360 stamps both from one device clock and a second
  // estimator would inject independent noise BETWEEN the two streams, which is
  // exactly the quantity undistortion is sensitive to.
  TimeSync ts;
  ImuIngest imu_ingest(ts, StreamId::kLidarMid360);

  lscan::ChunkHeader header;
  std::vector<std::uint8_t> payload;
  std::vector<PointVertex> batch;
  batch.reserve(mid360::kPointsPerPacket);

  std::uint64_t seen = 0;
  std::int64_t last_dev_ts = 0;
  bool have_dev_ts = false;
  std::int64_t prev_batch_t = 0;
  bool have_prev_batch = false;
  const std::uint32_t interval = cfg.progress_chunk_interval == 0 ? 2048u
                                                                 : cfg.progress_chunk_interval;

  for (;;) {
    const Status st = reader.next_chunk(&header, &payload);
    if (st.error() == ScanError::kAgain) break;
    if (!st.ok()) return st;
    if (header.type != lscan::ChunkType::kMid360Points &&
        header.type != lscan::ChunkType::kMid360Imu) {
      continue;
    }
    ++seen;
    if (count_stats) ++stats.chunks_read;
    if ((seen % interval) == 0) {
      if (is_cancelled()) return set_last_error(ScanError::kCancelled, "post: cancelled");
      report(stage_id, static_cast<double>(seen) / static_cast<double>(total_chunks), seen,
             total_chunks);
    }

    const mid360::PacketView v = mid360::parse_packet(payload.data(), payload.size());
    if (!v.valid()) {
      if (count_stats) ++stats.malformed_chunks;
      continue;
    }
    std::uint64_t dev_ts_u = 0;  // the datagram field is unaligned
    std::memcpy(&dev_ts_u, &v.header->timestamp, sizeof(dev_ts_u));
    const std::int64_t dev_ts = static_cast<std::int64_t>(dev_ts_u);

    if (v.header->data_type == mid360::kDataTypeImu) {
      if (count_stats) ++stats.imu_chunks;
      const mid360::ImuRaw* raw = reinterpret_cast<const mid360::ImuRaw*>(v.payload);
      const float gyro[3] = {raw->gyro_x, raw->gyro_y, raw->gyro_z};
      const float acc_g[3] = {raw->acc_x, raw->acc_y, raw->acc_z};
      const ImuSample s = imu_ingest.add_g(dev_ts, TimePoint{header.t_mono_ns}, gyro, acc_g);
      if (!on_imu(s)) return set_last_error(ScanError::kCancelled, "post: cancelled");
      continue;
    }
    if (v.header->data_type != mid360::kDataTypeCartesianHigh) continue;
    if (count_stats) ++stats.point_chunks;

    // The "device stamp went backwards" rule. A real capture opens with
    // datagrams stamped 0 while arrival advances (docs/A6-lio.md §7.2 counted
    // 150 of them); feeding those to the offset estimator poisons the mapping.
    if (have_dev_ts && dev_ts <= last_dev_ts) {
      if (count_stats) ++stats.nonmonotonic_packets;
      continue;
    }
    last_dev_ts = dev_ts;
    have_dev_ts = true;

    ts.add_pair(StreamId::kLidarMid360, dev_ts, TimePoint{header.t_mono_ns});
    const TimeModel model = ts.model(StreamId::kLidarMid360);
    const std::int64_t t_batch = model.apply(dev_ts);

    const mid360::CartesianHigh* pts = reinterpret_cast<const mid360::CartesianHigh*>(v.payload);
    batch.clear();
    for (std::uint32_t i = 0; i < v.point_count; ++i) {
      if (!mid360::point_passes(pts[i], cfg.point_filter,
                                count_stats ? &stats.filter : nullptr)) {
        continue;
      }
      PointVertex pv;
      pv.x = static_cast<float>(pts[i].x) * 1e-3f;
      pv.y = static_cast<float>(pts[i].y) * 1e-3f;
      pv.z = static_cast<float>(pts[i].z) * 1e-3f;
      pv.r = pv.g = pv.b = pts[i].reflectivity;
      pv.a = 255;
      batch.push_back(pv);
    }
    if (count_stats) stats.points_decoded += batch.size();
    if (!batch.empty()) {
      const std::int64_t t_prev = have_prev_batch ? prev_batch_t : t_batch;
      if (!on_points(batch, t_batch, t_prev)) {
        return set_last_error(ScanError::kCancelled, "post: cancelled");
      }
    }
    prev_batch_t = t_batch;
    have_prev_batch = true;
  }
  report(stage_id, 1.0, seen, total_chunks);
  return reader.close();
}

// --- keyframing -------------------------------------------------------------

void PostSlamPipeline::Impl::buffer_points(const std::vector<PointVertex>& batch,
                                           std::int64_t t_batch, std::int64_t t_prev) {
  // Per-point times interpolated across the batch, exactly as A6 does
  // (docs/A6-lio.md §3.3): the batch's stamp and the previous batch's stamp
  // bracket it. For a 96-point datagram that is ~5 us of granularity.
  const std::size_t n = batch.size();
  const double span = static_cast<double>(t_batch - t_prev);
  const float lo = cfg.lio.min_range_m;
  const float hi = cfg.lio.max_range_m;
  for (std::size_t i = 0; i < n; ++i) {
    const PointVertex& p = batch[i];
    const float r2 = p.x * p.x + p.y * p.y + p.z * p.z;
    if (r2 < lo * lo || (hi > 0.f && r2 > hi * hi)) continue;
    const double u = static_cast<double>(i + 1) / static_cast<double>(n);
    pending_pts.push_back(p);
    pending_t.push_back(t_prev + static_cast<std::int64_t>(span * u));
  }
  // Bounded buffer. Only reachable if the odometry stops producing poses
  // entirely (never initialized, or a total lidar dropout).
  const std::size_t live = pending_pts.size() - pending_consumed;
  if (cfg.keyframe_buffer_cap != 0 && live > cfg.keyframe_buffer_cap) {
    const std::size_t drop = live - cfg.keyframe_buffer_cap;
    pending_consumed += drop;
    stats.buffer_overflow_points += drop;
  }
}

void PostSlamPipeline::Impl::build_keyframe(std::int64_t t_kf, const Pose& kf_pose) {
  Keyframe kf;
  kf.t_ns = t_kf;
  for (int i = 0; i < 4; ++i) kf.q[i] = kf_pose.orientation[i];
  for (int i = 0; i < 3; ++i) kf.p[i] = kf_pose.position[i];
  for (int i = 0; i < 4; ++i) kf.q_opt[i] = kf.q[i];
  for (int i = 0; i < 3; ++i) kf.p_opt[i] = kf.p[i];

  double qk_inv[4], pk_inv[3];
  pose_inverse(kf.q, kf.p, qk_inv, pk_inv);

  const double* lt = cfg.lio.lidar_to_imu_t;
  const double* lq = cfg.lio.lidar_to_imu_q;
  const LioPoseSource& poses = lio->poses();

  std::vector<PointVertex> body;
  body.reserve(pending_pts.size() - pending_consumed);
  std::size_t i = pending_consumed;
  for (; i < pending_pts.size(); ++i) {
    const std::int64_t t = pending_t[i];
    if (t > t_kf) break;  // belongs to the next keyframe
    Pose at;
    if (!poses.pose_at(t, &at).ok()) continue;  // predates the filter's birth
    const PointVertex& src = pending_pts[i];
    const double pl[3] = {src.x, src.y, src.z};
    double pb[3];
    quat_rotate(lq, pl, pb);  // lidar -> body (IMU)
    pb[0] += lt[0];
    pb[1] += lt[1];
    pb[2] += lt[2];
    double pw[3];
    quat_rotate(at.orientation, pb, pw);  // body -> world, at this point's time
    pw[0] += at.position[0];
    pw[1] += at.position[1];
    pw[2] += at.position[2];
    double pk[3];
    quat_rotate(qk_inv, pw, pk);  // world -> keyframe body frame
    pk[0] += pk_inv[0];
    pk[1] += pk_inv[1];
    pk[2] += pk_inv[2];
    PointVertex out = src;
    out.x = static_cast<float>(pk[0]);
    out.y = static_cast<float>(pk[1]);
    out.z = static_cast<float>(pk[2]);
    body.push_back(out);
  }
  pending_consumed = i;

  VoxelDedupConfig dcfg;
  dcfg.voxel_size_m = cfg.keyframe_voxel_m;
  std::vector<PointVertex> down;
  voxel_downsample(Span<const PointVertex>(body.data(), body.size()), dcfg, &down, nullptr);

  // Cap by a deterministic stride, not by truncation: truncating keeps "the
  // first N voxels the scan happened to hit", which for a sweeping lidar is
  // one side of the room.
  if (cfg.max_points_per_keyframe > 0 && down.size() > cfg.max_points_per_keyframe) {
    const std::size_t stride =
        (down.size() + cfg.max_points_per_keyframe - 1) / cfg.max_points_per_keyframe;
    std::vector<PointVertex> thin;
    thin.reserve(cfg.max_points_per_keyframe);
    for (std::size_t k = 0; k < down.size(); k += stride) thin.push_back(down[k]);
    down.swap(thin);
  }
  stats.keyframe_points += down.size();
  kf.points.swap(down);
  keyframes.push_back(std::move(kf));

  if (pending_consumed > 0 && pending_consumed * 2 >= pending_pts.size()) {
    pending_pts.erase(pending_pts.begin(),
                      pending_pts.begin() + static_cast<std::ptrdiff_t>(pending_consumed));
    pending_t.erase(pending_t.begin(),
                    pending_t.begin() + static_cast<std::ptrdiff_t>(pending_consumed));
    pending_consumed = 0;
  }
}

void PostSlamPipeline::Impl::maybe_keyframe(bool force) {
  Pose latest;
  if (!lio->poses().latest(&latest)) return;
  if (keyframes.empty()) {
    build_keyframe(latest.t_mono_ns, latest);
    return;
  }
  const Keyframe& last = keyframes.back();
  if (latest.t_mono_ns <= last.t_ns) return;
  const double dx = latest.position[0] - last.p[0];
  const double dy = latest.position[1] - last.p[1];
  const double dz = latest.position[2] - last.p[2];
  const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  const double rot = rotation_angle_deg(latest.orientation, last.q);
  const double dt_s = static_cast<double>(latest.t_mono_ns - last.t_ns) * 1e-9;
  if (force || dist >= cfg.keyframe_translation_m || rot >= cfg.keyframe_rotation_deg ||
      (cfg.keyframe_max_interval_s > 0.0 && dt_s >= cfg.keyframe_max_interval_s)) {
    build_keyframe(latest.t_mono_ns, latest);
  }
}

// --- loop detection ---------------------------------------------------------

Status PostSlamPipeline::Impl::run_loop_detection() {
  const std::size_t n = keyframes.size();
  if (!cfg.detect_loops || n < 3) {
    report(PostStage::kLoopDetection, 1.0, 0, 0);
    return kOkStatus;
  }
  ScanContextDb db(cfg.scan_context);
  for (std::size_t k = 0; k < n; ++k) {
    db.add(Span<const PointVertex>(keyframes[k].points.data(), keyframes[k].points.size()),
           keyframes[k].t_ns);
  }

  std::vector<PointVertex> submap_raw;
  std::vector<PointVertex> submap;
  VoxelDedupConfig scfg;
  scfg.voxel_size_m = cfg.loop_submap_voxel_m;

  for (std::size_t qi = 0; qi < n; ++qi) {
    if (is_cancelled()) return set_last_error(ScanError::kCancelled, "post: cancelled");
    report(PostStage::kLoopDetection, static_cast<double>(qi) / static_cast<double>(n), qi, n);

    const ScanContextMatch m = db.query(static_cast<std::uint32_t>(qi));
    if (!m.found) continue;
    ++stats.loop_candidates;

    // Target: the local map around the match, in the MATCH keyframe's body
    // frame. One keyframe alone is a fraction of a second of a non-repetitive
    // scan pattern and overlaps a revisit far less than the local map does.
    const std::size_t mi = m.index;
    const std::size_t lo = mi > cfg.loop_submap_half_span ? mi - cfg.loop_submap_half_span : 0;
    const std::size_t hi = std::min(n - 1, mi + cfg.loop_submap_half_span);
    const Keyframe& mk = keyframes[mi];
    submap_raw.clear();
    for (std::size_t k = lo; k <= hi; ++k) {
      double qr[4], pr[3];
      pose_between(mk.q, mk.p, keyframes[k].q, keyframes[k].p, qr, pr);
      for (const PointVertex& p : keyframes[k].points) {
        const double v[3] = {p.x, p.y, p.z};
        double o[3];
        quat_rotate(qr, v, o);
        PointVertex out = p;
        out.x = static_cast<float>(o[0] + pr[0]);
        out.y = static_cast<float>(o[1] + pr[1]);
        out.z = static_cast<float>(o[2] + pr[2]);
        submap_raw.push_back(out);
      }
    }
    voxel_downsample(Span<const PointVertex>(submap_raw.data(), submap_raw.size()), scfg, &submap,
                     token);

    // Initialization: Scan Context's yaw, no translation. The descriptor
    // matched because the two scans SEE the same place, so the position offset
    // is at most a ring width; the yaw is the part ICP cannot recover on its
    // own from a 180-degree revisit, and it is exactly what Scan Context hands
    // over for free.
    const double half = 0.5 * m.yaw_rad;
    const double init_q[4] = {0.0, 0.0, std::sin(half), std::cos(half)};
    const double init_p[3] = {0.0, 0.0, 0.0};

    LoopClosure lc;
    lc.from = static_cast<std::uint32_t>(mi);
    lc.to = static_cast<std::uint32_t>(qi);
    lc.sc_distance = m.distance;
    lc.sc_yaw_rad = m.yaw_rad;
    lc.icp = icp_point_to_plane(
        Span<const PointVertex>(keyframes[qi].points.data(), keyframes[qi].points.size()),
        Span<const PointVertex>(submap.data(), submap.size()), init_q, init_p, cfg.icp, token);
    if (is_cancelled()) return set_last_error(ScanError::kCancelled, "post: cancelled");

    lc.accepted = loop_is_acceptable(lc.icp, cfg.loop_accept, &lc.reject_reason);
    for (int i = 0; i < 4; ++i) lc.q[i] = lc.icp.q[i];
    for (int i = 0; i < 3; ++i) lc.p[i] = lc.icp.p[i];
    if (lc.accepted) {
      ++stats.loops_accepted;
    } else {
      ++stats.loops_rejected;
    }
    loops.push_back(lc);
  }
  report(PostStage::kLoopDetection, 1.0, n, n);
  return kOkStatus;
}

// --- pose graph -------------------------------------------------------------

Status PostSlamPipeline::Impl::run_optimization() {
  const std::size_t n = keyframes.size();
  if (n < 2) {
    report(PostStage::kOptimization, 1.0, 0, 0);
    return kOkStatus;
  }
  for (std::size_t k = 0; k < n; ++k) {
    keyframes[k].node = graph.add_node(keyframes[k].q, keyframes[k].p);
  }
  // Gauge: node 0 is the session origin. Fixing it (rather than pinning it
  // with a large prior) keeps the system's condition number honest.
  SCAN_TRY(graph.set_fixed(0, true));

  const double odom_sr = cfg.odom_sigma_rot_deg * se3::kDegToRad;
  for (std::size_t k = 1; k < n; ++k) {
    double qz[4], pz[3];
    pose_between(keyframes[k - 1].q, keyframes[k - 1].p, keyframes[k].q, keyframes[k].p, qz, pz);
    SCAN_TRY(graph.add_between(static_cast<std::uint32_t>(k - 1), static_cast<std::uint32_t>(k),
                               qz, pz, odom_sr, cfg.odom_sigma_trans_m, 0.0, false));
  }

  const double loop_sr = cfg.loop_sigma_rot_deg * se3::kDegToRad;
  for (const LoopClosure& lc : loops) {
    if (!lc.accepted) continue;
    // A loop that fit badly pulls proportionally less. The ICP RMS is the only
    // measured quality this edge has; using it beats a constant.
    const double st = std::max(cfg.loop_sigma_trans_m, lc.icp.rms_m);
    SCAN_TRY(graph.add_between(lc.from, lc.to, lc.q, lc.p, loop_sr, st, cfg.loop_huber_sigmas,
                               true));
  }

  report(PostStage::kOptimization, 0.1, 0, cfg.graph.max_iterations);
  Result<PoseGraphSummary> res = graph.optimize(cfg.graph, token);
  if (!res.ok()) return Status(res.error());
  stats.graph = res.value();

  double sum = 0.0;
  double worst = 0.0;
  for (std::size_t k = 0; k < n; ++k) {
    const PoseNode& node = graph.node(keyframes[k].node);
    for (int i = 0; i < 4; ++i) keyframes[k].q_opt[i] = node.q[i];
    for (int i = 0; i < 3; ++i) keyframes[k].p_opt[i] = node.p[i];
    const double dx = node.p[0] - keyframes[k].p[0];
    const double dy = node.p[1] - keyframes[k].p[1];
    const double dz = node.p[2] - keyframes[k].p[2];
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    sum += d * d;
    if (d > worst) worst = d;
  }
  stats.keyframe_shift_rms_m = std::sqrt(sum / static_cast<double>(n));
  stats.keyframe_shift_max_m = worst;
  report(PostStage::kOptimization, 1.0, stats.graph.iterations, cfg.graph.max_iterations);
  return kOkStatus;
}

// --- corrected trajectory ---------------------------------------------------

void PostSlamPipeline::Impl::build_corrected_trajectory() {
  // Per-keyframe world-frame correction C_k = X_k * O_k^-1, applied to the
  // odometry pose and BLENDED between the two bracketing keyframes. The
  // alternative — rigidly attaching each segment to the keyframe that starts
  // it — is exact at the keyframes and discontinuous between them, and a
  // discontinuity in the trajectory is a visible seam in the re-integrated
  // cloud.
  const std::size_t nk = keyframes.size();
  std::vector<double> cq(nk * 4), cp(nk * 3);
  for (std::size_t k = 0; k < nk; ++k) {
    double qi[4], pi[3];
    pose_inverse(keyframes[k].q, keyframes[k].p, qi, pi);
    pose_compose(keyframes[k].q_opt, keyframes[k].p_opt, qi, pi, &cq[k * 4], &cp[k * 3]);
  }

  std::size_t seg = 0;
  for (const Pose& o : odom_poses) {
    Pose out = o;
    out.source = StreamId::kPoseLio;
    if (nk == 0) {
      (void)corrected.push_pose(out);
      continue;
    }
    while (seg + 1 < nk && keyframes[seg + 1].t_ns <= o.t_mono_ns) ++seg;
    const std::size_t a = seg;
    const std::size_t b = (seg + 1 < nk) ? seg + 1 : seg;

    double qa[4], pa[3];
    pose_compose(&cq[a * 4], &cp[a * 3], o.orientation, o.position, qa, pa);
    if (a == b) {
      for (int i = 0; i < 4; ++i) out.orientation[i] = qa[i];
      for (int i = 0; i < 3; ++i) out.position[i] = pa[i];
      (void)corrected.push_pose(out);
      continue;
    }
    double qb[4], pb[3];
    pose_compose(&cq[b * 4], &cp[b * 3], o.orientation, o.position, qb, pb);
    const double span = static_cast<double>(keyframes[b].t_ns - keyframes[a].t_ns);
    double u = span > 0.0 ? static_cast<double>(o.t_mono_ns - keyframes[a].t_ns) / span : 0.0;
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    double qo[4];
    se3::quat_slerp(qa, qb, u, qo);
    for (int i = 0; i < 4; ++i) out.orientation[i] = qo[i];
    for (int i = 0; i < 3; ++i) out.position[i] = pa[i] + (pb[i] - pa[i]) * u;
    (void)corrected.push_pose(out);
  }
}

// --- publish ----------------------------------------------------------------

Status PostSlamPipeline::Impl::publish(const std::vector<PointVertex>& cloud) {
  if (!cfg.publish_to_store || cloud.empty()) {
    report(PostStage::kPublishing, 1.0, 0, 0);
    return kOkStatus;
  }
  PageStore& ps = store();
  const std::size_t before = ps.page_count();
  const std::size_t chunk = 65536;
  const std::int64_t t = keyframes.empty() ? 0 : keyframes.back().t_ns;
  for (std::size_t i = 0; i < cloud.size(); i += chunk) {
    if (is_cancelled()) return set_last_error(ScanError::kCancelled, "post: cancelled");
    const std::size_t n = std::min(chunk, cloud.size() - i);
    std::uint32_t appended = 0;
    const Status st =
        ps.append(cfg.out_stream, Span<const PointVertex>(cloud.data() + i, n), t, &appended);
    stats.store_append_failures += (n - appended);
    if (!st.ok() && st.error() != ScanError::kCapacityExceeded) return st;
    report(PostStage::kPublishing, static_cast<double>(i + n) / static_cast<double>(cloud.size()),
           i + n, cloud.size());
  }
  stats.pages_appended = static_cast<std::uint32_t>(ps.page_count() - before);
  report(PostStage::kPublishing, 1.0, cloud.size(), cloud.size());
  return kOkStatus;
}

// ===========================================================================
// PostSlamPipeline
// ===========================================================================

PostSlamPipeline::PostSlamPipeline(const PostConfig& cfg) : impl_(new Impl) {
  impl_->cfg = cfg;
  impl_->cfg.lio.internal_thread = false;
  impl_->token = &impl_->own_token;
}

PostSlamPipeline::~PostSlamPipeline() = default;

void PostSlamPipeline::set_progress_callback(PostProgressFn cb) {
  impl_->progress_cb = std::move(cb);
}

void PostSlamPipeline::set_cancel_token(CancelToken* token) {
  impl_->token = token != nullptr ? token : &impl_->own_token;
}

void PostSlamPipeline::cancel() { impl_->own_token.cancel(); }

float PostSlamPipeline::progress() const {
  return impl_->fraction.load(std::memory_order_relaxed);
}

PostStage PostSlamPipeline::stage() const {
  return static_cast<PostStage>(impl_->stage_v.load(std::memory_order_relaxed));
}

const PostStats& PostSlamPipeline::stats() const { return impl_->stats; }
const std::vector<Keyframe>& PostSlamPipeline::keyframes() const { return impl_->keyframes; }
const std::vector<LoopClosure>& PostSlamPipeline::loops() const { return impl_->loops; }
const PoseGraph& PostSlamPipeline::graph() const { return impl_->graph; }
const LioPoseSource& PostSlamPipeline::trajectory() const { return impl_->corrected; }
const std::vector<PointVertex>& PostSlamPipeline::final_cloud() const {
  return impl_->final_cloud;
}
PageStore& PostSlamPipeline::out_store() { return impl_->store(); }
const PostConfig& PostSlamPipeline::config() const { return impl_->cfg; }

Status PostSlamPipeline::run(const std::string& lscan_dir) {
  Impl& im = *impl_;
  im.reset();
  const double t0 = now_ms();

  auto fail = [&im](Status s) {
    im.stage_v.store(static_cast<int>(s.error() == ScanError::kCancelled ? PostStage::kCancelled
                                                                        : PostStage::kFailed),
                     std::memory_order_relaxed);
    return s;
  };

  im.report(PostStage::kOpening, 0.0, 0, 0);
  if (im.is_cancelled()) return fail(set_last_error(ScanError::kCancelled, "post: cancelled"));

  // --- stage 1: full-density LIO re-run + keyframing ---------------------
  {
    const double t = now_ms();
    LioOdometry lio(im.cfg.lio);
    SCAN_TRY(lio.start());
    im.lio = &lio;
    lio.poses().set_callback([&im](const Pose& p) { im.odom_poses.push_back(p); });

    const Status st = im.decode_pass(
        lscan_dir, PostStage::kOdometry, true,
        [&im, &lio](const std::vector<PointVertex>& batch, std::int64_t t_batch,
                    std::int64_t t_prev) {
          if (im.is_cancelled()) return false;
          im.buffer_points(batch, t_batch, t_prev);
          const Status s =
              lio.push_points(Span<const PointVertex>(batch.data(), batch.size()), t_batch);
          (void)s;
          im.maybe_keyframe(false);
          return true;
        },
        [&im, &lio](const ImuSample& s) {
          if (im.is_cancelled()) return false;
          const Status r = lio.push_imu(s.t_engine_ns, s.gyro_rad_s, s.accel_m_s2);
          (void)r;
          return true;
        });
    if (!st.ok()) {
      im.lio = nullptr;
      return fail(st);
    }
    (void)lio.flush();
    im.maybe_keyframe(true);
    im.stats.lio = lio.stats();
    im.stats.trajectory_length_m = lio.poses().trajectory_length_m();
    im.stats.odom_poses = im.odom_poses.size();
    im.stats.keyframes = im.keyframes.size();
    lio.poses().set_callback(nullptr);
    (void)lio.stop();
    im.lio = nullptr;
    im.stats.ms_odometry = now_ms() - t;
  }
  if (im.keyframes.empty()) {
    return fail(set_last_error(ScanError::kCorruptData,
                               "post: the odometry produced no keyframes from '%s' "
                               "(no IMU initialization, or no usable points)",
                               lscan_dir.c_str()));
  }

  // --- stage 2: loop candidates ------------------------------------------
  {
    const double t = now_ms();
    const Status st = im.run_loop_detection();
    if (!st.ok()) return fail(st);
    im.stats.ms_loops = now_ms() - t;
  }

  // --- stage 3: pose graph ------------------------------------------------
  {
    const double t = now_ms();
    const Status st = im.run_optimization();
    if (!st.ok()) return fail(st);
    im.stats.ms_optimize = now_ms() - t;
  }

  // --- stage 4: re-integration -------------------------------------------
  im.build_corrected_trajectory();
  VoxelAccumulator accum(im.cfg.dedup.voxel_size_m > 0.0 ? im.cfg.dedup.voxel_size_m : 0.001,
                         im.cfg.dedup.average_position);
  {
    const double t = now_ms();
    Pose first_pose, last_pose;
    const bool have_last = im.corrected.latest(&last_pose);
    const bool have_first = im.corrected.pose_at(
        im.odom_poses.empty() ? 0 : im.odom_poses.front().t_mono_ns, &first_pose).ok();
    const double* lt = im.cfg.lio.lidar_to_imu_t;
    const double* lq = im.cfg.lio.lidar_to_imu_q;

    const Status st = im.decode_pass(
        lscan_dir, PostStage::kReintegration, false,
        [&](const std::vector<PointVertex>& batch, std::int64_t t_batch, std::int64_t t_prev) {
          if (im.is_cancelled()) return false;
          const std::size_t n = batch.size();
          const double span = static_cast<double>(t_batch - t_prev);
          const float lo = im.cfg.lio.min_range_m;
          const float hi = im.cfg.lio.max_range_m;
          for (std::size_t i = 0; i < n; ++i) {
            const PointVertex& p = batch[i];
            const float r2 = p.x * p.x + p.y * p.y + p.z * p.z;
            if (r2 < lo * lo || (hi > 0.f && r2 > hi * hi)) continue;
            const double u = static_cast<double>(i + 1) / static_cast<double>(n);
            const std::int64_t t_pt = t_prev + static_cast<std::int64_t>(span * u);
            Pose at;
            const Status ps = im.corrected.pose_at(t_pt, &at);
            if (!ps.ok()) {
              // Clamp to the endpoint when the point is barely outside the
              // trajectory (the tail of a capture whose last partial scan
              // produced no pose); drop it otherwise.
              if (ps.error() == ScanError::kAgain && have_last &&
                  t_pt - last_pose.t_mono_ns <= kPoseClampSlackNs) {
                at = last_pose;
              } else if (ps.error() == ScanError::kNotFound && have_first &&
                         first_pose.t_mono_ns - t_pt <= kPoseClampSlackNs) {
                at = first_pose;
              } else {
                ++im.stats.unposed_points;
                continue;
              }
            }
            const double pl[3] = {p.x, p.y, p.z};
            double pb[3];
            quat_rotate(lq, pl, pb);
            pb[0] += lt[0];
            pb[1] += lt[1];
            pb[2] += lt[2];
            double pw[3];
            quat_rotate(at.orientation, pb, pw);
            accum.add(pw[0] + at.position[0], pw[1] + at.position[1], pw[2] + at.position[2], p.r,
                      p.g, p.b, p.a);
            ++im.stats.reintegrated_points;
          }
          return true;
        },
        [](const ImuSample&) { return true; });
    if (!st.ok()) return fail(st);
    im.stats.dedup_points = accum.voxel_count();
    im.stats.ms_reintegrate = now_ms() - t;
  }

  // --- stage 5: outlier filter --------------------------------------------
  {
    const double t = now_ms();
    std::vector<PointVertex> deduped;
    accum.extract(&deduped);
    accum.clear();
    im.report(PostStage::kFiltering, 0.2, deduped.size(), deduped.size());
    std::vector<PointVertex> filtered;
    const Status st = statistical_outlier_filter(
        Span<const PointVertex>(deduped.data(), deduped.size()), im.cfg.outlier,
        im.cfg.dedup.voxel_size_m, &filtered, &im.stats.outlier, im.token);
    if (!st.ok()) return fail(st);
    im.stats.final_points = filtered.size();
    im.report(PostStage::kFiltering, 1.0, filtered.size(), deduped.size());

    const Status pst = im.publish(filtered);
    if (!pst.ok()) return fail(pst);
    if (im.cfg.keep_final_cloud) im.final_cloud.swap(filtered);
    im.stats.ms_filter = now_ms() - t;
  }

  im.stats.ms_total = now_ms() - t0;
  im.report(PostStage::kDone, 1.0, im.stats.final_points, im.stats.final_points);
  return kOkStatus;
}

}  // namespace post
}  // namespace scanengine
