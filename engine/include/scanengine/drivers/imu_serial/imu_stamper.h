// imu_stamper.h — arrival stamps → de-bursted sample stamps (A18).
//
// The JuxiTech module puts no timestamp and no sample counter on the wire, so
// the only clock available is the host's arrival time — and arrivals come in
// DECODE BURSTS: several samples' bytes sit in an OS buffer and are handed to
// the engine in one chunk, so their arrivals are identical or near-identical
// and the burst boundary shows an n-period hole. Published raw, that stream
// describes several milliseconds of motion happening in microseconds and then
// nothing for ~90 ms. LIO integrates those dt values directly.
//
// This is a line-for-line port of the ROS driver's `_stamp()`
// (IMU_ROS1/scripts/imu_driver_fastlio.py, MID70-FIX v1.2.3 / v1.3.2), whose
// behaviour was validated in simulation. The model: keep an estimate that
// advances by one MEASURED period per sample and is pulled gently toward each
// arrival, with three escapes —
//
//   arrival < previous arrival   the host clock stepped backwards (NTP, VM
//                                resume, suspend). No honest stamp exists:
//                                free-run at the measured period until the
//                                arrivals catch up, and re-anchor the period
//                                estimator, whose origin is on a timeline
//                                that no longer exists. The test is a
//                                backwards ARRIVAL, never a large negative
//                                err — inside a burst the estimate leads the
//                                arrivals by most of the burst by design.
//
//   err > max_gap                a hole far bigger than one period: samples
//                                were genuinely lost. Snap fully to arrival
//                                rather than smearing the hole across later
//                                samples. Only a hole past `period_reanchor`
//                                also restarts the period estimate: a burst n
//                                deep leaves an n-period hole at EVERY burst
//                                boundary, and restarting there would measure
//                                only within-burst spacing and underestimate
//                                the period by the burst depth.
//
//   otherwise                    est += phase_gain·err, and fold the arrival
//                                into the period estimate.
//
// Why the period is measured rather than taken from the configured rate: the
// module's actual output rate is not the rate you asked for (ROS measurement:
// asked 80 Hz, delivered ~86 Hz). Stepping the model by 1/report_rate drifted
// the IMU timeline against the lidar by ~2500 ppm, which mis-associates
// samples with scans outright. The measured-period model with phase_gain=0.02
// measured -3 ppm.
//
// The `clamps` counter is a bug detector, not a feature: it should be 0 in
// every scenario. It advances by one PERIOD (not 1 µs, which was
// self-perpetuating and published a nominal-rate stream of 1 µs dt values).
//
// Owner: A18 (serial IMU).
#ifndef SCANENGINE_DRIVERS_IMU_SERIAL_IMU_STAMPER_H
#define SCANENGINE_DRIVERS_IMU_SERIAL_IMU_STAMPER_H

#include <cstdint>

namespace scanengine {

struct ImuStamperConfig {
  // Seeds the period estimate only; it is replaced by the measured value once
  // `period_min_n` samples have been seen.
  double nominal_hz = 100.0;

  // A hole wider than this is lost data, not burst structure.
  double max_gap_ms = 25.0;

  // Diagnostic threshold only: the estimate leading the arrivals by more than
  // this means the decode bursts are deeper than that. Not a fault — it is
  // exactly what this class exists to smooth — but it is the number that says
  // how bursty the link really is.
  double max_lead_ms = 50.0;

  // A hole past this cannot be burst structure, so the period estimator is
  // re-anchored across it.
  double period_reanchor_s = 1.0;

  // Fraction of the arrival error folded in per sample. 0.02 measured best:
  // -3 ppm drift with no burst leakage.
  double phase_gain = 0.02;

  // Minimum samples before a measured period is trusted.
  std::uint32_t period_min_n = 50;
};

struct ImuStamperStats {
  std::uint64_t samples = 0;
  std::uint64_t gap_snaps = 0;         // snapped forward: samples were lost
  std::int64_t worst_gap_ns = 0;
  std::uint64_t clock_step_backs = 0;  // host clock moved backwards
  std::int64_t worst_step_back_ns = 0;
  std::uint64_t deep_bursts = 0;       // est led arrivals by > max_lead_ms
  std::uint64_t clamps = 0;            // monotonicity backstop fired — must be 0
  std::int64_t learned_period_ns = 0;
  bool resyncing = false;              // free-running after a backwards step
};

class ImuStamper {
 public:
  ImuStamper() : ImuStamper(ImuStamperConfig{}) {}
  explicit ImuStamper(const ImuStamperConfig& cfg);

  // Map one arrival instant onto a de-bursted sample stamp. Strictly
  // increasing across calls (the backstop guarantees it, and counts itself
  // when it has to).
  std::int64_t stamp(std::int64_t t_arrival_ns);

  // Forget all timing state, keeping the configuration. Called after a long
  // outage that the caller has already decided is not a gap to smooth — a
  // multi-second silence measured against a stale previous stamp is not a
  // measurement of anything.
  void reset();

  ImuStamperStats stats() const;
  const ImuStamperConfig& config() const { return cfg_; }

 private:
  void restart_period_estimate(double arrival_s);
  void update_period_estimate(double arrival_s);

  ImuStamperConfig cfg_;
  double nominal_dt_ = 0.01;
  double max_gap_s_ = 0.025;
  double max_lead_s_ = 0.05;

  // All internal time is seconds RELATIVE to the first arrival, so a
  // steady-clock value of ~1e15 ns does not eat the double's precision.
  bool have_origin_ = false;
  std::int64_t origin_ns_ = 0;

  double period_ = 0.01;
  double prev_stamp_ = 0.0;
  double prev_arrival_ = 0.0;
  bool started_ = false;
  bool resync_ = false;

  double t_first_ = 0.0;
  std::uint32_t n_since_ = 0;

  ImuStamperStats st_{};
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_IMU_SERIAL_IMU_STAMPER_H
