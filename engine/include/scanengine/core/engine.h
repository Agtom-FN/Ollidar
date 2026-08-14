// engine.h — the engine's C++ API. The Qt desktop app links this directly
// (same process, no FFI); Android reaches it through capi/scanengine_c.h.
//
// THREADING MODEL (the short version; DESIGN.md has the long one)
//   The Engine owns NO threads in A1. Work happens on the thread that
//   pushes data in (the app's serial reader) and on whatever threads later
//   modules introduce — A3's SDK2 receive thread, A6's odometry thread,
//   A15's job workers. Every such module must document its thread and reach
//   the rest of the engine only through the two thread-safe hubs: EventBus
//   and PageStore. The Engine's own methods are safe to call from any
//   thread; lifecycle calls (start_session/stop_session/add_device) are
//   serialized by one mutex and are expected from the app's control thread.
//
// LIFECYCLE
//   create() → kIdle → start_session() → kRunning → stop_session() → kIdle
//   Devices may be added in either kIdle or kRunning; a device added while
//   running is started immediately. Errors during a session put the offending
//   DEVICE into kFault; the engine only reaches kFaulted if a session-level
//   invariant breaks (e.g. the recorder cannot write).
//
// Owner: A1.
#ifndef SCANENGINE_CORE_ENGINE_H
#define SCANENGINE_CORE_ENGINE_H

#include <memory>
#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/core/log.h"
#include "scanengine/core/types.h"
#include "scanengine/drivers/d6/d6_driver.h"
#include "scanengine/drivers/mid360/mid360_driver.h"
#include "scanengine/record/lscan.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {

// Bumped whenever the C ABI changes shape (see capi/scanengine_c.h).
inline constexpr std::uint32_t kEngineAbiVersion = 1;
const char* engine_version_string();  // "scanengine 0.1.0 (<clock backend>)"

struct EngineConfig {
  std::string app_name = "scanengine";
  LogLevel log_level = LogLevel::kInfo;
  PageStoreConfig points{};
  std::uint32_t event_queue_capacity = 1024;  // for the built-in app subscription
};

struct SessionConfig {
  // .lscan directory. Empty means "do not record" — legal, but it breaks
  // Tech Spec §3's record-always rule, so it is only for tests and live
  // previews. The Engine logs a warning when it is empty.
  std::string lscan_dir;
  std::string profile = "quickscan";  // survey | floorplan | research | quickscan
  bool record = true;
};

struct DeviceConfig {
  DeviceKind kind = DeviceKind::kUnknown;
  D6Config d6{};
  Mid360Config mid360{};
};

class Engine {
 public:
  static Result<std::unique_ptr<Engine>> create(const EngineConfig& cfg);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // --- lifecycle ---------------------------------------------------------
  EngineState state() const;
  Status start_session(const SessionConfig& cfg);
  Status stop_session();
  bool session_active() const;
  const SessionConfig& session_config() const;
  std::uint64_t session_id() const;

  // --- devices -----------------------------------------------------------
  Result<DeviceId> add_device(const DeviceConfig& cfg);
  Status remove_device(DeviceId id);
  std::vector<DeviceId> device_ids() const;
  Result<DeviceHealth> device_health(DeviceId id) const;

  // App → engine bytes for push-mode transports (D6 over USB serial).
  // t_arrival {0} means "stamp now".
  Status push_serial_bytes(DeviceId id, ByteSpan bytes, TimePoint t_arrival = TimePoint{0});

  // --- shared services ---------------------------------------------------
  EventBus& events();
  PageStore& points();
  TimeSync& timesync();
  lscan::RecordWriter& recorder();

  // The built-in subscription the C ABI's poll_event()/set_event_callback()
  // drive. C++ consumers should make their own subscription instead.
  SubscriptionId app_subscription() const;
  Status set_app_event_callback(EventCallback cb, void* user_data);

 private:
  Engine();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_CORE_ENGINE_H
