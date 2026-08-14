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
// Owner: A1 (interface + passthrough) / A4 (real estimator, IMU ingestion) /
// A11 (constant clock-offset estimation in the calibration wizard).
#ifndef SCANENGINE_TIMESYNC_OFFSET_ESTIMATOR_H
#define SCANENGINE_TIMESYNC_OFFSET_ESTIMATOR_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "scanengine/core/error.h"
#include "scanengine/core/types.h"
#include "scanengine/timesync/clock.h"

namespace scanengine {

struct OffsetEstimate {
  std::int64_t offset_ns = 0;   // engine_time = device_time + offset_ns
  double drift_ppm = 0.0;       // device clock rate error, parts per million
  std::int64_t jitter_ns = 0;   // spread of the residuals; A11 budgets on this
  std::uint64_t samples = 0;
  bool valid = false;
};

class OffsetEstimator {
 public:
  virtual ~OffsetEstimator() = default;

  // One (device timestamp, host arrival timestamp) pair. Called on the
  // driver thread for every packet — must be O(1) amortized.
  virtual void add_pair(std::int64_t t_device_ns, TimePoint t_arrival) = 0;

  virtual OffsetEstimate estimate() const = 0;
  virtual void reset() = 0;

  std::int64_t to_engine_time(std::int64_t t_device_ns) const {
    return t_device_ns + estimate().offset_ns;
  }
};

// Offset = (last arrival − last device stamp). Correct for a stream whose
// device clock is already the engine clock (ARCore frames), and a usable
// placeholder everywhere else until A4 lands the drift-tracked median.
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
class TimeSync {
 public:
  TimeSync();
  ~TimeSync();

  // Registers a PassthroughOffsetEstimator on first use. A4 swaps the
  // factory here, and nothing upstream changes.
  OffsetEstimator& estimator(StreamId stream);
  Status set_estimator(StreamId stream, std::unique_ptr<OffsetEstimator> est);
  OffsetEstimate estimate(StreamId stream) const;
  void reset_all();

  const char* clock_backend() const { return SteadyClock::backend_name(); }

 private:
  mutable std::mutex m_;
  std::map<StreamId, std::unique_ptr<OffsetEstimator>> estimators_;
};

}  // namespace scanengine

#endif  // SCANENGINE_TIMESYNC_OFFSET_ESTIMATOR_H
