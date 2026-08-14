// offset_estimator.h — per-stream device→engine clock mapping.
//
// Tech Spec §3.2: "Arrival-stamping + per-stream device-timestamp offset
// with drift-tracked median filter." Every stream that carries its own
// device timestamp (Mid-360 points and IMU, GNSS NMEA time, camera frames
// via ARCore) gets one estimator; the D6 has no device clock at all and maps
// straight through arrival stamps.
//
// Why this is an interface in A1: colorization error is dominated by sync
// jitter (S6: at 15 ms jitter, sync alone eats 83% of the reprojection
// budget), so A4 and A11 will iterate on the estimator. Everything upstream
// only ever calls to_engine_time().
//
// A4 filled this in. The shipped estimator is MinDelayOffsetEstimator
// (timesync/min_delay_estimator.h) — a windowed minimum-delay filter with a
// drift fit, installed by default on every stream that has a device clock.
// The three things a consumer must know:
//
//   1. Map times with TimeSync::to_engine_time(stream, t) for one sample, or
//      take a TimeSync::model(stream) snapshot and call TimeModel::apply()
//      for a whole batch — apply() is pure arithmetic and takes no lock.
//   2. jitter_ns is a measurement, not a promise, and it is only meaningful
//      once `converged` is true. Gate on sync_quality(), never on jitter
//      alone. See kJitterBudget*Ns below for what the numbers mean.
//   3. A device that reboots mid-session produces a clock discontinuity; the
//      estimator detects it, rebuilds, drops `converged`, and reports it as
//      a ClockResync (published as EventType::kError by TimeSync). Samples
//      mapped while `converged` is false must be treated as unsynchronised.
//
// Owner: A1 (interface + passthrough) / A4 (real estimator, IMU ingestion) /
// A11 (constant clock-offset estimation in the calibration wizard).
#ifndef SCANENGINE_TIMESYNC_OFFSET_ESTIMATOR_H
#define SCANENGINE_TIMESYNC_OFFSET_ESTIMATOR_H

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/core/types.h"
#include "scanengine/timesync/clock.h"

namespace scanengine {

class EventBus;      // core/event_bus.h — only the .cpp needs the definition
struct ClockResync;  // timesync/min_delay_estimator.h

// --- what the S6 error budget says these numbers cost ---------------------
//
// Median colorization error at 3 m for the Mid-360, walking 1 m/s, turning
// 30 °/s, against a 20.2 px budget (S6 REPORT §1, §6):
//
//     5 ms jitter → 15.4 px   PASS
//    15 ms jitter → 22.0 px   FAIL unless keyframes are motion-gated < 15 °/s
//    30 ms jitter → 36.0 px   FAIL
//
// Time sync × turn rate is the single dominant term in that budget (83 % of
// it at 15 ms), which is why this file reports a real uncertainty.
inline constexpr std::int64_t kJitterBudgetGoodNs = 5'000'000;
inline constexpr std::int64_t kJitterBudgetGatedNs = 15'000'000;

// Reported as the mapping uncertainty when an estimator has no evidence yet.
// Deliberately the pessimistic end of the spec's 5–30 ms range: an A11 gate
// that reads it before convergence must fail closed.
inline constexpr std::int64_t kUnconvergedUncertaintyNs = 30'000'000;

struct OffsetEstimate {
  std::int64_t offset_ns = 0;   // engine_time = device_time + offset_ns
  double drift_ppm = 0.0;       // device clock rate error, parts per million
  std::int64_t jitter_ns = 0;   // spread of the residuals; A11 budgets on this
  std::uint64_t samples = 0;
  bool valid = false;

  // --- A4 ---------------------------------------------------------------
  // jitter_ns and drift_ppm may only be believed when this is true. Before
  // it, the offset is usable (it is seeded from the newest arrival) but its
  // uncertainty is unknown — see kUnconvergedUncertaintyNs.
  bool converged = false;
  std::int64_t t_ref_device_ns = 0;  // device time the offset is anchored at
  std::int64_t t_updated_ns = 0;     // arrival stamp of the last accepted pair
  std::uint32_t resyncs = 0;         // device clock discontinuities so far
};

// A snapshot of the mapping, safe to copy out once and apply to a whole
// batch of points without touching a lock. This is the accessor A3's
// Mid-360 receive thread should use for a datagram's worth of points.
struct TimeModel {
  std::int64_t offset_ns = 0;
  double drift_ppm = 0.0;
  std::int64_t t_ref_device_ns = 0;
  std::int64_t uncertainty_ns = kUnconvergedUncertaintyNs;
  bool converged = false;
  bool valid = false;

  std::int64_t apply(std::int64_t t_device_ns) const {
    if (!valid) return t_device_ns;
    const double dt = static_cast<double>(t_device_ns - t_ref_device_ns);
    return t_device_ns + offset_ns +
           static_cast<std::int64_t>(std::llround(dt * drift_ppm * 1e-6));
  }
};

// The gate A11 should use. Never classify on jitter alone: an estimator with
// two samples can report a tiny spread and be badly wrong.
enum class SyncQuality : std::uint8_t {
  kUnknown = 0,  // no estimate, or not converged — treat as unsynchronised
  kGood = 1,     // ≤ 5 ms: S6 says colorize (15.4 px at 3 m)
  kGated = 2,    // ≤ 15 ms: colorize only with motion-gated keyframes (22.0 px)
  kPoor = 3,     // > 15 ms: do not colorize
};

const char* to_string(SyncQuality q) noexcept;

inline SyncQuality sync_quality(const OffsetEstimate& e) noexcept {
  if (!e.valid || !e.converged) return SyncQuality::kUnknown;
  if (e.jitter_ns <= kJitterBudgetGoodNs) return SyncQuality::kGood;
  if (e.jitter_ns <= kJitterBudgetGatedNs) return SyncQuality::kGated;
  return SyncQuality::kPoor;
}

class OffsetEstimator {
 public:
  virtual ~OffsetEstimator() = default;

  // One (device timestamp, host arrival timestamp) pair. Called on the
  // driver thread for every packet — must be O(1) amortized.
  virtual void add_pair(std::int64_t t_device_ns, TimePoint t_arrival) = 0;

  virtual OffsetEstimate estimate() const = 0;
  virtual void reset() = 0;

  // Snapshot for batch mapping. The default builds one from estimate();
  // estimators that track drift override it.
  virtual TimeModel model() const;

  std::int64_t to_engine_time(std::int64_t t_device_ns) const {
    return model().apply(t_device_ns);
  }
};

// Offset = (last arrival − last device stamp). Correct for a stream whose
// device clock is already the engine clock (ARCore frames), and the right
// answer for a stream with no device clock at all (the D6, whose "device
// timestamp" is its arrival stamp, so the offset is 0 by construction).
// Everything with a real, independent device clock gets the min-delay
// estimator instead — see TimeSync::make_default_estimator.
class PassthroughOffsetEstimator final : public OffsetEstimator {
 public:
  void add_pair(std::int64_t t_device_ns, TimePoint t_arrival) override;
  OffsetEstimate estimate() const override;
  void reset() override;

 private:
  mutable std::mutex m_;
  OffsetEstimate est_{};
};

// Registry: one estimator per stream, owned by the Engine and handed to
// drivers through DriverContext.
//
// Threading: every method is safe from any thread. add_pair()/estimator()
// serialize on one registry mutex, so a hot path that pushes hundreds of
// pairs per second should hold the OffsetEstimator& from estimator() rather
// than going through the registry each time. Clock-discontinuity events are
// published from inside add_pair(); a callback-mode bus subscriber must not
// re-enter TimeSync from them (the general event-bus rule, DESIGN §2).
class TimeSync {
 public:
  TimeSync();
  ~TimeSync();

  // Registers make_default_estimator(stream) on first use.
  OffsetEstimator& estimator(StreamId stream);
  Status set_estimator(StreamId stream, std::unique_ptr<OffsetEstimator> est);
  OffsetEstimate estimate(StreamId stream) const;
  void reset_all();

  // --- A4 additions -----------------------------------------------------

  // Feed one pair. Convenience over estimator(stream).add_pair().
  void add_pair(StreamId stream, std::int64_t t_device_ns, TimePoint t_arrival);

  // Map one device timestamp. Returns t_device_ns unchanged for a stream
  // with no estimate yet — never a wild value.
  std::int64_t to_engine_time(StreamId stream, std::int64_t t_device_ns) const;

  // Batch-friendly snapshot; TimeModel::apply() takes no lock.
  TimeModel model(StreamId stream) const;

  SyncQuality quality(StreamId stream) const;

  Status reset(StreamId stream);
  std::vector<StreamId> streams() const;

  // Publishing seam. When a bus is installed, a device-clock discontinuity
  // (reboot or confirmed step — not a routine stream gap) is logged at WARN
  // and published as EventType::kError with ScanError::kProtocolError and
  // the offending stream, so the app and A5's recorder both see it.
  void set_event_bus(EventBus* bus, DeviceId device = kInvalidDeviceId);

  // Direct observer, for tests and for A5's .lscan sync chunk.
  void set_resync_observer(void (*fn)(const ClockResync&, void*), void* user_data);

  std::uint32_t resyncs() const;  // total across all streams

  // Streams with an independent device clock get the min-delay estimator;
  // the rest get passthrough. Public so a driver or a test can build the
  // same thing explicitly.
  static std::unique_ptr<OffsetEstimator> make_default_estimator(StreamId stream);
  static bool stream_has_device_clock(StreamId stream) noexcept;

  const char* clock_backend() const { return SteadyClock::backend_name(); }

 private:
  struct Hook;

  void attach_hook_(StreamId stream, OffsetEstimator& est);
  static void on_resync_(const ClockResync& ev, void* user_data);

  mutable std::mutex m_;
  std::map<StreamId, std::unique_ptr<OffsetEstimator>> estimators_;
  std::map<StreamId, std::unique_ptr<Hook>> hooks_;

  // Separate lock: the resync handler runs from inside an estimator's
  // add_pair(), which may already be under m_.
  mutable std::mutex sink_m_;
  EventBus* bus_ = nullptr;
  DeviceId device_ = kInvalidDeviceId;
  void (*observer_)(const ClockResync&, void*) = nullptr;
  void* observer_user_ = nullptr;
  std::uint32_t resyncs_ = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_TIMESYNC_OFFSET_ESTIMATOR_H
