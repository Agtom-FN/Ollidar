// Two SDK-free backends for the Mid-70: listen-only raw UDP, and injection.
//
// Neither can bring a device up — SDK v1's handshake is what tells the
// lidar where to stream. What they buy is that the decode / gap-loss /
// filter / watchdog / reconnect logic is exercisable with no SDK checkout,
// which is every CI leg.
#include <memory>

#include "mid70_backend.h"
#include "scanengine/core/log.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "mid70";

class RawUdpBackend final : public Mid70BackendImpl {
 public:
  RawUdpBackend(Mid70Driver& driver, DeviceId id, const Mid70Config& cfg)
      : driver_(driver), id_(id), cfg_(cfg) {}
  ~RawUdpBackend() override { close(); }

  const char* backend_name() const override { return "raw-udp"; }

  Status open() override {
    close();
    // ONE socket: the Mid-70 has no IMU stream, so the Android two-descriptor
    // seam the Mid-360 backend needs does not apply here.
    UdpConfig point_cfg = cfg_.udp;
    point_cfg.bind_port = cfg_.udp.bind_port != 0 ? cfg_.udp.bind_port : cfg_.udp.host_point_port;
    point_cfg.prebound_imu_fd = -1;
    point_ = std::make_unique<UdpSource>(point_cfg);
    point_->set_sink([this](ByteSpan d, TimePoint t) {
      driver_.on_point_packet(d.data(), d.size(), t);
    });
    SCAN_TRY(point_->start());
    driver_.on_device_connected(cfg_.broadcast_code.c_str(), cfg_.udp.lidar_ip.c_str(), "");
    SCAN_LOG_INFO(kMod, "device %u: raw-UDP backend listening on %u", id_,
                  static_cast<unsigned>(point_cfg.bind_port));
    return kOkStatus;
  }

  void close() override {
    if (point_) {
      (void)point_->stop();
      point_.reset();
    }
  }

 private:
  Mid70Driver& driver_;
  DeviceId id_;
  Mid70Config cfg_;
  std::unique_ptr<UdpSource> point_;
};

class InjectBackend final : public Mid70BackendImpl {
 public:
  InjectBackend(Mid70Driver& driver, DeviceId id) : driver_(driver), id_(id) {}
  const char* backend_name() const override { return "inject"; }
  Status open() override {
    ++opens_;
    // A replay has no handshake; "connected" is the moment the backend opens,
    // so a replayed session shows a device row like a live one would.
    driver_.on_device_connected("", "", "");
    SCAN_LOG_DEBUG(kMod, "device %u: inject backend open (#%u)", id_, opens_);
    return kOkStatus;
  }
  void close() override {}

 private:
  Mid70Driver& driver_;
  DeviceId id_;
  std::uint32_t opens_ = 0;
};

}  // namespace

std::unique_ptr<Mid70BackendImpl> make_raw_udp_backend(Mid70Driver& driver, DeviceId id,
                                                       const Mid70Config& cfg) {
  return std::make_unique<RawUdpBackend>(driver, id, cfg);
}

std::unique_ptr<Mid70BackendImpl> make_inject_backend(Mid70Driver& driver, DeviceId id) {
  return std::make_unique<InjectBackend>(driver, id);
}

}  // namespace scanengine
