// d6_driver.h — COIN-D6 driver: S1's parser behind the Driver interface.
//
// The parser itself (d6_parser.h/.cpp, commands.h) is the finished S1 spike
// artefact, copied in unmodified except for include paths; the S1 tree stays
// as the historical record. This file is the engine integration: transport
// in, ACK handling, health model, points into the PageStore, status onto the
// EventBus.
//
// Coordinate frame produced here is the SENSOR frame of a single 2D sweep:
//   x = d·sin(θ), y = d·cos(θ), z = 0   (θ = 0 is +y, matching the S1 tools)
// The D6 is mounted VERTICALLY and the 3-D cloud comes from sweeping that
// profile along a trajectory — that assembly is A8 (pushbroom), which will
// replace this driver's page writes with trajectory-transformed points.
// Until then these points are exactly what the S1 polar plot showed, which
// is what makes the live "is the sensor seeing anything" view work in M1.
//
// Owner: A1 (this integration) / A2 (reconnect, speed-adjust filtering,
// fault states) / A8 (pushbroom assembly replaces the point transform).
#ifndef SCANENGINE_DRIVERS_D6_D6_DRIVER_H
#define SCANENGINE_DRIVERS_D6_D6_DRIVER_H

#include <memory>
#include <mutex>
#include <vector>

#include "scanengine/drivers/d6/d6_parser.h"
#include "scanengine/drivers/driver.h"
#include "scanengine/transport/usb_serial_source.h"

namespace scanengine {

struct D6Config {
  UsbSerialConfig serial{};
  d6::Config parser{};

  // Send AA 55 F0 0F on start() / AA 55 F5 0A on stop() when the transport
  // has a write function. Off when the app drives the device itself.
  bool send_start_stop_commands = true;

  // Wait for the start ACK before declaring kStreaming. When false (or when
  // no write function exists) the first decoded packet promotes the state.
  bool require_start_ack = false;

  // Health thresholds. The S1 exit criterion is a checksum pass rate above
  // 99.5%; below it the device is kDegraded, not kFault — data is still
  // usable and A2 owns the recovery policy.
  double min_checksum_pass_rate = 0.995;
  std::uint64_t health_min_packets = 200;  // before rating pass rate

  // Points are batched into the PageStore rather than appended one at a
  // time: one 40-sample packet per append is ~640 B, which is the granularity
  // the S3 renderer measured at 0.11 ms p95 for 200k pts/s.
  std::uint32_t max_batch_points = 4096;

  bool drop_zero_range_points = true;  // no-return samples are not geometry
};

class D6Driver final : public Driver {
 public:
  D6Driver(DeviceId id, const D6Config& cfg, const DriverContext& ctx);
  ~D6Driver() override;

  const char* name() const override { return "d6"; }
  DeviceKind kind() const override { return DeviceKind::kD6; }
  DeviceId id() const override { return id_; }

  Status start() override;
  Status stop() override;
  DeviceState state() const override;
  DeviceHealth health() const override;
  Status push_bytes(ByteSpan bytes, TimePoint t_arrival) override;

  // Diagnostics used by tests and engine_cli --replay.
  d6::Stats parser_stats() const;
  UsbSerialSource& transport() { return *serial_; }

 private:
  void on_bytes(ByteSpan bytes, TimePoint t);
  void on_point(const d6::Point& p);
  void flush_batch(std::int64_t t_ns);
  void set_state(DeviceState next, ScanError err);
  void scan_for_acks(ByteSpan bytes);

  DeviceId id_;
  D6Config cfg_;
  DriverContext ctx_;

  std::unique_ptr<UsbSerialSource> serial_;
  d6::Parser parser_;

  mutable std::mutex m_;
  DeviceState state_ = DeviceState::kDisconnected;
  ScanError last_error_ = ScanError::kOk;
  std::vector<PointVertex> batch_;
  std::int64_t t_current_ns_ = 0;
  std::uint64_t points_out_ = 0;
  std::uint64_t rotations_seen_ = 0;
  std::uint32_t points_in_rotation_ = 0;
  std::uint64_t drops_ = 0;
  bool saw_start_ack_ = false;
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_D6_D6_DRIVER_H
