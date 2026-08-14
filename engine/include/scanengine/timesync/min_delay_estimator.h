// min_delay_estimator.h — the real device→engine clock estimator (task A4).
//
// Tech Spec §3.2 asks for "arrival-stamping + per-stream device-timestamp
// offset with drift-tracked median filter". A median is the wrong statistic
// for a one-way timestamp stream, and this file implements what the
// literature (and every practical one-way-delay clock filter: NTP's clock
// filter, Paxson's / Moon-Skelly-Towsley's linear-programming line fit,
// Zhang-Liu-Xia's convex-hull estimator) actually uses:
//
//   delta_i = t_arrival_i − t_device_i = offset + drift·t_device_i + q_i,
//   with the queueing/transport delay q_i ≥ 0 and ONE-SIDED.
//
// Because q_i is one-sided, the *lower envelope* of delta over time is the
// clock line, and a median (or a mean) is biased by the whole delay
// distribution instead of by only its floor. Concretely, for the two
// transports this engine actually has:
//
//   • CH340 USB-serial (COIN-D6, and any UART GNSS): the host delivers
//     ~64-byte chunks, so a burst of samples shares one arrival stamp and
//     the later ones look up to a full chunk-time late. A median tracks the
//     middle of that sawtooth; the min tracks its (correct) leading edge.
//   • Mid-360 UDP: the sensor batches ~100 points per datagram and the OS
//     may coalesce datagrams; occasional scheduler/Wi-Fi stalls add tens of
//     milliseconds to single samples. The min filter ignores them entirely,
//     a median only downweights them.
//
// Implementation: a *windowed* minimum-delay filter (one lower-envelope
// point per `window_ns`, kept in a ring of `windows` entries) plus an
// ordinary least-squares line fit through those envelope points, which is a
// bounded-memory approximation of the convex-hull/LP estimator. The exact
// convex hull needs unbounded state, is O(n) to rebuild after any clock
// discontinuity, and — this is what decides it — cannot forget a stale
// envelope point, which is precisely what you need when a device warms up
// and its crystal drift changes sign. See engine/docs/A4-timesync.md §2.
//
// WHAT IS ESTIMATED AND WHAT IS NOT. The min filter recovers the clock line
// up to the *minimum* one-way delay, which is unobservable from one-way
// stamps: the reported offset is (true offset + min transport latency).
// That residue is SYSTEMATIC, not jitter, it does not move within a session,
// and it is exactly what S6 §7.1 item 1 asks A11's 8-second wizard sweep to
// measure and remove. `jitter_ns` deliberately does not hide it.
//
// Owner: A4.
#ifndef SCANENGINE_TIMESYNC_MIN_DELAY_ESTIMATOR_H
#define SCANENGINE_TIMESYNC_MIN_DELAY_ESTIMATOR_H

#include <cstdint>
#include <mutex>
#include <vector>

#include "scanengine/core/types.h"
#include "scanengine/timesync/clock.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {

// Why a mapping was thrown away and rebuilt. A `kDeviceReset` or `kClockStep`
// is a data-integrity event (the device's clock is not the clock we modelled)
// and is published on the event bus by TimeSync; `kStreamGap` is routine.
enum class ResyncReason : std::uint8_t {
  kNone = 0,
  kColdStart = 1,    // first pair of the stream / after reset()
  kDeviceReset = 2,  // device timestamp went backwards — reboot mid-session
  kClockStep = 3,    // confirmed forward/backward step of the device clock
  kStreamGap = 4,    // silence longer than max_gap_ns; delays may have changed
};

const char* to_string(ResyncReason r) noexcept;

struct ClockResync {
  StreamId stream = StreamId::kUnknown;
  ResyncReason reason = ResyncReason::kNone;
  std::int64_t t_engine_ns = 0;          // arrival stamp of the triggering pair
  std::int64_t step_ns = 0;              // observed discontinuity
  std::int64_t previous_offset_ns = 0;
  std::int64_t new_offset_ns = 0;
  std::uint32_t index = 0;               // 1-based count for this stream
};

// Fired INLINE from add_pair() with the estimator's lock held: it must be
// quick and must not call back into the estimator or into TimeSync.
using ClockResyncFn = void (*)(const ClockResync& ev, void* user_data);

struct MinDelayConfig {
  // One lower-envelope sample per window. 500 ms × 64 = 32 s of history:
  // long enough that a 20 ppm drift (640 µs over the span) is far above the
  // envelope noise, short enough to forget a thermal drift change.
  std::int64_t window_ns = 500'000'000;
  std::uint32_t windows = 64;

  // Residual ring used for jitter_ns. 512 pairs ≈ 2.5 s at 200 Hz, ≈ 8 min at
  // 1 Hz — always the *recent* delay distribution, never the session average.
  std::uint32_t residual_window = 512;

  // Convergence gate: how much evidence before jitter_ns / drift_ppm may be
  // believed. See docs §4 for the convergence budget this implies.
  std::int64_t converged_span_ns = 2'000'000'000;
  std::uint32_t converged_min_windows = 4;
  std::uint32_t converged_min_residuals = 16;

  // Sanity clamp. Real crystals are ±100 ppm; anything beyond this is a bug
  // or a clock step, and must never be extrapolated.
  double max_drift_ppm = 500.0;

  // Discontinuity detection. A residual this large is not a delay spike:
  // 250 ms is ~5× the worst arrival stall S2-sim ever saw and 8× the top of
  // the spec's 5–30 ms jitter range.
  std::int64_t step_threshold_ns = 250'000'000;
  std::uint32_t step_confirm = 3;          // consecutive pairs before we act
  std::int64_t backward_tolerance_ns = 200'000'000;  // below this = reordering
  std::int64_t max_gap_ns = 5'000'000'000;           // silence → soft resync

  // What to report as mapping uncertainty before convergence. The pessimistic
  // end of the spec's stated 5–30 ms range, so an A11 gate that reads it
  // *before* the estimator has evidence turns colorization off rather than on.
  std::int64_t unconverged_uncertainty_ns = 30'000'000;
};

class MinDelayOffsetEstimator final : public OffsetEstimator {
 public:
  explicit MinDelayOffsetEstimator(const MinDelayConfig& cfg = MinDelayConfig{});
  ~MinDelayOffsetEstimator() override;

  // O(1) amortized; O(windows) once per window_ns, O(k log k) on the jitter
  // refresh. Safe from one producer thread at a time (the driver's).
  void add_pair(std::int64_t t_device_ns, TimePoint t_arrival) override;
  OffsetEstimate estimate() const override;
  TimeModel model() const override;
  void reset() override;

  // Identifies this stream in ClockResync events. TimeSync sets it.
  void set_stream(StreamId s);
  void set_resync_observer(ClockResyncFn fn, void* user_data);

  const MinDelayConfig& config() const { return cfg_; }

  struct Diagnostics {
    std::uint64_t pairs_seen = 0;
    std::uint64_t pairs_accepted = 0;   // folded into the model
    std::uint64_t pairs_reordered = 0;  // device stamp went back < tolerance
    std::uint64_t pairs_rejected = 0;   // |residual| > step_threshold, unconfirmed
    std::uint32_t closed_windows = 0;
    std::uint32_t resyncs = 0;
    std::int64_t history_span_ns = 0;   // device-time span of the envelope ring
  };
  Diagnostics diagnostics() const;

 private:
  struct Envelope {
    std::int64_t t_device_ns = 0;
    std::int64_t delta_ns = 0;
  };

  void seed_(std::int64_t t_device_ns, std::int64_t delta_ns, std::int64_t arrival_ns);
  void accept_(std::int64_t t_device_ns, std::int64_t delta_ns, std::int64_t residual_ns,
               std::int64_t arrival_ns);
  void resync_(ResyncReason reason, std::int64_t t_device_ns, std::int64_t delta_ns,
               std::int64_t arrival_ns, std::int64_t step_ns);
  void clear_model_();
  void close_window_(const Envelope& e);
  void refit_();
  void push_residual_(std::int64_t r);
  void refresh_jitter_();
  void refresh_cache_();
  std::int64_t line_at_(std::int64_t t_device_ns) const;
  const Envelope& hull_at_(std::size_t i) const;

  MinDelayConfig cfg_;
  mutable std::mutex m_;

  // Lower envelope: one point per closed window, circular.
  std::vector<Envelope> hull_;
  std::size_t hull_head_ = 0;
  std::size_t hull_size_ = 0;

  // Open window.
  bool have_window_ = false;
  std::int64_t win_start_device_ = 0;
  Envelope win_min_{};

  // Fitted line, kept in relative coordinates so that a device epoch of 0 and
  // an engine epoch of ~1e15 ns cannot cost us precision in a double.
  bool have_fit_ = false;
  std::int64_t fit_anchor_ns_ = 0;   // device time the fit is anchored at
  std::int64_t fit_base_ns_ = 0;     // integer part of delta at the anchor
  double fit_corr_ns_ = 0.0;         // fractional/derived part of delta at anchor
  double fit_slope_ = 0.0;           // d(delta)/d(device time), dimensionless

  // Fit scratch, sized once in the constructor: refit_() runs on the driver
  // thread and must not allocate.
  std::vector<double> fit_t_;
  std::vector<double> fit_d_;
  std::vector<double> fit_resid_;
  std::vector<double> fit_sorted_;
  std::vector<unsigned char> fit_keep_;

  // Residuals, circular.
  std::vector<std::int64_t> resid_;
  std::size_t resid_head_ = 0;
  std::size_t resid_size_ = 0;
  std::uint32_t since_jitter_ = 0;
  std::int64_t jitter_ns_ = 0;
  std::vector<std::int64_t> scratch_;  // sort buffer, never allocated on the hot path

  bool have_ = false;
  std::int64_t last_device_ns_ = 0;
  std::int64_t last_arrival_ns_ = 0;
  std::uint32_t pending_step_ = 0;

  Diagnostics diag_{};
  OffsetEstimate cache_{};
  StreamId stream_ = StreamId::kUnknown;
  ClockResyncFn observer_ = nullptr;
  void* observer_user_ = nullptr;
};

}  // namespace scanengine

#endif  // SCANENGINE_TIMESYNC_MIN_DELAY_ESTIMATOR_H
