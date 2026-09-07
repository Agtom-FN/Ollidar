// mid70_backend.h — private to src/drivers/mid70/.
//
// The one thing the driver needs from whatever owns the sockets: bring the
// link up, tear it down. Same seam and same reason as mid360_backend.h —
// `close()` then `open()` IS the reconnect path, and having it behind an
// interface is what lets the state machine be tested against a backend that
// costs nothing to recreate.
#ifndef SCANENGINE_SRC_DRIVERS_MID70_BACKEND_H
#define SCANENGINE_SRC_DRIVERS_MID70_BACKEND_H

#include <cstdint>
#include <memory>

#include "scanengine/drivers/mid70/mid70_driver.h"

namespace scanengine {

class Mid70BackendImpl {
 public:
  virtual ~Mid70BackendImpl() = default;
  virtual const char* backend_name() const = 0;
  virtual Status open() = 0;
  virtual void close() = 0;
};

// make_sdk1_backend() fails with kNotSupported when the engine was built
// without ENGINE_WITH_LIVOX_SDK1 — the message names the fetch script.
std::unique_ptr<Mid70BackendImpl> make_sdk1_backend(Mid70Driver& driver, DeviceId id,
                                                    const Mid70Config& cfg);
std::unique_ptr<Mid70BackendImpl> make_raw_udp_backend(Mid70Driver& driver, DeviceId id,
                                                       const Mid70Config& cfg);
std::unique_ptr<Mid70BackendImpl> make_inject_backend(Mid70Driver& driver, DeviceId id);

// PPS+GPS passthrough (SDK v1 backend only, and the only command the engine
// sends outside the bring-up handshake). A Mid-70 wired to a GNSS receiver's
// PPS line still needs the matching GPRMC/GNRMC sentence over the command
// channel before its datagram timestamps switch to UTC; this hands one
// through to whichever device the open SDK v1 backend is connected to.
//
// Deliberately a free function and not a Driver method: nothing in the engine
// owns a GNSS sentence source yet (the PPS+GPS bridge is on the plan's
// follow-up list), so giving Mid70Driver an API for it would be inventing a
// caller. Returns kInvalidState when no SDK v1 backend is open or no device
// has connected, and kNotSupported when the engine was built without the SDK.
// The command is asynchronous: a success here means "sent", and whether the
// device locked onto it shows up as time_sync_status in the datagrams.
Status mid70_sdk1_set_rmc_sync_time(const char* rmc, std::uint32_t len);

}  // namespace scanengine

#endif  // SCANENGINE_SRC_DRIVERS_MID70_BACKEND_H
