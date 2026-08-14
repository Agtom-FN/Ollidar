// Two SDK-free backends: listen-only raw UDP, and pure injection.
//
// Neither can bring an out-of-the-box Mid-360 up — that needs the handshake
// only SDK2 implements. What they buy is that every line of the driver's
// decode / filter / loss / watchdog / reconnect logic is exercisable on a
// machine with no SDK checkout, which is all five CI legs.
#include <memory>

#include "mid360_backend.h"
#include "scanengine/core/log.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "mid360";

// Binds the host's point and IMU ports and decodes whatever the (already
// configured) device sends. Two sockets, two threads — one per stream, so a
// 200 Hz IMU never waits behind a 2,083 pkt/s point burst.
class RawUdpBackend final : public Mid360BackendImpl {
 public:
  RawUdpBackend(Mid360Driver& driver, DeviceId id, const Mid360Config& cfg)
      : driver_(driver), id_(id), cfg_(cfg) {}

  ~RawUdpBackend() override { close(); }

  const char* backend_name() const override { return "raw-udp"; }

  Status open() override {
    close();

    UdpConfig point_cfg = cfg_.udp;
    point_cfg.bind_port = cfg_.udp.host_point_port;
    point_ = std::make_unique<UdpSource>(point_cfg);
    point_->set_sink([this](ByteSpan d, TimePoint t) {
      driver_.on_point_packet(d.data(), d.size(), t);
    });
    SCAN_TRY(point_->start());

    if (cfg_.publish_imu) {
      UdpConfig imu_cfg = cfg_.udp;
      imu_cfg.bind_port = cfg_.udp.host_imu_port;
      imu_ = std::make_unique<UdpSource>(imu_cfg);
      imu_->set_sink([this](ByteSpan d, TimePoint t) {
        driver_.on_imu_packet(d.data(), d.size(), t);
      });
      const Status s = imu_->start();
      if (!s.ok()) {
        (void)point_->stop();
        point_.reset();
        imu_.reset();
        return s;
      }
    }

    // There is no handshake here, so "connected" means "the sockets are
    // open"; the driver's watchdog is what decides whether the device is
    // actually talking.
    driver_.on_device_connected("", cfg_.udp.lidar_ip.c_str());
    SCAN_LOG_INFO(kMod, "device %u: raw-UDP backend listening (point %u, imu %u)", id_,
                  static_cast<unsigned>(point_cfg.bind_port),
                  static_cast<unsigned>(cfg_.udp.host_imu_port));
    return kOkStatus;
  }

  void close() override {
    if (point_) {
      (void)point_->stop();
      point_.reset();
    }
    if (imu_) {
      (void)imu_->stop();
      imu_.reset();
    }
  }

 private:
  Mid360Driver& driver_;
  DeviceId id_;
  Mid360Config cfg_;
  std::unique_ptr<UdpSource> point_;
  std::unique_ptr<UdpSource> imu_;
};

// Owns nothing. open()/close() exist so the reconnect state machine has
// something to drive; a "re-init" here is a no-op that still exercises every
// transition around it.
class InjectBackend final : public Mid360BackendImpl {
 public:
  explicit InjectBackend(DeviceId id) : id_(id) {}
  const char* backend_name() const override { return "inject"; }
  Status open() override {
    ++opens_;
    SCAN_LOG_DEBUG(kMod, "device %u: inject backend open (#%u)", id_, opens_);
    return kOkStatus;
  }
  void close() override {}

 private:
  DeviceId id_;
  std::uint32_t opens_ = 0;
};

}  // namespace

std::unique_ptr<Mid360BackendImpl> make_raw_udp_backend(Mid360Driver& driver, DeviceId id,
                                                        const Mid360Config& cfg) {
  return std::make_unique<RawUdpBackend>(driver, id, cfg);
}

std::unique_ptr<Mid360BackendImpl> make_inject_backend(DeviceId id) {
  return std::make_unique<InjectBackend>(id);
}

}  // namespace scanengine
