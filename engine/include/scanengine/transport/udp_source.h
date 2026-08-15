// udp_source.h — Ethernet datagram transport for the Livox Mid-360.
//
// Owner: A3.
//
// SCOPE, AND WHY IT IS SMALL. On the production path SDK2 owns the sockets:
// it has to, because discovery, the 0x0100 host-IP configuration push and
// the heartbeat all live in its command state machine, and re-implementing
// that to save one class would be trading a known-good handshake for an
// unknown one. UdpSource is therefore NOT the Mid-360's main transport. It
// is a real, self-contained datagram source used for three things the SDK
// cannot cover:
//
//   • Mid360Backend::kRawUdp — a listen-only ingest path for a device that
//     is ALREADY configured to stream at this host, and for replay/injection
//     harnesses. This is what the unit tests drive, which is what makes the
//     driver's decode/filter/loss/watchdog logic testable with no SDK.
//   • A place to own receive-buffer sizing. S2 §8 flags NIC behaviour as the
//     single biggest untested risk: loopback has no MTU, no driver ring and
//     no interrupt coalescing, so `recv_buffer_bytes` (SO_RCVBUF) and the
//     actual granted size reported in TransportStats are the first things to
//     look at when real hardware starts dropping.
//   • The Android seam: the app binds the socket to the USB-Ethernet
//     Network object (ConnectivityManager TRANSPORT_ETHERNET +
//     Network.bindSocket) and hands the bound descriptor down, because the
//     engine cannot reach ConnectivityManager. That is `prebound_fd`.
//
// Findings from the S2-sim spike that constrain everything here:
//   • Stock Livox SDK2 does NOT run on macOS: its broadcast bind fails with
//     EADDRNOTAVAIL. The engine vendors a pinned, 3-patch SDK2
//     (engine/third_party/fetch_sdk2.sh). macOS therefore needs an EXPLICIT
//     lidar IP — there is no broadcast discovery path on that OS, and
//     explicit IP is the default everywhere.
//   • Addressing: lidar on 192.168.1.1xx, host static on 192.168.1.x.
//   • Loss detection uses the FREE-RUNNING udp_cnt model; it lives in
//     drivers/mid360/mid360_packets.h, not here, because it is protocol and
//     not transport.
#ifndef SCANENGINE_TRANSPORT_UDP_SOURCE_H
#define SCANENGINE_TRANSPORT_UDP_SOURCE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "scanengine/transport/byte_source.h"

namespace scanengine {

struct UdpConfig {
  std::string host_ip;    // e.g. "192.168.1.5" — static, set by the app wizard
  std::string lidar_ip;   // e.g. "192.168.1.100" — REQUIRED on macOS

  // Device-side ports (where the lidar listens / sends from).
  std::uint16_t point_port = 56300;
  std::uint16_t imu_port = 56400;
  std::uint16_t cmd_port = 56100;
  std::uint16_t push_port = 56200;
  std::uint16_t log_port = 56500;

  // Host-side ports the device is told to stream to. SDK2's config file
  // wants all five; the +1 convention matches both Livox's own samples and
  // the S2 spike's loopback config, and keeps host and device ports visibly
  // distinct in a packet capture.
  std::uint16_t host_cmd_port = 56101;
  std::uint16_t host_push_port = 56201;
  std::uint16_t host_point_port = 56301;
  std::uint16_t host_imu_port = 56401;
  std::uint16_t host_log_port = 56501;

  // Which host port THIS UdpSource binds. 0 = host_point_port.
  std::uint16_t bind_port = 0;

  // SO_RCVBUF request. A Mid-360 is ~23 Mbit/s of 1380-byte datagrams; the
  // default socket buffer on several platforms is 64–256 KB, which is under
  // 100 ms of headroom and is the first thing to overflow when a big-core
  // scheduling hiccup stalls the receive thread. 4 MB ≈ 1.4 s of slack.
  // The kernel may grant less: TransportStats reports what it actually got.
  int recv_buffer_bytes = 4 * 1024 * 1024;

  // Android: a socket already bound to the USB-Ethernet Network, or -1.
  // When set, UdpSource does not create or bind a socket, and does not close
  // the descriptor on stop() — the app owns it.
  //
  // ONE UdpSource, ONE descriptor: this is the socket THIS source receives on,
  // whichever port it is. `bind_port` is ignored when it is set (the app
  // already chose the port when it bound).
  int prebound_fd = -1;

  // The SECOND half of the Android seam (android/NOTES.md §8, B3 finding 2).
  //
  // `Mid360Backend::kRawUdp` opens TWO sources — points and IMU — from ONE
  // UdpConfig, so before this field a pre-bound capture could only be
  // point-only: a single descriptor recvfrom()'d by two receive threads has
  // them steal each other's datagrams, silently and at random. B3 worked
  // around it by setting `publish_imu = false` and saying so in the UI.
  //
  // `RawUdpBackend` gives this one to the IMU source and `prebound_fd` to the
  // point source. It is NOT read by UdpSource itself — a source has exactly
  // one socket — which is why it can live here without making the
  // one-source-one-fd rule above ambiguous.
  //
  // Never-close applies to BOTH: whichever source receives a pre-bound
  // descriptor leaves it open at stop(), because the app owns it and will
  // close it after the engine has torn down (order matters: no receive thread
  // may still be inside recvfrom on it).
  int prebound_imu_fd = -1;
};

// Receive-side statistics beyond the generic TransportStats.
struct UdpSourceStats {
  std::uint64_t datagrams = 0;
  std::uint64_t bytes = 0;
  std::uint64_t recv_errors = 0;
  std::uint64_t oversize = 0;      // datagram larger than the receive buffer
  int recv_buffer_granted = 0;     // SO_RCVBUF actually in effect
  std::uint16_t bound_port = 0;
};

// One bound UDP port, one receive thread, datagrams handed to a sink.
//
// Threading: start() spawns the receive thread; the sink is invoked on it,
// with no lock held, exactly once per datagram, with a buffer valid only for
// the duration of the call. stop() joins. The sink must be installed before
// start() and must not be changed while running.
class UdpSource final : public PacketSource {
 public:
  explicit UdpSource(const UdpConfig& cfg);
  ~UdpSource() override;

  UdpSource(const UdpSource&) = delete;
  UdpSource& operator=(const UdpSource&) = delete;

  const char* name() const override { return "udp"; }
  Status start() override;
  Status stop() override;
  bool running() const override { return running_.load(std::memory_order_acquire); }
  void set_sink(PacketSink sink) override { sink_ = std::move(sink); }

  // Host → device datagram, sent to lidar_ip:cmd_port. Present so a future
  // non-SDK control path has a place to live; the SDK2 backend never uses it.
  Status send(ByteSpan datagram) override;

  TransportStats stats() const override;
  UdpSourceStats udp_stats() const;

  const UdpConfig& config() const { return cfg_; }
  std::uint16_t bound_port() const;

 private:
  void receive_loop();

  UdpConfig cfg_;
  PacketSink sink_;

  mutable std::mutex m_;
  TransportStats stats_{};
  UdpSourceStats udp_stats_{};

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  int fd_ = -1;
  bool owns_fd_ = false;
  std::thread rx_;
  std::vector<std::uint8_t> rx_buf_;
};

}  // namespace scanengine

#endif  // SCANENGINE_TRANSPORT_UDP_SOURCE_H
