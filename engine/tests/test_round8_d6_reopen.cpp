// test_round8_d6_reopen.cpp — ROUND 8, owner item 27: "when i check the
// recording, it still show a 2D scan. i need a 3d mapping."
//
// This file exists to make that sentence falsifiable. It records a synthetic
// walk through a REAL Engine with a REAL FileRecordWriter, seals the
// container, throws the engine away, REOPENS the directory from disk and
// asserts that what comes back is the three-dimensional room — not the raw 2D
// fan the sensor emitted.
//
// The three claims, in the order they are proved below:
//
//   1. A sealed D6 `.lscan` CONTAINS its trajectory. Before ROUND 8 it did
//      not: ChunkType::kPoseAr was defined, mapped and never written, so the
//      third dimension of a D6 scan — which is entirely the phone's motion —
//      was thrown away at the end of every session.
//   2. Reopened, it resolves to the same geometry the live pass produced, and
//      that geometry is a straight wall (ROUND 7's bar, applied to the
//      REOPENED project rather than to an in-memory assembler).
//   3. Replay == capture, bit for bit, INCLUDING the cloud — not just the
//      decoded bytes. That is Tech Spec §3 key rule 2, and it was previously
//      true of a D6 capture's bytes only.
//
// The falsifiable controls matter as much as the claims, so each one is
// spelled out where it appears rather than collected at the end.
//
// The stimulus (gait model, mount, wall ray-cast, plane fit) is deliberately
// the same experiment ROUND 7 ran in test_pushbroom.cpp, re-derived here
// against the recording path instead of the assembler's API. Same room, same
// walk, same bar — the only difference is that this one goes through a file.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/engine.h"
#include "scanengine/drivers/d6/d6_fan.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"
#include "scanengine/record/replay.h"
#include "scanengine/slam/post/d6_resolve.h"

using namespace scanengine;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = se3::kPi;

struct Quat {
  double x = 0, y = 0, z = 0, w = 1;
};

void qrot(const Quat& q, const double v[3], double out[3]) {
  const double u[3] = {q.x, q.y, q.z};
  const double uv[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                        u[0] * v[1] - u[1] * v[0]};
  const double uuv[3] = {u[1] * uv[2] - u[2] * uv[1], u[2] * uv[0] - u[0] * uv[2],
                         u[0] * uv[1] - u[1] * uv[0]};
  for (int i = 0; i < 3; ++i) out[i] = v[i] + 2.0 * q.w * uv[i] + 2.0 * uuv[i];
}

Quat axis_angle(double ax, double ay, double az, double angle_rad) {
  const double n = std::sqrt(ax * ax + ay * ay + az * az);
  const double s = std::sin(angle_rad * 0.5) / (n > 0 ? n : 1.0);
  return Quat{ax * s, ay * s, az * s, std::cos(angle_rad * 0.5)};
}

Quat qmul(const Quat& a, const Quat& b) {
  return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// ROUND 7's walking operator, verbatim in intent: 1 m/s along +x, 2 Hz gait,
// +/- 2 cm sway, +/- 3 cm bob, +/- 3 deg yaw, +/- 1.7 deg roll.
struct Gait {
  double speed = 1.0;
  double step_hz = 2.0;

  void position_at(double t, double out[3]) const {
    const double w = 2.0 * kPi * step_hz;
    out[0] = speed * t;
    out[1] = 0.020 * std::sin(w * t);
    out[2] = 1.35 + 0.030 * std::sin(w * t + 0.9);
  }
  Quat orientation_at(double t) const {
    const double w = 2.0 * kPi * step_hz;
    return qmul(axis_angle(0, 0, 1, 0.052 * std::sin(w * t + 0.4)),
                axis_angle(1, 0, 0, 0.030 * std::sin(w * t + 2.1)));
  }
};

// The owner's rig: D6 flat on the BACK of the phone, scan fan VERTICAL and
// across the direction of travel. lidar +x -> world +y, +y -> world +z,
// +z -> world +x (the spin axis, along the walk).
void mount_matrix(double m[16], Quat* q_out) {
  const double R[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
  const double t[3] = {0.0, 0.0, 0.0};
  se3::mat4_from_rt(R, t, m);
  double q[4];
  se3::matrix_to_quat(R, q);
  *q_out = Quat{q[0], q[1], q[2], q[3]};
}

// The room: one flat wall at y = +2.4 m, floor at 0, ceiling at 2.9 m.
constexpr double kWallY = 2.4;

// Range from the rig at true time `t` to the wall, along fan angle
// `angle_deg`. Negative = this return is not the wall (missed, behind, out of
// the D6's window, or into floor/ceiling).
double range_to_wall(const Gait& g, const Quat& q_mount, double t, double angle_deg) {
  // The ray leaves the sensor in the production fan frame (d6_fan.h). Using
  // the shared definition rather than a local copy is what keeps this fixture
  // honest across a convention change — ROUND 9 item 34 flipped the sweep, and
  // a hand-copied sin/cos here would have kept the test green while the room
  // came out mirrored.
  double dir_l[3];
  d6::fan_point(angle_deg, 1.0, dir_l);
  double dir_p[3], dir_w[3], pos[3];
  qrot(q_mount, dir_l, dir_p);
  qrot(g.orientation_at(t), dir_p, dir_w);
  g.position_at(t, pos);
  if (std::fabs(dir_w[1]) < 1e-6) return -1.0;
  const double d = (kWallY - pos[1]) / dir_w[1];
  if (d < 0.4 || d > 8.0) return -1.0;
  const double z = pos[2] + d * dir_w[2];
  if (z < 0.05 || z > 2.9) return -1.0;
  return d;
}

// RMS distance to the points' OWN best-fit plane y = a + b*x + c*z. Best-fit
// rather than the known wall on purpose: a constant offset is a latency
// symptom, not a bending one, and must not be allowed to masquerade as one.
double plane_fit_rms(const std::vector<PointVertex>& pts) {
  const std::size_t n = pts.size();
  if (n < 8) return 1e9;
  double S[3][4] = {};
  for (const auto& p : pts) {
    const double b[3] = {1.0, static_cast<double>(p.x), static_cast<double>(p.z)};
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) S[i][j] += b[i] * b[j];
      S[i][3] += b[i] * static_cast<double>(p.y);
    }
  }
  for (int c = 0; c < 3; ++c) {  // Gauss-Jordan, 3x3
    int piv = c;
    for (int r = c + 1; r < 3; ++r) {
      if (std::fabs(S[r][c]) > std::fabs(S[piv][c])) piv = r;
    }
    if (std::fabs(S[piv][c]) < 1e-12) return 1e9;
    if (piv != c) {
      for (int k = 0; k < 4; ++k) std::swap(S[c][k], S[piv][k]);
    }
    const double d = S[c][c];
    for (int k = 0; k < 4; ++k) S[c][k] /= d;
    for (int r = 0; r < 3; ++r) {
      if (r == c) continue;
      const double f = S[r][c];
      for (int k = 0; k < 4; ++k) S[r][k] -= f * S[c][k];
    }
  }
  double acc = 0.0;
  for (const auto& p : pts) {
    const double fit = S[0][3] + S[1][3] * p.x + S[2][3] * p.z;
    const double e = static_cast<double>(p.y) - fit;
    acc += e * e;
  }
  return std::sqrt(acc / static_cast<double>(n));
}

std::vector<PointVertex> drain_stream(const PageStore& store, StreamId stream) {
  std::vector<PointVertex> out;
  for (PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid() || v.stream != stream) continue;
    out.insert(out.end(), v.data, v.data + v.count);
  }
  return out;
}

std::string fresh_dir(const char* tag) {
  const fs::path p = fs::temp_directory_path() / (std::string("scanengine-r8-") + tag);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p.string();
}

// --- the capture ------------------------------------------------------------
//
// One place builds the recording, so every case below is looking at the same
// bytes. Packets are pushed ONE AT A TIME with their own arrival stamp: that
// is what a 230400-baud UART actually delivers into a reader that is keeping
// up, and it keeps the synthetic ranges honest, since a return's modelled time
// is then within one D6Config::time_slice_bytes slice (~2.8 ms, i.e. ~2.8 mm
// at 1 m/s) of the time the driver will assign it.
struct CaptureResult {
  std::string dir;
  std::vector<PointVertex> live_map;   // what the live pass resolved
  std::uint64_t poses_pushed = 0;
  std::uint64_t d6_chunks = 0;
};

constexpr int kSamplesPerPacket = 20;
constexpr int kPacketsPerRev = 18;                       // 360 returns / revolution
constexpr double kRevPeriodS = 0.1;                      // 10 Hz
constexpr double kWalkSeconds = 4.0;
constexpr std::int64_t kT0 = 5'000'000'000LL;            // not 0: 0 means "stamp on arrival"
constexpr std::int64_t kPosePeriodNs = 33'333'333LL;     // ~30 Hz, ARCore's rate

Pose make_pose(const Gait& g, std::int64_t t_ns) {
  Pose p;
  p.t_mono_ns = t_ns;
  const double t = static_cast<double>(t_ns - kT0) * 1e-9;
  g.position_at(t, p.position);
  const Quat q = g.orientation_at(t);
  p.orientation[0] = q.x;
  p.orientation[1] = q.y;
  p.orientation[2] = q.z;
  p.orientation[3] = q.w;
  p.source = StreamId::kPoseAr;
  p.quality = PoseQuality::kGood;
  p.tracking_lost = 0;
  return p;
}

CaptureResult record_walk(const char* tag) {
  CaptureResult out;
  out.dir = fresh_dir(tag) + "/walk.lscan";

  EngineConfig ec{};
  auto engine = Engine::create(ec);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  e.set_recorder(std::make_unique<lscan::FileRecordWriter>());

  DeviceConfig dc{};
  dc.kind = DeviceKind::kD6;
  dc.d6.send_start_stop_commands = false;
  const auto id = e.add_device(dc);
  REQUIRE(id.ok());

  const Gait g;
  Quat q_mount;
  double mount[16];
  mount_matrix(mount, &q_mount);

  SessionConfig sc{};
  sc.record = true;
  sc.pushbroom = true;  // start_session() re-applies this; setting it above is not enough
  sc.lscan_dir = out.dir;
  sc.profile = "quickscan";
  REQUIRE(e.start_session(sc).ok());

  // The extrinsic is applied AFTER Start, deliberately: that is the order the
  // Android capture flow actually uses (its log reads `[session] start:` and
  // then `[pushbroom] extrinsic applied:` ~70 ms later), because a mid-session
  // mount re-zero has to work and one code path serves both. Setting it before
  // Start here would have made this test pass while the field case wrote a
  // manifest with `"mountCalibration": null` — which is exactly what the
  // emulator test caught. See Engine::set_mount_extrinsics().
  REQUIRE(e.set_mount_extrinsics(mount).ok());

  const int revs = static_cast<int>(kWalkSeconds / kRevPeriodS);
  std::int64_t next_pose_ns = kT0;
  for (int rev = 0; rev < revs; ++rev) {
    for (int pk = 0; pk < kPacketsPerRev; ++pk) {
      const double frac =
          (static_cast<double>(rev) + static_cast<double>(pk) / kPacketsPerRev) * kRevPeriodS;
      const std::int64_t t_ns = kT0 + static_cast<std::int64_t>(frac * 1e9);

      // Poses first, and up to the packet's own time: the assembler resolves a
      // return the moment its bracketing poses exist, which is what a live
      // capture does too.
      while (next_pose_ns <= t_ns) {
        REQUIRE(e.push_pose(make_pose(g, next_pose_ns)).ok());
        ++out.poses_pushed;
        next_pose_ns += kPosePeriodNs;
      }

      const double a0 = 360.0 * static_cast<double>(pk) / kPacketsPerRev;
      const double step = 360.0 / (kPacketsPerRev * kSamplesPerPacket);
      d6test::PacketSpec ps;
      ps.first_angle_deg = a0;
      ps.last_angle_deg = a0 + step * (kSamplesPerPacket - 1);
      for (int s = 0; s < kSamplesPerPacket; ++s) {
        const double ang = a0 + step * s;
        const double d = range_to_wall(g, q_mount, frac, ang);
        // A miss is encoded as distance 0 — "no return", exactly what the
        // sensor emits and what D6Config::drop_zero_range_points discards.
        const std::uint16_t mm =
            d < 0 ? 0 : static_cast<std::uint16_t>(std::lround(d * 1000.0));
        ps.samples.push_back(d6test::Sample{mm, 140, false});
      }
      const std::vector<std::uint8_t> bytes = d6test::build(ps);
      REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(bytes.data(), bytes.size()),
                                  TimePoint{t_ns})
                  .ok());
      ++out.d6_chunks;
    }
  }

  REQUIRE(e.pushbroom_flush().ok());
  out.live_map = drain_stream(e.points(), StreamId::kSlamMap);
  REQUIRE(e.stop_session().ok());
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

// ===========================================================================
// 1. THE RECORDING CONTAINS THE TRAJECTORY
// ===========================================================================
TEST_CASE("round8/a_sealed_d6_capture_carries_its_poses_its_map_and_its_mount") {
  const CaptureResult cap = record_walk("carries");

  // --- the streams that must now exist ------------------------------------
  lscan::FileRecordReader r;
  REQUIRE(r.open(cap.dir).ok());

  std::uint64_t d6_chunks = 0, pose_chunks = 0, map_chunks = 0;
  for (const auto& s : r.stream_summaries()) {
    if (s.stream == StreamId::kLidarD6) d6_chunks = s.chunk_count;
    if (s.stream == StreamId::kPoseAr) pose_chunks = s.chunk_count;
    if (s.stream == StreamId::kSlamMap) map_chunks = s.chunk_count;
  }
  MESSAGE("sealed container: " << d6_chunks << " kD6Raw, " << pose_chunks << " kPoseAr, "
                               << map_chunks << " kPointsXyzRgba chunks");

  CHECK(d6_chunks == cap.d6_chunks);
  // THE assertion this whole round turns on. Before ROUND 8 this was 0 for
  // every capture ever made, and a 0 here means a D6 project is once again a
  // pile of range/angle pairs from which no 3D result can be rebuilt by
  // anyone.
  CHECK(pose_chunks == cap.poses_pushed);
  CHECK(pose_chunks > 100);
  // The resolved cloud is cached alongside it, so Review can draw the map
  // without paying for a re-resolve first.
  CHECK(map_chunks > 0);

  // The pose stream lives in its own file, and the map in its own file — not
  // interleaved into lidar.bin, where a kD6Raw replay would have to read and
  // CRC every one of them on the way past.
  CHECK(fs::exists(cap.dir + "/" + lscan::kPoseArStreamFile));
  CHECK(fs::exists(cap.dir + "/" + lscan::kMapStreamFile));

  // --- the manifest --------------------------------------------------------
  //
  // Both of these were checked against a REAL exported capture from the
  // owner's Pixel 8 Pro (captures/scan-015/manifest.json), which said
  // `"sensors": []` and `"mountCalibration": null`. Both were engine bugs:
  // add_sensor() had existed since A5 with no caller, and nothing ever filled
  // in the extrinsic.
  const std::string manifest = read_file(cap.dir + "/" + lscan::kManifestFile);
  CHECK(manifest.find("\"sealed\": true") != std::string::npos);
  CHECK(manifest.find("\"phoneFromLidar\"") != std::string::npos);
  CHECK(manifest.find("\"sensors\": []") == std::string::npos);
  CHECK(manifest.find("\"kind\": \"coin-d6\"") != std::string::npos);

  // ... and it round-trips to the matrix that was actually applied. A manifest
  // that decodes to a DIFFERENT extrinsic resolves the project into a
  // different room, which is the failure this key exists to prevent.
  double m_read[16];
  REQUIRE(post::read_manifest_mount(cap.dir, m_read));
  Quat unused;
  double m_expect[16];
  mount_matrix(m_expect, &unused);
  for (int i = 0; i < 16; ++i) CHECK(m_read[i] == doctest::Approx(m_expect[i]).epsilon(1e-12));

  (void)r.close();
}

// ===========================================================================
// 2. REOPENED, IT IS THE 3D ROOM — owner item 27d
// ===========================================================================
TEST_CASE("round8/reopening_a_saved_d6_project_yields_the_3d_resolved_room") {
  const CaptureResult cap = record_walk("reopen");

  // Nothing from the capture survives into this block except the directory:
  // this is a cold open, the same one the Review screen performs.
  bool is_d6 = false;
  REQUIRE(post::lscan_is_d6_project(cap.dir, &is_d6).ok());
  CHECK(is_d6);

  PageStore store;
  post::D6ResolveConfig cfg;
  cfg.store = &store;
  // No mount supplied on purpose — it must come out of the container, because
  // "self-contained" is the property being tested.
  post::D6ResolvePipeline pipeline(cfg);
  REQUIRE(pipeline.run(cap.dir).ok());

  const post::D6ResolveStats st = pipeline.stats();
  MESSAGE("reopened: " << st.lidar_chunks << " chunks, " << st.poses_read << " poses -> "
                       << st.points_out << " world points (mount from "
                       << std::string(st.mount_from_manifest ? "manifest" : "caller") << ")");
  CHECK(st.mount_from_manifest);
  CHECK(st.poses_read == cap.poses_pushed);
  // 720 packets x 20 returns = 14,400 fan samples, of which the ones that
  // actually land on the wall (rather than the floor, the ceiling or empty
  // space behind the operator) are the geometry. A few thousand is the shape
  // of a 4 s walk past one wall.
  CHECK(st.points_out > 2'000);

  const std::vector<PointVertex> pts = drain_stream(store, StreamId::kSlamMap);
  REQUIRE(pts.size() == st.points_out);

  // --- claim A: it is a straight wall (ROUND 7's bar, on a REOPENED project)
  const double rms = plane_fit_rms(pts);
  MESSAGE("reopened wall plane-fit RMS = " << rms * 100.0 << " cm");
  CHECK(rms < 0.02);  // the field bar: 2 cm

  // --- claim B: it is THREE-dimensional, in the precise sense the owner's
  // own export was not.
  //
  // This is not a stylistic assertion. The real scan-015 export's
  // processed/preview.f32 had 2027 of its 4040 points at z == 0.0f EXACTLY —
  // the signature of raw sensor-frame fan returns, which lie in the lidar's
  // own scan plane by construction. A resolved cloud cannot look like that:
  // the walk sweeps the fan through the room, so the points must span the
  // walk axis, must span the wall's height, and must not pile up on one plane
  // through the origin.
  float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
  std::size_t exact_zero_z = 0;
  for (const auto& p : pts) {
    xmin = std::min(xmin, p.x);
    xmax = std::max(xmax, p.x);
    zmin = std::min(zmin, p.z);
    zmax = std::max(zmax, p.z);
    if (p.z == 0.0f) ++exact_zero_z;
  }
  MESSAGE("reopened extents: x " << xmin << ".." << xmax << " m, z " << zmin << ".." << zmax
                                 << " m; z==0 exactly: " << exact_zero_z);
  CHECK((xmax - xmin) > 3.5);  // a 4 s walk at 1 m/s; one fan revolution cannot
  CHECK((zmax - zmin) > 1.5);  // floor-to-ceiling on the wall
  CHECK(exact_zero_z * 20 < pts.size());  // < 5 %, vs. the 50 % a raw fan gives

  // --- claim C: reopening reproduces the LIVE result -----------------------
  //
  // Not "similar" — the same points. Same chunks, same driver, same assembler,
  // so anything less than equality means the offline path has drifted away
  // from the one the operator watched, and the two would then disagree in the
  // field with nobody able to say which was right.
  REQUIRE(cap.live_map.size() == pts.size());
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const PointVertex& a = cap.live_map[i];
    const PointVertex& b = pts[i];
    if (a.x != b.x || a.y != b.y || a.z != b.z || a.r != b.r || a.g != b.g || a.b != b.b ||
        a.a != b.a) {
      ++mismatches;
    }
  }
  CHECK(mismatches == 0);
}

// ===========================================================================
// 2b. THE REVIEW FAST PATH — the cached map opens without re-resolving
// ===========================================================================
TEST_CASE("round8/the_recorded_map_reloads_without_a_re_resolve_and_agrees_with_it") {
  const CaptureResult cap = record_walk("fastpath");

  PageStore cached;
  std::uint64_t n = 0;
  REQUIRE(post::load_recorded_cloud(cap.dir, &cached, StreamId::kSlamMap, &n).ok());
  MESSAGE("recorded map fast path: " << n << " points");
  CHECK(n > 2'000);

  // The cache and the re-resolve must agree, or Review and Process would show
  // an operator two different rooms and neither would be labelled.
  PageStore resolved;
  post::D6ResolveConfig cfg;
  cfg.store = &resolved;
  post::D6ResolvePipeline pipeline(cfg);
  REQUIRE(pipeline.run(cap.dir).ok());

  const std::vector<PointVertex> a = drain_stream(cached, StreamId::kSlamMap);
  const std::vector<PointVertex> b = drain_stream(resolved, StreamId::kSlamMap);
  REQUIRE(a.size() == b.size());
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) ++mismatches;
  }
  CHECK(mismatches == 0);

  // A container with no map stream is not an error — it is every recording
  // made before ROUND 8, and the caller falls through to the re-resolve.
  std::error_code ec;
  fs::remove(cap.dir + "/" + lscan::kMapStreamFile, ec);
  PageStore empty;
  std::uint64_t none = 999;
  CHECK(post::load_recorded_cloud(cap.dir, &empty, StreamId::kSlamMap, &none).ok());
  CHECK(none == 0);
}

// ===========================================================================
// 3. THE CONTROL: without the trajectory there is no 3D, and it says so
// ===========================================================================
TEST_CASE("round8/control_a_project_with_no_poses_refuses_and_names_the_reason") {
  const CaptureResult cap = record_walk("nopose");

  // Delete the pose stream — which is EXACTLY what every recording made before
  // 0.5.0 looks like, including the owner's scan-015. If this case ever starts
  // passing the resolve, something has begun inventing a trajectory.
  std::error_code ec;
  fs::remove(cap.dir + "/" + lscan::kPoseArStreamFile, ec);
  REQUIRE(!ec);

  PageStore store;
  post::D6ResolveConfig cfg;
  cfg.store = &store;
  post::D6ResolvePipeline pipeline(cfg);
  const Status st = pipeline.run(cap.dir);

  CHECK_FALSE(st.ok());
  CHECK(st.error() == ScanError::kNotFound);
  CHECK(pipeline.stats().poses_read == 0);
  // The message is what the Review screen turns into its inline explanation,
  // so it has to name the version rather than say "not found".
  // last_error_message(), not Status::message(): Status carries only the enum
  // (core/error.h), and the sentence a UI shows lives in the thread-local the
  // set_last_error() call filled in.
  const std::string msg = last_error_message();
  MESSAGE("refusal: " << msg);
  CHECK(msg.find("0.5.0") != std::string::npos);
  CHECK(store.stats().total_points == 0);
}

// ===========================================================================
// 4. REPLAY == CAPTURE, INCLUDING THE CLOUD — owner item 27a
// ===========================================================================
TEST_CASE("round8/replaying_a_recorded_d6_walk_reproduces_the_capture_bit_for_bit") {
  const CaptureResult cap = record_walk("replay");

  EngineConfig ec{};
  auto engine = Engine::create(ec);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  // No recorder: a replay reproduces a capture, it does not make a second one.
  DeviceConfig dc{};
  dc.kind = DeviceKind::kD6;
  dc.d6.send_start_stop_commands = false;
  const auto id = e.add_device(dc);
  REQUIRE(id.ok());

  double mount[16];
  Quat unused;
  mount_matrix(mount, &unused);
  REQUIRE(e.set_mount_extrinsics(mount).ok());
  REQUIRE(e.set_pushbroom_enabled(true).ok());

  SessionConfig sc{};
  sc.record = false;
  sc.pushbroom = true;
  REQUIRE(e.start_session(sc).ok());

  lscan::ReplayConfig rc;
  rc.lscan_dir = cap.dir;
  rc.target_device = id.value();
  rc.chunk_type = lscan::ChunkType::kD6Raw;
  rc.speed = 0.0;  // unpaced
  lscan::ReplaySource replay(e);
  REQUIRE(replay.run(rc).ok());
  REQUIRE(e.pushbroom_flush().ok());

  MESSAGE("replay: " << replay.stats().chunks_replayed << " kD6Raw chunks, "
                     << replay.stats().poses_replayed << " kPoseAr chunks");
  CHECK(replay.stats().chunks_replayed == cap.d6_chunks);
  // ReplayConfig::replay_poses defaults to true, and this is why: the same
  // bytes without the trajectory replay into a flat fan.
  CHECK(replay.stats().poses_replayed == cap.poses_pushed);

  const std::vector<PointVertex> replayed = drain_stream(e.points(), StreamId::kSlamMap);
  REQUIRE(replayed.size() == cap.live_map.size());
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < replayed.size(); ++i) {
    const PointVertex& a = cap.live_map[i];
    const PointVertex& b = replayed[i];
    if (a.x != b.x || a.y != b.y || a.z != b.z) ++mismatches;
  }
  CHECK(mismatches == 0);
  REQUIRE(e.stop_session().ok());
}

// ===========================================================================
// 5. THE OTHER CONTROL: bytes alone are a 2D fan
// ===========================================================================
TEST_CASE("round8/control_replaying_the_bytes_without_the_poses_produces_no_room") {
  const CaptureResult cap = record_walk("bytesonly");

  EngineConfig ec{};
  auto engine = Engine::create(ec);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  DeviceConfig dc{};
  dc.kind = DeviceKind::kD6;
  dc.d6.send_start_stop_commands = false;
  const auto id = e.add_device(dc);
  REQUIRE(id.ok());
  double mount[16];
  Quat unused;
  mount_matrix(mount, &unused);
  REQUIRE(e.set_mount_extrinsics(mount).ok());
  REQUIRE(e.set_pushbroom_enabled(true).ok());
  SessionConfig sc{};
  sc.record = false;
  sc.pushbroom = true;
  REQUIRE(e.start_session(sc).ok());

  lscan::ReplayConfig rc;
  rc.lscan_dir = cap.dir;
  rc.target_device = id.value();
  rc.speed = 0.0;
  rc.replay_poses = false;  // the pre-ROUND-8 world, on purpose
  lscan::ReplaySource replay(e);
  REQUIRE(replay.run(rc).ok());
  REQUIRE(e.pushbroom_flush().ok());

  CHECK(replay.stats().chunks_replayed == cap.d6_chunks);
  CHECK(replay.stats().poses_replayed == 0);
  // Every return is dropped for want of a pose. This is the shape of the bug:
  // the same recording, the same bytes, the same assembler — and no geometry,
  // because the trajectory is the geometry.
  const std::vector<PointVertex> replayed = drain_stream(e.points(), StreamId::kSlamMap);
  MESSAGE("bytes without poses -> " << replayed.size() << " world points ("
                                    << e.pushbroom_stats().dropped_no_pose << " dropped for "
                                    << "want of a pose)");
  CHECK(replayed.empty());
  CHECK(e.pushbroom_stats().dropped_no_pose > 2'000);
  REQUIRE(e.stop_session().ok());
}
