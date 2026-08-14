// imu_ingest.h — the IMU sample path (task A4).
//
// The Mid-360 emits IMU at 200 Hz on the same device clock as its points but
// on a separate stream (StreamId::kImu), and DESIGN §6 is explicit that IMU
// must NOT go into the PageStore. So it needs somewhere to land, and this is
// it: a lock-guarded ring of the most recent samples, each already carrying
// its engine-monotonic timestamp and the uncertainty of that timestamp.
//
// Three consumers, all of which need the same mapped samples:
//
//   • A6 LIO propagates the ESKF at 200 Hz and must have IMU in the same
//     clock domain as the scan-to-map input, or the propagation window is
//     wrong by the clock offset.
//   • A11 colorization motion-gates keyframe selection on angular rate —
//     S6 §6.3 measures that preferring keyframes below 15 °/s converts the
//     15 ms jitter case from 22.0 px (fail) to 16.2 px (pass), which is the
//     cheapest fix in the whole error budget. angular_rate_at() is that
//     query, and it is why this class keeps the samples rather than just
//     mapping a timestamp and forgetting them.
//   • A5 records mapped IMU into the .lscan imu stream.
//
// Ownership: a driver (A3) constructs one of these next to its receive loop
// and calls add() from that thread; the class does not own a thread and does
// not publish events. It never blocks a producer: the ring overwrites.
//
// Owner: A4.
#ifndef SCANENGINE_TIMESYNC_IMU_INGEST_H
#define SCANENGINE_TIMESYNC_IMU_INGEST_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "scanengine/core/types.h"
#include "scanengine/timesync/clock.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {

inline constexpr float kStandardGravity = 9.80665f;  // m/s² per g

struct ImuSample {
  std::int64_t t_device_ns = 0;
  std::int64_t t_engine_ns = 0;      // mapped through the stream's estimator
  std::int64_t uncertainty_ns = 0;   // how far t_engine_ns may be off
  float gyro_rad_s[3] = {0, 0, 0};
  float accel_m_s2[3] = {0, 0, 0};
  bool time_converged = false;       // false ⇒ treat as unsynchronised
};

struct ImuIngestStats {
  std::uint64_t samples = 0;
  std::uint64_t dropped_out_of_order = 0;  // device stamp went backwards
  std::uint64_t overwritten = 0;           // ring wrapped before a reader took them
  std::int64_t t_first_engine_ns = 0;
  std::int64_t t_last_engine_ns = 0;
  double rate_hz = 0.0;                    // measured over the ring
  std::int64_t uncertainty_ns = kUnconvergedUncertaintyNs;
  bool converged = false;
};

class ImuIngest {
 public:
  // `ts` must outlive this object (the Engine owns the TimeSync).
  explicit ImuIngest(TimeSync& ts, StreamId stream = StreamId::kImu,
                     std::size_t capacity = 2048);

  // Called on the driver's receive thread, once per IMU sample. Feeds the
  // stream's offset estimator with (device stamp, arrival stamp) and returns
  // the sample with its engine time filled in — so a driver that only wants
  // the timestamp can ignore the ring entirely.
  ImuSample add(std::int64_t t_device_ns, TimePoint t_arrival, const float gyro_rad_s[3],
                const float accel_m_s2[3]);

  // Same, for a device that reports acceleration in g. The Mid-360 does (S2
  // measured mean |acc| = 1.0000 g over 120,009 packets), and
  // drivers/mid360/mid360_driver.h states that converting it is A4's
  // business rather than the driver's — so this is where it happens.
  ImuSample add_g(std::int64_t t_device_ns, TimePoint t_arrival, const float gyro_rad_s[3],
                  const float accel_g[3]);

  bool latest(ImuSample* out) const;

  // Most recent `max` samples, oldest first. Returns how many were written.
  std::size_t recent(ImuSample* out, std::size_t max) const;

  // Mean |gyro| over [t_engine_ns − window_ns, t_engine_ns], in rad/s.
  // Returns false when the window holds no samples. This is A11's motion
  // gate: `rate * 180/pi <= 15` is the S6 threshold at 15 ms jitter.
  bool angular_rate_at(std::int64_t t_engine_ns, std::int64_t window_ns,
                       double* rad_per_s) const;

  ImuIngestStats stats() const;
  StreamId stream() const { return stream_; }
  void reset();

 private:
  TimeSync& ts_;
  StreamId stream_;
  mutable std::mutex m_;
  std::vector<ImuSample> ring_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  std::int64_t last_device_ns_ = 0;
  bool have_ = false;
  ImuIngestStats stats_{};
};

}  // namespace scanengine

#endif  // SCANENGINE_TIMESYNC_IMU_INGEST_H
