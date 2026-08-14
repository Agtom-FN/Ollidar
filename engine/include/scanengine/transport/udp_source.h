// udp_source.h — Ethernet datagram transport for the Livox Mid-360.
//
// STUB. Owner: A3 (Mid-360 driver productionization).
//
// What A3 must honour here, from the S2-sim spike (spikes/s2-mid360-sim/):
//   • Stock Livox SDK2 does NOT run on macOS: its broadcast bind fails with
//     EADDRNOTAVAIL. The engine vendors a pinned, 3-patch SDK2 (patches live
//     in spikes/s2-mid360-sim/patches/). macOS therefore needs an EXPLICIT
//     lidar IP — there is no broadcast discovery path on that OS.
//   • Addressing: lidar on 192.168.1.1xx, host static on 192.168.1.x.
//   • Loss detection uses the FREE-RUNNING udp_cnt model: real devices
//     free-run udp_cnt and leave frame_cnt at 0, verified against Livox's
//     own .lvx2 sample. Do not implement the published table's model.
//   • Android binds the socket to the USB-Ethernet Network object
//     (ConnectivityManager TRANSPORT_ETHERNET + Network.bindSocket) — the
//     bind happens app-side and the bound fd is handed down, which is why
//     UdpConfig carries an optional pre-bound descriptor.
//
// Whether A3 implements this class directly or lets SDK2 own the sockets and
// only reports through it is A3's call; the seam exists so drivers/mid360
// has one place to be told "here is your transport" and so replay can
// substitute a file-backed packet source.
#ifndef SCANENGINE_TRANSPORT_UDP_SOURCE_H
#define SCANENGINE_TRANSPORT_UDP_SOURCE_H

#include <cstdint>
#include <string>

#include "scanengine/transport/byte_source.h"

namespace scanengine {

struct UdpConfig {
  std::string host_ip;    // e.g. "192.168.1.5" — static, set by the app wizard
  std::string lidar_ip;   // e.g. "192.168.1.100" — REQUIRED on macOS
  std::uint16_t point_port = 56300;
  std::uint16_t imu_port = 56400;
  std::uint16_t cmd_port = 56100;

  // Android: a socket already bound to the USB-Ethernet Network, or -1.
  int prebound_fd = -1;
};

class UdpSource final : public PacketSource {
 public:
  explicit UdpSource(const UdpConfig& cfg) : cfg_(cfg) {}
  ~UdpSource() override = default;

  const char* name() const override { return "udp"; }
  Status start() override;   // A3
  Status stop() override;    // A3
  bool running() const override { return false; }
  void set_sink(PacketSink sink) override { sink_ = std::move(sink); }
  Status send(ByteSpan datagram) override;  // A3
  TransportStats stats() const override { return stats_; }

  const UdpConfig& config() const { return cfg_; }

 private:
  UdpConfig cfg_;
  PacketSink sink_;
  TransportStats stats_{};
};

}  // namespace scanengine

#endif  // SCANENGINE_TRANSPORT_UDP_SOURCE_H
