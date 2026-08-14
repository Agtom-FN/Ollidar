#include "scanengine/drivers/mid360/mid360_driver.h"

#include "scanengine/core/log.h"

namespace scanengine {

Mid360Driver::Mid360Driver(DeviceId id, const Mid360Config& cfg, const DriverContext& ctx)
    : id_(id), cfg_(cfg), ctx_(ctx) {
  state_ = DeviceState::kDisconnected;
}

Mid360Driver::~Mid360Driver() = default;

Status Mid360Driver::start() {
  SCAN_LOG_WARN("mid360", "device %u: start() is task A3 (lidar_ip='%s')", id_,
                cfg_.udp.lidar_ip.c_str());
  return set_last_error(ScanError::kUnimplemented,
                        "Mid360Driver::start(): SDK2 integration is task A3");
}

Status Mid360Driver::stop() { return kOkStatus; }

Status Mid360Driver::push_bytes(ByteSpan, TimePoint) {
  // The Mid-360 owns its sockets (SDK2); bytes are never pushed from the app.
  return set_last_error(ScanError::kNotSupported,
                        "Mid-360 is a self-driven UDP source; push_bytes() does not apply");
}

DeviceHealth Mid360Driver::health() const {
  DeviceHealth h{};
  h.id = id_;
  h.kind = DeviceKind::kMid360;
  h.state = state_;
  h.last_error = ScanError::kUnimplemented;
  return h;
}

}  // namespace scanengine
