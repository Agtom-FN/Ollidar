// test_round14_session_reset.cpp — ROUND 14: a capture starts from zero.
//
// > "does the origin and IMU data offset zero every time when the capture
// >  starts?"
//
// It did not, and the proof was already in the owner's hands. scan-033, -034
// and -035 are three captures from ONE app run, and their manifests say the
// rig had 1, 3 and 6 sensors respectively — for a rig that never had more than
// two. Nothing had grown a sensor: `FileRecordWriter::add_sensor()` APPENDS and
// the writer outlives the containers it writes, so each capture described
// itself with its own devices plus every earlier capture's.
//
// That is the visible half. The invisible half is worse, because it moves
// geometry rather than metadata: `Engine::start_session()` also left the ARCore
// pose ring, the gyro densifier's bias estimate and the GNSS/georef ENU origin
// alive from the previous capture, so capture N+1 could bracket a return
// against capture N's trajectory, integrate with capture N's bias, and measure
// itself against capture N's local zero.
//
// The four cases below are one per leak, plus ROUND 12's backlog item: the
// densifier's fallback reasons never summed to `fallbacks` — on scan-034,
// 63,805 fallbacks of which only 11,522 had a reason — because the commonest
// bucket of all, "the pose source had nothing usable here", had no counter.
//
// Everything here is live-session lifecycle. None of it touches the offline
// resolve path, and the round's selfcheck numbers on scan-033/034/035 are
// unchanged to the last digit.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/core/engine.h"
#include "scanengine/gnss/nmea.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/poses/imu_densified_pose.h"
#include "scanengine/record/lscan.h"

using namespace scanengine;
namespace fs = std::filesystem;

namespace {

std::string fresh_dir(const char* tag) {
  static std::atomic<long long> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path p = fs::temp_directory_path() /
                     (std::string("scanengine-r14-") + tag + "_" + std::to_string(now) + "_" +
                      std::to_string(id));
  std::error_code ec;
  fs::remove_all(p, ec);
  return p.string();
}

struct TempDirGuard {
  std::string path;
  explicit TempDirGuard(std::string p) : path(std::move(p)) {}
  ~TempDirGuard() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDirGuard(const TempDirGuard&) = delete;
  TempDirGuard& operator=(const TempDirGuard&) = delete;
};

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The `"sensors": [...]` array of a manifest, verbatim. Deliberately textual:
// the point of the case is that two containers carry the SAME bytes there, and
// a parser would give this test an opinion about JSON it does not need.
std::string sensors_array(const std::string& lscan_dir) {
  const std::string text = slurp(lscan_dir + "/manifest.json");
  const std::size_t key = text.find("\"sensors\"");
  REQUIRE(key != std::string::npos);
  const std::size_t open = text.find('[', key);
  const std::size_t close = text.find(']', open);
  REQUIRE(open != std::string::npos);
  REQUIRE(close != std::string::npos);
  return text.substr(open, close - open + 1);
}

std::size_t count_entries(const std::string& sensors) {
  std::size_t n = 0;
  for (std::size_t i = sensors.find("\"id\""); i != std::string::npos;
       i = sensors.find("\"id\"", i + 1)) {
    ++n;
  }
  return n;
}

Pose ar_pose(std::int64_t t_ns, bool tracking_lost = false) {
  Pose p;
  p.t_mono_ns = t_ns;
  p.position[0] = 0.0;
  p.position[1] = 0.0;
  p.position[2] = 1.35;
  p.orientation[0] = 0.0;
  p.orientation[1] = 0.0;
  p.orientation[2] = 0.0;
  p.orientation[3] = 1.0;
  p.source = StreamId::kPoseAr;
  p.quality = PoseQuality::kGood;
  p.tracking_lost = tracking_lost ? 1 : 0;
  return p;
}

constexpr std::int64_t kMs = 1'000'000LL;

// --- NMEA, the same shape tests/test_engine.cpp builds ----------------------
std::string nmea_line(const std::string& body) {
  char cs[8];
  std::snprintf(cs, sizeof(cs), "*%02X", nmea::checksum_of(body));
  return "$" + body + cs + "\r\n";
}

std::string nmea_dm(double deg, int int_width) {
  const double a = std::fabs(deg);
  char b[32];
  std::snprintf(b, sizeof(b), "%0*d%09.6f", int_width, static_cast<int>(a),
                (a - static_cast<int>(a)) * 60.0);
  return b;
}

// One RTK-fixed epoch. Sharing a UTC across the sentences is what closes it
// (docs/A10-gnss.md §7); a DIFFERENT UTC on the next one is what publishes it.
std::string nmea_epoch(int sod, double lat, double lon) {
  const int hh = sod / 3600, mm = (sod / 60) % 60, ss = sod % 60;
  char t[16];
  std::snprintf(t, sizeof(t), "%02d%02d%02d.00", hh, mm, ss);
  char buf[256];
  std::snprintf(buf, sizeof(buf), "GNGGA,%s,%s,%c,%s,%c,4,22,0.6,50.00,M,-2.0,M,,", t,
                nmea_dm(lat, 2).c_str(), lat < 0 ? 'S' : 'N', nmea_dm(lon, 3).c_str(),
                lon < 0 ? 'W' : 'E');
  return nmea_line(buf);
}

Status push_epoch(Engine& e, DeviceId id, const std::string& text, std::int64_t t_ns) {
  return e.push_serial_bytes(
      id, ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
      TimePoint{t_ns});
}

}  // namespace

// ===========================================================================
// 1. THE ONE THAT SHIPPED — a container describes ITS OWN capture
// ===========================================================================
//
// Fails against pre-ROUND-14 code: the second manifest carries four sensor
// entries (this capture's two, plus the first capture's two).

TEST_CASE("round14/session/two_captures_describe_themselves_and_not_each_other") {
  const std::string root = fresh_dir("sensors");
  TempDirGuard guard(root);

  EngineConfig ec{};
  auto engine = Engine::create(ec);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  DeviceConfig d6{};
  d6.kind = DeviceKind::kD6;
  d6.d6.send_start_stop_commands = false;
  const auto d6_id = e.add_device(d6);
  REQUIRE(d6_id.ok());

  DeviceConfig rtk{};
  rtk.kind = DeviceKind::kRtkRover;
  const auto rtk_id = e.add_device(rtk);
  REQUIRE(rtk_id.ok());

  const auto run = [&](const std::string& dir) {
    SessionConfig sc;
    sc.record = true;
    sc.pushbroom = false;
    sc.lscan_dir = dir;
    sc.profile = "quickscan";
    REQUIRE(e.start_session(sc).ok());
    REQUIRE(e.stop_session().ok());
    return sensors_array(dir);
  };

  const std::string first = run(root + "/scan-a.lscan");
  const std::string second = run(root + "/scan-b.lscan");
  const std::string third = run(root + "/scan-c.lscan");

  // Two devices attached, two entries — in every capture of the run, not just
  // the first one.
  CHECK(count_entries(first) == 2);
  CHECK(count_entries(second) == 2);
  CHECK(count_entries(third) == 2);
  CHECK(second == first);
  CHECK(third == first);
  CHECK(first.find("coin-d6") != std::string::npos);
  CHECK(first.find("rtk-rover") != std::string::npos);

  // "The devices attached for THAT session" is the actual claim, so a rig that
  // loses its rover between captures must say so.
  REQUIRE(e.remove_device(rtk_id.value()).ok());
  const std::string fourth = run(root + "/scan-d.lscan");
  CHECK(count_entries(fourth) == 1);
  CHECK(fourth.find("coin-d6") != std::string::npos);
  CHECK(fourth.find("rtk-rover") == std::string::npos);

  MESSAGE("manifest sensors: " << first << " then " << fourth);
}

// ===========================================================================
// 2. THE POSE RING — capture N's trajectory is not capture N+1's
// ===========================================================================
//
// The ring is emptied at the END of stop_session(), not at start_session(), so
// the second half of this case is as load-bearing as the first: an app pushes
// poses while it is still lining the scan up, and that lead-in is what brackets
// the very first return of the capture. Clearing at Start would throw it away
// and break docs/A8-pushbroom.md §3.6's "push the whole trajectory up front".

TEST_CASE("round14/session/the_pose_ring_does_not_survive_into_the_next_capture") {
  EngineConfig ec{};
  auto engine = Engine::create(ec);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  const std::int64_t t0 = 5'000'000'000LL;

  SessionConfig sc;
  sc.record = false;
  sc.pushbroom = false;
  REQUIRE(e.start_session(sc).ok());

  for (int i = 0; i < 30; ++i) REQUIRE(e.push_pose(ar_pose(t0 + i * 33 * kMs)).ok());
  const std::int64_t t_inside = t0 + 15 * 33 * kMs + 7 * kMs;
  const PoseSample during = e.pose_at(t_inside);
  REQUIRE(during.has_pose);
  CHECK(during.gate == PoseGate::kOk);
  CHECK(e.poses().size() == 30);

  REQUIRE(e.stop_session().ok());

  // Nothing of that capture is left to interpolate.
  CHECK(e.poses().size() == 0);
  const PoseSample orphan = e.pose_at(t_inside);
  CHECK_FALSE(orphan.has_pose);
  CHECK(orphan.gate == PoseGate::kNoData);

  // A second capture cannot be served capture 1's poses, whether it asks
  // before its own trajectory arrives or after.
  REQUIRE(e.start_session(sc).ok());
  const PoseSample stale = e.pose_at(t_inside);
  CHECK_FALSE(stale.has_pose);
  CHECK(stale.gate == PoseGate::kNoData);

  const std::int64_t t1 = t0 + 600'000'000'000LL;  // ten minutes later
  for (int i = 0; i < 30; ++i) REQUIRE(e.push_pose(ar_pose(t1 + i * 33 * kMs)).ok());
  const PoseSample still_stale = e.pose_at(t_inside);
  CHECK_FALSE(still_stale.has_pose);
  CHECK(still_stale.gate == PoseGate::kBeforeFirst);
  REQUIRE(e.stop_session().ok());

  // And the lead-in a real app pushes BEFORE Start is still there when the
  // capture begins — this is why the clear lives at Stop.
  const std::int64_t t2 = t1 + 600'000'000'000LL;
  for (int i = 0; i < 30; ++i) REQUIRE(e.push_pose(ar_pose(t2 + i * 33 * kMs)).ok());
  REQUIRE(e.start_session(sc).ok());
  CHECK(e.poses().size() == 30);
  const PoseSample lead_in = e.pose_at(t2 + 15 * 33 * kMs + 7 * kMs);
  CHECK(lead_in.has_pose);
  CHECK(lead_in.gate == PoseGate::kOk);
  REQUIRE(e.stop_session().ok());
}

// ===========================================================================
// 3. THE GYRO BIAS — an estimate about this rig over this minute
// ===========================================================================
//
// Fails against pre-ROUND-14 code, which only ever cleared the densifier as a
// SIDE EFFECT of set_imu_extrinsics() rebuilding it. This case deliberately
// does NOT re-apply the extrinsic between the two captures, which is exactly
// the case the side effect never covered.

TEST_CASE("round14/session/the_gyro_bias_is_re_estimated_by_every_capture") {
  EngineConfig ec{};
  auto engine = Engine::create(ec);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  const double identity[4] = {0.0, 0.0, 0.0, 1.0};
  REQUIRE(e.set_imu_extrinsics(identity).ok());

  SessionConfig sc;
  sc.record = false;
  sc.pushbroom = false;
  REQUIRE(e.start_session(sc).ok());

  // A still rig whose gyro reads a constant 0.01 rad/s about +x: ARCore says
  // the orientation never changed, so the whole closing error IS the bias and
  // the estimator should walk straight to it.
  const std::int64_t t0 = 5'000'000'000LL;
  const float gyro[3] = {0.01f, 0.0f, 0.0f};
  const float accel[3] = {0.0f, 0.0f, 9.81f};
  for (std::int64_t t = t0 - 50 * kMs; t <= t0 + 1000 * kMs; t += 2'500'000LL) {
    REQUIRE(e.push_phone_imu(t, gyro, accel).ok());
  }
  for (int i = 0; i <= 30; ++i) REQUIRE(e.push_pose(ar_pose(t0 + i * 33 * kMs)).ok());
  for (int i = 0; i < 30; ++i) {
    (void)e.densified_pose_at(t0 + i * 33 * kMs + 16 * kMs);
  }

  const ImuDensifyStats first = e.imu_densify_stats();
  REQUIRE(first.densified > 0);
  REQUIRE(first.bias_updates > 0);
  CHECK(first.samples_in > 400);
  CHECK(std::fabs(first.bias_rad_s[0]) > 1e-4);
  MESSAGE("capture 1: bias " << first.bias_rad_s[0] << " rad/s from " << first.bias_updates
                             << " updates, " << first.densified << " densified");

  REQUIRE(e.stop_session().ok());
  REQUIRE(e.start_session(sc).ok());

  const ImuDensifyStats second = e.imu_densify_stats();
  CHECK(second.bias_rad_s[0] == 0.0);
  CHECK(second.bias_rad_s[1] == 0.0);
  CHECK(second.bias_rad_s[2] == 0.0);
  CHECK(second.samples_in == 0);
  CHECK(second.queries == 0);
  CHECK(second.densified == 0);
  CHECK(second.fallbacks == 0);

  // The sample ring went with it, so the new capture has no gyro history to
  // integrate over until it is fed again.
  for (int i = 0; i <= 30; ++i) REQUIRE(e.push_pose(ar_pose(t0 + i * 33 * kMs)).ok());
  (void)e.densified_pose_at(t0 + 16 * kMs);
  CHECK(e.imu_densify_stats().fallback_no_imu == 1);
  REQUIRE(e.stop_session().ok());
}

// ===========================================================================
// 4. THE ENU ORIGIN — each capture is measured about its own zero
// ===========================================================================

TEST_CASE("round14/session/the_enu_origin_is_re_derived_by_every_capture") {
  EngineConfig ec{};
  auto engine = Engine::create(ec);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  e.set_recorder(std::make_unique<lscan::NullRecordWriter>());

  DeviceConfig rtk{};
  rtk.kind = DeviceKind::kRtkRover;
  const auto id = e.add_device(rtk);
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.record = false;
  sc.pushbroom = false;

  // Two epochs per capture: an epoch is published when the NEXT one starts, so
  // one fix on its own never anchors anything.
  const auto capture_at = [&](double lat, double lon, int sod, std::int64_t t_ns) {
    REQUIRE(e.start_session(sc).ok());
    REQUIRE(push_epoch(e, id.value(), nmea_epoch(sod, lat, lon), t_ns).ok());
    REQUIRE(push_epoch(e, id.value(), nmea_epoch(sod + 1, lat, lon), t_ns + 1'000'000'000LL)
                .ok());
    REQUIRE(e.gnss().has_origin());
    crs::Geodetic o{};
    REQUIRE(e.gnss().origin(&o));
    return o;
  };

  const crs::Geodetic first = capture_at(22.2830, 114.1585, 43200, 5'000'000'000LL);
  REQUIRE(e.stop_session().ok());

  REQUIRE(e.start_session(sc).ok());
  // A new capture has no origin, no ENU frame and no observation window until
  // its own first fix says where it is.
  CHECK_FALSE(e.gnss().has_origin());
  CHECK_FALSE(e.georef().has_frame());
  CHECK(e.georef().stats().offered == 0);
  REQUIRE(e.stop_session().ok());

  // Somewhere else entirely: the second capture must anchor on ITS fix.
  const crs::Geodetic second = capture_at(22.3193, 114.1694, 43800, 30'000'000'000LL);
  CHECK(second.lat_deg != first.lat_deg);
  CHECK(second.lon_deg != first.lon_deg);
  CHECK(std::fabs(second.lat_deg - 22.3193) < 1e-6);
  MESSAGE("origin 1 " << first.lat_deg << ", " << first.lon_deg << "  ->  origin 2 "
                      << second.lat_deg << ", " << second.lon_deg);
  REQUIRE(e.stop_session().ok());
}

// ===========================================================================
// 5. THE ACCOUNTING — the fallback reasons add up
// ===========================================================================
//
// ROUND 12's backlog item. One densifier, one timeline, every fallback path
// walked in turn; the invariant is that the reasons sum to the total and the
// total plus the densified queries account for every query made.

TEST_CASE("round14/densify/every_fallback_has_exactly_one_reason") {
  ExternalPoseConfig pcfg;
  // 1 s, so a deliberately WIDE bracket below reaches the densifier as a
  // healthy sample rather than being gated kStale by the source first — the
  // two rejections are different findings and this case wants both.
  pcfg.max_gap_ns = 1'000'000'000LL;
  ExternalPoseSource poses(pcfg);

  ImuDensifyConfig dcfg;  // defaults: 25 ms gap, 200 ms bracket, 20 deg closing
  ImuDensifiedPoseSource dens(&poses, dcfg);

  const std::int64_t t0 = 1'000'000'000LL;

  // (a) nothing pushed at all — the source has no data.
  CHECK_FALSE(dens.sample_at(t0 - 10 * kMs).has_pose);

  const std::int64_t p1 = t0;
  const std::int64_t p2 = t0 + 33 * kMs;
  const std::int64_t p3 = t0 + 66 * kMs;   // tracking lost
  const std::int64_t p4 = t0 + 99 * kMs;   // tracking lost
  const std::int64_t p5 = t0 + 132 * kMs;
  const std::int64_t p6 = t0 + 165 * kMs;
  const std::int64_t p7 = t0 + 465 * kMs;  // 300 ms after p6: too wide to trust
  const std::int64_t p8 = t0 + 498 * kMs;
  const std::int64_t p9 = t0 + 531 * kMs;
  for (const std::int64_t t : {p1, p2}) REQUIRE(poses.push_pose(ar_pose(t)).ok());
  for (const std::int64_t t : {p3, p4}) REQUIRE(poses.push_pose(ar_pose(t, true)).ok());
  for (const std::int64_t t : {p5, p6, p7, p8, p9}) REQUIRE(poses.push_pose(ar_pose(t)).ok());

  // (b) older than the first pose.
  CHECK_FALSE(dens.sample_at(t0 - 10 * kMs).has_pose);
  // (c) a healthy bracket, but no gyro has ever been pushed.
  CHECK(dens.sample_at(t0 + 16 * kMs).gate == PoseGate::kOk);
  // (d) a bracket ARCore itself flagged.
  CHECK(dens.sample_at(t0 + 80 * kMs).gate == PoseGate::kTrackingLost);

  // The gyro: continuous at 400 Hz except for one 60 ms hole across p5→p6, and
  // spinning implausibly fast over p8→p9.
  const auto push_imu = [&](std::int64_t from, std::int64_t to, float rate) {
    for (std::int64_t t = from; t <= to; t += 2'500'000LL) {
      PhoneImuSample s;
      s.t_mono_ns = t;
      s.gyro_rad_s[0] = rate;
      s.accel_m_s2[2] = 9.81f;
      REQUIRE(dens.push_imu(s));
    }
  };
  // The hole is 30 ms wide as the integrator walks it (135 ms -> 165 ms), i.e.
  // wider than max_imu_gap_ns by enough that the boundary case is not what is
  // being tested. The ring still straddles the bracket, so this is a HOLE and
  // not a starved integrator — which is the whole distinction between
  // fallback_gap and fallback_no_imu.
  push_imu(t0 + 120 * kMs, t0 + 135 * kMs, 0.01f);
  push_imu(t0 + 200 * kMs, t0 + 497 * kMs, 0.01f);
  push_imu(t0 + 498 * kMs, t0 + 560 * kMs, 12.0f);

  // (e) the hole lands inside the p5→p6 bracket.
  CHECK(dens.sample_at(t0 + 150 * kMs).gate == PoseGate::kOk);
  // (f) 300 ms of bracket: past where a linear error distribution is a model.
  CHECK(dens.sample_at(t0 + 300 * kMs).gate == PoseGate::kOk);
  // (g) the honest path — this fixture must not be all failure.
  CHECK(dens.sample_at(t0 + 480 * kMs).gate == PoseGate::kOk);
  // (h) 12 rad/s over 33 ms is 22.7 deg of closing error against an ARCore
  //     that says the rig did not move.
  CHECK(dens.sample_at(t0 + 515 * kMs).gate == PoseGate::kOk);

  const ImuDensifyStats s = dens.stats();
  MESSAGE("queries " << s.queries << " = densified " << s.densified << " + fallbacks "
                     << s.fallbacks << " (no-pose " << s.fallback_no_pose << ", gated "
                     << s.fallback_gate << ", no-imu " << s.fallback_no_imu << ", gap "
                     << s.fallback_gap << ", wide-bracket " << s.fallback_bracket << ", closing "
                     << s.fallback_closing << ")");

  // Every path was actually walked — otherwise the sum below proves nothing.
  CHECK(s.fallback_no_pose == 2);
  CHECK(s.fallback_gate == 1);
  CHECK(s.fallback_no_imu == 1);
  CHECK(s.fallback_gap == 1);
  CHECK(s.fallback_bracket == 1);
  CHECK(s.fallback_closing == 1);
  CHECK(s.densified == 1);

  // THE INVARIANT. Before ROUND 14 the left-hand side was short by
  // no_pose + gate, which on the owner's scan-034 was 82 % of the total.
  const std::uint64_t by_reason = s.fallback_no_pose + s.fallback_gate + s.fallback_no_imu +
                                  s.fallback_gap + s.fallback_bracket + s.fallback_closing;
  CHECK(by_reason == s.fallbacks);
  CHECK(s.queries == s.densified + s.fallbacks);
}
