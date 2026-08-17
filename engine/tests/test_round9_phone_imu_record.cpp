// test_round9_phone_imu_record.cpp — ROUND 9, owner item 35, the recording half.
//
// > "lidar data and the imu position data need sync the frequency"
//
// tests/test_round9_imu_densify.cpp already proved the GEOMETRY claim in
// memory: with 1.5 deg of 12 Hz jitter between 30 Hz ARCore poses, resolving
// through `ImuDensifiedPoseSource` takes a flat wall's plane-fit RMS from
// 0.739 cm to 0.021 cm. This file proves the four claims that make that
// survivable — i.e. that the improvement is a property of the RECORDING and not
// of one live process's memory:
//
//   1. The phone IMU has a chunk type, a payload and its OWN stream file, and
//      it round-trips through the writer and the reader bit for bit.
//   2. A sealed capture CONTAINS its IMU track, and it does not contaminate
//      `streams/imu.bin` — which is the Mid-360's, and which two offline
//      pipelines read as "this container is a Mid-360 project".
//   3. Reopened cold off disk, the container re-resolves to the SAME cloud the
//      live pass produced. Not similar — the same points. If the offline path
//      ignored the recorded gyro this would fail, and the field would then have
//      two different rooms from one recording with nobody able to say which was
//      right.
//   4. The falsifiable controls. Re-resolving the SAME container with
//      densification off must produce a DIFFERENT and MEASURABLY WORSE wall;
//      deleting the kPhoneImu chunks must reproduce that worse answer EXACTLY;
//      and a capture that never pushed an IMU at all must be bit-identical to
//      the pre-ROUND-9 path, because the densifier is now wired in
//      unconditionally and must be provably inert when it is unfed.
//
// The stimulus is test_round8_d6_reopen.cpp's walk (same room, same gait, same
// mount) with test_round9_imu_densify.cpp's 12 Hz jitter added — deliberately,
// so that a failure here is a failure of the RECORDING and not of a new fixture.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/engine.h"
#include "scanengine/drivers/d6/d6_fan.h"
#include "scanengine/poses/imu_densified_pose.h"
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

Quat qmul(const Quat& a, const Quat& b) {
  const double qa[4] = {a.x, a.y, a.z, a.w};
  const double qb[4] = {b.x, b.y, b.z, b.w};
  double out[4];
  se3::quat_mul(qa, qb, out);
  return Quat{out[0], out[1], out[2], out[3]};
}

Quat axis_angle(double ax, double ay, double az, double angle_rad) {
  const double n = std::sqrt(ax * ax + ay * ay + az * az);
  const double s = std::sin(angle_rad * 0.5) / (n > 0 ? n : 1.0);
  return Quat{ax * s, ay * s, az * s, std::cos(angle_rad * 0.5)};
}

void qrot(const Quat& q, const double v[3], double out[3]) {
  const double qq[4] = {q.x, q.y, q.z, q.w};
  double R[9];
  se3::quat_to_matrix(qq, R);
  for (int i = 0; i < 3; ++i) {
    out[i] = R[i * 3 + 0] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2];
  }
}

// --- the rig ---------------------------------------------------------------
//
// ROUND 8's walking operator (1 m/s along +x, 2 Hz gait, +/- 2 cm sway,
// +/- 3 cm bob, +/- 3 deg yaw, +/- 1.7 deg roll) plus ROUND 9's 12 Hz
// rotational jitter about all three axes at different phases. 12 Hz is below
// the 30 Hz pose Nyquist on purpose — the poses DO sample it, at 2.5 samples
// per cycle, which is nowhere near enough for a slerp between them to follow
// the arc. That is the regime real walking lives in.
struct Rig {
  double jitter_amp_deg = 1.5;
  double jitter_hz = 12.0;

  void position_at(double t, double out[3]) const {
    const double w = 2.0 * kPi * 2.0;
    out[0] = 1.0 * t;
    out[1] = 0.020 * std::sin(w * t);
    out[2] = 1.35 + 0.030 * std::sin(w * t + 0.9);
  }

  Quat orientation_at(double t) const {
    const double w = 2.0 * kPi * 2.0;
    Quat q = qmul(axis_angle(0, 0, 1, 0.052 * std::sin(w * t + 0.4)),
                  axis_angle(1, 0, 0, 0.030 * std::sin(w * t + 2.1)));
    if (jitter_amp_deg > 0.0) {
      const double a = jitter_amp_deg * kPi / 180.0;
      const double wj = 2.0 * kPi * jitter_hz;
      const Quat j = qmul(qmul(axis_angle(0, 0, 1, a * std::sin(wj * t)),
                               axis_angle(1, 0, 0, a * std::sin(wj * t + 1.7))),
                          axis_angle(0, 1, 0, a * std::sin(wj * t + 3.1)));
      q = qmul(q, j);
    }
    return q;
  }

  // Body-frame angular velocity by finite difference of the true attitude —
  // what a gyro measures. This is in the CAMERA frame; see gyro_in_imu_frame().
  void gyro_at(double t, double out[3]) const {
    const double h = 1.0 / (jitter_hz > 0 ? jitter_hz : 1.0) / 200.0;
    const Quat q0 = orientation_at(t - h);
    const Quat q1 = orientation_at(t + h);
    const double a[4] = {q0.x, q0.y, q0.z, q0.w};
    const double b[4] = {q1.x, q1.y, q1.z, q1.w};
    double a_conj[4], rel[4], rv[3];
    se3::quat_conj(a, a_conj);
    se3::quat_mul(a_conj, b, rel);
    se3::quat_to_rotvec(rel, rv);
    for (int i = 0; i < 3; ++i) out[i] = rv[i] / (2.0 * h);
  }
};

// The IMU->camera rotation the fixture uses: 90 degrees about +z, i.e. the
// ordinary phone where Android's sensor frame is defined against the DISPLAY
// and ARCore's against the CAMERA IMAGE. Identity would make the whole
// extrinsic path untested, and a wrong extrinsic is exactly the failure
// `ImuDensifyConfig::camera_from_imu` exists to let an app avoid.
const double kCameraFromImu[4] = {0.0, 0.0, 0.70710678118654752, 0.70710678118654752};

// A gyro reports in ITS OWN frame, so the fixture must produce what the sensor
// would actually emit: w_imu = R(camera_from_imu)^T * w_camera. Feeding the
// camera-frame rate directly would make the test pass with the extrinsic
// ignored, which is the bug most worth catching here.
void gyro_in_imu_frame(const Rig& g, double t, float out[3]) {
  double w_cam[3];
  g.gyro_at(t, w_cam);
  double R[9];
  se3::quat_to_matrix(kCameraFromImu, R);
  for (int i = 0; i < 3; ++i) {
    double acc = 0.0;
    for (int k = 0; k < 3; ++k) acc += R[k * 3 + i] * w_cam[k];  // R^T * w
    out[i] = static_cast<float>(acc);
  }
}

// The owner's rig: D6 flat on the back of the phone, fan VERTICAL and across
// the direction of travel.
void mount_matrix(double m[16], Quat* q_out) {
  const double R[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
  const double t[3] = {0.0, 0.0, 0.0};
  se3::mat4_from_rt(R, t, m);
  double q[4];
  se3::matrix_to_quat(R, q);
  *q_out = Quat{q[0], q[1], q[2], q[3]};
}

constexpr double kWallY = 2.4;

double range_to_wall(const Rig& g, const Quat& q_mount, double t, double angle_deg) {
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
// rather than the known wall: a constant offset is a latency symptom, not a
// bending one, and must not be allowed to masquerade as one.
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
  for (int c = 0; c < 3; ++c) {
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
  return std::sqrt(acc / static_cast<double>(n)) /
         std::sqrt(1.0 + S[1][3] * S[1][3] + S[2][3] * S[2][3]);
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

std::size_t cloud_mismatches(const std::vector<PointVertex>& a,
                             const std::vector<PointVertex>& b) {
  if (a.size() != b.size()) return a.size() + b.size();
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) ++n;
  }
  return n;
}

// --- how far each point is from where the TRUTH arm put it ------------------
//
// THE metric this file argues from, and it is chosen to be immune to the
// fixture rather than flattering to the code. The synthetic ranges are modelled
// at each PACKET's time, while the D6 driver back-dates every sample inside the
// packet to its own instant (ROUND 7's slicing, ROUND 9's per-sample stamps) and
// applies the datasheet's mechanical angle correction. Both are deliberate
// production behaviour and both make the modelled wall and the resolved wall
// disagree by a fixed amount that has nothing to do with the trajectory — a
// flat-wall RMS therefore measures the fixture's floor as much as the
// interpolation.
//
// Comparing two RESOLVES OF THE SAME BYTES cancels all of it exactly. Each arm
// records an identical D6 stream and differs only in what trajectory the same
// returns are resolved through, so point i in one arm and point i in another are
// the same return, and the distance between them is entirely
// trajectory-estimation error.
double cloud_rms_to(const std::vector<PointVertex>& a, const std::vector<PointVertex>& b) {
  if (a.size() != b.size() || a.empty()) return 1e9;
  double acc = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double dx = static_cast<double>(a[i].x) - static_cast<double>(b[i].x);
    const double dy = static_cast<double>(a[i].y) - static_cast<double>(b[i].y);
    const double dz = static_cast<double>(a[i].z) - static_cast<double>(b[i].z);
    acc += dx * dx + dy * dy + dz * dz;
  }
  return std::sqrt(acc / static_cast<double>(a.size()));
}

std::string fresh_dir(const char* tag) {
  const fs::path p = fs::temp_directory_path() / (std::string("scanengine-r9imu-") + tag);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p.string();
}

// --- the capture ------------------------------------------------------------

constexpr int kSamplesPerPacket = 20;
constexpr int kPacketsPerRev = 18;                    // 360 returns / revolution
constexpr double kRevPeriodS = 0.1;                   // 10 Hz
constexpr double kWalkSeconds = 3.0;
constexpr std::int64_t kT0 = 5'000'000'000LL;         // not 0: 0 means "stamp on arrival"
// ~33 Hz, ARCore's rate — and a WHOLE MULTIPLE of the IMU period, which is not
// cosmetic. `PushbroomConfig::drain_on_push` resolves a return the instant a
// pose brackets it, i.e. at the trailing edge of both streams, and the densifier
// refuses to extrapolate across ANY shortfall between its newest sample and the
// bracket's end. With the two rates coprime, the sample landing exactly on the
// closing pose does not exist and ~20 % of returns fall back for want of one
// 2.5 ms step. That is a real effect on a real phone (nothing aligns ARCore's
// stamps to the gyro's) and it is what the `fallback_gap` counter is for; here
// it would simply dilute the measurement, so the fixture aligns them and the
// residual is measured rather than tolerated.
constexpr std::int64_t kPosePeriodNs = 30'000'000LL;
constexpr std::int64_t kImuPeriodNs = 2'500'000LL;    // 400 Hz, the phone's gyro
// The sensors are already running when the lidar starts — which is true of a
// real capture (ARCore is tracking and the SensorEventListener is registered
// long before Start) and is also what gives the first D6 packet a pose bracket
// and an IMU ring to resolve against.
constexpr std::int64_t kLeadInNs = 300'000'000LL;

struct CaptureResult {
  std::string dir;
  std::vector<PointVertex> live_map;
  std::uint64_t poses_pushed = 0;
  std::uint64_t imu_pushed = 0;
  std::uint64_t d6_chunks = 0;
  ImuDensifyStats imu{};
};

Pose make_pose(const Rig& g, std::int64_t t_ns) {
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

// `with_imu == false` is the pre-ROUND-9 world, recorded by the ROUND-9 engine:
// the densifier is wired in and simply never fed.
//
// `pose_period_ns` exists for ONE purpose: the TRUTH arm. Recording the same
// walk with a 1 kHz pose stream produces a container whose D6 bytes are
// byte-identical (the ranges come from the analytic rig, not from the poses) but
// whose trajectory has nothing left to interpolate. Resolving that container is
// the reference every other arm is measured against — see cloud_rms_to().
CaptureResult record_walk(const char* tag, bool with_imu,
                          std::int64_t pose_period_ns = kPosePeriodNs) {
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

  const Rig g;
  Quat q_mount;
  double mount[16];
  mount_matrix(mount, &q_mount);

  // Before the session and before any sample: applying it rebuilds the
  // densifier, so this is the only sane place for it — exactly as documented.
  REQUIRE(e.set_imu_extrinsics(kCameraFromImu).ok());

  SessionConfig sc{};
  sc.record = true;
  sc.pushbroom = true;
  sc.lscan_dir = out.dir;
  sc.profile = "quickscan";
  REQUIRE(e.start_session(sc).ok());
  REQUIRE(e.set_mount_extrinsics(mount).ok());

  std::int64_t next_pose_ns = kT0 - kLeadInNs;
  std::int64_t next_imu_ns = kT0 - kLeadInNs;

  // The pump both loops below share: everything the phone produced up to `t`,
  // in the order a real app produces it.
  const auto pump_sensors = [&](std::int64_t t_ns) {
    while (next_pose_ns <= t_ns) {
      REQUIRE(e.push_pose(make_pose(g, next_pose_ns)).ok());
      ++out.poses_pushed;
      next_pose_ns += pose_period_ns;
    }
    while (next_imu_ns <= t_ns) {
      if (with_imu) {
        float gyro[3];
        float accel[3] = {0.0f, 0.0f, 9.81f};
        gyro_in_imu_frame(g, static_cast<double>(next_imu_ns - kT0) * 1e-9, gyro);
        REQUIRE(e.push_phone_imu(next_imu_ns, gyro, accel).ok());
        ++out.imu_pushed;
      }
      next_imu_ns += kImuPeriodNs;
    }
  };

  pump_sensors(kT0);

  const int revs = static_cast<int>(kWalkSeconds / kRevPeriodS);
  for (int rev = 0; rev < revs; ++rev) {
    for (int pk = 0; pk < kPacketsPerRev; ++pk) {
      const double frac =
          (static_cast<double>(rev) + static_cast<double>(pk) / kPacketsPerRev) * kRevPeriodS;
      const std::int64_t t_ns = kT0 + static_cast<std::int64_t>(frac * 1e9);
      pump_sensors(t_ns);

      const double a0 = 360.0 * static_cast<double>(pk) / kPacketsPerRev;
      const double step = 360.0 / (kPacketsPerRev * kSamplesPerPacket);
      d6test::PacketSpec ps;
      ps.first_angle_deg = a0;
      ps.last_angle_deg = a0 + step * (kSamplesPerPacket - 1);
      for (int s = 0; s < kSamplesPerPacket; ++s) {
        const double ang = a0 + step * s;
        const double d = range_to_wall(g, q_mount, frac, ang);
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
  out.imu = e.imu_densify_stats();
  REQUIRE(e.stop_session().ok());
  return out;
}

// Re-resolve a container cold, exactly as the Review screen does.
struct ResolveResult {
  std::vector<PointVertex> pts;
  post::D6ResolveStats stats{};
};

ResolveResult reopen(const std::string& dir, bool densify) {
  ResolveResult r;
  PageStore store;
  post::D6ResolveConfig cfg;
  cfg.store = &store;
  cfg.densify_with_phone_imu = densify;
  cfg.have_imu_extrinsics = true;
  for (int i = 0; i < 4; ++i) cfg.imu_camera_from_imu[i] = kCameraFromImu[i];
  post::D6ResolvePipeline pipeline(cfg);
  REQUIRE(pipeline.run(dir).ok());
  r.stats = pipeline.stats();
  r.pts = drain_stream(store, StreamId::kSlamMap);
  return r;
}

}  // namespace

// ===========================================================================
// 1. THE PAYLOAD — a phone IMU sample survives the container bit for bit
// ===========================================================================
TEST_CASE("round9/the_phone_imu_chunk_round_trips_through_the_container") {
  // --- the codec, in isolation ---------------------------------------------
  lscan::PhoneImuChunkRecord in;
  in.gyro_rad_s[0] = 0.125f;
  in.gyro_rad_s[1] = -2.5e-3f;
  in.gyro_rad_s[2] = 3.0e7f;
  in.accel_m_s2[0] = -9.80665f;
  in.accel_m_s2[1] = 0.0f;
  in.accel_m_s2[2] = 1.401298464e-45f;  // the smallest subnormal float

  std::uint8_t buf[lscan::kPhoneImuChunkPayloadBytes];
  lscan::encode_phone_imu_chunk(in, buf);
  CHECK(lscan::kPhoneImuChunkPayloadBytes == 24);

  // Little-endian, in the documented order, with no padding. Asserted against
  // the BYTES rather than against a decode, because a codec that is wrong in
  // both directions round-trips perfectly and still writes a file nobody else
  // can read.
  std::uint32_t bits = 0;
  std::memcpy(&bits, &in.gyro_rad_s[0], 4);
  for (int i = 0; i < 4; ++i) {
    CHECK(buf[i] == static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu));
  }
  std::memcpy(&bits, &in.accel_m_s2[0], 4);
  for (int i = 0; i < 4; ++i) {
    CHECK(buf[12 + i] == static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu));
  }

  lscan::PhoneImuChunkRecord back{};
  REQUIRE(lscan::decode_phone_imu_chunk(ByteSpan(buf, sizeof buf), &back));
  for (int i = 0; i < 3; ++i) {
    CHECK(back.gyro_rad_s[i] == in.gyro_rad_s[i]);   // bit-exact, not Approx
    CHECK(back.accel_m_s2[i] == in.accel_m_s2[i]);
  }

  // A SHORT payload is refused; a LONGER one is accepted with its tail ignored
  // — the forward-compatibility rule the framing itself follows, so a future
  // engine that appends a field stays readable by this one.
  lscan::PhoneImuChunkRecord untouched{};
  untouched.gyro_rad_s[0] = 42.0f;
  CHECK_FALSE(lscan::decode_phone_imu_chunk(ByteSpan(buf, sizeof buf - 1), &untouched));
  CHECK(untouched.gyro_rad_s[0] == 42.0f);
  std::uint8_t longer[lscan::kPhoneImuChunkPayloadBytes + 8];
  std::memcpy(longer, buf, sizeof buf);
  std::memset(longer + sizeof buf, 0xEE, 8);
  lscan::PhoneImuChunkRecord from_long{};
  REQUIRE(lscan::decode_phone_imu_chunk(ByteSpan(longer, sizeof longer), &from_long));
  for (int i = 0; i < 3; ++i) CHECK(from_long.gyro_rad_s[i] == in.gyro_rad_s[i]);

  // --- the routing ---------------------------------------------------------
  CHECK(lscan::stream_of(lscan::ChunkType::kPhoneImu) == StreamId::kImuPhone);
  CHECK(std::string(lscan::stream_file_of(StreamId::kImuPhone)) ==
        std::string(lscan::kPhoneImuStreamFile));
  // NOT the Mid-360's file. This is the whole reason kImuPhone exists as a
  // separate stream: `lscan_is_d6_project()` and A7's post pipeline both read a
  // non-empty StreamId::kImu summary as "Mid-360 project", so a phone sample in
  // imu.bin would route a pure D6 capture to a pipeline that cannot resolve it.
  CHECK(std::string(lscan::stream_file_of(StreamId::kImuPhone)) !=
        std::string(lscan::kImuStreamFile));
  CHECK(std::string(to_string(StreamId::kImuPhone)) == "imu-phone");
  // Engine time, not a device clock: an Android SensorEvent stamp IS
  // CLOCK_BOOTTIME, the same domain ARCore stamps poses in.
  CHECK_FALSE(TimeSync::stream_has_device_clock(StreamId::kImuPhone));

  // --- through a real writer and a real reader -----------------------------
  const std::string dir = fresh_dir("codec") + "/codec.lscan";
  {
    lscan::FileRecordWriter w;
    REQUIRE(w.open(dir).ok());
    for (int k = 0; k < 64; ++k) {
      lscan::PhoneImuChunkRecord s;
      s.gyro_rad_s[0] = static_cast<float>(k) * 0.001f;
      s.gyro_rad_s[1] = -static_cast<float>(k) * 0.002f;
      s.gyro_rad_s[2] = static_cast<float>(k) * 1e-6f;
      s.accel_m_s2[0] = static_cast<float>(k);
      s.accel_m_s2[1] = 0.5f;
      s.accel_m_s2[2] = 9.81f;
      std::uint8_t p[lscan::kPhoneImuChunkPayloadBytes];
      lscan::encode_phone_imu_chunk(s, p);
      REQUIRE(w.write_chunk(lscan::ChunkType::kPhoneImu, 1'000'000LL + k * 2'500'000LL,
                            ByteSpan(p, sizeof p))
                  .ok());
    }
    REQUIRE(w.close().ok());
  }
  CHECK(fs::exists(dir + "/" + lscan::kPhoneImuStreamFile));
  CHECK_FALSE(fs::exists(dir + "/" + lscan::kImuStreamFile));
  {
    lscan::FileRecordReader r;
    REQUIRE(r.open(dir).ok());
    int k = 0;
    for (;;) {
      lscan::ChunkHeader h{};
      std::vector<std::uint8_t> payload;
      const Status st = r.next_chunk(&h, &payload);
      if (st.error() == ScanError::kAgain) break;
      REQUIRE(st.ok());
      REQUIRE(h.type == lscan::ChunkType::kPhoneImu);
      CHECK(r.last_stream() == StreamId::kImuPhone);
      CHECK(h.t_mono_ns == 1'000'000LL + k * 2'500'000LL);
      CHECK(payload.size() == lscan::kPhoneImuChunkPayloadBytes);
      lscan::PhoneImuChunkRecord got{};
      REQUIRE(lscan::decode_phone_imu_chunk(ByteSpan(payload.data(), payload.size()), &got));
      CHECK(got.gyro_rad_s[0] == static_cast<float>(k) * 0.001f);
      CHECK(got.gyro_rad_s[2] == static_cast<float>(k) * 1e-6f);
      CHECK(got.accel_m_s2[0] == static_cast<float>(k));
      ++k;
    }
    CHECK(k == 64);
    (void)r.close();
  }
}

// ===========================================================================
// 2. A SEALED CAPTURE CARRIES ITS IMU TRACK
// ===========================================================================
TEST_CASE("round9/a_sealed_capture_carries_its_phone_imu_in_its_own_stream") {
  const CaptureResult cap = record_walk("carries", /*with_imu=*/true);

  lscan::FileRecordReader r;
  REQUIRE(r.open(cap.dir).ok());
  std::uint64_t d6 = 0, poses = 0, phone_imu = 0, mid360_imu = 0;
  for (const auto& s : r.stream_summaries()) {
    if (s.stream == StreamId::kLidarD6) d6 = s.chunk_count;
    if (s.stream == StreamId::kPoseAr) poses = s.chunk_count;
    if (s.stream == StreamId::kImuPhone) phone_imu = s.chunk_count;
    if (s.stream == StreamId::kImu) mid360_imu = s.chunk_count;
  }
  (void)r.close();

  MESSAGE("sealed container: " << d6 << " kD6Raw, " << poses << " kPoseAr, " << phone_imu
                               << " kPhoneImu chunks");
  CHECK(d6 == cap.d6_chunks);
  CHECK(poses == cap.poses_pushed);
  // THE assertion this half of the round turns on.
  CHECK(phone_imu == cap.imu_pushed);
  CHECK(phone_imu > 1'000);
  CHECK(fs::exists(cap.dir + "/" + lscan::kPhoneImuStreamFile));

  // ... and NOT in imu.bin. A single chunk in the wrong file here would make
  // lscan_is_d6_project() say "Mid-360", and the Review screen would route this
  // capture to A7's pipeline, which returns kNotFound for it.
  CHECK(mid360_imu == 0);
  CHECK_FALSE(fs::exists(cap.dir + "/" + lscan::kImuStreamFile));
  bool is_d6 = false;
  REQUIRE(post::lscan_is_d6_project(cap.dir, &is_d6).ok());
  CHECK(is_d6);

  // The gyro was actually used during the LIVE pass, not silently ignored.
  MESSAGE("live densifier: " << cap.imu.densified << " densified, " << cap.imu.fallbacks
                             << " fallbacks (no-imu " << cap.imu.fallback_no_imu << ", gap "
                             << cap.imu.fallback_gap << ", bracket " << cap.imu.fallback_bracket
                             << ", closing " << cap.imu.fallback_closing << "); worst closing "
                             << cap.imu.worst_closing_deg << " deg");
  CHECK(cap.imu.samples_in == cap.imu_pushed);
  CHECK(cap.imu.samples_rejected == 0);
  CHECK(cap.imu.densified > 1'800);
  // Not one of the four guards fired. Each would mean something specific and
  // each is worth naming, because between them they are the entire failure
  // surface of the feature:
  //   no_imu   nothing is pushing, or the ring does not reach the bracket
  //   gap      the sensor stuttered inside the bracket
  //   bracket  ARCore dropped poses and the span is too wide to trust
  //   closing  the gyro and ARCore disagree past `max_closing_deg`, which in
  //            practice means camera_from_imu is wrong
  // `gap == 0` in particular is what the aligned pose/IMU rates buy (see
  // kPosePeriodNs): with rates that do not divide, ~20 % of returns fall back
  // at the trailing edge for want of a single 2.5 ms step.
  CHECK(cap.imu.fallback_no_imu == 0);
  CHECK(cap.imu.fallback_gap == 0);
  CHECK(cap.imu.fallback_closing == 0);
  CHECK(cap.imu.fallback_bracket == 0);
  // The remaining fallbacks are queries the WRAPPED source could not answer
  // usably at all (before the first pose, or a flagged gate) — the densifier
  // hands those straight back untouched, which is the first branch of
  // sample_at() and not a guard.
  CHECK(cap.imu.densified + cap.imu.fallbacks == cap.imu.queries);
  CHECK(cap.imu.fallbacks * 5 < cap.imu.densified);

  // The cost claim from lscan.h, measured rather than asserted from arithmetic.
  const auto imu_bytes =
      fs::file_size(fs::path(cap.dir) / lscan::kPhoneImuStreamFile);
  const auto lidar_bytes = fs::file_size(fs::path(cap.dir) / lscan::kLidarStreamFile);
  MESSAGE("imu_phone.bin " << imu_bytes << " B vs lidar.bin " << lidar_bytes << " B ("
                           << 100.0 * static_cast<double>(imu_bytes) /
                                  static_cast<double>(lidar_bytes)
                           << "% of the raw UART stream)");
  CHECK(imu_bytes > 0);
}

// ===========================================================================
// 3. REOPENED COLD, IT IS THE SAME CLOUD THE LIVE PASS PRODUCED
// ===========================================================================
TEST_CASE("round9/reopening_an_imu_capture_reproduces_the_live_cloud_exactly") {
  const CaptureResult cap = record_walk("reopen", /*with_imu=*/true);
  const ResolveResult re = reopen(cap.dir, /*densify=*/true);

  MESSAGE("reopened: " << re.stats.lidar_chunks << " D6 chunks, " << re.stats.poses_read
                       << " poses, " << re.stats.imu_read << " IMU samples ("
                       << re.stats.imu_accepted << " accepted; " << re.stats.imu_densified
                       << " returns densified, " << re.stats.imu_fallbacks << " fell back) -> "
                       << re.stats.points_out << " world points");

  CHECK(re.stats.mount_from_manifest);
  CHECK(re.stats.poses_read == cap.poses_pushed);
  CHECK(re.stats.imu_read == cap.imu_pushed);
  CHECK(re.stats.imu_accepted == cap.imu_pushed);
  CHECK(re.stats.imu_densified > 1'000);
  CHECK(re.stats.points_out > 1'500);

  // Not "similar" — the same points. The offline pass runs the same driver, the
  // same pose source, the same densifier and the same assembler over the same
  // chunks, so anything less than equality means one of the two has drifted and
  // the field would get two different rooms from one recording.
  REQUIRE(re.pts.size() == cap.live_map.size());
  CHECK(cloud_mismatches(cap.live_map, re.pts) == 0);

  // And the densifier reached the same internal verdict on both passes, which
  // is the stronger statement: the counts matching means the chronological read
  // presented the gyro to the interpolator in the same order the live capture
  // did, bracket for bracket.
  CHECK(re.stats.imu_densified == cap.imu.densified);
  CHECK(re.stats.imu_fallbacks == cap.imu.fallbacks);

  // It is a straight wall, at the bar ROUND 8 set for the reopened project.
  const double rms = plane_fit_rms(re.pts);
  MESSAGE("reopened wall plane-fit RMS = " << rms * 100.0 << " cm");
  CHECK(rms < 0.02);
}

// ===========================================================================
// 3b. THE OFFLINE RE-RESOLVE ONLY EVER LOOKS BACKWARD
// ===========================================================================
//
// `PushbroomConfig::drain_on_push` is true in the offline pipeline, so a return
// is resolved the moment the chunk carrying it is decoded — there is no second
// pass to fix up a query that guessed. That is safe only because the densifier
// integrates over the bracket the wrapped source ALREADY chose, and a bracket
// exists only once the pose ending it has been pushed; the reader's
// chronological merge then guarantees every IMU sample stamped at or before that
// pose has already arrived.
//
// This case makes the claim falsifiable in the only way that counts: feed the
// SAME container with the IMU stream artificially truncated to its first half.
// If anything read forward, the second half would resolve against samples that
// had not arrived yet. What must happen instead is a clean degradation — the
// early returns densify, the late ones fall back, and the late points come out
// EXACTLY where the no-gyro arm put them.
TEST_CASE("round9/an_offline_resolve_never_reads_imu_it_has_not_reached_yet") {
  const CaptureResult cap = record_walk("backward", /*with_imu=*/true);

  const ResolveResult full = reopen(cap.dir, /*densify=*/true);
  const ResolveResult none = reopen(cap.dir, /*densify=*/false);

  // Truncate imu_phone.bin to its stream header plus the first ~40 % of its
  // chunks. The container's truncated-tail rule makes this a legal .lscan: the
  // reader stops at the first chunk that runs past EOF.
  const std::string half = fresh_dir("halfimu") + "/walk.lscan";
  fs::copy(cap.dir, half, fs::copy_options::recursive);
  const fs::path imu_path = fs::path(half) / lscan::kPhoneImuStreamFile;
  const auto full_bytes = fs::file_size(imu_path);
  std::error_code ec;
  fs::resize_file(imu_path, full_bytes * 4 / 10, ec);
  REQUIRE(!ec);

  const ResolveResult truncated = reopen(half, /*densify=*/true);
  MESSAGE("IMU truncated to 40%: " << truncated.stats.imu_read << " samples read (vs "
                                   << full.stats.imu_read << "), " << truncated.stats.imu_densified
                                   << " densified (vs " << full.stats.imu_densified << ")");

  CHECK(truncated.stats.imu_read > 0);
  CHECK(truncated.stats.imu_read < full.stats.imu_read);
  // Some returns still densified — the ones the surviving samples cover.
  CHECK(truncated.stats.imu_densified > 0);
  CHECK(truncated.stats.imu_densified < full.stats.imu_densified);

  // The shape of the degradation is the actual assertion. Walking all three
  // clouds together: every point is either exactly the densified one or exactly
  // the plain one, never a third value — which is what "the densifier's only
  // two outcomes are the gyro path and the wrapped source's answer" means, and
  // what a forward read would break.
  REQUIRE(truncated.pts.size() == full.pts.size());
  REQUIRE(truncated.pts.size() == none.pts.size());
  std::size_t like_dense = 0, like_plain = 0, neither = 0;
  for (std::size_t i = 0; i < truncated.pts.size(); ++i) {
    const PointVertex& t = truncated.pts[i];
    const PointVertex& d = full.pts[i];
    const PointVertex& p = none.pts[i];
    if (t.x == d.x && t.y == d.y && t.z == d.z) {
      ++like_dense;
    } else if (t.x == p.x && t.y == p.y && t.z == p.z) {
      ++like_plain;
    } else {
      ++neither;
    }
  }
  MESSAGE("truncated-IMU cloud: " << like_dense << " points identical to the full densified "
                                  << "resolve, " << like_plain << " identical to the plain one, "
                                  << neither << " neither");
  CHECK(neither == 0);
  CHECK(like_dense > 0);
  CHECK(like_plain > 0);
}

// ===========================================================================
// 4. THE A/B CONTROL — the same container, resolved with and without the gyro
// ===========================================================================
TEST_CASE("round9/the_same_container_resolved_without_the_gyro_is_measurably_worse") {
  const CaptureResult cap = record_walk("ab", /*with_imu=*/true);

  const ResolveResult with = reopen(cap.dir, /*densify=*/true);
  const ResolveResult without = reopen(cap.dir, /*densify=*/false);

  // The reference: the SAME walk recorded with a 1 kHz pose stream, so there is
  // nothing left between two poses to interpolate. Its D6 bytes are identical
  // (the analytic ranges do not depend on how often the trajectory was sampled),
  // so point i is the same return in all three clouds and the distance between
  // them is purely trajectory-estimation error. See cloud_rms_to().
  const CaptureResult truth_cap = record_walk("abtruth", /*with_imu=*/false,
                                              /*pose_period_ns=*/1'000'000LL);
  const ResolveResult truth = reopen(truth_cap.dir, /*densify=*/false);

  // Same returns every way — the gyro changes WHERE each point goes, never
  // whether it resolves. If these ever diverge the comparison below is
  // measuring a different point set, not a different trajectory.
  REQUIRE(with.pts.size() == without.pts.size());
  REQUIRE(truth.pts.size() == without.pts.size());
  CHECK(without.stats.imu_read == with.stats.imu_read);  // read, but not used
  CHECK(without.stats.imu_densified == 0);
  CHECK(with.stats.imu_densified > 1'000);

  const double err_with = cloud_rms_to(with.pts, truth.pts);
  const double err_without = cloud_rms_to(without.pts, truth.pts);
  MESSAGE("re-resolved from ONE container, 3D error against the 1 kHz truth: with gyro "
          << err_with * 100.0 << " cm, without " << err_without * 100.0 << " cm ("
          << err_without / err_with << "x better)");
  // Printed to show WHY the 3D error above is the metric and flatness is not:
  // the TRUTH arm's wall is no flatter than either estimate's. That residual is
  // the fixture's own floor (packet-time modelling vs per-sample resolve times,
  // plus the datasheet angle correction), it is common to all three arms, and a
  // flatness comparison would be reading it rather than the trajectory.
  MESSAGE("wall plane-fit RMS for the same three clouds: with gyro "
          << plane_fit_rms(with.pts) * 100.0 << " cm, without "
          << plane_fit_rms(without.pts) * 100.0 << " cm, 1 kHz truth "
          << plane_fit_rms(truth.pts) * 100.0 << " cm — all three at the fixture's floor");

  // 1. The two arms genuinely differ — the toggle is not a no-op.
  CHECK(cloud_mismatches(with.pts, without.pts) > with.pts.size() / 2);
  // 2. And the gyro arm is the one closer to truth, by a margin that is not
  //    subtle: 0.036 cm against 2.68 cm on this fixture, i.e. ~75x, with the
  //    bar set at 20x so ordinary drift in the numbers does not fail the build.
  //    This is the whole claim of item 35, restated against a file on disk
  //    rather than against an in-memory fixture.
  CHECK(err_with < 0.05 * err_without);
  // 3. The corroboration, and the reason the flatness numbers below are printed
  //    rather than asserted on: the densified arm's residual against truth is
  //    smaller than the fixture's own floor, so on this stimulus the recorded
  //    gyro recovers essentially ALL of the sub-pose motion, not merely some.
  CHECK(err_with < 0.001);  // sub-millimetre

  // 3. THE FALSIFIABLE HALF. Strip the kPhoneImu chunks and the densified arm
  //    must collapse onto the plain one EXACTLY — not approximately. That is
  //    what proves the improvement above comes from the recorded gyro and not
  //    from some constant advantage of the densified code path.
  const std::string stripped = fresh_dir("stripped") + "/walk.lscan";
  fs::copy(cap.dir, stripped, fs::copy_options::recursive);
  std::error_code ec;
  fs::remove(fs::path(stripped) / lscan::kPhoneImuStreamFile, ec);
  REQUIRE(!ec);

  const ResolveResult starved = reopen(stripped, /*densify=*/true);
  MESSAGE("kPhoneImu chunks deleted: " << starved.stats.imu_read << " read, "
                                       << starved.stats.imu_densified << " densified, "
                                       << starved.stats.imu_fallbacks << " fallbacks");
  CHECK(starved.stats.imu_read == 0);
  CHECK(starved.stats.imu_densified == 0);
  CHECK(starved.stats.imu_fallbacks > 0);  // it TRIED, on every query
  REQUIRE(starved.pts.size() == without.pts.size());
  CHECK(cloud_mismatches(starved.pts, without.pts) == 0);
}

// ===========================================================================
// 5. THE INERTNESS CONTROL — an unfed densifier changes nothing
// ===========================================================================
TEST_CASE("round9/an_engine_that_is_never_given_an_imu_behaves_exactly_as_before") {
  const CaptureResult cap = record_walk("noimu", /*with_imu=*/false);

  CHECK(cap.imu_pushed == 0);
  CHECK(cap.imu.samples_in == 0);
  CHECK(cap.imu.densified == 0);
  // Every query fell through, and it said WHY.
  CHECK(cap.imu.queries > 1'000);
  CHECK(cap.imu.fallback_no_imu > 1'000);

  // No IMU stream file is created at all — a capture that pushes nothing pays
  // nothing, which is A5's lazy stream-file rule applied to the new stream.
  CHECK_FALSE(fs::exists(cap.dir + "/" + lscan::kPhoneImuStreamFile));

  // THE assertion: resolving this container through the RAW ExternalPoseSource
  // (densify off — literally the pre-ROUND-9 code path) reproduces the live
  // cloud bit for bit. The densifier is wired between the pose source and the
  // assembler unconditionally, and this is the proof that doing so cost an app
  // which never calls push_phone_imu() exactly nothing.
  const ResolveResult plain = reopen(cap.dir, /*densify=*/false);
  REQUIRE(plain.pts.size() == cap.live_map.size());
  CHECK(cloud_mismatches(cap.live_map, plain.pts) == 0);

  // ... and so does resolving it with densification ON, because an empty ring
  // falls back on every query. A pre-ROUND-9 container therefore re-resolves
  // identically whatever the caller asks for, which is what makes
  // `densify_with_phone_imu = true` a safe default for every `.lscan` already
  // in existence.
  const ResolveResult densified = reopen(cap.dir, /*densify=*/true);
  REQUIRE(densified.pts.size() == cap.live_map.size());
  CHECK(cloud_mismatches(cap.live_map, densified.pts) == 0);
  CHECK(densified.stats.imu_read == 0);
}

// ===========================================================================
// 6. REPLAY == CAPTURE, WITH THE IMU IN THE LOOP
// ===========================================================================
TEST_CASE("round9/replaying_a_capture_feeds_the_imu_back_through_the_same_entry_point") {
  const CaptureResult cap = record_walk("replay", /*with_imu=*/true);

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

  REQUIRE(e.set_imu_extrinsics(kCameraFromImu).ok());
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

  MESSAGE("replay: " << replay.stats().chunks_replayed << " kD6Raw, "
                     << replay.stats().poses_replayed << " kPoseAr, "
                     << replay.stats().imu_replayed << " kPhoneImu chunks");
  CHECK(replay.stats().chunks_replayed == cap.d6_chunks);
  CHECK(replay.stats().poses_replayed == cap.poses_pushed);
  // ReplayConfig::replay_phone_imu defaults to true, and this is why: the same
  // bytes and the same poses WITHOUT the gyro replay into a measurably
  // different room (case 4 above).
  CHECK(replay.stats().imu_replayed == cap.imu_pushed);

  const std::vector<PointVertex> replayed = drain_stream(e.points(), StreamId::kSlamMap);
  REQUIRE(replayed.size() == cap.live_map.size());
  CHECK(cloud_mismatches(cap.live_map, replayed) == 0);
  CHECK(e.imu_densify_stats().densified == cap.imu.densified);
  REQUIRE(e.stop_session().ok());

  // The control, one flag away: replaying the poses but NOT the IMU.
  {
    auto e2r = Engine::create(ec);
    REQUIRE(e2r.ok());
    Engine& e2 = *e2r.value();
    DeviceConfig dc2{};
    dc2.kind = DeviceKind::kD6;
    dc2.d6.send_start_stop_commands = false;
    const auto id2 = e2.add_device(dc2);
    REQUIRE(id2.ok());
    REQUIRE(e2.set_imu_extrinsics(kCameraFromImu).ok());
    REQUIRE(e2.set_mount_extrinsics(mount).ok());
    REQUIRE(e2.set_pushbroom_enabled(true).ok());
    SessionConfig sc2{};
    sc2.record = false;
    sc2.pushbroom = true;
    REQUIRE(e2.start_session(sc2).ok());

    lscan::ReplayConfig rc2 = rc;
    rc2.target_device = id2.value();
    rc2.replay_phone_imu = false;  // the pre-ROUND-9 world, on purpose
    lscan::ReplaySource replay2(e2);
    REQUIRE(replay2.run(rc2).ok());
    REQUIRE(e2.pushbroom_flush().ok());
    CHECK(replay2.stats().imu_replayed == 0);

    const std::vector<PointVertex> plain = drain_stream(e2.points(), StreamId::kSlamMap);
    REQUIRE(plain.size() == replayed.size());
    MESSAGE("replay without the IMU: " << cloud_mismatches(plain, replayed)
                                       << " of " << plain.size()
                                       << " points land somewhere else");
    CHECK(cloud_mismatches(plain, replayed) > plain.size() / 2);
    REQUIRE(e2.stop_session().ok());
  }
}
