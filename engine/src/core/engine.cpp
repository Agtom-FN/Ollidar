#include "scanengine/core/engine.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
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

  // --- A8: the trajectory in, and the D6 pushbroom out --------------------
  //
  // Both live for the Engine's whole lifetime, not per session: an app pushes
  // ARCore poses and sets the mount extrinsic while it is still lining the
  // scan up, i.e. before start_session(). The assembler is NOT internally
  // synchronized (A8's contract: one D6 decode thread drives it), so every
  // touch of it goes through `pushbroom_m` — the app's flush()/stats() calls
  // arrive on the control thread while the serial reader is pushing points.
  std::unique_ptr<ExternalPoseSource> poses;
  std::unique_ptr<D6PushbroomAssembler> pushbroom;
  mutable std::mutex pushbroom_m;
  std::atomic<bool> pushbroom_on{false};

  // --- A4/A6: one estimator for the Mid-360's device clock ----------------
  //
  // ImuIngest is constructed on StreamId::kLidarMid360, not kImu: the device
  // stamps its points and its IMU from one clock and the driver feeds the same
  // estimator (docs/A6-lio.md §7.2, docs/A4-timesync.md).
  std::unique_ptr<ImuIngest> imu;

  // Live SLAM, per session. Held by shared_ptr because the Mid-360 receive
  // thread reaches it through the page/IMU sinks while the control thread may
  // be tearing the session down: the sinks copy the pointer out under `lio_m`
  // and then work on their own reference.
  std::shared_ptr<LioOdometry> lio;
  mutable std::mutex lio_m;
  // A5 seam (wired by orchestrator): defaults to the real on-disk writer;
  // tests that must not touch disk install a NullRecordWriter via
  // Engine::set_recorder().
  //
  // record_m serializes write_chunk() across producers: FileRecordWriter is
  // not internally synchronized (record/ owns no thread), and since the
  // Mid-360 raw shim landed, the D6 serial thread and the Mid-360 receive
  // thread can both record concurrently.
  std::unique_ptr<lscan::RecordWriter> recorder =
      std::make_unique<lscan::FileRecordWriter>();
  mutable std::mutex record_m;

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

  std::shared_ptr<LioOdometry> live_lio() const {
    std::lock_guard<std::mutex> lock(lio_m);
    return lio;
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

    // --- A6: the raw Mid-360 cloud is also the odometry's input ------------
    //
    // docs/A6-lio.md §2 wires the LIO off the PageStore rather than off a
    // second driver callback, so live SLAM consumes exactly the points the
    // renderer sees — same filtering, same decimation, same batch boundaries,
    // which is what makes a replay reproduce a capture's map bit for bit.
    //
    // Re-entrancy: the LIO appends its registered map back into this same
    // store, which calls this function again with StreamId::kSlamMap. That is
    // safe (PageStore::append notifies with no lock held) and terminates
    // because only kLidarMid360 is forwarded. With
    // LioConfig::internal_thread = true — what a live capture uses —
    // push_points() only enqueues, so the receive thread never runs the
    // odometry at all.
    if (u.stream != StreamId::kLidarMid360) return;
    std::shared_ptr<LioOdometry> lio = self->live_lio();
    if (!lio) return;
    const PageView v = self->points->page_view(u.page);
    if (!v.valid() || u.first + u.count > v.count) return;
    (void)lio->push_points(Span<const PointVertex>(v.data + u.first, u.count), v.t_last_ns);
  }

  // --- driver → engine sinks ------------------------------------------------
  //
  // Both chain to whatever the app configured, so installing the engine's own
  // sink never takes a seam away from a caller that was already using it.
  struct D6ProfileShim {
    Impl* self = nullptr;
    D6ProfileSink user = nullptr;
    void* user_data = nullptr;
  };
  struct Mid360ImuShim {
    Impl* self = nullptr;
    Mid360ImuSink user = nullptr;
    void* user_data = nullptr;
  };
  // C2/C3 finding: without this shim a Mid-360 capture streamed live but
  // recorded 0 chunks — only D6's push_serial_bytes() ever reached the
  // recorder. Raw datagrams now land as kMid360Points/kMid360Imu chunks,
  // replayable through Mid360Backend::kInject (one datagram per call).
  struct Mid360RawShim {
    Impl* self = nullptr;
    Mid360RawSink user = nullptr;
    void* user_data = nullptr;
  };
  // Owned for the Engine's lifetime: a driver keeps the raw pointer, and a
  // device may be removed while its receive thread is still unwinding.
  std::vector<std::unique_ptr<D6ProfileShim>> d6_shims;
  std::vector<std::unique_ptr<Mid360ImuShim>> imu_shims;
  std::vector<std::unique_ptr<Mid360RawShim>> raw_shims;

  static void on_d6_profile(float angle_deg, float range_m, std::uint8_t intensity,
                            std::uint8_t high_reflectivity, std::int64_t t_engine_ns, void* user) {
    auto* shim = static_cast<D6ProfileShim*>(user);
    Impl* self = shim->self;
    if (self->pushbroom_on.load(std::memory_order_acquire)) {
      ProfilePoint p{};
      p.t_mono_ns = t_engine_ns;
      p.angle_deg = angle_deg;
      p.range_m = range_m;
      p.intensity = intensity;
      p.high_reflectivity = high_reflectivity;
      // push_profile(), not push_point(): the one-element span is what honours
      // PushbroomConfig::drain_on_push, so a point whose pose has already
      // arrived is resolved immediately instead of waiting for a flush, and
      // `points_pending` stays live for the health panel. resolve_() stops at
      // the first still-future point, so this is amortized O(1) per return.
      std::lock_guard<std::mutex> lock(self->pushbroom_m);
      (void)self->pushbroom->push_profile(Span<const ProfilePoint>(&p, 1));
    }
    if (shim->user != nullptr) {
      shim->user(angle_deg, range_m, intensity, high_reflectivity, t_engine_ns, shim->user_data);
    }
  }

  static void on_mid360_raw(const std::uint8_t* data, std::size_t len, bool is_imu,
                            std::int64_t t_arrival_ns, void* user) {
    auto* shim = static_cast<Mid360RawShim*>(user);
    Impl* self = shim->self;
    {
      std::lock_guard<std::mutex> lock(self->record_m);
      if (self->recorder->is_open()) {
        const auto type = is_imu ? lscan::ChunkType::kMid360Imu : lscan::ChunkType::kMid360Points;
        (void)self->recorder->write_chunk(type, t_arrival_ns, ByteSpan(data, len));
      }
    }
    if (shim->user != nullptr) shim->user(data, len, is_imu, t_arrival_ns, shim->user_data);
  }

  static void on_mid360_imu(const Mid360ImuSample* samples, std::size_t count, void* user) {
    auto* shim = static_cast<Mid360ImuShim*>(user);
    Impl* self = shim->self;
    std::shared_ptr<LioOdometry> lio = self->live_lio();
    for (std::size_t i = 0; i < count; ++i) {
      const Mid360ImuSample& s = samples[i];
      // The driver reports acceleration in g exactly as the device does
      // (S2: mean |acc| = 1.0000 g over 120,009 packets); converting it —
      // and mapping the device stamp — is A4's business, so it happens here
      // and nowhere else.
      const ImuSample m = self->imu->add_g(static_cast<std::int64_t>(s.t_device_ns),
                                           TimePoint{s.t_mono_ns}, s.gyro, s.acc);
      if (lio) (void)lio->push_imu(m.t_engine_ns, m.gyro_rad_s, m.accel_m_s2);
    }
    if (shim->user != nullptr) shim->user(samples, count, shim->user_data);
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

  // A8: one pose source and one assembler for the Engine's lifetime. The pose
  // stamps are mapped through A4 on the way in (identity for kPoseAr today,
  // but the seam is what lets a pose stream with its own clock work without
  // touching any consumer — docs/A8-pushbroom.md §3.5).
  ExternalPoseConfig pc;
  pc.stream = StreamId::kPoseAr;
  pc.timesync = &e->impl_->timesync;
  e->impl_->poses = std::make_unique<ExternalPoseSource>(pc);
  (void)e->impl_->poses->start();
  e->impl_->pushbroom =
      std::make_unique<D6PushbroomAssembler>(e->impl_->points.get(), engine_pushbroom_defaults());
  e->impl_->pushbroom->set_pose_source(e->impl_->poses.get());

  // A4/A6: the Mid-360's one estimator (see Impl::imu).
  e->impl_->imu = std::make_unique<ImuIngest>(e->impl_->timesync, StreamId::kLidarMid360);

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

  // A8: a new capture starts with an empty pending queue and zeroed counters,
  // but KEEPS the mount extrinsic — that is a property of the bracket, not of
  // the session, and re-asking the user for it between two scans of the same
  // room would be absurd.
  {
    std::lock_guard<std::mutex> lock(impl_->pushbroom_m);
    impl_->pushbroom->reset();
    PushbroomConfig pcfg = cfg.pushbroom_cfg;
    impl_->pushbroom->set_config(pcfg);
  }
  impl_->pushbroom_on.store(cfg.pushbroom, std::memory_order_release);
  if (cfg.pushbroom && !impl_->pushbroom->has_mount_extrinsics()) {
    SCAN_LOG_WARN(kMod,
                  "pushbroom enabled without a mount extrinsic; assembly stays pending until "
                  "set_mount_extrinsics() is called (docs/A8-pushbroom.md §4)");
  }

  // A6: live SLAM is one LioOdometry for the session, publishing its map into
  // the Engine's own PageStore on StreamId::kSlamMap. It is fed by
  // Impl::on_page_update (points) and Impl::on_mid360_imu (IMU).
  if (cfg.live_slam) {
    LioConfig lc = cfg.lio;
    lc.map_store = impl_->points.get();  // wiring, not a choice
    lc.map_stream = StreamId::kSlamMap;
    auto lio = std::make_shared<LioOdometry>(lc);
    const Status s = lio->start();
    if (!s.ok()) {
      // A capture must survive live SLAM failing to come up: the raw streams
      // are still recorded and still previewed, which is Record-only.
      SCAN_LOG_ERROR(kMod, "live SLAM failed to start: %s; continuing record-only",
                     error_str(s.error()));
    } else {
      std::lock_guard<std::mutex> lock(impl_->lio_m);
      impl_->lio = std::move(lio);
      SCAN_LOG_INFO(kMod, "live SLAM on: map -> %s%s", to_string(StreamId::kSlamMap),
                    lc.internal_thread ? " (odometry thread)" : " (inline)");
    }
  }

  if (cfg.record) {
    if (cfg.lscan_dir.empty()) {
      SCAN_LOG_WARN(kMod,
                    "session started with recording requested but no .lscan directory; "
                    "raw streams will NOT be persisted (Tech Spec §3 rule 2)");
    } else {
      // A5 seam: the workflow profile flows into the manifest before open().
      std::lock_guard<std::mutex> rlock(impl_->record_m);
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

  // Producers are stopped, so nothing else can touch either consumer now.
  // A8 §7.2 item 4: flush() resolves what the poses allow and gives up on the
  // rest as dropped_no_pose — end of stream is the one moment at which a
  // pending point is genuinely unresolvable.
  {
    std::lock_guard<std::mutex> lock(impl_->pushbroom_m);
    (void)impl_->pushbroom->flush();
  }
  impl_->pushbroom_on.store(false, std::memory_order_release);

  // A6: close the last partial scan (it is otherwise never registered), then
  // release the odometry — and with it, its thread.
  std::shared_ptr<LioOdometry> lio;
  {
    std::lock_guard<std::mutex> lock(impl_->lio_m);
    lio.swap(impl_->lio);
  }
  if (lio) {
    (void)lio->flush();
    (void)lio->stop();
    lio.reset();
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
    case DeviceKind::kD6: {
      // A8 §7.2 item 3: route the decoded profile to the assembler. Installed
      // unconditionally — the sink itself checks whether assembly is on, so
      // scan_engine_pushbroom_enable() works mid-session without re-adding
      // the device.
      D6Config dcfg = cfg.d6;
      auto shim = std::make_unique<Impl::D6ProfileShim>();
      shim->self = impl_.get();
      shim->user = dcfg.profile_sink;
      shim->user_data = dcfg.profile_sink_user_data;
      dcfg.profile_sink = &Impl::on_d6_profile;
      dcfg.profile_sink_user_data = shim.get();
      impl_->d6_shims.push_back(std::move(shim));
      driver = std::make_unique<D6Driver>(id, dcfg, ctx);
      break;
    }
    case DeviceKind::kMid360: {
      // A4/A6: every IMU sample goes through the Engine's one ImuIngest (and
      // therefore through the kLidarMid360 estimator the point path also
      // feeds) before it reaches the odometry.
      Mid360Config mcfg = cfg.mid360;
      auto shim = std::make_unique<Impl::Mid360ImuShim>();
      shim->self = impl_.get();
      shim->user = mcfg.imu_sink;
      shim->user_data = mcfg.imu_sink_user_data;
      mcfg.imu_sink = &Impl::on_mid360_imu;
      mcfg.imu_sink_user_data = shim.get();
      impl_->imu_shims.push_back(std::move(shim));
      // Record-always for a driver that owns its own sockets (C2/C3 finding).
      auto raw = std::make_unique<Impl::Mid360RawShim>();
      raw->self = impl_.get();
      raw->user = mcfg.raw_sink;
      raw->user_data = mcfg.raw_sink_user_data;
      mcfg.raw_sink = &Impl::on_mid360_raw;
      mcfg.raw_sink_user_data = raw.get();
      impl_->raw_shims.push_back(std::move(raw));
      driver = std::make_unique<Mid360Driver>(id, mcfg, ctx);
      break;
    }
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
  //
  // D6 only: Mid-360 datagrams (kInject replay or live sockets alike) are
  // recorded by the driver-level raw shim as typed kMid360Points/kMid360Imu
  // chunks — recording them here too would both mislabel them as kD6Raw and
  // duplicate every datagram.
  if (d->kind() == DeviceKind::kD6) {
    std::lock_guard<std::mutex> rlock(impl_->record_m);
    if (impl_->recorder->is_open()) {
      const std::int64_t t = t_arrival.nanos != 0 ? t_arrival.nanos : SteadyClock::now().nanos;
      (void)impl_->recorder->write_chunk(lscan::ChunkType::kD6Raw, t, bytes);
    }
  }
  return d->push_bytes(bytes, t_arrival);
}

// --- A8: trajectory in ------------------------------------------------------

Status Engine::push_pose(const Pose& pose) {
  const Status s = impl_->poses->push_pose(pose);
  if (!s.ok()) return s;
  publish_pose_(pose);
  return kOkStatus;
}

Status Engine::push_pose(const Pose& pose, float confidence) {
  const Status s = impl_->poses->push_pose(pose, confidence);
  if (!s.ok()) return s;
  publish_pose_(pose);
  return kOkStatus;
}

// A8 §7.2 item 5: PoseUpdatePayload has existed since A1 and nothing published
// it. The app's AR overlay and the desktop trajectory ribbon both read it, and
// it is the only way a C-ABI consumer learns that its own pose was accepted.
void Engine::publish_pose_(const Pose& p) {
  PoseUpdatePayload u{};
  u.source = p.source;
  for (int i = 0; i < 3; ++i) u.position[i] = static_cast<float>(p.position[i]);
  for (int i = 0; i < 4; ++i) u.quaternion[i] = static_cast<float>(p.orientation[i]);
  // PoseQuality is 0..3; the payload's field is documented 0..255, so scale
  // rather than truncate — a consumer comparing against 255 must not see 3.
  u.quality = p.tracking_lost != 0
                  ? std::uint8_t{0}
                  : static_cast<std::uint8_t>(static_cast<int>(p.quality) * 85);
  impl_->bus.publish(EventType::kPoseUpdate, u, p.t_mono_ns);
}

PoseSample Engine::pose_at(std::int64_t t_mono_ns) const {
  return impl_->poses->sample_at(t_mono_ns);
}

ExternalPoseSource& Engine::poses() { return *impl_->poses; }
const ExternalPoseSource& Engine::poses() const { return *impl_->poses; }

// --- A8: pushbroom ----------------------------------------------------------

Status Engine::set_mount_extrinsics(const double phone_from_lidar[16]) {
  if (phone_from_lidar == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "mount extrinsic is null");
  }
  std::lock_guard<std::mutex> lock(impl_->pushbroom_m);
  return impl_->pushbroom->set_mount_extrinsics(phone_from_lidar);
}

Status Engine::set_pushbroom_enabled(bool on) {
  if (on && !impl_->pushbroom->has_mount_extrinsics()) {
    return set_last_error(ScanError::kInvalidState,
                          "pushbroom needs a mount extrinsic: call set_mount_extrinsics() first");
  }
  impl_->pushbroom_on.store(on, std::memory_order_release);
  return kOkStatus;
}

bool Engine::pushbroom_enabled() const {
  return impl_->pushbroom_on.load(std::memory_order_acquire);
}

Status Engine::pushbroom_flush() {
  std::lock_guard<std::mutex> lock(impl_->pushbroom_m);
  return impl_->pushbroom->flush();
}

PushbroomStats Engine::pushbroom_stats() const {
  std::lock_guard<std::mutex> lock(impl_->pushbroom_m);
  return impl_->pushbroom->stats();
}

// --- A6: live SLAM ----------------------------------------------------------

LioOdometry* Engine::live_slam() {
  std::lock_guard<std::mutex> lock(impl_->lio_m);
  return impl_->lio.get();
}

const LioOdometry* Engine::live_slam() const {
  std::lock_guard<std::mutex> lock(impl_->lio_m);
  return impl_->lio.get();
}

ImuIngest& Engine::imu() { return *impl_->imu; }

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
