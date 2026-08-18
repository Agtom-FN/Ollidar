// reanchor.cpp — ROUND 17 item 63. See reanchor.h for the derivation.

#include "scanengine/poses/reanchor.h"

#include <cmath>

#include "scanengine/poses/se3.h"

namespace scanengine {
namespace reanchor {
namespace {

bool finite3(const double v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
bool finite4(const double v[4]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]);
}

double quat_angle_deg(const double q[4]) {
  double R[9];
  se3::quat_to_matrix(q, R);
  const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  return se3::rot_angle_deg(eye, R);
}

// The analytic transform, ROUND 13's line verbatim: T = M_a * M_b^-1.
void frame_change(const double q_b[4], const double p_b[3], const double q_a[4],
                  const double p_a[3], double out[16]) {
  double ma[16], mb[16], mbi[16];
  se3::mat4_from_quat_pos(q_a, p_a, ma);
  se3::mat4_from_quat_pos(q_b, p_b, mb);
  se3::mat4_inverse_rigid(mb, mbi);
  se3::mat4_mul(ma, mbi, out);
}

GapResult degenerate(const char* why) {
  GapResult r;
  r.verdict = GapVerdict::kRefusedDegenerate;
  r.reason = why;
  return r;
}

}  // namespace

const char* to_string(GapVerdict v) {
  switch (v) {
    case GapVerdict::kSnap: return "snap";
    case GapVerdict::kBridged: return "bridged";
    case GapVerdict::kNegligible: return "negligible";
    case GapVerdict::kRefusedNoGyro: return "refused-no-gyro";
    case GapVerdict::kRefusedTooLong: return "refused-too-long";
    case GapVerdict::kRefusedDisagree: return "refused-gyro-disagrees";
    case GapVerdict::kRefusedDegenerate: return "refused-degenerate";
  }
  return "unknown";
}

bool applies(GapVerdict v) { return v == GapVerdict::kSnap || v == GapVerdict::kBridged; }

bool is_refusal(GapVerdict v) {
  return v == GapVerdict::kRefusedNoGyro || v == GapVerdict::kRefusedTooLong ||
         v == GapVerdict::kRefusedDisagree || v == GapVerdict::kRefusedDegenerate;
}

GapResult resolve_reanchor(const double q_before[4], const double p_before[3],
                           std::int64_t t_before_ns, const double q_after[4],
                           const double p_after[3], std::int64_t t_after_ns,
                           const double* q_gyro_rel, bool gyro_had_hole,
                           const GapPolicy& policy) {
  if (t_after_ns <= t_before_ns) return degenerate("the two poses are not in order");
  if (!finite4(q_before) || !finite4(q_after) || !finite3(p_before) || !finite3(p_after)) {
    return degenerate("a pose is not finite");
  }
  double qb[4], qa[4];
  for (int i = 0; i < 4; ++i) {
    qb[i] = q_before[i];
    qa[i] = q_after[i];
  }
  if (!se3::quat_normalize(qb) || !se3::quat_normalize(qa)) {
    return degenerate("a pose's rotation is degenerate");
  }

  GapResult r;
  r.gap_s = static_cast<double>(t_after_ns - t_before_ns) * 1e-9;

  const double dp[3] = {p_after[0] - p_before[0], p_after[1] - p_before[1],
                        p_after[2] - p_before[2]};
  r.reported_translation_m = se3::norm3(dp);
  {
    double Rb[9], Ra[9];
    se3::quat_to_matrix(qb, Rb);
    se3::quat_to_matrix(qa, Ra);
    r.reported_rotation_deg = se3::rot_angle_deg(Rb, Ra);
  }

  // --- the short gap: ROUND 13's assumption holds, so ROUND 13's answer ------
  //
  // ROUND 18 item 69: holds AND the jump is the size of a re-anchor. The snap
  // branch used to apply T wholesale for ANY rotation, on the argument that
  // over 33 ms the operator is a statue so the jump is all frame. That
  // argument cuts the other way too: a statue's gyro reads ~zero, so the
  // residual against a gyro prediction IS the reported jump — and a reported
  // 56.85 deg (the owner's scan-047 break #3, healed live at an implied
  // 1720 deg/s) fails the same 25 deg bound every bridged gap is held to.
  // Round 13 measured real ARCore re-anchors at 8-13.5 deg; a one-frame jump
  // past `max_residual_rotation_deg` is a relocalisation or a frame restart,
  // not a re-anchor, and it takes the gyro-checked route below instead of
  // being taken on faith. Snaps at or under the bound — every snap round 13
  // and 15 measured and every existing fixture — are bit-identical.
  //
  // Translation is deliberately NOT part of this gate: the gyro is a witness
  // to rotation only, round 13 verified large translation-only snaps against
  // gravity (scan-030's 1.118 m), and charging a 33 ms jump against a walk
  // bound would under-correct a genuine frame shift by up to the bound.
  if (t_after_ns - t_before_ns <= policy.snap_gap_ns &&
      r.reported_rotation_deg <= policy.max_residual_rotation_deg) {
    frame_change(qb, p_before, qa, p_after, r.correction);
    if (!se3::mat4_is_rigid(r.correction, 1e-4)) {
      return degenerate("the pose pair does not define a rigid transform");
    }
    r.verdict = GapVerdict::kSnap;
    r.residual_translation_m = r.reported_translation_m;
    r.residual_rotation_deg = r.reported_rotation_deg;
    r.reason = "a snap: too short for the operator to have moved, so the jump IS the frame";
    return r;
  }

  if (t_after_ns - t_before_ns > policy.max_bridge_gap_ns) {
    r.verdict = GapVerdict::kRefusedTooLong;
    r.reason = "the tracker was blind for longer than the gyro is trusted to bridge";
    return r;
  }
  if (q_gyro_rel == nullptr || gyro_had_hole || !finite4(q_gyro_rel)) {
    r.verdict = GapVerdict::kRefusedNoGyro;
    // ROUND 18 item 69: an over-bounds SNAP lands here when the gyro cannot
    // check it. Its own reason, because "the operator's own motion is unknown"
    // is false for a one-frame gap — the motion is known (a statue), which is
    // exactly why the jump cannot be believed.
    r.reason = (t_after_ns - t_before_ns <= policy.snap_gap_ns)
                   ? "a one-frame jump larger than any re-anchor round 13 measured, and no gyro "
                     "to check it against — not applied"
                   : "no continuous gyro across the gap — the operator's own motion is unknown";
    return r;
  }

  // --- the bridge -----------------------------------------------------------
  double qg[4];
  for (int i = 0; i < 4; ++i) qg[i] = q_gyro_rel[i];
  if (!se3::quat_normalize(qg)) {
    r.verdict = GapVerdict::kRefusedNoGyro;
    r.reason = "the gyro's integrated rotation is degenerate";
    return r;
  }
  r.gyro_used = true;
  r.gyro_rotation_deg = quat_angle_deg(qg);

  // Where the operator really ended up, as far as anything can say. Rotation
  // from the gyro; position bounded rather than predicted (header).
  double q_pred[4];
  se3::quat_mul(qb, qg, q_pred);
  se3::quat_normalize(q_pred);

  r.walk_bound_m = policy.walk_speed_mps * r.gap_s + policy.walk_bound_slack_m;
  const double excess = r.reported_translation_m > r.walk_bound_m
                            ? r.reported_translation_m - r.walk_bound_m
                            : 0.0;
  double u[3] = {dp[0], dp[1], dp[2]};
  if (excess > 0.0) {
    (void)se3::normalize3(u);
  } else {
    u[0] = u[1] = u[2] = 0.0;
  }
  const double p_pred[3] = {p_after[0] - u[0] * excess, p_after[1] - u[1] * excess,
                            p_after[2] - u[2] * excess};

  {
    double Rp[9], Ra[9];
    se3::quat_to_matrix(q_pred, Rp);
    se3::quat_to_matrix(qa, Ra);
    r.residual_rotation_deg = se3::rot_angle_deg(Rp, Ra);
  }
  r.residual_translation_m = excess;

  if (r.residual_rotation_deg > policy.max_residual_rotation_deg) {
    r.reason =
        "the gyro and the tracker disagree about the blind window by more than a re-anchor "
        "can be — the frame is left alone and the seam is recorded";
    r.verdict = GapVerdict::kRefusedDisagree;
    return r;
  }
  if (r.residual_translation_m > policy.max_residual_translation_m) {
    r.reason = "the leftover displacement is larger than a re-anchor can be";
    r.verdict = GapVerdict::kRefusedDisagree;
    return r;
  }

  if (r.residual_rotation_deg < policy.min_correction_rotation_deg &&
      r.residual_translation_m < policy.min_correction_translation_m) {
    r.verdict = GapVerdict::kNegligible;
    r.reason = "the tracker and the gyro agree: the jump was the operator, not the frame";
    return r;
  }

  frame_change(q_pred, p_pred, qa, p_after, r.correction);
  if (!se3::mat4_is_rigid(r.correction, 1e-4)) {
    return degenerate("the bridged pair does not define a rigid transform");
  }
  r.verdict = GapVerdict::kBridged;
  r.reason = "gyro-bridged: only what the operator's own motion cannot explain is corrected";
  return r;
}

}  // namespace reanchor
}  // namespace scanengine
