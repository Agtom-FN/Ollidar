// reanchor.h — ROUND 17 item 63. WHAT A POSE JUMP ACROSS A LONG GAP IS, AND
// WHY THE ROUND-13/15 TRANSFORM IS THE WRONG ANSWER TO IT.
//
// The owner's scan-040 (2026-08-19, 81 s): *"the scan is not good. the shift of
// my position shifted quite a lot."* One live heal fired, and its numbers are
// the whole story:
//
//     jump = 0.678 m / 66.21 deg     gapMs = 6065
//
// ROUND 13 derived the analytic transform for a seam and stated its own
// assumption in the same breath:
//
//     T_k = pose_after * pose_before^-1
//     "(the operator's own motion during the 33 ms gap is inside T_k too, but
//      the gyro bounds it at ~1 deg and the trajectory bounds the translation
//      at ~1 cm, so T_k is the frame change to within that.)"
//
// THIRTY-THREE MILLISECONDS. Every one of the breaks ROUND 13 measured was one
// ARCore frame wide, and over one frame a human is, to a very good
// approximation, a statue: whatever the pose did, the world did. Over 6.065
// SECONDS a human is not a statue. They walk, and they turn, and every metre
// and every degree of that is inside T_k with no label on it saying so.
//
// Measured on scan-040's actual bytes, with its own recorded 399.2 Hz gyro:
//
//     ARCore, last tracked -> first re-acquired      66.21 deg
//     gyro, integrated across the same 6.065 s      144.94 deg
//
// ARCore froze: all 181 poses inside the loss carry the last good pose
// verbatim (0.00 deg, 0.000 m of motion across six seconds — a phone in a
// walking hand does not do that). The gyro did not freeze. So the 66.21 deg
// is not a frame correction; it is the leftover of a 145 deg turn that the
// tracker only saw the end of. Feeding it to the ROUND-15 live healer applied
// a 66 deg world rotation to every point already on the screen. That is
// "the shift of my position shifted quite a lot", exactly.
//
// --- THE FIX: PREDICT, THEN HEAL ONLY THE RESIDUAL ---------------------------
//
// The gyro is trustworthy over exactly this span and it is the only witness to
// the blind window. ROUND 12 measured its drift at under 1.1 deg over 16 s;
// re-measured here on the three owner captures, against ARCore itself over
// every clean 1 s window:
//
//     scan-040   2188 windows   median 0.109 deg   p90 0.363 deg   max 2.430
//     scan-041   1058 windows   median 0.152 deg   p90 0.494 deg   max 1.637
//     scan-042    667 windows   median 0.465 deg   p90 0.711 deg   max 5.173
//
// So: predict where the operator ended up, and heal only what the tracker and
// the prediction disagree about.
//
//     q_pred = q_before * q_gyro           (the operator's real rotation)
//     p_pred = p_after - u * excess        (see below)
//     T      = M_after * M_pred^-1         (what is left over = the frame)
//
// TRANSLATION IS NOT PREDICTED, IT IS BOUNDED. Double-integrating a consumer
// accelerometer over six seconds gives metres of nonsense (the item-46 note,
// restated) and inventing a displacement would be exactly the null space
// ROUND 16 spent its round refusing to fill. What CAN be said without
// inventing anything is how far a person could possibly have walked:
// `walk_speed_mps * gap + slack`. Displacement inside that bound is
// attributed to the operator and left alone; only the EXCESS — motion no walk
// could produce — is attributed to the frame. On scan-040 the bound is 11.2 m
// against a reported 0.678 m, so the translation correction is exactly zero,
// which is the honest answer: nobody knows where he was, and the tracker's
// claim is not impossible.
//
// --- AND WHEN THE TWO WITNESSES DISAGREE, REFUSE -----------------------------
//
// scan-040's residual rotation is 78.91 deg. That is not a re-anchor: every
// re-anchor ROUND 13 measured was 8-14 deg, and ARCore correcting itself by 79
// degrees against its own session map is not a thing that happens. Something
// else is true — the tracker restarted its frame, or the gyro is lying, or
// both — and NOTHING in the container can say which. So the answer is not a
// better guess, it is a refusal: leave the frame alone, record the seam, tell
// the operator, and let the offline pass (which has the whole cloud, and can
// measure surfaces rather than trust a pose) decide with more evidence than
// a live frame has.
//
// That refusal is the headline behaviour change of item 63. The live map stops
// being rotated by a number nobody can justify.
//
// --- ONE POLICY, TWO CALLERS -------------------------------------------------
//
// core/engine.cpp (live, ARCore pump thread, gyro from the densifier's ring)
// and slam/post/section_stitch.cpp (offline, whole stream in hand) both call
// resolve_reanchor(). They must not drift apart: ROUND 13 already duplicated
// the DETECTION constants across the C++/Kotlin boundary and named the
// duplication; duplicating the DECISION as well is how a scan gets healed one
// way on the phone and another way in Process.
//
// Deterministic: no clock, no RNG, no iteration count that depends on data.
//
// Owner: ROUND 17.
#ifndef SCANENGINE_POSES_REANCHOR_H
#define SCANENGINE_POSES_REANCHOR_H

#include <cstdint>

namespace scanengine {
namespace reanchor {

// The one witness to a blind window, as an interface, because the live caller
// asks a ring on the ARCore pump thread and the offline caller asks a copy of
// the whole recorded stream — same integrator, same answer, two lifetimes.
// ImuDensifiedPoseSource implements it.
class GyroBridge {
 public:
  virtual ~GyroBridge() = default;

  // Relative rotation of the BODY across [t0, t1], expressed in the frame the
  // poses are reported in. False when the stream does not straddle the
  // interval at all; `*saw_hole` (never null) says it did but stuttered
  // inside, which resolve_reanchor() treats as no gyro.
  virtual bool relative_rotation(std::int64_t t0, std::int64_t t1, double q_rel[4],
                                 double* peak_rate_rad_s, bool* saw_hole) const = 0;
};

struct GapPolicy {
  // At or under this, the ROUND-13 assumption holds and the analytic transform
  // IS the frame change: one ARCore frame is 33 ms, and 100 ms is three of
  // them — enough slack for a dropped frame or two, far short of a step.
  //
  // ROUND 18 item 69: ...provided the reported ROTATION is at or under
  // `max_residual_rotation_deg`. The statue assumption cuts both ways — over
  // 33 ms the operator's gyro reads ~zero, so a one-frame jump past anything
  // a re-anchor can be (the owner's scan-047: 56.85 deg at an implied
  // 1,720 deg/s, gyro 0.67 deg, self-check 6.92 -> 3.42 cm once refused) is a
  // relocalisation or a frame restart, and it takes the gyro-checked bridge
  // route below instead of being applied on faith. Snaps at round-13's
  // measured sizes (8-13.5 deg) are bit-identical. Translation is not part of
  // the gate: the gyro cannot witness it, and round 13 verified the large
  // translation-only snaps against gravity.
  std::int64_t snap_gap_ns = 100'000'000;

  // The longest gap the gyro is asked to bridge. `ImuDensifyConfig::capacity`
  // is 3200 samples = 8 s at 400 Hz, so past this the live ring cannot answer
  // anyway; the offline stream could, but a tracker that has been blind for
  // more than eight seconds is not describing the same room and the seam
  // belongs to the offline registration problem, not to a transform.
  std::int64_t max_bridge_gap_ns = 8'000'000'000;

  // How fast a person walks, for the translation bound. 1.8 m/s is a brisk
  // walk; PoseSectionTracker::MAX_SPEED_MPS (6.0) is deliberately NOT reused —
  // that one is "no human could exceed this even lunging", which is the right
  // number for calling a 33 ms step impossible and far too generous as a
  // six-second bound.
  double walk_speed_mps = 1.8;
  // Added to the bound so a short gap is not judged on a metre of walking that
  // is really the tracker's own uncertainty at the moment it let go.
  double walk_bound_slack_m = 0.30;

  // Residual rotation past which the two witnesses are not disagreeing about a
  // frame, they are disagreeing about reality. Every ARCore re-anchor ROUND 13
  // measured was 8-14 deg; 25 deg is generous against that and refuses
  // scan-040's 78.91 deg without argument.
  double max_residual_rotation_deg = 25.0;

  // Same idea for what is left after the walk bound is taken out. A frame that
  // has moved more than this is not a re-anchor either.
  double max_residual_translation_m = 2.0;

  // Below BOTH of these the correction is not worth making and, more
  // importantly, not worth CLAIMING: a seam that resolves to 8 mm and a fifth
  // of a degree is a seam the container is better off recording and leaving
  // alone. Offline this is also what keeps every capture that has nothing to
  // fix bit-identical to the round-16 result.
  double min_correction_translation_m = 0.010;
  double min_correction_rotation_deg = 0.25;
};

enum class GapVerdict {
  kSnap = 0,          // short gap: the analytic transform, unchanged from ROUND 13/15
  kBridged,           // long gap, gyro agreed: only the residual is applied
  kNegligible,        // long gap, gyro agreed, and the residual is not worth applying
  kRefusedNoGyro,     // the gyro does not cover the gap (or stutters inside it)
  kRefusedTooLong,    // past max_bridge_gap_ns
  kRefusedDisagree,   // gyro and tracker disagree past the bound — scan-040
  kRefusedDegenerate  // not finite, not ordered, not a rotation
};

const char* to_string(GapVerdict v);

// True when the caller should apply `correction`. kNegligible is deliberately
// NOT applied but is also NOT a refusal: nothing went wrong, there was just
// nothing to do, and the operator must not be cued about it.
bool applies(GapVerdict v);

// True when the operator should be told (i.e. the seam survives uncorrected).
bool is_refusal(GapVerdict v);

struct GapResult {
  GapVerdict verdict = GapVerdict::kRefusedDegenerate;
  // Row-major 4x4, world-frame left-multiplication taking the OLD frame to the
  // NEW one. Identity whenever `applies(verdict)` is false, so a caller that
  // ignores the verdict and multiplies anyway is merely wasteful, never wrong.
  double correction[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  double gap_s = 0.0;
  // What the tracker claims across the gap.
  double reported_translation_m = 0.0;
  double reported_rotation_deg = 0.0;
  // What the gyro says the operator actually turned across the same span.
  // Zero when there was no gyro to ask.
  double gyro_rotation_deg = 0.0;
  // What is left once the operator's own motion is taken out.
  double residual_translation_m = 0.0;
  double residual_rotation_deg = 0.0;
  // The bound the reported displacement was judged against.
  double walk_bound_m = 0.0;
  bool gyro_used = false;
  const char* reason = "";
};

// `q_*` are (x, y, z, w) unit quaternions, `p_*` metres, both world_from_phone
// and both in the frame the tracker reported them in.
//
// `q_gyro_rel` is the relative rotation of the BODY across [t_before, t_after],
// already in the pose's frame (ImuDensifiedPoseSource::relative_rotation
// returns exactly that). Pass nullptr when there is no gyro; pass
// `gyro_had_hole = true` when the stream stuttered inside the interval, which
// is treated as no gyro at all.
GapResult resolve_reanchor(const double q_before[4], const double p_before[3],
                           std::int64_t t_before_ns, const double q_after[4],
                           const double p_after[3], std::int64_t t_after_ns,
                           const double* q_gyro_rel, bool gyro_had_hole,
                           const GapPolicy& policy);

}  // namespace reanchor
}  // namespace scanengine

#endif  // SCANENGINE_POSES_REANCHOR_H
