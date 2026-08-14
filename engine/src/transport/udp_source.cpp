#include "scanengine/transport/udp_source.h"

namespace scanengine {

// A3 fills these in. They return kUnimplemented rather than silently
// succeeding so a Mid-360 capture attempt before A3 lands fails loudly with
// a message that names the owning task.
Status UdpSource::start() {
  return set_last_error(ScanError::kUnimplemented,
                        "UdpSource::start(): Mid-360 transport is task A3 "
                        "(lidar_ip='%s', host_ip='%s')",
                        cfg_.lidar_ip.c_str(), cfg_.host_ip.c_str());
}

Status UdpSource::stop() { return kOkStatus; }

Status UdpSource::send(ByteSpan) {
  return set_last_error(ScanError::kUnimplemented, "UdpSource::send(): task A3");
}

}  // namespace scanengine
