// mid360_driver.h — Livox Mid-360 driver.
//
// STUB / INTERFACE SEAM. Owner: A3 (Mid-360 driver productionization).
//
// A1 fixes only the shape: a Driver like any other, so the Engine, the C
// ABI, the event vocabulary and the PageStore need no Mid-360-specific code
// paths. What A3 inherits from the S2-sim spike (spikes/s2-mid360-sim/):
//
//   • Vendor SDK2 is VENDORED AND PATCHED, pinned, 3 patches; stock SDK2
//     cannot bind on macOS. Do not add livox-sdk2 as a plain vcpkg port.
//   • macOS requires an explicit lidar IP; there is no broadcast discovery.
//   • Packet loss must be tracked with the FREE-RUNNING udp_cnt model
//     (frame_cnt stays 0 on real devices — verified against Livox's own
//     .lvx2 sample, contrary to the published protocol table).
//   • The soak target that S2-sim met in loopback: 200k pts/s for 10 min
//     with no packet-loss growth, IMU at 200 Hz.
//   • IMU samples are a separate stream (StreamId::kImu) and go to A4's
//     ingestion path + the .lscan imu.bin chunk — NOT into the PageStore.
//
// Points must land in the PageStore in the session's local metric frame,
// batched (S3 measured 200k pts/s ingest at 0.11 ms p95 with ~1 M-point
// pages), with EventType::kPointsAvailable emitted by the Engine's page
// subscriber exactly as for D6.
#ifndef SCANENGINE_DRIVERS_MID360_MID360_DRIVER_H
#define SCANENGINE_DRIVERS_MID360_MID360_DRIVER_H

#include "scanengine/drivers/driver.h"
#include "scanengine/transport/udp_source.h"

namespace scanengine {

struct Mid360Config {
  UdpConfig udp{};

  // Live decimation budget (Tech Spec §3.3: ~40k pts/s into live LIO out of
  // the sensor's 200k). 0 = no decimation (post-processing / replay).
  std::uint32_t live_points_per_sec = 40000;

  bool publish_imu = true;
  std::uint32_t max_batch_points = 8192;
};

class Mid360Driver final : public Driver {
 public:
  Mid360Driver(DeviceId id, const Mid360Config& cfg, const DriverContext& ctx);
  ~Mid360Driver() override;

  const char* name() const override { return "mid360"; }
  DeviceKind kind() const override { return DeviceKind::kMid360; }
  DeviceId id() const override { return id_; }

  Status start() override;  // A3
  Status stop() override;   // A3
  DeviceState state() const override { return state_; }
  DeviceHealth health() const override;
  Status push_bytes(ByteSpan bytes, TimePoint t_arrival) override;  // not applicable

 private:
  DeviceId id_;
  Mid360Config cfg_;
  DriverContext ctx_;
  DeviceState state_ = DeviceState::kDisconnected;
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_MID360_MID360_DRIVER_H
