// driver.h — what every sensor driver looks like from the engine's side.
//
// A driver owns: one transport, one protocol decoder, one health model. It
// publishes points into the PageStore and status onto the EventBus, and it
// never talks to the UI, the filesystem, or another driver.
//
// DriverContext is how a driver reaches the engine's shared services. It is
// passed by reference and outlives the driver (the Engine owns both).
//
// Owner: A1 (interface) / A2 (D6) / A3 (Mid-360) / A10 (RTK rover).
#ifndef SCANENGINE_DRIVERS_DRIVER_H
#define SCANENGINE_DRIVERS_DRIVER_H

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/core/types.h"
#include "scanengine/timesync/clock.h"

namespace scanengine {

class TimeSync;  // timesync/time_sync.h

struct DriverContext {
  EventBus* bus = nullptr;
  PageStore* points = nullptr;
  TimeSync* timesync = nullptr;  // may be null in unit tests
  ClockFn clock = &steady_now;   // replay/tests substitute a deterministic clock

  bool valid() const { return bus != nullptr && points != nullptr; }
};

class Driver {
 public:
  virtual ~Driver() = default;

  virtual const char* name() const = 0;
  virtual DeviceKind kind() const = 0;
  virtual DeviceId id() const = 0;

  // start() may block only for as long as it takes to hand a command to the
  // transport; it must never wait for the device. Reaching kStreaming is
  // reported asynchronously through EventType::kDeviceState.
  virtual Status start() = 0;
  virtual Status stop() = 0;

  virtual DeviceState state() const = 0;
  virtual DeviceHealth health() const = 0;

  // Raw bytes from the app for push-mode transports (USB serial). Sources
  // that own their own I/O (UDP, once A3 lands) return kNotSupported.
  virtual Status push_bytes(ByteSpan bytes, TimePoint t_arrival) = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_DRIVER_H
