#include "scanengine/core/engine.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "scanengine/color/frames_idx.h"
#include "scanengine/core/log.h"
#include "scanengine/jobs/job_queue.h"

namespace scanengine {
namespace {
constexpr const char* kMod = "engine";

std::string trim_copy(const std::string& s) {
  const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
  auto b = std::find_if(s.begin(), s.end(), not_space);
  auto e = std::find_if(s.rbegin(), s.rend(), not_space).base();
  return b < e ? std::string(b, e) : std::string();
}

// A structural check on a caller-supplied CRS string, NOT a geodetic one.
//
// The engine cannot verify that a WKT describes the grid it claims to — that
// needs the registry it deliberately does not ship. What it CAN refuse is the
// class of mistake that actually happens across an FFI: a PROJ.4 string, a
// bare EPSG code, a truncated copy-paste, or a JSON blob handed to a field
// that a LAS writer will embed verbatim. Every one of those produces a file
// that opens and is wrong, which is the failure mode gnss/crs.h's header calls
// out. Balanced brackets outside quotes is the cheap half of "it parses".
Status validate_wkt(const std::string& w) {
  static const char* const kHeads[] = {"PROJCS",  "GEOGCS",   "GEOCCS",  "COMPD_CS", "VERT_CS",
                                       "LOCAL_CS", "PROJCRS", "GEOGCRS", "GEODCRS",  "COMPOUNDCRS",
                                       "VERTCRS",  "ENGCRS",  "BOUNDCRS"};
  std::string head;
  for (char c : w) {
    if (c == '[' || c == '(') break;
    head.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  bool known = false;
  for (const char* h : kHeads) known = known || (head == h);
  if (!known) {
    return set_last_error(ScanError::kInvalidArgument,
                          "Engine::set_crs: '%s' does not start with an OGC WKT CRS keyword "
                          "(PROJCS/GEOGCS/COMPD_CS/… or their WKT2 spellings) — a PROJ.4 string "
                          "or a bare EPSG code is not a WKT",
                          head.empty() ? w.c_str() : head.c_str());
  }
  int depth = 0;
  bool in_quotes = false, saw_name = false, deepest_ok = true;
  for (std::size_t i = 0; i < w.size(); ++i) {
    const char c = w[i];
    if (c == '"') {
      in_quotes = !in_quotes;
      if (in_quotes && depth == 1 && !saw_name) saw_name = true;
      continue;
    }
    if (in_quotes) continue;
    if (c == '[' || c == '(') ++depth;
    if (c == ']' || c == ')') {
      if (--depth < 0) deepest_ok = false;
    }
  }
  if (in_quotes || depth != 0 || !deepest_ok) {
    return set_last_error(ScanError::kInvalidArgument,
                          "Engine::set_crs: the WKT's brackets/quotes do not balance — it is "
                          "truncated or corrupted (a LAS VLR would embed it verbatim)");
  }
  if (!saw_name) {
    return set_last_error(ScanError::kInvalidArgument,
                          "Engine::set_crs: the WKT has no quoted CRS name");
  }
  return kOkStatus;
}
}  // namespace

const char* to_string(TrajectorySource s) noexcept {
  switch (s) {
    case TrajectorySource::kExternal: return "external";
    case TrajectorySource::kGnss: return "gnss";
  }
  return "?";
}

const char* engine_version_string() {
  static std::string s = std::string("scanengine " SCANENGINE_VERSION " (clock: ") +
                         SteadyClock::backend_name() + ")";
  return s.c_str();
}

namespace {

// The kRtkRover device (docs/A10-gnss.md §9.3 item 1).
//
// It owns no transport and no decoder: the app reads the rover's Bluetooth SPP
// / USB-serial link and pushes the bytes in, exactly as it does for the D6, and
// the Engine's single GnssSource does the framing and the epoch assembly. So
// this adapter is deliberately thin — it exists to give the rover a DeviceId,
// a DeviceState and a health row, which is what the rest of the engine (and
// B9's device list) is built around.
//
// Health mapping, and why it is the NMEA stats and not the fix state: this row
// answers "is the LINK working", which is a different question from "is the sky
// working". A rover sitting under a bridge streams perfect NMEA with fix 0, and
// that is kStreaming with a health row full of good checksums — the fix quality
// reaches the UI through EventType::kGnssFix and GnssStats::by_fix instead.
class RtkRoverDriver final : public Driver {
 public:
  RtkRoverDriver(DeviceId id, GnssSource* gnss, const DriverContext& ctx)
      : id_(id), gnss_(gnss), ctx_(ctx) {}

  const char* name() const override { return "rtk-rover"; }
  DeviceKind kind() const override { return DeviceKind::kRtkRover; }
  DeviceId id() const override { return id_; }

  Status start() override {
    const Status s = gnss_->start();
    if (!s.ok()) {
      set_state_(DeviceState::kFault, s.error());
      return s;
    }
    set_state_(DeviceState::kStarting, ScanError::kOk);
    return kOkStatus;
  }

  Status stop() override {
    // stop() flushes the pending epoch, which is the one place a 1 Hz
    // receiver's last fix would otherwise be lost.
    (void)gnss_->stop();
    set_state_(DeviceState::kIdle, ScanError::kOk);
    return kOkStatus;
  }

  DeviceState state() const override {
    std::lock_guard<std::mutex> lock(m_);
    return state_;
  }

  DeviceHealth health() const override {
    const GnssStats gs = gnss_->stats();
    DeviceHealth h{};
    h.id = id_;
    h.kind = DeviceKind::kRtkRover;
    h.state = state();
    h.last_error = last_error_;
    h.bytes_in = gs.nmea.bytes_in;
    h.packets_ok = gs.nmea.sentences_ok;
    h.packets_bad = gs.nmea.checksum_failed + gs.nmea.malformed + gs.nmea.oversize;
    h.points_out = gs.fixes_published;  // a rover's "points" are its fixes
    h.drops = gs.overwritten;
    h.checksum_pass_rate = gs.nmea.checksum_pass_rate();
    h.t_last_data_ns = gs.nmea.t_last_sentence_ns;
    return h;
  }

  Status push_bytes(ByteSpan bytes, TimePoint t_arrival) override {
    const std::int64_t t = t_arrival.nanos != 0 ? t_arrival.nanos : ctx_.clock().nanos;
    const Status s = gnss_->push_nmea(bytes, t);
    // First bytes through: the device is streaming. A rover has no handshake,
    // so arrival IS the transition.
    if (s.ok()) set_state_(DeviceState::kStreaming, ScanError::kOk);
    return s;
  }

 private:
  void set_state_(DeviceState next, ScanError err) {
    DeviceState prev;
    {
      std::lock_guard<std::mutex> lock(m_);
      if (state_ == next) return;
      prev = state_;
      state_ = next;
      last_error_ = err;
    }
    SCAN_LOG_INFO(kMod, "device %u (rtk-rover): %s -> %s", id_, to_string(prev), to_string(next));
    if (ctx_.bus == nullptr) return;
    DeviceStatePayload p{};
    p.device = id_;
    p.kind = DeviceKind::kRtkRover;
    p.state = next;
    p.previous = prev;
    p.error = err;
    ctx_.bus->publish(EventType::kDeviceState, p);
  }

  DeviceId id_;
  GnssSource* gnss_;
  DriverContext ctx_;
  mutable std::mutex m_;
  DeviceState state_ = DeviceState::kIdle;
  ScanError last_error_ = ScanError::kOk;
};

}  // namespace

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
  std::atomic<TrajectorySource> trajectory{TrajectorySource::kExternal};

  // --- A10: the GNSS/RTK stack --------------------------------------------
  //
  // All three are Engine-lifetime, like the pose source and for the same
  // reason: the rover is paired, the caster joined and RTK Fixed reached
  // BEFORE the session starts (§3.4's capture gate is that decision).
  //
  // Each object is internally synchronized, so no engine-level mutex guards
  // them. `gnss_m` guards only the two pieces of engine-owned state hanging
  // off their callbacks: the app's RTCM sink and the last published
  // convergence, both of which are written from the NTRIP/GNSS threads and
  // read from the control thread.
  std::unique_ptr<GnssSource> gnss;
  std::unique_ptr<TcpNtripClient> ntrip;
  std::unique_ptr<GeorefFusion> georef;
  mutable std::mutex gnss_m;
  Engine::RtcmSink rtcm_sink = nullptr;
  void* rtcm_user = nullptr;
  bool georef_converged = false;
  // The §3.4 EPSG-picker override (Engine::set_crs). Guarded by gnss_m, which
  // is the lock the rest of the CRS surface already reads under.
  std::string crs_override_epsg;
  std::string crs_override_wkt;

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

  // --- A15's job queue, created on first Engine::jobs() (INT-34) ----------
  //
  // Lazy because it starts a worker thread and most engines never submit a
  // job. Guarded by `m` for construction only; JobQueue is itself thread-safe
  // afterwards. Destroyed explicitly at the top of ~Engine — a running job
  // touches the bus, the page store and (through the post pipeline) the
  // recorder, all of which are declared above it.
  std::unique_ptr<jobs::JobQueue> job_queue;

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
    p.update_kind = static_cast<std::uint8_t>(u.kind);
    self->bus.publish(EventType::kPointsAvailable, p);

    // A recolour republishes a range that already went through the odometry
    // once (INT-34). Only the r/g/b/a bytes changed, and feeding the same
    // points to the LIO a second time would insert duplicate geometry into
    // the voxel map, so the forwarding below is append-only.
    if (u.kind != PageUpdateKind::kAppended) return;

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
  //
  // A15's job worker goes FIRST (INT-34). A running job publishes progress on
  // the bus and appends points into the page store, and a kPostProcess job
  // reads the .lscan the recorder may still be writing — all of which are
  // members declared before it and would otherwise be destroyed underneath
  // it. ~JobQueue calls stop(), which drains the queue and joins the worker.
  impl_->job_queue.reset();
  (void)stop_session();
  // A10: the NTRIP worker is the one thread that outlives a session, and its
  // RTCM handler reaches the recorder and the event bus. Join it FIRST —
  // Impl's members are destroyed in reverse declaration order, which would
  // otherwise free `recorder` while that thread is still writing to it.
  if (impl_->ntrip) (void)impl_->ntrip->disconnect();
  if (impl_->gnss) (void)impl_->gnss->stop();
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

  // --- A10: GnssSource + TcpNtripClient + GeorefFusion, wired together -----
  //
  // docs/A10-gnss.md §9.3. Three objects, four connections, and none of them
  // needs a thread from the Engine: the fix callback runs on whichever thread
  // pushed the NMEA, and the NTRIP client brings its own receive thread.
  Engine* raw_engine = e.get();
  {
    GnssSourceConfig gc = cfg.gnss;
    gc.stream = StreamId::kGnss;         // wiring, not a choice
    gc.timesync = &e->impl_->timesync;   // §3.2: NMEA time via arrival correlation
    e->impl_->gnss = std::make_unique<GnssSource>(gc);
  }
  e->impl_->georef = std::make_unique<GeorefFusion>(cfg.georef);
  e->impl_->ntrip = std::make_unique<TcpNtripClient>();

  // The local trajectory the transform is estimated against. Deliberately the
  // ExternalPoseSource and not the GnssSource: pairing GNSS against GNSS is
  // degenerate by construction. An integrator with A6's LIO track points this
  // somewhere else with set_georef_local_source().
  e->impl_->georef->set_local_source(e->impl_->poses.get());

  e->impl_->gnss->set_fix_callback(
      [raw_engine](const GnssFix& f) { raw_engine->on_gnss_fix_(f); });

  // GGA upload: the rover's OWN last sentence, verbatim, so a VRS caster sees
  // exactly what a stand-alone rover would send (docs/A10-gnss.md §8).
  {
    GnssSource* src = e->impl_->gnss.get();
    e->impl_->ntrip->set_gga_provider([src](std::string* out) {
      *out = src->last_gga_sentence();
      return !out->empty();
    });
  }

  // Corrections out. The engine records every frame it forwards (record-always
  // applies to the RTCM leg too: a replay that re-runs the rover's RTK engine
  // needs the same corrections the capture had) and then hands it to whatever
  // the app installed with set_rtcm_sink().
  {
    Impl* impl = e->impl_.get();
    e->impl_->ntrip->set_rtcm_handler([impl](ByteSpan rtcm) {
      {
        std::lock_guard<std::mutex> lock(impl->record_m);
        if (impl->recorder->is_open()) {
          (void)impl->recorder->write_chunk(lscan::ChunkType::kGnssRtcm,
                                            SteadyClock::now().nanos, rtcm);
        }
      }
      Engine::RtcmSink cb = nullptr;
      void* user = nullptr;
      {
        std::lock_guard<std::mutex> lock(impl->gnss_m);
        cb = impl->rtcm_sink;
        user = impl->rtcm_user;
      }
      // Outside every engine lock: this is the app's Bluetooth write, and a
      // slow one must not stall the recorder or the control thread.
      if (cb != nullptr) cb(rtcm, user);
    });
    e->impl_->ntrip->set_state_callback([impl](NtripState s, ScanError err) {
      const NtripStats st = impl->ntrip->stats();
      NtripStatePayload p{};
      p.state = static_cast<std::uint8_t>(s);
      p.error = err;
      p.backoff_ms = st.backoff_ms;
      p.bytes_received = st.bytes_received;
      p.correction_age_s = impl->ntrip->correction_age_s();
      impl->bus.publish(EventType::kNtripState, p);
    });
  }
  (void)e->impl_->gnss->start();

  SCAN_LOG_INFO(kMod, "%s created for '%s' (pages: %u × %u pts)", engine_version_string(),
                cfg.app_name.c_str(), e->impl_->points->config().max_pages,
                e->impl_->points->config().page_capacity);
  return e;
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
    // A10 §9.3 item 3 / spec §3.3's "Desktop D6 capture: no ARCore →
    // RTK-trajectory mode only". Not a code path: GnssSource IS a
    // PoseInterpolator, so the assembler cannot tell the two apart.
    impl_->pushbroom->set_pose_source(cfg.trajectory == TrajectorySource::kGnss
                                          ? static_cast<const PoseInterpolator*>(impl_->gnss.get())
                                          : static_cast<const PoseInterpolator*>(
                                                impl_->poses.get()));
  }
  impl_->trajectory.store(cfg.trajectory, std::memory_order_release);
  impl_->pushbroom_on.store(cfg.pushbroom, std::memory_order_release);
  if (cfg.pushbroom && !impl_->pushbroom->has_mount_extrinsics()) {
    SCAN_LOG_WARN(kMod,
                  "pushbroom enabled without a mount extrinsic; assembly stays pending until "
                  "set_mount_extrinsics() is called (docs/A8-pushbroom.md §4)");
  }

  // The live window starts empty — but ONLY for an app that opted into live
  // page eviction (set_live_page_eviction), i.e. one that has told the engine
  // this store is a live capture's view and not a post-processing workspace.
  //
  // Why it must: every start_session() builds a NEW LioOdometry whose first
  // pose is the origin, so the pages the previous session left behind are in a
  // frame that no longer exists — they would sit in the live map misregistered
  // against everything the new session adds. And they are not free: before
  // this, a preview + N record cycles on ONE connect all stacked into the same
  // 64 pages, which is most of why the field session hit the ceiling during a
  // PREVIEW. recycle_all() is used rather than clear() because a renderer may
  // be reading a PageView right now: it retires the pages without freeing a
  // byte (page_store.h).
  if (impl_->points->stats().when_full == PageFullPolicy::kEvictOldest) {
    impl_->points->recycle_all();
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
      // A10 §9.3 item 1: route it to the Engine's one GnssSource. There is no
      // per-device GNSS config here on purpose — the source is Engine-lifetime
      // (EngineConfig::gnss configures it) because the operator pairs the rover
      // and waits for RTK Fixed before any session exists. A second rover would
      // need the source keyed by DeviceId; the spec's rigs have one.
      driver = std::make_unique<RtkRoverDriver>(id, impl_->gnss.get(), ctx);
      break;
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

Result<Mid360Stats> Engine::mid360_stats(DeviceId id) const {
  std::lock_guard<std::mutex> lock(impl_->m);
  Driver* d = impl_->find(id);
  if (d == nullptr) return set_last_error(ScanError::kNotFound, "no device %u", id);
  // dynamic_cast rather than a virtual on Driver: these counters are the
  // Mid-360's own vocabulary (a forced SDK re-init has no D6 analogue), and a
  // virtual returning a per-driver struct would be a Driver interface that
  // knows about every driver.
  auto* m = dynamic_cast<Mid360Driver*>(d);
  if (m == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "device %u is %s, not a Mid-360", id,
                          to_string(d->kind()));
  }
  return m->stats();
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
  //
  // A10: the rover's NMEA takes the same route, as kGnssNmea chunks. The chunk
  // is the pushed BUFFER, not one sentence — exactly the kD6Raw contract —
  // because the Bluetooth SPP link hands the app 20–990-byte MTU fragments and
  // re-framing them here would mean parsing before recording, which is the one
  // thing record-always forbids. GnssSource::push_nmea frames arbitrary chunks,
  // so a replay that feeds these back byte for byte reproduces the capture.
  const DeviceKind kind = d->kind();
  if (kind == DeviceKind::kD6 || kind == DeviceKind::kRtkRover) {
    std::lock_guard<std::mutex> rlock(impl_->record_m);
    if (impl_->recorder->is_open()) {
      const std::int64_t t = t_arrival.nanos != 0 ? t_arrival.nanos : SteadyClock::now().nanos;
      (void)impl_->recorder->write_chunk(
          kind == DeviceKind::kD6 ? lscan::ChunkType::kD6Raw : lscan::ChunkType::kGnssNmea, t,
          bytes);
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

Status Engine::set_trajectory_source(TrajectorySource src) {
  const PoseInterpolator* p = nullptr;
  switch (src) {
    case TrajectorySource::kGnss: p = impl_->gnss.get(); break;
    case TrajectorySource::kExternal: p = impl_->poses.get(); break;
  }
  if (p == nullptr) return set_last_error(ScanError::kInvalidArgument, "unknown trajectory source");
  {
    std::lock_guard<std::mutex> lock(impl_->pushbroom_m);
    impl_->pushbroom->set_pose_source(p);
  }
  impl_->trajectory.store(src, std::memory_order_release);
  SCAN_LOG_INFO(kMod, "pushbroom trajectory -> %s", to_string(src));
  return kOkStatus;
}

TrajectorySource Engine::trajectory_source() const {
  return impl_->trajectory.load(std::memory_order_acquire);
}

// --- A10: GNSS / RTK --------------------------------------------------------

// One closed epoch: the fix is already in the source's ring, and this runs on
// whichever thread pushed the NMEA (the app's rover reader), with no GnssSource
// lock held — which is what lets add_fix() turn around and interpolate the
// local trajectory without deadlocking.
void Engine::on_gnss_fix_(const GnssFix& fix) {
  // The fusion's global frame is the source's ENU frame, anchored on the first
  // fix at or above min_fix_for_origin and then never moved. It can only be
  // installed once the origin exists, so this is checked per fix rather than at
  // create() time.
  if (!impl_->georef->has_frame() && impl_->gnss->has_origin()) {
    (void)impl_->georef->set_enu_frame(impl_->gnss->enu_frame());
  }
  // kAgain simply means the local pose for this instant has not arrived yet;
  // the fusion counts it (skipped_no_pose) and the next fix tries again.
  (void)impl_->georef->add_fix(fix);

  GnssFixPayload p{};
  p.fix_type = static_cast<std::uint8_t>(fix.fix);
  p.satellites = fix.satellites;
  p.hdop = fix.hdop;
  p.correction_age_s = fix.correction_age_s;
  p.sigma_h_m = fix.sigma_horizontal_m;
  p.lat_deg = fix.lat_deg;
  p.lon_deg = fix.lon_deg;
  p.alt_m = fix.alt_m;
  impl_->bus.publish(EventType::kGnssFix, p, fix.t_mono_ns);

  // The moment the session becomes exportable in a real CRS — and the moment it
  // stops being, which matters just as much to a UI that already said it was.
  const GeorefSolution sol = impl_->georef->solution();
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(impl_->gnss_m);
    if (sol.converged != impl_->georef_converged) {
      impl_->georef_converged = sol.converged;
      changed = true;
    }
  }
  if (!changed) return;
  GeorefConvergedPayload g{};
  g.cep95_m = sol.cep95_m;
  g.horizontal_sigma_m = sol.horizontal_sigma_m;
  g.samples = static_cast<std::uint32_t>(sol.inliers);
  g.epsg = impl_->georef->epsg();
  g.converged = sol.converged ? 1 : 0;
  impl_->bus.publish(EventType::kGeorefConverged, g, fix.t_mono_ns);
  SCAN_LOG_INFO(kMod, "georef %s: CEP95 %.3f m over %u fixes (%s)",
                sol.converged ? "converged" : "lost convergence", sol.cep95_m, g.samples,
                sol.converged ? impl_->georef->epsg_string().c_str() : sol.blocker);
}

GnssSource& Engine::gnss() { return *impl_->gnss; }
const GnssSource& Engine::gnss() const { return *impl_->gnss; }
TcpNtripClient& Engine::ntrip() { return *impl_->ntrip; }
const TcpNtripClient& Engine::ntrip() const { return *impl_->ntrip; }
GeorefFusion& Engine::georef() { return *impl_->georef; }
const GeorefFusion& Engine::georef() const { return *impl_->georef; }

void Engine::set_rtcm_sink(RtcmSink cb, void* user_data) {
  std::lock_guard<std::mutex> lock(impl_->gnss_m);
  impl_->rtcm_sink = cb;
  impl_->rtcm_user = user_data;
}

GnssFix Engine::last_fix() const { return impl_->gnss->last_fix(); }
GnssStats Engine::gnss_stats() const { return impl_->gnss->stats(); }
GeorefSolution Engine::georef_solution() const { return impl_->georef->solution(); }
// Gated on convergence, and GeorefFusion::crs_wkt() is not — deliberately, and
// the two answer different questions.
//
// `GeorefFusion::crs_wkt()` answers "what CRS is this SITE in", which is known
// as soon as an ENU origin exists: the UTM zone is a property of where the
// rover is standing. `Engine::crs_wkt()` is the A9 export seam, and it answers
// "what CRS may I LABEL this cloud with" — which additionally needs the
// local→global transform, because until that converges the points are still in
// the local frame. Handing A9 a UTM WKT for a local-frame cloud would produce a
// file that opens in QGIS, lands in the wrong hemisphere, and never says why.
// Empty is exactly A9's documented "embed the local-frame placeholder" input.
std::string Engine::crs_wkt() const {
  if (!impl_->georef->converged()) return std::string();
  {
    std::lock_guard<std::mutex> lock(impl_->gnss_m);
    if (!impl_->crs_override_wkt.empty()) return impl_->crs_override_wkt;
    if (!impl_->crs_override_epsg.empty()) {
      // An EPSG the engine CAN render, supplied without a WKT.
      return crs::wkt1_for_epsg(crs::parse_epsg_string(impl_->crs_override_epsg));
    }
  }
  return impl_->georef->crs_wkt();
}

std::string Engine::crs_epsg() const {
  if (!impl_->georef->converged()) return std::string();
  {
    std::lock_guard<std::mutex> lock(impl_->gnss_m);
    if (!impl_->crs_override_epsg.empty()) return impl_->crs_override_epsg;
    // A caller-supplied WKT with no EPSG is legal (some grids have no code);
    // reporting the auto-UTM zone beside it would name a CRS the exported WKT
    // is not, so the code is empty rather than wrong.
    if (!impl_->crs_override_wkt.empty()) return std::string();
  }
  return impl_->georef->epsg_string();
}

// The survey profile's escape hatch. See engine.h for the contract; this is
// the validation, which is the whole of the implementation.
Status Engine::set_crs(const std::string& epsg, const std::string& wkt) {
  const std::string e = trim_copy(epsg);
  const std::string w = trim_copy(wkt);

  if (e.empty() && w.empty()) {  // clear
    std::lock_guard<std::mutex> lock(impl_->gnss_m);
    impl_->crs_override_epsg.clear();
    impl_->crs_override_wkt.clear();
    SCAN_LOG_INFO(kMod, "CRS override cleared (back to auto-UTM)");
    return kOkStatus;
  }

  int code = 0;
  if (!e.empty()) {
    code = crs::parse_epsg_string(e);
    if (code <= 0) {
      return set_last_error(ScanError::kInvalidArgument,
                            "Engine::set_crs: '%s' is not an EPSG code (expected \"EPSG:2326\" "
                            "or \"2326\")",
                            e.c_str());
    }
  }
  if (!w.empty()) {
    SCAN_TRY(validate_wkt(w));
  } else if (crs::wkt1_for_epsg(code).empty()) {
    // The combination that would silently produce an unlabelled export.
    return set_last_error(ScanError::kInvalidArgument,
                          "Engine::set_crs: this engine cannot render a WKT for EPSG:%d (it "
                          "knows WGS 84 and the UTM zones only — gnss/crs.h explains why there "
                          "is no PROJ here), so a national grid must arrive WITH the WKT its "
                          "geodetic authority publishes",
                          code);
  }

  std::lock_guard<std::mutex> lock(impl_->gnss_m);
  impl_->crs_override_epsg = e.empty() ? std::string() : crs::epsg_string(code);
  impl_->crs_override_wkt = w;
  SCAN_LOG_INFO(kMod, "CRS override set: %s%s",
                impl_->crs_override_epsg.empty() ? "(no EPSG)" : impl_->crs_override_epsg.c_str(),
                w.empty() ? "" : " with a caller-supplied WKT");
  return kOkStatus;
}

std::string Engine::configured_crs_epsg() const {
  std::lock_guard<std::mutex> lock(impl_->gnss_m);
  return impl_->crs_override_epsg;
}

std::string Engine::configured_crs_wkt() const {
  std::lock_guard<std::mutex> lock(impl_->gnss_m);
  return impl_->crs_override_wkt;
}

void Engine::set_georef_local_source(const PoseInterpolator* src) {
  impl_->georef->set_local_source(src != nullptr ? src : impl_->poses.get());
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

Status Engine::record_keyframe(const Keyframe& kf) {
  // Encode (which validates) BEFORE taking the record lock: a rejected
  // keyframe must not stall the three other producers behind it.
  std::vector<std::uint8_t> rec;
  SCAN_TRY(color::encode_keyframe_record(kf, &rec));

  std::lock_guard<std::mutex> lock(impl_->record_m);
  if (!impl_->recorder->is_open()) {
    return set_last_error(ScanError::kInvalidState,
                          "engine: record_keyframe with no recording session open");
  }
  // ChunkType::kCameraFrameIndex on StreamId::kCameraFrames —
  // lscan::stream_file_of() already routes that to
  // "streams/frames/frames.idx", so this is byte-identical to what A11's
  // standalone KeyframeIndexWriter produces (asserted by test_color.cpp's
  // fidx/is_byte_identical_to_what_A5s_recorder_writes).
  return impl_->recorder->write_chunk(lscan::ChunkType::kCameraFrameIndex, kf.t_mono_ns,
                                      ByteSpan(rec.data(), rec.size()));
}

jobs::JobQueue& Engine::jobs() {
  std::lock_guard<std::mutex> lock(impl_->m);
  if (!impl_->job_queue) {
    // Constructed with this engine's bus, which is what makes
    // EventType::kJobProgress land on the same subscription as every other
    // event instead of on a second, app-invented one.
    impl_->job_queue = std::make_unique<jobs::JobQueue>(&impl_->bus);
  }
  return *impl_->job_queue;
}

EventBus& Engine::events() { return impl_->bus; }
Status Engine::set_live_page_eviction(bool enabled) {
  return impl_->points->set_full_policy(enabled ? PageFullPolicy::kEvictOldest
                                                : PageFullPolicy::kReject);
}

bool Engine::live_page_eviction() const {
  return impl_->points->stats().when_full == PageFullPolicy::kEvictOldest;
}

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
