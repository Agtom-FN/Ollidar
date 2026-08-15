// icp.cpp — pairwise refinement and the overlap gate (A13).
//
// The ICP kernel is A7's `post::icp_point_to_plane`, called one iteration at
// a time so the residual trace survives; see merge/icp.h for why that trade
// (one target-index rebuild per iteration) is the right one here.
#include "scanengine/merge/icp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "scanengine/poses/se3.h"
#include "scanengine/slam/post/cloud_filter.h"

#include "voxel_grid.h"

namespace scanengine {
namespace merge {
namespace {

void stride_cap(std::vector<PointVertex>* v, std::uint32_t cap) {
  if (cap == 0 || v->size() <= cap) return;
  // Stride, never truncate: the first N points of a lidar cloud are one side
  // of the room (A7 §3 makes the same argument about its keyframe cap).
  const std::size_t step = (v->size() + cap - 1) / cap;
  std::vector<PointVertex> out;
  out.reserve(cap);
  for (std::size_t i = 0; i < v->size(); i += step) out.push_back((*v)[i]);
  v->swap(out);
}

double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

}  // namespace

MergeIcpConfig::MergeIcpConfig() {
  // See the header: 0.5 m rather than A7's 1.0 m, because past ~0.5 m the
  // correspondences that survive are surface-edge artefacts and they inflate
  // the reported RMS 5x from the same, correct, alignment. The first
  // iterations still run at 3x this (coarse_gate_scale), which is where A6
  // §7.3's "do not tighten the gate" argument is honoured.
  icp.max_correspondence_m = 0.5;
  icp.target_cell_m = 0.5;
  icp.huber_m = 0.10;
}

void downsample(const SessionCloud& in, double voxel_m, std::uint32_t max_points,
                std::vector<PointVertex>* out) {
  out->clear();
  if (in.chunks.empty()) return;
  if (!(voxel_m > 0.0)) {
    for (const auto& c : in.chunks) out->insert(out->end(), c.begin(), c.end());
    stride_cap(out, max_points);
    return;
  }
  // A7's accumulator: insertion-ordered, averaging, and it never needs the
  // whole session resident at once — which is the point, because a session
  // arrives as pages.
  post::VoxelAccumulator acc(voxel_m, true);
  for (const auto& c : in.chunks) acc.add(c);
  acc.extract(out);
  stride_cap(out, max_points);
}

OverlapEstimate estimate_overlap(Span<const PointVertex> a, Span<const PointVertex> b,
                                 const double b_from_a[16], double voxel_m,
                                 std::uint32_t max_samples) {
  OverlapEstimate out;
  out.voxel_m = voxel_m;
  if (a.size() == 0 || b.size() == 0 || !(voxel_m > 0.0)) return out;

  double a_from_b[16];
  se3::mat4_inverse_rigid(b_from_a, a_from_b);

  detail::VoxelSet set_a(voxel_m), set_b(voxel_m);
  for (std::size_t i = 0; i < a.size(); ++i) set_a.insert(a[i].x, a[i].y, a[i].z);
  for (std::size_t i = 0; i < b.size(); ++i) set_b.insert(b[i].x, b[i].y, b[i].z);

  const std::size_t step_a =
      max_samples == 0 ? 1 : std::max<std::size_t>(1, (a.size() + max_samples - 1) / max_samples);
  const std::size_t step_b =
      max_samples == 0 ? 1 : std::max<std::size_t>(1, (b.size() + max_samples - 1) / max_samples);

  std::uint64_t hit_a = 0;
  for (std::size_t i = 0; i < a.size(); i += step_a) {
    const double p[3] = {a[i].x, a[i].y, a[i].z};
    double q[3];
    se3::mat4_apply(b_from_a, p, q);
    if (set_b.near(q[0], q[1], q[2])) ++hit_a;
    ++out.samples_a;
  }
  std::uint64_t hit_b = 0;
  for (std::size_t i = 0; i < b.size(); i += step_b) {
    const double p[3] = {b[i].x, b[i].y, b[i].z};
    double q[3];
    se3::mat4_apply(a_from_b, p, q);
    if (set_a.near(q[0], q[1], q[2])) ++hit_b;
    ++out.samples_b;
  }
  out.a_in_b = out.samples_a == 0 ? 0.0
                                  : static_cast<double>(hit_a) / static_cast<double>(out.samples_a);
  out.b_in_a = out.samples_b == 0 ? 0.0
                                  : static_cast<double>(hit_b) / static_cast<double>(out.samples_b);
  out.symmetric = std::min(out.a_in_b, out.b_in_a);
  return out;
}

namespace {

PairIcpResult refine_downsampled(const std::vector<PointVertex>& src,
                                 const std::vector<PointVertex>& tgt,
                                 const double init_b_from_a[16], const MergeIcpConfig& cfg,
                                 post::CancelToken* cancel, double t_start_ms) {
  PairIcpResult out;
  for (int i = 0; i < 16; ++i) out.init_b_from_a[i] = init_b_from_a[i];
  for (int i = 0; i < 16; ++i) out.b_from_a[i] = init_b_from_a[i];
  out.source_points = src.size();
  out.target_points = tgt.size();

  if (src.empty() || tgt.empty()) {
    out.blocker = "empty cloud";
    out.ms = now_ms() - t_start_ms;
    return out;
  }
  if (!se3::mat4_is_rigid(init_b_from_a, 1e-5)) {
    out.blocker = "initial transform is not rigid";
    out.ms = now_ms() - t_start_ms;
    return out;
  }

  const Span<const PointVertex> ssrc(src.data(), src.size());
  const Span<const PointVertex> stgt(tgt.data(), tgt.size());

  out.overlap = estimate_overlap(ssrc, stgt, init_b_from_a, cfg.overlap_voxel_m,
                                 cfg.overlap_samples);
  if (out.overlap.symmetric < cfg.min_overlap) {
    // NOT a bad fit — no fit at all. Reported as such so a workbench can say
    // "these two do not see the same place" instead of showing a residual.
    out.low_overlap = true;
    out.blocker = "overlap below threshold";
    out.ms = now_ms() - t_start_ms;
    return out;
  }

  double R0[9], t0[3];
  se3::mat4_get_rt(init_b_from_a, R0, t0);
  double q[4], p[3];
  se3::matrix_to_quat(R0, q);
  for (int i = 0; i < 3; ++i) p[i] = t0[i];
  double q_prev[4] = {q[0], q[1], q[2], q[3]};
  double p_prev[3] = {p[0], p[1], p[2]};

  post::IcpConfig one = cfg.icp;
  one.max_iterations = 1;
  // Convergence is the outer loop's business; a zero threshold can never be
  // met inside, so the inner call is a pure single Gauss-Newton step.
  one.converge_rot_rad = 0.0;
  one.converge_trans_m = 0.0;
  const double fine_gate = cfg.icp.max_correspondence_m;
  const double coarse_gate =
      cfg.coarse_gate_scale > 1.0 ? fine_gate * cfg.coarse_gate_scale : fine_gate;

  double prev_rms = std::numeric_limits<double>::infinity();
  bool have_prev = false;
  bool fine_stage = cfg.coarse_iterations == 0 || coarse_gate == fine_gate;
  out.refined = true;

  for (std::uint32_t it = 0; it < cfg.max_iterations; ++it) {
    if (post::cancelled(cancel)) {
      out.blocker = "cancelled";
      break;
    }
    if (!fine_stage && it >= cfg.coarse_iterations) fine_stage = true;
    const double gate = fine_stage ? fine_gate : coarse_gate;
    if (gate != one.max_correspondence_m) {
      one.max_correspondence_m = gate;
      one.target_cell_m = gate;
      // A new correspondence set is a new cost function: the previous
      // residual is not comparable to this one, so the monotonicity test
      // restarts at the stage boundary rather than firing on a step that
      // never happened.
      have_prev = false;
      prev_rms = std::numeric_limits<double>::infinity();
    }
    const post::IcpResult r = post::icp_point_to_plane(ssrc, stgt, q, p, one, cancel);
    if (r.iterations == 0) {
      // The kernel bailed before taking a step: fewer than six usable
      // correspondences, or a failed solve.
      if (!have_prev) out.blocker = "too few correspondences for a solve";
      break;
    }
    if (have_prev && r.rms_m > prev_rms * cfg.max_rms_increase_ratio) {
      // The previous step made it worse. Undo it, and do not record the worse
      // residual — which is what keeps `trace` monotone by construction.
      for (int i = 0; i < 4; ++i) q[i] = q_prev[i];
      for (int i = 0; i < 3; ++i) p[i] = p_prev[i];
      ++out.rejected_steps;
      if (!fine_stage) {
        // Stationary for the COARSE gate only. That is the signal to tighten
        // it, not to stop — the fine stage is where the answer is.
        fine_stage = true;
        have_prev = false;
        prev_rms = std::numeric_limits<double>::infinity();
        continue;
      }
      out.blocker = "residual stopped improving; last step rolled back";
      // A stationary point IS convergence — provided at least one earlier
      // step did improve. A rollback on the very first step means the
      // initialization was already the local optimum, which is a different
      // (and much less reassuring) statement.
      out.converged = out.trace.size() >= 2;
      break;
    }

    IcpIteration entry;
    entry.index = it;
    entry.gate_m = gate;
    entry.rms_m = r.rms_m;
    entry.fitness_m = r.fitness_m;
    entry.inliers = r.inliers;
    entry.inlier_ratio = r.inlier_ratio;

    double Rc[9], Rn[9];
    se3::quat_to_matrix(q, Rc);
    se3::quat_to_matrix(r.q, Rn);
    entry.step_rot_deg = se3::rot_angle_deg(Rc, Rn);
    const double dt[3] = {r.p[0] - p[0], r.p[1] - p[1], r.p[2] - p[2]};
    entry.step_trans_m = std::sqrt(dt[0] * dt[0] + dt[1] * dt[1] + dt[2] * dt[2]);
    out.trace.push_back(entry);

    prev_rms = r.rms_m;
    have_prev = true;
    for (int i = 0; i < 4; ++i) q_prev[i] = q[i];
    for (int i = 0; i < 3; ++i) p_prev[i] = p[i];
    for (int i = 0; i < 4; ++i) q[i] = r.q[i];
    for (int i = 0; i < 3; ++i) p[i] = r.p[i];
    ++out.iterations;
    out.inliers = r.inliers;
    out.inlier_ratio = r.inlier_ratio;

    if (entry.step_rot_deg * se3::kDegToRad < cfg.converge_rot_rad &&
        entry.step_trans_m < cfg.converge_trans_m) {
      if (!fine_stage) {
        // Converged for the coarse gate. Tighten and keep going: stopping
        // here would return an estimate fitted to correspondences the fine
        // stage is about to throw away.
        fine_stage = true;
        have_prev = false;
        prev_rms = std::numeric_limits<double>::infinity();
        continue;
      }
      out.converged = true;
      break;
    }
  }

  // One last evaluation AT the returned estimate, so trace.back() is the
  // residual of what the caller is actually given.
  if (!out.trace.empty()) {
    post::IcpConfig eval = one;
    eval.max_correspondence_m = fine_gate;
    eval.target_cell_m = fine_gate;
    const post::IcpResult fin = post::icp_point_to_plane(ssrc, stgt, q, p, eval, cancel);
    IcpIteration entry;
    entry.index = static_cast<std::uint32_t>(out.trace.size());
    entry.gate_m = fine_gate;
    entry.rms_m = fin.rms_m;
    entry.fitness_m = fin.fitness_m;
    entry.inliers = fin.inliers;
    entry.inlier_ratio = fin.inlier_ratio;
    out.trace.push_back(entry);
    out.inliers = fin.inliers;
    out.inlier_ratio = fin.inlier_ratio;
    out.rms_before_m = out.trace.front().rms_m;
    out.rms_after_m = entry.rms_m;
  }

  double Rf[9];
  se3::quat_to_matrix(q, Rf);
  se3::mat4_from_rt(Rf, p, out.b_from_a);
  out.overlap = estimate_overlap(ssrc, stgt, out.b_from_a, cfg.overlap_voxel_m,
                                 cfg.overlap_samples);
  out.ms = now_ms() - t_start_ms;
  return out;
}

}  // namespace

PairIcpResult refine_pair(const SessionCloud& source, const SessionCloud& target,
                          const double init_b_from_a[16], const MergeIcpConfig& cfg,
                          post::CancelToken* cancel) {
  const double t0 = now_ms();
  std::vector<PointVertex> src, tgt;
  downsample(source, cfg.source_voxel_m, cfg.max_source_points, &src);
  downsample(target, cfg.target_voxel_m, 0, &tgt);
  return refine_downsampled(src, tgt, init_b_from_a, cfg, cancel, t0);
}

PairIcpResult refine_pair(Span<const PointVertex> source, Span<const PointVertex> target,
                          const double init_b_from_a[16], const MergeIcpConfig& cfg,
                          post::CancelToken* cancel) {
  SessionCloud a, b;
  a.add(source);
  b.add(target);
  return refine_pair(a, b, init_b_from_a, cfg, cancel);
}

}  // namespace merge
}  // namespace scanengine
