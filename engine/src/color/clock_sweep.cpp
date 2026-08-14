#include "scanengine/color/clock_sweep.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "scanengine/core/log.h"

namespace scanengine {
namespace color {

namespace {

constexpr const char* kMod = "color";

// Linear interpolation of a time-ordered track at `t`. The caller guarantees
// `t` is inside [front, back], so this only has to find the bracket.
double sample_track(Span<const RateSample> s, std::size_t& cursor, std::int64_t t) {
  while (cursor + 2 < s.size() && s[cursor + 1].t_ns < t) ++cursor;
  const RateSample& a = s[cursor];
  const RateSample& b = s[cursor + 1];
  const std::int64_t span = b.t_ns - a.t_ns;
  if (span <= 0) return a.value;
  const double u = static_cast<double>(t - a.t_ns) / static_cast<double>(span);
  return a.value + (b.value - a.value) * u;
}

Status check_track(Span<const RateSample> s, const char* which) {
  if (s.size() < 8) {
    return set_last_error(ScanError::kInvalidArgument,
                          "color: clock sweep %s track has %zu samples (need >= 8)", which,
                          s.size());
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (!std::isfinite(s[i].value)) {
      return set_last_error(ScanError::kInvalidArgument,
                            "color: clock sweep %s track sample %zu is not finite", which, i);
    }
    if (i > 0 && s[i].t_ns < s[i - 1].t_ns) {
      return set_last_error(ScanError::kInvalidArgument,
                            "color: clock sweep %s track is not time-ordered at sample %zu", which,
                            i);
    }
  }
  return kOkStatus;
}

struct Prepared {
  std::vector<double> v;  // mean-removed
  double std_dev = 0.0;
  double range = 0.0;
  std::uint32_t zero_crossings = 0;
};

Prepared prepare(const std::vector<double>& raw) {
  Prepared p;
  p.v = raw;
  if (raw.empty()) return p;
  double mean = 0.0;
  for (const double x : raw) mean += x;
  mean /= static_cast<double>(raw.size());
  double lo = raw[0], hi = raw[0], sq = 0.0;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    p.v[i] = raw[i] - mean;
    sq += p.v[i] * p.v[i];
    lo = std::min(lo, raw[i]);
    hi = std::max(hi, raw[i]);
  }
  p.std_dev = std::sqrt(sq / static_cast<double>(raw.size()));
  p.range = hi - lo;
  // Count sign changes of the mean-removed signal, ignoring exact zeros — a
  // sweep left and right crosses its own mean twice per cycle.
  int last_sign = 0;
  for (const double x : p.v) {
    const int s = (x > 0.0) ? 1 : ((x < 0.0) ? -1 : 0);
    if (s == 0) continue;
    if (last_sign != 0 && s != last_sign) ++p.zero_crossings;
    last_sign = s;
  }
  return p;
}

// Normalised cross-correlation of two equal-length mean-removed series at an
// integer lag, computed over their overlap only.
double ncc_at(const std::vector<double>& a, const std::vector<double>& b, int lag) {
  const int n = static_cast<int>(a.size());
  const int begin = std::max(0, -lag);
  const int end = std::min(n, n - lag);
  const int count = end - begin;
  if (count < 8) return 0.0;
  double num = 0.0, sa = 0.0, sb = 0.0;
  for (int i = begin; i < end; ++i) {
    const double x = a[static_cast<std::size_t>(i)];
    const double y = b[static_cast<std::size_t>(i + lag)];
    num += x * y;
    sa += x * x;
    sb += y * y;
  }
  const double den = std::sqrt(sa * sb);
  if (!(den > 0.0)) return 0.0;
  return num / den;
}

}  // namespace

const char* to_string(ClockSweepVerdict v) noexcept {
  switch (v) {
    case ClockSweepVerdict::kAccepted: return "accepted";
    case ClockSweepVerdict::kTooShort: return "too-short";
    case ClockSweepVerdict::kTooFewSamples: return "too-few-samples";
    case ClockSweepVerdict::kNoMotion: return "no-motion";
    case ClockSweepVerdict::kNoSweep: return "no-sweep";
    case ClockSweepVerdict::kWeakCorrelation: return "weak-correlation";
    case ClockSweepVerdict::kAmbiguous: return "ambiguous";
    case ClockSweepVerdict::kAtSearchEdge: return "at-search-edge";
  }
  return "unknown";
}

Status estimate_clock_offset(Span<const RateSample> camera, Span<const RateSample> lidar,
                             const ClockSweepConfig& cfg, ClockSweepResult* out) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "color: estimate_clock_offset(out == null)");
  }
  *out = ClockSweepResult{};
  if (cfg.resample_dt_ns <= 0 || cfg.max_offset_ns <= 0) {
    return set_last_error(ScanError::kInvalidArgument,
                          "color: clock sweep needs resample_dt_ns > 0 and max_offset_ns > 0");
  }
  SCAN_TRY(check_track(camera, "camera"));
  SCAN_TRY(check_track(lidar, "lidar"));

  // The lag search shifts the camera track by up to ±max_offset, so the grid
  // must stay inside both tracks at both extremes: shrink the overlap by the
  // search radius on each side.
  const std::int64_t lo =
      std::max(camera[0].t_ns, lidar[0].t_ns) + cfg.max_offset_ns;
  const std::int64_t hi =
      std::min(camera[camera.size() - 1].t_ns, lidar[lidar.size() - 1].t_ns) - cfg.max_offset_ns;
  out->overlap_ns = hi - lo;
  if (hi <= lo) {
    out->verdict = ClockSweepVerdict::kTooShort;
    return kOkStatus;
  }
  const std::int64_t n64 = (hi - lo) / cfg.resample_dt_ns + 1;
  if (n64 < 16) {
    out->verdict = ClockSweepVerdict::kTooFewSamples;
    return kOkStatus;
  }
  const std::size_t n = static_cast<std::size_t>(n64);
  out->grid_samples = static_cast<std::uint32_t>(n);

  std::vector<double> cam_raw(n), lid_raw(n);
  std::size_t ci = 0, li = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::int64_t t = lo + static_cast<std::int64_t>(i) * cfg.resample_dt_ns;
    cam_raw[i] = sample_track(camera, ci, t);
    lid_raw[i] = sample_track(lidar, li, t);
  }
  const Prepared cam = prepare(cam_raw);
  const Prepared lid = prepare(lid_raw);
  out->zero_crossings = cam.zero_crossings;

  if (out->overlap_ns < cfg.min_span_ns) {
    out->verdict = ClockSweepVerdict::kTooShort;
    return kOkStatus;
  }
  // "Did it move?" — relative, so it is unit-free and works for a bearing in
  // pixels as well as a rate in rad/s.
  const bool cam_flat = !(cam.range > 0.0) || cam.std_dev / cam.range < cfg.min_relative_std;
  const bool lid_flat = !(lid.range > 0.0) || lid.std_dev / lid.range < cfg.min_relative_std;
  if (cam_flat || lid_flat) {
    out->verdict = ClockSweepVerdict::kNoMotion;
    return kOkStatus;
  }
  if (cam.zero_crossings < cfg.min_zero_crossings) {
    out->verdict = ClockSweepVerdict::kNoSweep;
    return kOkStatus;
  }

  const int max_lag = static_cast<int>(cfg.max_offset_ns / cfg.resample_dt_ns);
  // camera(t) == lidar(t + offset): a positive offset finds the camera's
  // pattern in the lidar track LATER in the grid, i.e. at a positive lag.
  int best_lag = 0;
  double best = -2.0;
  std::vector<double> corr(static_cast<std::size_t>(2 * max_lag + 1), 0.0);
  for (int lag = -max_lag; lag <= max_lag; ++lag) {
    const double c = ncc_at(cam.v, lid.v, lag);
    corr[static_cast<std::size_t>(lag + max_lag)] = c;
    if (c > best) {
      best = c;
      best_lag = lag;
    }
  }
  out->correlation = best;
  out->offset_ns = static_cast<std::int64_t>(best_lag) * cfg.resample_dt_ns;

  // Sub-grid refinement: a parabola through the peak and its two neighbours.
  // This is what turns a 2 ms grid into a sub-millisecond answer.
  const std::size_t bi = static_cast<std::size_t>(best_lag + max_lag);
  if (bi > 0 && bi + 1 < corr.size()) {
    const double y0 = corr[bi - 1], y1 = corr[bi], y2 = corr[bi + 1];
    const double denom = y0 - 2.0 * y1 + y2;
    if (denom < 0.0) {  // a real maximum
      double delta = 0.5 * (y0 - y2) / denom;
      if (delta > 1.0) delta = 1.0;
      if (delta < -1.0) delta = -1.0;
      out->offset_ns = static_cast<std::int64_t>(
          std::llround((static_cast<double>(best_lag) + delta) * static_cast<double>(cfg.resample_dt_ns)));
      out->correlation = y1 - 0.25 * (y0 - y2) * delta;
      // Curvature → 1σ: with correlation ρ at the peak and curvature c per
      // grid step, the classic estimate is dt·sqrt((1−ρ²)/(N·c)).
      //
      // N is the number of INDEPENDENT samples, not of grid points. The grid
      // is deliberately finer than either input (2 ms against ARCore's 33 ms),
      // so counting grid points would divide by the oversampling factor and
      // report an uncertainty ~4x too small. The effective spacing is the
      // coarsest of the three.
      const double curvature = -denom;
      const double rho = std::min(0.999999, std::max(0.0, out->correlation));
      const double cam_dt =
          static_cast<double>(camera[camera.size() - 1].t_ns - camera[0].t_ns) /
          static_cast<double>(camera.size() - 1);
      const double lid_dt = static_cast<double>(lidar[lidar.size() - 1].t_ns - lidar[0].t_ns) /
                            static_cast<double>(lidar.size() - 1);
      const double eff_dt =
          std::max({cam_dt, lid_dt, static_cast<double>(cfg.resample_dt_ns)});
      const double n_eff =
          std::max(4.0, static_cast<double>(out->overlap_ns) / std::max(1.0, eff_dt));
      const double var = (1.0 - rho * rho) / (std::max(1e-9, curvature) * n_eff);
      out->sigma_ns = static_cast<double>(cfg.resample_dt_ns) * std::sqrt(std::max(0.0, var));
    }
  }

  // A rival PEAK — a second local maximum outside this peak's own lobe —
  // means the motion was periodic enough that a whole period fits inside the
  // search window, and the true lag cannot be told from lag ± T. Walking out
  // to the first local minimum is what separates "a rival" from "the shoulder
  // of the winner": the wizard's ~1 Hz sweep still correlates at 0.98 thirty
  // milliseconds off, and refusing that would refuse every good capture.
  std::size_t left = bi, right = bi;
  while (left > 0 && corr[left - 1] < corr[left]) --left;
  while (right + 1 < corr.size() && corr[right + 1] < corr[right]) ++right;
  double rival = -2.0;
  for (std::size_t i = 0; i < corr.size(); ++i) {
    if (i >= left && i <= right) continue;
    rival = std::max(rival, corr[i]);
  }
  const bool have_rival = rival > -1.5;
  out->rival_correlation = have_rival ? rival : 0.0;

  if (out->correlation < cfg.min_correlation) {
    out->verdict = ClockSweepVerdict::kWeakCorrelation;
    return kOkStatus;
  }
  if (std::abs(best_lag) >= max_lag) {
    out->verdict = ClockSweepVerdict::kAtSearchEdge;
    return kOkStatus;
  }
  if (have_rival && out->correlation - rival < cfg.min_peak_margin) {
    out->verdict = ClockSweepVerdict::kAmbiguous;
    return kOkStatus;
  }

  out->accepted = true;
  out->verdict = ClockSweepVerdict::kAccepted;
  SCAN_LOG_INFO(kMod, "clock sweep: offset %.3f ms (rho %.3f, sigma %.3f ms, %u samples)",
                static_cast<double>(out->offset_ns) * 1e-6, out->correlation,
                out->sigma_ns * 1e-6, out->grid_samples);
  return kOkStatus;
}

ColorizationPolicy policy_for(SyncQuality quality, bool allow_poor) {
  ColorizationPolicy p;
  switch (quality) {
    case SyncQuality::kGood:
      // S6 T8 at 5 ms: 15.3 px even at 30 °/s, inside the 20.2 px budget. The
      // gate is kept generous rather than absent, because 60 °/s is 20.8 px
      // and out of budget even here.
      p.colorize = true;
      p.motion_gate_deg_s = 30.f;
      p.motion_reject_deg_s = 90.f;
      p.reason = "sync good (<= 5 ms): colorize";
      break;
    case SyncQuality::kGated:
      // S6 §6.3: at 15 ms, preferring keyframes below 15 °/s gives 16.2 px.
      p.colorize = true;
      p.motion_gate_deg_s = 15.f;
      p.motion_reject_deg_s = 60.f;
      p.reason = "sync gated (<= 15 ms): keyframes below 15 deg/s";
      break;
    case SyncQuality::kPoor:
      // S6 §6.3: at 30 ms a 10 °/s gate still lands at 18.7 px — usable, but
      // only as a deliberate override, because ungated it is 36 px.
      p.colorize = allow_poor;
      p.motion_gate_deg_s = 10.f;
      p.motion_reject_deg_s = 20.f;
      p.reason = allow_poor ? "sync poor (> 15 ms): overridden, keyframes below 10 deg/s"
                            : "sync poor (> 15 ms): do not colorize";
      break;
    case SyncQuality::kUnknown:
    default:
      // A4 §4: jitter is meaningless before convergence, so this fails closed.
      p.colorize = false;
      p.motion_gate_deg_s = 10.f;
      p.motion_reject_deg_s = 20.f;
      p.reason = "sync unknown (not converged): do not colorize";
      break;
  }
  return p;
}

}  // namespace color
}  // namespace scanengine
