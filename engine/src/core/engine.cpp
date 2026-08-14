#include "scanengine/core/engine.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "scanengine/core/log.h"

namespace scanengine {
namespace {
constexpr const char* kMod = "engine";
}

const char* engine_version_string() {
  static std::string s = std::string("scanengine " SCANENGINE_VERSION " (clock: ") +
                         SteadyClock::backend_name() + ")";
  return s.c_str();
}

struct Engine::Impl {
  EngineConfig cfg;
  EventBus bus;
  std::unique_ptr<PageStore> points;
  TimeSync timesync;
  // A5 seam (wired by orchestrator): defaults to the real on-disk writer;
  // tests that must not touch disk install a NullRecordWriter via
  // Engine::set_recorder().
  std::unique_ptr<lscan::RecordWriter> recorder =
      std::make_unique<lscan::FileRecordWriter>();

  mutable std::mutex m;  // guards lifecycle + the device map
  EngineState state = EngineState::kIdle;
  SessionConfig session{};
  std::uint64_t session_id = 0;
  std::map<DeviceId, std::unique_ptr<Driver>> devices;
  DeviceId next_device_id = 1;

  SubscriptionId app_sub = kInvalidSubscription;
  PageSubscriptionId page_sub = 0;

  void set_state(EngineState next) {
    EngineState prev;
    {
      std::lock_guard<std::mutex> lock(m);
      if (state == next) return;
      prev = state;
      state = next;
    }
    SCAN_LOG_INFO(kMod, "%s -> %s", to_string(prev), to_string(next));
    EngineStatePayload p{};
    p.state = next;
    p.previous = prev;
    bus.publish(EventType::kEngineState, p);
  }

  Driver* find(DeviceId id) const {
    auto it = devices.find(id);
    return it == devices.end() ? nullptr : it->second.get();
  }

  // PageStore → EventBus bridge. ONE place turns page updates into
  // kPointsAvailable events, so every producer (D6 today, Mid-360/SLAM/
  // colorization later) gets identical render-facing semantics. Runs on the
  // producer's thread, inside PageStore::append().
  static void on_page_update(const PageUpdate& u, void* user) {
    auto* self = static_cast<Impl*>(user);
    PointsAvailablePayload p{};
    p.page = u.page;
    p.first = u.first;
    p.count = u.count;
    p.stream = u.stream;
    p.page_created = u.page_created ? 1 : 0;
    self->bus.publish(EventType::kPointsAvailable, p);
  }
};

Engine::Engine() : impl_(new Impl) {}

Engine::~Engine() {
  // Order matters: stop producers, then unsubscribe the page bridge, then
  // close the bus so no callback can run against a half-destroyed engine.
  (void)stop_session();
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->devices.clear();
  }
  if (impl_->page_sub != 0) (void)impl_->points->unsubscribe(impl_->page_sub);
  impl_->bus.close();
}

Result<std::unique_ptr<Engine>> Engine::create(const EngineConfig& cfg) {
  std::unique_ptr<Engine> e(new Engine());
  e->impl_->cfg = cfg;
  set_log_min_level(cfg.log_level);
  e->impl_->points = std::make_unique<PageStore>(cfg.points);

  SubscriptionOptions opts;
  opts.capacity = cfg.event_queue_capacity == 0 ? 1024 : cfg.event_queue_capacity;
  opts.policy = OverflowPolicy::kDropOldest;
  auto sub = e->impl_->bus.subscribe(opts);
  if (!sub.ok()) return sub.error();
  e->impl_->app_sub = sub.value();

  e->impl_->page_sub = e->impl_->points->subscribe(&Impl::on_page_update, e->impl_.get());
  if (e->impl_->page_sub == 0) {
    return set_last_error(ScanError::kUnknown, "engine: could not subscribe to the page store");
  }

  // A4 seam: device-clock discontinuities reach the app as events.
  e->impl_->timesync.set_event_bus(&e->impl_->bus);

  SCAN_LOG_INFO(kMod, "%s created for '%s' (pages: %u × %u pts)", engine_version_string(),
                cfg.app_name.c_str(), e->impl_->points->config().max_pages,
                e->impl_->points->config().page_capacity);
  return std::move(e);
}

EngineState Engine::state() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->state;
}

bool Engine::session_active() const { return state() == EngineState::kRunning; }

const SessionConfig& Engine::session_config() const { return impl_->session; }

std::uint64_t Engine::session_id() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->session_id;
}

Status Engine::start_session(const SessionConfig& cfg) {
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->state == EngineState::kRunning || impl_->state == EngineState::kStarting) {
      return set_last_error(ScanError::kInvalidState, "session already running");
    }
    if (impl_->state == EngineState::kStopping) {
      return set_last_error(ScanError::kBusy, "a session is still stopping");
    }
    impl_->session = cfg;
    ++impl_->session_id;
  }
  impl_->set_state(EngineState::kStarting);

  if (cfg.record) {
    if (cfg.lscan_dir.empty()) {
      SCAN_LOG_WARN(kMod,
                    "session started with recording requested but no .lscan directory; "
                    "raw streams will NOT be persisted (Tech Spec §3 rule 2)");
    } else {
      // A5 seam: the workflow profile flows into the manifest before open().
      if (auto* fw = dynamic_cast<lscan::FileRecordWriter*>(impl_->recorder.get())) {
        fw->set_profile(cfg.profile);
      }
      const Status s = impl_->recorder->open(cfg.lscan_dir);
      if (!s.ok()) {
        impl_->set_state(EngineState::kFaulted);
        return s;
      }
    }
  }

  // Start every registered device. A device that fails to start is reported
  // and left in kFault; it does not abort the session (a two-sensor capture
  // must survive one sensor being unplugged).
  std::vector<Driver*> to_start;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    for (auto& kv : impl_->devices) to_start.push_back(kv.second.get());
  }
  for (Driver* d : to_start) {
    const Status s = d->start();
    if (!s.ok()) {
      SCAN_LOG_ERROR(kMod, "device %u (%s) failed to start: %s", d->id(), d->name(),
                     error_str(s.error()));
    }
  }

  impl_->set_state(EngineState::kRunning);

  SessionStatePayload p{};
  p.recording = (cfg.record && !cfg.lscan_dir.empty()) ? 1 : 0;
  p.session_id = session_id();
  p.bytes_written = 0;
  impl_->bus.publish(EventType::kSessionState, p);
  return kOkStatus;
}

Status Engine::stop_session() {
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->state == EngineState::kIdle) return kOkStatus;
  }
  impl_->set_state(EngineState::kStopping);

  std::vector<Driver*> to_stop;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    for (auto& kv : impl_->devices) to_stop.push_back(kv.second.get());
  }
  for (Driver* d : to_stop) {
    const Status s = d->stop();
    if (!s.ok()) SCAN_LOG_WARN(kMod, "device %u stop: %s", d->id(), error_str(s.error()));
  }

  if (impl_->recorder->is_open()) {
    (void)impl_->recorder->flush();
    (void)impl_->recorder->close();
  }

  SessionStatePayload p{};
  p.recording = 0;
  p.session_id = session_id();
  p.bytes_written = impl_->recorder->stats().bytes_written;
  impl_->bus.publish(EventType::kSessionState, p);

  impl_->set_state(EngineState::kIdle);
  return kOkStatus;
}

Result<DeviceId> Engine::add_device(const DeviceConfig& cfg) {
  DriverContext ctx;
  ctx.bus = &impl_->bus;
  ctx.points = impl_->points.get();
  ctx.timesync = &impl_->timesync;

  std::unique_ptr<Driver> driver;
  DeviceId id;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    id = impl_->next_device_id++;
  }

  switch (cfg.kind) {
    case DeviceKind::kD6:
      driver = std::make_unique<D6Driver>(id, cfg.d6, ctx);
      break;
    case DeviceKind::kMid360:
      driver = std::make_unique<Mid360Driver>(id, cfg.mid360, ctx);
      break;
    case DeviceKind::kRtkRover:
      return set_last_error(ScanError::kUnimplemented,
                            "RTK rover ingestion is task A10");
    case DeviceKind::kUnknown:
      return set_last_error(ScanError::kInvalidArgument, "device kind not set");
  }

  Driver* raw = driver.get();
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->devices.emplace(id, std::move(driver));
  }
  SCAN_LOG_INFO(kMod, "added device %u (%s)", id, to_string(cfg.kind));

  // A device added mid-session starts immediately, so hot-plugging a second
  // sensor does not require restarting the capture.
  if (session_active()) {
    const Status s = raw->start();
    if (!s.ok()) {
      SCAN_LOG_ERROR(kMod, "device %u failed to start: %s", id, error_str(s.error()));
    }
  }
  return id;
}

Status Engine::remove_device(DeviceId id) {
  std::unique_ptr<Driver> victim;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    auto it = impl_->devices.find(id);
    if (it == impl_->devices.end()) {
      return set_last_error(ScanError::kNotFound, "no device %u", id);
    }
    victim = std::move(it->second);
    impl_->devices.erase(it);
  }
  (void)victim->stop();
  return kOkStatus;
}

std::vector<DeviceId> Engine::device_ids() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  std::vector<DeviceId> ids;
  ids.reserve(impl_->devices.size());
  for (const auto& kv : impl_->devices) ids.push_back(kv.first);
  return ids;
}

Result<DeviceHealth> Engine::device_health(DeviceId id) const {
  std::lock_guard<std::mutex> lock(impl_->m);
  Driver* d = impl_->find(id);
  if (d == nullptr) return set_last_error(ScanError::kNotFound, "no device %u", id);
  return d->health();
}

Status Engine::push_serial_bytes(DeviceId id, ByteSpan bytes, TimePoint t_arrival) {
  Driver* d = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    d = impl_->find(id);
    if (d == nullptr) return set_last_error(ScanError::kNotFound, "no device %u", id);
  }
  // Deliberately outside the lock: this call runs the whole decode pipeline
  // and must not block add_device()/health() on a UI thread.
  //
  // Record-always: the raw bytes are handed to the recorder BEFORE they are
  // parsed, so a crash mid-parse still leaves the capture on disk.
  if (impl_->recorder->is_open()) {
    const std::int64_t t = t_arrival.nanos != 0 ? t_arrival.nanos : SteadyClock::now().nanos;
    (void)impl_->recorder->write_chunk(lscan::ChunkType::kD6Raw, t, bytes);
  }
  return d->push_bytes(bytes, t_arrival);
}

EventBus& Engine::events() { return impl_->bus; }
PageStore& Engine::points() { return *impl_->points; }
TimeSync& Engine::timesync() { return impl_->timesync; }
lscan::RecordWriter& Engine::recorder() { return *impl_->recorder; }

void Engine::set_recorder(std::unique_ptr<lscan::RecordWriter> w) {
  if (w) impl_->recorder = std::move(w);
}

SubscriptionId Engine::app_subscription() const { return impl_->app_sub; }

Status Engine::set_app_event_callback(EventCallback cb, void* user_data) {
  // Swap the built-in subscription between queued and callback mode. Any
  // events queued but not yet polled are dropped by design — a caller that
  // installs a callback is declaring it no longer polls.
  if (impl_->app_sub != kInvalidSubscription) {
    (void)impl_->bus.unsubscribe(impl_->app_sub);
    impl_->app_sub = kInvalidSubscription;
  }
  SubscriptionOptions opts;
  opts.capacity = impl_->cfg.event_queue_capacity == 0 ? 1024 : impl_->cfg.event_queue_capacity;
  opts.callback = cb;
  opts.user_data = user_data;
  auto sub = impl_->bus.subscribe(opts);
  if (!sub.ok()) return sub.status();
  impl_->app_sub = sub.value();
  return kOkStatus;
}

}  // namespace scanengine
