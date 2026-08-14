// clock_sweep.h — the wizard's 8-second clock-offset sweep, and the S6
// sync-quality policy colorization runs under (A11, feeding B7).
//
// S6 §7.1 ranks this as "the highest value per unit of work in the whole
// spike", because time sync — not calibration — is what breaks the
// colorization budget: at 15 ms of residual jitter the sync × turn-rate term
// alone is 16.7 px of a 20.2 px budget (REPORT §6.1).
//
// A4 explains why an offset survives at all (docs/A4-timesync.md §3): a
// one-way timestamp stream is only observable up to the MINIMUM transport
// delay, so every mapped timestamp carries `true offset + min latency`. That
// residue is systematic — identical for every sample, constant within a
// session — and no amount of filtering can see it. Two independent sensors
// watching the SAME physical motion can: that is this file.
//
// WIZARD.md screen 3 is the capture: "sweep the phone smoothly left and right
// across the board, about one sweep per second", 8 seconds. Both sensors
// observe the same target through the motion; cross-correlating the two
// angular-rate (or target-bearing) tracks recovers the constant offset.
//
// The estimator is deliberately signal-agnostic: it takes two scalar tracks
// with timestamps and no units. In the wizard they are the target's angular
// bearing rate in the camera and in the lidar; on a bench they can be
// ARCore's gyro against the Mid-360's IMU. What matters is that both measure
// the same physical quantity up to an arbitrary scale and offset, which the
// normalised correlation removes.
//
// Owner: A11.
#ifndef SCANENGINE_COLOR_CLOCK_SWEEP_H
#define SCANENGINE_COLOR_CLOCK_SWEEP_H

#include <cstdint>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {
namespace color {

// One sample of "how fast the thing is turning", on that sensor's own clock.
struct RateSample {
  std::int64_t t_ns = 0;
  double value = 0.0;  // rad/s, px/s, or any scale — normalised away
};

struct ClockSweepConfig {
  // Search window. ±100 ms covers the spec's 5–30 ms residual jitter with a
  // wide margin, and a wrong peak beyond it would be a different failure
  // (a mis-set clock domain), not an offset to estimate.
  std::int64_t max_offset_ns = 100'000'000;
  // Resampling grid. 2 ms is well inside the ±1 ms accuracy target once the
  // parabolic peak refinement below is applied, and keeps an 8 s sweep at
  // 4,000 samples per track — a correlation that costs microseconds.
  std::int64_t resample_dt_ns = 2'000'000;
  // Minimum overlap of the two tracks. WIZARD.md asks the user for 8 s; 4 s
  // is the floor at which ~4 sweeps still constrain the peak.
  std::int64_t min_span_ns = 4'000'000'000;
  // The capture must actually MOVE. A flat track correlates with anything.
  // Expressed as a fraction of the track's own peak-to-peak, so it is unit
  // free: std / range.
  double min_relative_std = 0.12;
  // Peak normalised correlation below which the estimate is refused.
  double min_correlation = 0.70;
  // The peak must beat any RIVAL peak by this margin. "Rival" means a second
  // local maximum outside the main peak's own lobe — found by walking out
  // from the peak until the correlation stops falling — not merely the
  // shoulder of the peak itself. The distinction matters: the wizard's sweep
  // is a ~1 Hz sinusoid, whose correlation is still 0.98 at a 30 ms lag, so a
  // fixed exclusion window would refuse every good capture. What must be
  // refused is a signal periodic enough that a WHOLE PERIOD fits inside the
  // search window, where the true lag genuinely cannot be told from lag ± T.
  double min_peak_margin = 0.10;
  // Zero crossings of the mean-removed camera track: one sweep per second for
  // 8 s is ~16. Fewer than this and the user did not sweep.
  std::uint32_t min_zero_crossings = 4;
};

enum class ClockSweepVerdict : std::uint8_t {
  kAccepted = 0,
  kTooShort = 1,        // overlap below min_span_ns
  kTooFewSamples = 2,   // a track has < 8 samples, or the grid is empty
  kNoMotion = 3,        // one of the tracks is flat
  kNoSweep = 4,         // moved, but not back and forth
  kWeakCorrelation = 5, // peak below min_correlation
  kAmbiguous = 6,       // a rival peak is within min_peak_margin
  kAtSearchEdge = 7,    // peak sits on the ±max_offset boundary
};

const char* to_string(ClockSweepVerdict v) noexcept;

// SIGN CONVENTION, stated once and asserted by the tests:
//
//     t_engine_ns = t_camera_ns + offset_ns
//
// i.e. `offset_ns` is what to ADD to a camera timestamp to express it on the
// lidar/engine clock. Equivalently `camera(t) == lidar(t + offset_ns)`: a
// positive offset means the camera clock reads EARLY relative to the lidar's.
// The colorizer applies it to every keyframe stamp before projecting
// (`ColorizeConfig::camera_clock_offset_ns`).
struct ClockSweepResult {
  std::int64_t offset_ns = 0;
  double correlation = 0.0;      // normalised, at the refined peak
  double rival_correlation = 0.0;  // best peak outside the exclusion window
  // 1σ of the offset, from the curvature of the correlation peak. Honest to
  // an order of magnitude, not to a decimal — its job is to let the wizard
  // say "±2 ms" rather than to weight a least-squares fit.
  double sigma_ns = 0.0;
  std::uint32_t grid_samples = 0;
  std::uint32_t zero_crossings = 0;
  std::int64_t overlap_ns = 0;
  bool accepted = false;
  ClockSweepVerdict verdict = ClockSweepVerdict::kTooFewSamples;
};

// Estimates the constant camera↔lidar clock offset from two rate tracks.
//
// Both tracks are linearly resampled onto a common uniform grid over their
// overlap, mean-removed, and cross-correlated over ±max_offset_ns; the peak
// is refined by a parabola through its two neighbours (which is what buys
// sub-grid accuracy — a 2 ms grid recovers a 37 ms offset to well under 1 ms).
//
// Returns kInvalidArgument only for structurally bad input (unsorted or empty
// tracks, non-finite values, nonsense config). A capture that simply is not
// good enough returns kOkStatus with `accepted == false` and a verdict —
// because "the user did not sweep" is a thing the wizard must SAY, not an
// error to propagate.
Status estimate_clock_offset(Span<const RateSample> camera, Span<const RateSample> lidar,
                             const ClockSweepConfig& cfg, ClockSweepResult* out);

// --- the colorization go/no-go (S6 §1, §6.3; A4 §7) -------------------------
//
// "Never gate on jitter_ns alone" — A4 §7. The input is `SyncQuality`, which
// is `kUnknown` unless the estimator has converged, so an unconverged session
// fails closed.
struct ColorizationPolicy {
  bool colorize = false;
  // Angular-rate gate for keyframe selection, deg/s. S6 T8: at 15 ms of
  // jitter, preferring keyframes below 15 °/s brings the 3 m error to 16.2 px
  // (pass); at 30 ms the threshold tightens to 10 °/s → 18.7 px (pass).
  float motion_gate_deg_s = 15.f;
  // Above this, a keyframe is not merely penalised but refused.
  float motion_reject_deg_s = 60.f;
  // Points coloured from frames above the gate are kept and flagged
  // low-confidence — S6 §6.3's explicit instruction, and the same treatment
  // §3.3 gives ARCore tracking-loss points.
  const char* reason = "";
};

// `allow_poor` is the operator override behind which the kPoor case sits:
// S6's 30 ms row passes at a 10 °/s gate (18.7 px), so the data is not
// worthless — but the default must be the conservative one, because at 30 ms
// the ungated error is 36 px against a 20.2 px budget.
ColorizationPolicy policy_for(SyncQuality quality, bool allow_poor = false);

}  // namespace color
}  // namespace scanengine

#endif  // SCANENGINE_COLOR_CLOCK_SWEEP_H
