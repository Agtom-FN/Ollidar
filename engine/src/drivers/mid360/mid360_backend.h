// mid360_backend.h — private to src/drivers/mid360/.
//
// The one thing the driver needs from whatever owns the sockets: "bring the
// link up" and "tear it down". Everything else (decode, filter, loss,
// watchdog, health) is backend-independent and lives in Mid360Driver.
//
// This seam exists for exactly one reason, and it is the S2 power-cycle
// finding: recovery from a device that forgot its configuration REQUIRES a
// full teardown and re-create of the SDK, because the SDK will never
// re-handshake a handle it has already seen. `close()` then `open()` is that
// operation, and having it behind an interface is what lets the reconnect
// state machine be unit-tested against a backend that costs nothing to
// recreate.
#ifndef SCANENGINE_SRC_DRIVERS_MID360_BACKEND_H
#define SCANENGINE_SRC_DRIVERS_MID360_BACKEND_H

#include <memory>

#include "scanengine/drivers/mid360/mid360_driver.h"

namespace scanengine {

class Mid360BackendImpl {
 public:
  virtual ~Mid360BackendImpl() = default;

  virtual const char* backend_name() const = 0;

  // Bring the link up from nothing: bind sockets / init the SDK, install
  // callbacks, ask the device to stream. Must be safe to call again after
  // close() — that pair IS the reconnect path.
  virtual Status open() = 0;

  // Tear everything down and release the sockets. Must be idempotent, must
  // join any thread that could still call into the driver, and must return
  // with no callback in flight.
  virtual void close() = 0;
};

// Both factories return a non-null backend or set the thread-local last
// error. make_sdk2_backend() fails with kNotSupported when the engine was
// built without ENGINE_WITH_LIVOX_SDK2 — the message names the fetch script.
std::unique_ptr<Mid360BackendImpl> make_sdk2_backend(Mid360Driver& driver, DeviceId id,
                                                     const Mid360Config& cfg);
std::unique_ptr<Mid360BackendImpl> make_raw_udp_backend(Mid360Driver& driver, DeviceId id,
                                                        const Mid360Config& cfg);
std::unique_ptr<Mid360BackendImpl> make_inject_backend(DeviceId id);

}  // namespace scanengine

#endif  // SCANENGINE_SRC_DRIVERS_MID360_BACKEND_H
