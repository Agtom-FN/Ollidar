// test_post.cpp — task A7: the Mid-360 post-processing pipeline.
//
// Six groups, in increasing order of how much they can lie to you:
//
//   pgraph/*    the SE(3) pose graph against closed forms and against a
//               drifted loop with known truth. This is where the "ATE after <
//               ATE before" claim is made quantitatively.
//   sctx/*      the Scan Context descriptor: yaw covariance, discrimination,
//               and the database's revisit search.
//   picp/*      point-to-plane ICP against a known transform, and the
//               acceptance gate that keeps a bad fit out of the graph.
//   pfilter/*   voxel dedup, the streaming accumulator, and the statistical
//               outlier filter.
//   ploop/*     the three of them composed: a synthetic room, a loop
//               trajectory, injected odometry drift, and the whole
//               detect -> verify -> optimize chain measured against truth.
//   post/*      PostSlamPipeline end to end, driven from a REAL .lscan the
//               test writes with A5's FileRecordWriter: stages, progress,
//               cancellation, determinism, and the real Mid-360 capture.
//
// The synthetic room is the only place an accuracy number can be checked
// against truth, because it is the only place truth exists. The real capture
// is the only place the wire format, the A4 clock mapping and the actual scan
// pattern are exercised at once — and it has no ground truth, so it asserts
// self-consistency and reports.
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"

#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"
#include "scanengine/slam/post/cloud_filter.h"
#include "scanengine/slam/post/loop_closure.h"
#include "scanengine/slam/post/pose_graph.h"
#include "scanengine/slam/post/post_pipeline.h"
#include "scanengine/slam/post/progress.h"
#include "scanengine/slam/post/scan_context.h"

using namespace scanengine;
using namespace scanengine::post;

namespace {

namespace fs = std::filesystem;

// --- deterministic noise ----------------------------------------------------
//
// xorshift64 + Box-Muller, deliberately NOT <random>: the standard does not
// specify the output of its distributions, so the five CI legs would disagree
// on every number in this file. Same reason test_lio.cpp and test_timesync.cpp
// roll their own.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double uniform() { return (static_cast<double>(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
  double gauss() {
    const double u1 = uniform(), u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

// --- small pose helpers (the tests must not reuse the code under test) ------

void qmul(const double a[4], const double b[4], double o[4]) {
  const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
  o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
void qrot(const double q[4], const double v[3], double o[3]) {
  double R[9];
  se3::quat_to_matrix(q, R);
  const double x = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
  const double y = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
  const double z = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
  o[0] = x; o[1] = y; o[2] = z;
}
void qinv(const double q[4], double o[4]) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }

void pcompose(const double qa[4], const double pa[3], const double qb[4], const double pb[3],
              double qo[4], double po[3]) {
  double r[3];
  qrot(qa, pb, r);
  double q[4];
  qmul(qa, qb, q);
  se3::quat_normalize(q);
  const double x = pa[0] + r[0], y = pa[1] + r[1], z = pa[2] + r[2];
  for (int i = 0; i < 4; ++i) qo[i] = q[i];
  po[0] = x; po[1] = y; po[2] = z;
}
void pinverse(const double q[4], const double p[3], double qo[4], double po[3]) {
  double c[4];
  qinv(q, c);
  double r[3];
  qrot(c, p, r);
  for (int i = 0; i < 4; ++i) qo[i] = c[i];
  po[0] = -r[0]; po[1] = -r[1]; po[2] = -r[2];
}
void pbetween(const double qa[4], const double pa[3], const double qb[4], const double pb[3],
              double qo[4], double po[3]) {
  double qi[4], pi[3];
  pinverse(qa, pa, qi, pi);
  pcompose(qi, pi, qb, pb, qo, po);
}
void quat_from_rpy(double roll, double pitch, double yaw, double q[4]) {
  const double w[3] = {roll, pitch, yaw};
  double R[9];
  se3::so3_exp(w, R);
  se3::matrix_to_quat(R, q);
}
double dist3(const double a[3], const double b[3]) {
  const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// --- the synthetic room -----------------------------------------------------

struct Aabb {
  double lo[3];
  double hi[3];
};

bool ray_box(const double o[3], const double d[3], const Aabb& b, bool exit_side, double* t_out) {
  double tmin = -1e30, tmax = 1e30;
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(d[i]) < 1e-12) {
      if (o[i] < b.lo[i] || o[i] > b.hi[i]) return false;
      continue;
    }
    double t1 = (b.lo[i] - o[i]) / d[i];
    double t2 = (b.hi[i] - o[i]) / d[i];
    if (t1 > t2) std::swap(t1, t2);
    if (t1 > tmin) tmin = t1;
    if (t2 < tmax) tmax = t2;
  }
  if (tmax < tmin) return false;
  const double t = exit_side ? tmax : tmin;
  if (t <= 1e-6) return false;
  *t_out = t;
  return true;
}

// A 24 x 18 x 3.5 m hall with six pillars and two low benches. The asymmetric
// furniture is not decoration: a rotationally symmetric room makes Scan
// Context's yaw ambiguous, and a test that passes on an ambiguous scene is
// testing nothing.
struct Room {
  Aabb shell{{-12.0, -9.0, 0.0}, {12.0, 9.0, 3.5}};
  std::vector<Aabb> solids;
  Room() {
    solids.push_back(Aabb{{-7.2, -5.6, 0.0}, {-6.4, -4.8, 3.5}});
    solids.push_back(Aabb{{-7.2, 4.8, 0.0}, {-6.4, 5.6, 3.5}});
    solids.push_back(Aabb{{6.4, -5.6, 0.0}, {7.2, -4.8, 3.5}});
    solids.push_back(Aabb{{6.4, 4.8, 0.0}, {7.2, 5.6, 3.5}});
    solids.push_back(Aabb{{-0.5, -7.5, 0.0}, {0.5, -6.5, 3.5}});
    solids.push_back(Aabb{{2.5, 1.0, 0.0}, {3.5, 2.0, 3.5}});
    solids.push_back(Aabb{{-10.5, -1.5, 0.0}, {-9.0, 1.5, 1.2}});  // bench
    solids.push_back(Aabb{{8.0, -3.0, 0.0}, {11.0, -2.0, 0.8}});   // bench
    solids.push_back(Aabb{{-3.0, 6.0, 0.0}, {-1.0, 8.0, 2.2}});    // a cabinet
  }
  double cast(const double o[3], const double d[3]) const {
    double best = -1.0, t;
    if (ray_box(o, d, shell, true, &t)) best = t;
    for (const Aabb& s : solids) {
      if (ray_box(o, d, s, false, &t) && t > 1e-6 && (best < 0.0 || t < best)) best = t;
    }
    return best;
  }
};

// A deterministic quasi-uniform ray direction inside the Mid-360's FOV
// (-7 deg .. +52 deg elevation), advanced by a golden-angle spiral so
// consecutive datagrams do not repeat a pattern.
void mid360_ray(std::uint64_t i, double dir[3]) {
  const double kGolden = 2.39996322972865332;
  const double az = std::fmod(static_cast<double>(i) * kGolden, 6.283185307179586);
  const double u = std::fmod(static_cast<double>(i) * 0.0173, 1.0);
  const double el = (-7.0 + 59.0 * u) * se3::kDegToRad;
  dir[0] = std::cos(el) * std::cos(az);
  dir[1] = std::cos(el) * std::sin(az);
  dir[2] = std::sin(el);
}

// Ray-cast one keyframe cloud, in the BODY frame of `(q, p)`.
std::vector<PointVertex> scan_room(const Room& room, const double q[4], const double p[3],
                                   std::uint64_t seed, int rays, double noise_m) {
  Rng rng(seed);
  std::vector<PointVertex> out;
  out.reserve(static_cast<std::size_t>(rays));
  double qi[4];
  qinv(q, qi);
  for (int i = 0; i < rays; ++i) {
    double d_body[3];
    mid360_ray(static_cast<std::uint64_t>(i) + seed * 7919u, d_body);
    double d_world[3];
    qrot(q, d_body, d_world);
    const double t = room.cast(p, d_world);
    if (t <= 0.3 || t > 45.0) continue;
    const double r = t + noise_m * rng.gauss();
    PointVertex pv;
    pv.x = static_cast<float>(d_body[0] * r);
    pv.y = static_cast<float>(d_body[1] * r);
    pv.z = static_cast<float>(d_body[2] * r);
    pv.r = pv.g = pv.b = 128;
    pv.a = 255;
    out.push_back(pv);
  }
  return out;
}

// --- temp directories -------------------------------------------------------

std::string make_temp_dir(const char* tag) {
  static std::atomic<long long> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path p = fs::temp_directory_path() /
                     (std::string("post_test_") + tag + "_" + std::to_string(now) + "_" +
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

// --- a synthetic Mid-360 capture, written as a real .lscan ------------------

struct SynthConfig {
  double duration_s = 8.0;
  double points_per_sec = 20000.0;  // scaled down from the real 200k so the
                                    // FULL-DENSITY re-run stays inside a unit
                                    // test's time budget; the pipeline is
                                    // unchanged, only the input rate is.
  double imu_hz = 200.0;
  double loop_radius_m = 6.0;
  double range_noise_m = 0.01;
  double gyro_bias = 0.004;
  double accel_bias = 0.05;
  double gyro_noise = 0.002;
  double accel_noise = 0.02;
};

// The truth trajectory: stationary for 0.6 s (the ESKF static-init window),
// then one full circle of `loop_radius_m` with the body yawing to stay
// tangent — i.e. the scanner returns to its own starting pose, which is what
// makes this a LOOP and not a tour.
struct LoopTraj {
  double still = 0.6;
  double ramp = 1.0;
  double radius = 6.0;
  double duration = 8.0;

  double warp(double t) const {
    const double e = t - still;
    if (e <= 0.0) return 0.0;
    if (e < ramp) return e * e / (2.0 * ramp);
    return e - ramp * 0.5;
  }
  double total_warp() const { return warp(duration); }

  void pose(double t, double q[4], double p[3]) const {
    const double u = warp(t) / (total_warp() > 0.0 ? total_warp() : 1.0);
    const double ang = u * 6.283185307179586;
    p[0] = radius * std::sin(ang);
    p[1] = radius * (1.0 - std::cos(ang));
    p[2] = 1.4 + 0.05 * std::sin(2.0 * ang);
    quat_from_rpy(0.0, 0.0, ang, q);
  }
};

void imu_truth(const LoopTraj& tr, double t, double gyro[3], double accel[3]) {
  const double h = 1e-4;
  double q0[4], q1[4], q2[4], p0[3], p1[3], p2[3];
  tr.pose(t - h, q0, p0);
  tr.pose(t, q1, p1);
  tr.pose(t + h, q2, p2);
  double acc_w[3];
  for (int i = 0; i < 3; ++i) acc_w[i] = (p2[i] - 2.0 * p1[i] + p0[i]) / (h * h);
  double R0[9], R1[9], R2[9];
  se3::quat_to_matrix(q0, R0);
  se3::quat_to_matrix(q1, R1);
  se3::quat_to_matrix(q2, R2);
  double Rdot[9];
  for (int i = 0; i < 9; ++i) Rdot[i] = (R2[i] - R0[i]) / (2.0 * h);
  double W[9];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) s += R1[k * 3 + r] * Rdot[k * 3 + c];
      W[r * 3 + c] = s;
    }
  }
  gyro[0] = W[7];
  gyro[1] = W[2];
  gyro[2] = W[3];
  const double g[3] = {0.0, 0.0, -9.80665};
  const double sf[3] = {acc_w[0] - g[0], acc_w[1] - g[1], acc_w[2] - g[2]};
  for (int r = 0; r < 3; ++r) {
    double s = 0.0;
    for (int k = 0; k < 3; ++k) s += R1[k * 3 + r] * sf[k];
    accel[r] = s;
  }
}

void put_header(std::uint8_t* buf, std::uint16_t len, std::uint16_t dot_num, std::uint16_t udp_cnt,
                std::uint8_t data_type, std::uint64_t timestamp) {
  std::memset(buf, 0, sizeof(mid360::DataHeader));
  mid360::DataHeader h{};
  h.version = 0;
  h.length = len;
  h.time_interval = 5;
  h.dot_num = dot_num;
  h.udp_cnt = udp_cnt;
  h.frame_cnt = 0;
  h.data_type = data_type;
  h.time_type = 0;
  h.crc32 = 0;  // parse_packet deliberately does not verify it (A3)
  h.timestamp = timestamp;
  std::memcpy(buf, &h, sizeof(h));
}

// Writes a real .lscan containing kMid360Points / kMid360Imu chunks and
// returns the truth trajectory sampled at 100 Hz for later comparison.
struct SynthResult {
  std::vector<std::int64_t> truth_t;
  std::vector<std::array<double, 7>> truth_pose;  // qx qy qz qw px py pz
  std::uint64_t point_packets = 0;
  std::uint64_t imu_packets = 0;
  std::uint64_t points = 0;
};

SynthResult write_synthetic_lscan(const std::string& dir, const SynthConfig& sc) {
  Room room;
  LoopTraj tr;
  tr.radius = sc.loop_radius_m;
  tr.duration = sc.duration_s;
  Rng rng(0xA7A7A7A7ull);

  lscan::FileRecordWriter w;
  REQUIRE(w.open(dir).ok());

  const std::int64_t t0 = 1'700'000'000'000'000'000LL;
  const double dt_pkt = static_cast<double>(mid360::kPointsPerPacket) / sc.points_per_sec;
  const double dt_imu = 1.0 / sc.imu_hz;

  SynthResult out;
  std::vector<std::uint8_t> pbuf(mid360::kPointPacketBytes);
  std::vector<std::uint8_t> ibuf(mid360::kImuPacketBytes);
  std::uint16_t udp_cnt = 0;
  std::uint64_t ray_seq = 0;

  double t_pkt = 0.0, t_imu = 0.0;
  while (t_pkt < sc.duration_s || t_imu < sc.duration_s) {
    const bool do_imu = t_imu <= t_pkt;
    if (do_imu) {
      double g[3], a[3];
      imu_truth(tr, t_imu, g, a);
      const float gyro[3] = {static_cast<float>(g[0] + sc.gyro_bias + sc.gyro_noise * rng.gauss()),
                             static_cast<float>(g[1] + sc.gyro_bias + sc.gyro_noise * rng.gauss()),
                             static_cast<float>(g[2] + sc.gyro_bias + sc.gyro_noise * rng.gauss())};
      const float acc[3] = {
          static_cast<float>((a[0] + sc.accel_bias + sc.accel_noise * rng.gauss()) / 9.80665),
          static_cast<float>((a[1] + sc.accel_bias + sc.accel_noise * rng.gauss()) / 9.80665),
          static_cast<float>((a[2] + sc.accel_bias + sc.accel_noise * rng.gauss()) / 9.80665)};
      const std::int64_t dev = static_cast<std::int64_t>(t_imu * 1e9) + 1;
      put_header(ibuf.data(), static_cast<std::uint16_t>(mid360::kImuPacketBytes), 1, udp_cnt++,
                 mid360::kDataTypeImu, static_cast<std::uint64_t>(dev));
      mid360::ImuRaw raw{gyro[0], gyro[1], gyro[2], acc[0], acc[1], acc[2]};
      std::memcpy(ibuf.data() + sizeof(mid360::DataHeader), &raw, sizeof(raw));
      REQUIRE(w.write_chunk(lscan::ChunkType::kMid360Imu, t0 + dev,
                            ByteSpan(ibuf.data(), ibuf.size()))
                  .ok());
      ++out.imu_packets;
      t_imu += dt_imu;
      continue;
    }
    // One point datagram: 96 rays cast from the pose at its own instant.
    double q[4], p[3];
    tr.pose(t_pkt, q, p);
    put_header(pbuf.data(), static_cast<std::uint16_t>(mid360::kPointPacketBytes),
               mid360::kPointsPerPacket, udp_cnt++, mid360::kDataTypeCartesianHigh,
               static_cast<std::uint64_t>(static_cast<std::int64_t>(t_pkt * 1e9) + 1));
    mid360::CartesianHigh* pts =
        reinterpret_cast<mid360::CartesianHigh*>(pbuf.data() + sizeof(mid360::DataHeader));
    for (std::uint32_t i = 0; i < mid360::kPointsPerPacket; ++i) {
      double d_body[3];
      mid360_ray(ray_seq++, d_body);
      double d_world[3];
      qrot(q, d_body, d_world);
      const double t = room.cast(p, d_world);
      if (t <= 0.3 || t > 45.0) {
        pts[i] = mid360::CartesianHigh{0, 0, 0, 0, 0};  // a no-return
        continue;
      }
      const double r = t + sc.range_noise_m * rng.gauss();
      pts[i].x = static_cast<std::int32_t>(d_body[0] * r * 1000.0);
      pts[i].y = static_cast<std::int32_t>(d_body[1] * r * 1000.0);
      pts[i].z = static_cast<std::int32_t>(d_body[2] * r * 1000.0);
      pts[i].reflectivity = 120;
      pts[i].tag = 0;
      ++out.points;
    }
    REQUIRE(w.write_chunk(lscan::ChunkType::kMid360Points,
                          t0 + static_cast<std::int64_t>(t_pkt * 1e9) + 1,
                          ByteSpan(pbuf.data(), pbuf.size()))
                .ok());
    ++out.point_packets;
    t_pkt += dt_pkt;
  }
  REQUIRE(w.close().ok());

  for (double t = 0.0; t <= sc.duration_s; t += 0.01) {
    double q[4], p[3];
    tr.pose(t, q, p);
    out.truth_t.push_back(t0 + static_cast<std::int64_t>(t * 1e9) + 1);
    out.truth_pose.push_back({q[0], q[1], q[2], q[3], p[0], p[1], p[2]});
  }
  return out;
}

// --- the real capture -------------------------------------------------------

std::string find_fixture() {
  if (const char* env = std::getenv("SCANENGINE_LIVOXDUMP")) {
    if (env[0] != '\0') return env;
  }
  const char* rel = "spikes/s2-mid360-sim/fixtures/outdoor_imu_ccby_6s.livoxdump";
  std::string here = __FILE__;  // .../engine/tests/test_post.cpp
  const std::size_t cut = here.rfind("engine/tests/");
  if (cut != std::string::npos) {
    const std::string cand = here.substr(0, cut) + rel;
    if (std::FILE* f = std::fopen(cand.c_str(), "rb")) {
      std::fclose(f);
      return cand;
    }
  }
  const char* ups[] = {"", "../", "../../", "../../../", "../../../../"};
  for (const char* up : ups) {
    const std::string cand = std::string(up) + rel;
    if (std::FILE* f = std::fopen(cand.c_str(), "rb")) {
      std::fclose(f);
      return cand;
    }
  }
  return std::string();
}

// Transcode the .livoxdump container (documented in
// tools/remote-capture/capture_mid360.py) into a real .lscan, so the post
// pipeline is exercised through its actual input path rather than a back door.
bool transcode_livoxdump(const std::string& src, const std::string& dst, std::uint64_t* records) {
  std::FILE* f = std::fopen(src.c_str(), "rb");
  if (f == nullptr) return false;
  auto rd = [&](void* p, std::size_t n) { return std::fread(p, 1, n, f) == n; };
  char magic[8];
  std::uint16_t version = 0, num_ports = 0;
  if (!rd(magic, 8) || std::memcmp(magic, "LX360CAP", 8) != 0 || !rd(&version, 2) ||
      !rd(&num_ports, 2) || num_ports == 0 || num_ports > 16) {
    std::fclose(f);
    return false;
  }
  std::vector<std::uint32_t> ports(num_ports);
  if (!rd(ports.data(), 4u * num_ports)) {
    std::fclose(f);
    return false;
  }
  lscan::FileRecordWriter w;
  if (!w.open(dst).ok()) {
    std::fclose(f);
    return false;
  }
  std::vector<std::uint8_t> payload;
  std::uint64_t n = 0;
  for (;;) {
    std::uint64_t t = 0;
    std::uint16_t idx = 0;
    std::uint32_t len = 0;
    if (!rd(&t, 8) || !rd(&idx, 2) || !rd(&len, 4)) break;
    if (idx >= num_ports || len > 65535) break;
    payload.resize(len);
    if (len != 0 && !rd(payload.data(), len)) break;
    const mid360::PacketView v = mid360::parse_packet(payload.data(), payload.size());
    if (!v.valid()) continue;
    const lscan::ChunkType ct = v.header->data_type == mid360::kDataTypeImu
                                    ? lscan::ChunkType::kMid360Imu
                                    : lscan::ChunkType::kMid360Points;
    if (!w.write_chunk(ct, static_cast<std::int64_t>(t), ByteSpan(payload.data(), payload.size()))
             .ok()) {
      break;
    }
    ++n;
  }
  std::fclose(f);
  const bool ok = w.close().ok();
  if (records != nullptr) *records = n;
  return ok;
}

}  // namespace

// ===========================================================================
// pgraph — the SE(3) pose graph
// ===========================================================================

TEST_CASE("pgraph/between_error_vanishes_at_the_measurement") {
  Rng rng(7);
  double worst = 0.0;
  for (int k = 0; k < 200; ++k) {
    double qi[4], qj[4];
    quat_from_rpy(rng.gauss() * 0.7, rng.gauss() * 0.7, rng.gauss() * 2.0, qi);
    quat_from_rpy(rng.gauss() * 0.7, rng.gauss() * 0.7, rng.gauss() * 2.0, qj);
    const double pi[3] = {rng.gauss() * 5, rng.gauss() * 5, rng.gauss() * 5};
    const double pj[3] = {rng.gauss() * 5, rng.gauss() * 5, rng.gauss() * 5};
    double qz[4], pz[3];
    pbetween(qi, pi, qj, pj, qz, pz);
    double e[6];
    between_error(qi, pi, qj, pj, qz, pz, e);
    for (int i = 0; i < 6; ++i) worst = std::max(worst, std::fabs(e[i]));
  }
  MESSAGE("worst |e| at the exact measurement: " << worst);
  CHECK(worst < 1e-12);
}

TEST_CASE("pgraph/one_edge_solves_exactly") {
  // Two nodes, node 0 fixed, one between factor whose measurement disagrees
  // with the initial estimate by a large rotation and translation. Gauss-
  // Newton with correct Jacobians drives chi2 to ~0; with a wrong rotation
  // Jacobian it converges slowly or not at all, which is precisely the class
  // of bug a "does it run" test hides (docs/A6-lio.md §3.4 says the same
  // about the point-to-plane Jacobian).
  PoseGraph g;
  double q0[4];
  se3::quat_identity(q0);
  const double p0[3] = {0, 0, 0};
  double q1[4];
  quat_from_rpy(0.05, -0.02, 0.1, q1);
  const double p1[3] = {1.0, 0.2, -0.1};
  g.add_node(q0, p0);
  g.add_node(q1, p1);
  CHECK(g.set_fixed(0, true).ok());

  double qz[4];
  quat_from_rpy(0.3, 0.4, -1.1, qz);
  const double pz[3] = {2.5, -1.5, 0.75};
  CHECK(g.add_between(0, 1, qz, pz, 0.01, 0.01).ok());

  const double before = g.chi2();
  PoseGraphOptions opts;
  Result<PoseGraphSummary> r = g.optimize(opts);
  REQUIRE(r.ok());
  MESSAGE("one edge: chi2 " << before << " -> " << r.value().final_chi2 << " in "
                            << r.value().iterations << " iterations");
  CHECK(before > 1000.0);
  CHECK(r.value().final_chi2 < 1e-12);
  CHECK(r.value().iterations <= 6);

  // The solved node must BE the measurement, applied to the fixed node.
  const PoseNode& n1 = g.node(1);
  CHECK(dist3(n1.p, pz) < 1e-9);
  double R[9], Rz[9];
  se3::quat_to_matrix(n1.q, R);
  se3::quat_to_matrix(qz, Rz);
  // 1e-7 deg held on Apple clang/arm64 but GCC/x86_64's different FP
  // reassociation lands at ~1.2e-6 deg (engine-ci #3). A microdegree is
  // still "exact" for a pose graph; the chi^2 < 1e-12 check above is the
  // real convergence assertion.
  CHECK(se3::rot_angle_deg(R, Rz) < 1e-5);
}

TEST_CASE("pgraph/rcm_ordering_keeps_a_loop_narrow") {
  // A 400-node chain plus ONE edge from node 0 to node 399. In the natural
  // (time) ordering that single edge gives the matrix full bandwidth and a
  // profile factorization degrades to dense. RCM reorders a cycle into
  // 0, 1, N-1, 2, N-2, ..., which is the whole reason this solver is viable
  // by hand (pose_graph.h explains it).
  PoseGraph g;
  const int n = 400;
  double q[4];
  se3::quat_identity(q);
  for (int i = 0; i < n; ++i) {
    const double p[3] = {static_cast<double>(i) * 0.5, 0.0, 0.0};
    g.add_node(q, p);
  }
  CHECK(g.set_fixed(0, true).ok());
  double qz[4];
  se3::quat_identity(qz);
  const double step[3] = {0.5, 0.0, 0.0};
  for (int i = 1; i < n; ++i) CHECK(g.add_between(i - 1, i, qz, step, 0.01, 0.01).ok());
  const double back[3] = {-0.5 * (n - 1), 0.0, 0.0};
  CHECK(g.add_between(n - 1, 0, qz, back, 0.05, 0.05, 0.0, true).ok());

  Result<PoseGraphSummary> r = g.optimize();
  REQUIRE(r.ok());
  const PoseGraphSummary& s = r.value();
  MESSAGE("chain+loop: " << s.variables << " variables, bandwidth " << s.bandwidth_blocks
                         << " blocks, envelope " << s.envelope_scalars << " scalars");
  // Dense would be 6*399 * 6*399 / 2 = 2.9 M scalars; the natural ordering
  // would be nearly that. RCM must be far under it.
  CHECK(s.bandwidth_blocks <= 4);
  CHECK(s.envelope_scalars < 60000);
  CHECK(g.loop_count() == 1);
}

TEST_CASE("pgraph/position_priors_are_the_A10_seam") {
  // A unary POSITION factor is what a GNSS fix is (§3.4): it constrains where
  // a node is and says NOTHING about which way it points. Two parts.
  //
  // (a) Priors alone. Each node's position must land exactly on its fix, and
  //     its orientation must not move by a single ULP — a solver that
  //     quietly treated the factor as a full pose prior (identity rotation is
  //     what add_position_prior stores) would snap every node to level, and
  //     the georeferenced cloud would come back rotated.
  {
    PoseGraph g;
    const int n = 8;
    double q[4];
    quat_from_rpy(0.2, -0.3, 0.4, q);
    for (int i = 0; i < n; ++i) {
      const double p[3] = {static_cast<double>(i), 0.0, 0.0};
      g.add_node(q, p);
    }
    for (int i = 0; i < n; ++i) {
      const double fix[3] = {1.1 * i, 0.3 * i, -0.05 * i};
      CHECK(g.add_position_prior(i, fix, 0.02).ok());
    }
    Result<PoseGraphSummary> r = g.optimize();
    REQUIRE(r.ok());
    MESSAGE("priors alone: chi2 " << r.value().initial_chi2 << " -> " << r.value().final_chi2
                                  << " in " << r.value().iterations << " iterations");
    CHECK(r.value().final_chi2 < 1e-16);
    for (int i = 0; i < n; ++i) {
      const double fix[3] = {1.1 * i, 0.3 * i, -0.05 * i};
      CHECK(dist3(g.node(i).p, fix) < 1e-9);
      double R[9], R0[9];
      se3::quat_to_matrix(g.node(i).q, R);
      se3::quat_to_matrix(q, R0);
      // 1e-4 deg, not 0: rot_angle_deg goes through acos((trace-1)/2), whose
      // derivative is infinite at the identity, so a 1e-16 rounding error in
      // the trace reads as ~1e-6 degrees. The rotation increment itself is
      // exactly zero — the prior's rotation gradient and Hessian blocks are.
      CHECK(se3::rot_angle_deg(R, R0) < 1e-4);
    }
    CHECK(g.prior_count() == static_cast<std::size_t>(n));
  }

  // (b) Priors against an odometry chain — the actual A10 shape. The chain
  //     says 1.00 m steps; the fixes say 1.10 m. With a loose translation
  //     sigma on the chain and a tight one on the fixes, the fixes win the
  //     scale, which is exactly the georeferencing job.
  {
    PoseGraph g;
    const int n = 12;
    double q[4];
    se3::quat_identity(q);
    for (int i = 0; i < n; ++i) {
      const double p[3] = {static_cast<double>(i), 0.0, 0.0};
      g.add_node(q, p);
    }
    double qz[4];
    se3::quat_identity(qz);
    for (int i = 1; i < n; ++i) {
      double qb[4], pb[3];
      pbetween(q, g.node(i - 1).p, q, g.node(i).p, qb, pb);
      CHECK(g.add_between(i - 1, i, qb, pb, 0.001, 0.5).ok());
    }
    for (int i = 0; i < n; ++i) {
      const double fix[3] = {1.1 * i, 0.0, 0.0};
      CHECK(g.add_position_prior(i, fix, 0.02).ok());
    }
    Result<PoseGraphSummary> r = g.optimize();
    REQUIRE(r.ok());
    MESSAGE("chain + priors: chi2 " << r.value().initial_chi2 << " -> " << r.value().final_chi2
                                    << ", end x = " << g.node(n - 1).p[0] << " (fix "
                                    << 1.1 * (n - 1) << ")");
    CHECK(r.value().final_chi2 < r.value().initial_chi2);
    CHECK(std::fabs(g.node(n - 1).p[0] - 1.1 * (n - 1)) < 0.2);
    // The chain is straight and so are the fixes, so nothing should have
    // rotated even though the coupling now exists.
    double R[9], R0[9];
    se3::quat_to_matrix(g.node(n - 1).q, R);
    se3::quat_to_matrix(q, R0);
    CHECK(se3::rot_angle_deg(R, R0) < 1e-4);
  }
}

TEST_CASE("pgraph/huber_contains_one_wrong_loop") {
  // A straight 40-node chain with one utterly wrong loop edge claiming node 39
  // sits on top of node 0. Without a robust kernel that edge folds the chain;
  // with Huber it bends it. Odometry edges deliberately get no kernel.
  auto build = [](double huber) {
    PoseGraph g;
    const int n = 40;
    double q[4];
    se3::quat_identity(q);
    for (int i = 0; i < n; ++i) {
      const double p[3] = {static_cast<double>(i), 0.0, 0.0};
      g.add_node(q, p);
    }
    (void)g.set_fixed(0, true);
    double qz[4];
    se3::quat_identity(qz);
    const double step[3] = {1.0, 0.0, 0.0};
    for (int i = 1; i < n; ++i) (void)g.add_between(i - 1, i, qz, step, 0.005, 0.01);
    const double zero[3] = {0.0, 0.0, 0.0};
    (void)g.add_between(0, n - 1, qz, zero, 0.05, 0.05, huber, true);
    (void)g.optimize();
    return g.node(n - 1).p[0];
  };
  const double x_plain = build(0.0);
  const double x_huber = build(2.0);
  MESSAGE("bad loop: end node x = " << x_plain << " (no kernel) vs " << x_huber << " (huber)");
  CHECK(x_huber > x_plain + 1.0);
  CHECK(x_huber > 20.0);  // the chain is still recognisably 39 m long
}

TEST_CASE("pgraph/optimize_honours_the_cancel_token") {
  PoseGraph g;
  double q[4];
  se3::quat_identity(q);
  for (int i = 0; i < 200; ++i) {
    const double p[3] = {static_cast<double>(i) * 0.5, 0.0, 0.0};
    g.add_node(q, p);
  }
  CHECK(g.set_fixed(0, true).ok());
  double qz[4];
  se3::quat_identity(qz);
  const double step[3] = {0.4, 0.05, 0.0};
  for (int i = 1; i < 200; ++i) CHECK(g.add_between(i - 1, i, qz, step, 0.01, 0.01).ok());
  CancelToken tok;
  tok.cancel();
  Result<PoseGraphSummary> r = g.optimize(PoseGraphOptions{}, &tok);
  CHECK_FALSE(r.ok());
  CHECK(r.error() == ScanError::kCancelled);
}

// ===========================================================================
// sctx — Scan Context
// ===========================================================================

TEST_CASE("sctx/descriptor_is_yaw_covariant_and_recovers_the_yaw") {
  Room room;
  double q[4];
  se3::quat_identity(q);
  const double p[3] = {-2.0, 1.0, 1.4};
  const std::vector<PointVertex> a = scan_room(room, q, p, 11, 40000, 0.0);
  REQUIRE(a.size() > 5000);

  ScanContextConfig cfg;
  ScanContextDescriptor da;
  REQUIRE(build_scan_context(Span<const PointVertex>(a.data(), a.size()), cfg, 0, &da).ok());

  // Six exact multiples of the sector width: the descriptor should shift by
  // whole columns and the recovered yaw should be exact to the bin.
  const double sector = 6.283185307179586 / cfg.sectors;
  for (int k = 1; k <= 6; ++k) {
    const double yaw = sector * (k * 7);  // 7, 14, ... sectors
    double qy[4];
    quat_from_rpy(0.0, 0.0, yaw, qy);
    // The SAME world points, seen from a body frame rotated by +yaw.
    std::vector<PointVertex> b;
    b.reserve(a.size());
    double qyi[4];
    qinv(qy, qyi);
    for (const PointVertex& v : a) {
      const double w[3] = {v.x, v.y, v.z};
      double o[3];
      qrot(qyi, w, o);
      PointVertex n = v;
      n.x = static_cast<float>(o[0]);
      n.y = static_cast<float>(o[1]);
      n.z = static_cast<float>(o[2]);
      b.push_back(n);
    }
    ScanContextDescriptor db;
    REQUIRE(build_scan_context(Span<const PointVertex>(b.data(), b.size()), cfg, 0, &db).ok());
    std::uint32_t shift = 0;
    const double d = scan_context_distance(db, da, &shift);  // query = b, match = a
    const double recovered = static_cast<double>(shift) * sector;
    MESSAGE("yaw " << (yaw * se3::kRadToDeg) << " deg -> distance " << d << ", recovered "
                   << (recovered * se3::kRadToDeg) << " deg");
    CHECK(d < 0.02);
    CHECK(std::fabs(recovered - yaw) < 1e-9);
  }
}

TEST_CASE("sctx/tells_two_places_apart") {
  // Five places in the hall, each described twice from independent noise
  // seeds. The claim is not "the distance clears some absolute number" — it
  // is that revisiting a place is ALWAYS closer than visiting a different
  // one, by a wide margin, which is the only property the search relies on.
  //
  // The absolute numbers are worth reading, and are the reason
  // ScanContextConfig::distance_threshold is documented as scene-dependent
  // and the ICP gate is documented as the real filter: a flat indoor ceiling
  // puts nearly every occupied bin at the same height, so the cosine term
  // saturates and the whole distance range compresses by an order of
  // magnitude against the paper's outdoor KITTI figures. See
  // engine/docs/A7-post.md §5.
  Room room;
  double q[4];
  se3::quat_identity(q);
  const double places[5][3] = {{-9.0, -6.0, 1.4},
                               {2.0, 7.0, 1.4},
                               {9.5, -1.0, 1.4},
                               {-4.0, 3.0, 1.4},
                               {0.0, -7.8, 1.4}};
  ScanContextConfig cfg;
  ScanContextDescriptor d1[5], d2[5];
  for (int i = 0; i < 5; ++i) {
    const std::vector<PointVertex> a =
        scan_room(room, q, places[i], 3u + static_cast<std::uint64_t>(i), 40000, 0.005);
    const std::vector<PointVertex> b =
        scan_room(room, q, places[i], 900u + static_cast<std::uint64_t>(i), 40000, 0.005);
    REQUIRE(build_scan_context(Span<const PointVertex>(a.data(), a.size()), cfg, 0, &d1[i]).ok());
    REQUIRE(build_scan_context(Span<const PointVertex>(b.data(), b.size()), cfg, 0, &d2[i]).ok());
  }
  double worst_same = 0.0;
  double best_diff = 1.0;
  for (int i = 0; i < 5; ++i) {
    std::string row;
    for (int j = 0; j < 5; ++j) {
      const double d = scan_context_distance(d1[i], d2[j], nullptr);
      row += " " + std::to_string(d).substr(0, 7);
      if (i == j) {
        worst_same = std::max(worst_same, d);
      } else {
        best_diff = std::min(best_diff, d);
      }
      // The diagonal must be the row minimum: a revisit beats every
      // non-revisit, which is exactly what the database's search needs.
      if (i != j) CHECK(d > scan_context_distance(d1[i], d2[i], nullptr));
    }
    MESSAGE("place " << i << " distances:" << row);
  }
  MESSAGE("worst same-place distance " << worst_same << ", best different-place distance "
                                       << best_diff << " (separation "
                                       << (best_diff / worst_same) << "x)");
  CHECK(best_diff > worst_same * 3.0);
}

TEST_CASE("sctx/db_finds_the_revisit_and_ignores_the_neighbours") {
  Room room;
  ScanContextConfig cfg;
  cfg.min_index_gap = 10;
  cfg.min_time_gap_s = 0.0;
  ScanContextDb db(cfg);

  // 30 keyframes along a path that returns to its start at index 29.
  std::vector<std::array<double, 3>> path;
  for (int i = 0; i < 30; ++i) {
    const double u = static_cast<double>(i) / 29.0 * 6.283185307179586;
    path.push_back({6.0 * std::sin(u), 6.0 * (1.0 - std::cos(u)), 1.4});
  }
  for (int i = 0; i < 30; ++i) {
    double q[4];
    quat_from_rpy(0.0, 0.0, static_cast<double>(i) / 29.0 * 6.283185307179586, q);
    const double p[3] = {path[i][0], path[i][1], path[i][2]};
    const std::vector<PointVertex> c = scan_room(room, q, p, 100u + i, 20000, 0.01);
    const std::uint32_t idx = db.add(Span<const PointVertex>(c.data(), c.size()),
                                     static_cast<std::int64_t>(i) * 100'000'000LL);
    CHECK(idx == static_cast<std::uint32_t>(i));
  }
  const ScanContextMatch m = db.query(29);
  MESSAGE("revisit query: found " << m.found << ", index " << m.index << ", distance "
                                  << m.distance << ", yaw " << (m.yaw_rad * se3::kRadToDeg));
  REQUIRE(m.found);
  CHECK(m.index <= 2);  // the start of the loop
  CHECK(m.distance < cfg.distance_threshold);
  // Early keyframes have nothing old enough to match.
  CHECK_FALSE(db.query(3).found);
}

// ===========================================================================
// picp — point-to-plane ICP
// ===========================================================================

TEST_CASE("picp/recovers_a_known_transform") {
  Room room;
  double q[4];
  se3::quat_identity(q);
  const double p[3] = {0.0, 0.0, 1.4};
  const std::vector<PointVertex> target = scan_room(room, q, p, 21, 30000, 0.005);
  REQUIRE(target.size() > 10000);

  // The source is the same place seen from a body frame displaced by a known
  // rigid transform. ICP must recover its inverse.
  double q_ts[4];
  quat_from_rpy(0.02, -0.03, 0.35, q_ts);
  const double p_ts[3] = {0.45, -0.30, 0.06};
  double q_st[4], p_st[3];
  pinverse(q_ts, p_ts, q_st, p_st);
  std::vector<PointVertex> source;
  source.reserve(target.size());
  for (const PointVertex& v : target) {
    const double w[3] = {v.x, v.y, v.z};
    double o[3];
    qrot(q_st, w, o);
    PointVertex n = v;
    n.x = static_cast<float>(o[0] + p_st[0]);
    n.y = static_cast<float>(o[1] + p_st[1]);
    n.z = static_cast<float>(o[2] + p_st[2]);
    source.push_back(n);
  }

  // Initialize with the yaw only, as the pipeline does from Scan Context.
  double init_q[4];
  quat_from_rpy(0.0, 0.0, 0.35, init_q);
  const double init_p[3] = {0.0, 0.0, 0.0};
  IcpConfig cfg;
  const IcpResult r = icp_point_to_plane(Span<const PointVertex>(source.data(), source.size()),
                                         Span<const PointVertex>(target.data(), target.size()),
                                         init_q, init_p, cfg);
  double R[9], Rt[9];
  se3::quat_to_matrix(r.q, R);
  se3::quat_to_matrix(q_ts, Rt);
  const double rot_err = se3::rot_angle_deg(R, Rt);
  const double trans_err = dist3(r.p, p_ts);
  MESSAGE("icp: converged " << r.converged << " in " << r.iterations << " iters, inliers "
                            << r.inliers << " (" << (r.inlier_ratio * 100.0) << "%), rms "
                            << (r.rms_m * 100.0) << " cm");
  MESSAGE("icp: rotation error " << rot_err << " deg, translation error " << (trans_err * 1000.0)
                                 << " mm");
  CHECK(r.converged);
  CHECK(rot_err < 0.5);
  CHECK(trans_err < 0.03);
  CHECK(r.inlier_ratio > 0.5);

  const char* reason = nullptr;
  LoopAcceptConfig acc;
  CHECK(loop_is_acceptable(r, acc, &reason));
  CHECK(std::string(reason) == "accepted");
}

TEST_CASE("picp/the_acceptance_gate_rejects_a_bad_fit") {
  LoopAcceptConfig acc;
  const char* reason = nullptr;

  IcpResult r;
  r.converged = false;
  CHECK_FALSE(loop_is_acceptable(r, acc, &reason));
  CHECK(std::string(reason) == "icp did not converge");

  r.converged = true;
  r.inliers = 10;
  CHECK_FALSE(loop_is_acceptable(r, acc, &reason));
  CHECK(std::string(reason) == "too few inliers");

  r.inliers = 1000;
  r.inlier_ratio = 0.05;
  CHECK_FALSE(loop_is_acceptable(r, acc, &reason));
  CHECK(std::string(reason) == "inlier ratio below threshold");

  r.inlier_ratio = 0.8;
  r.rms_m = 1.0;
  CHECK_FALSE(loop_is_acceptable(r, acc, &reason));
  CHECK(std::string(reason) == "residual rms above threshold");

  r.rms_m = 0.05;
  r.p[0] = 50.0;
  CHECK_FALSE(loop_is_acceptable(r, acc, &reason));
  CHECK(std::string(reason) == "translation correction implausible");

  // A 180-degree loop is a LEGITIMATE loop — the gate measures how far ICP
  // moved, not how far the loop is from identity.
  r.p[0] = 0.1;
  quat_from_rpy(0.0, 0.0, 3.14159, r.q);
  quat_from_rpy(0.0, 0.0, 3.14159, r.init_q);
  CHECK(loop_is_acceptable(r, acc, &reason));
}

// ===========================================================================
// pfilter — dedup, accumulator, outlier removal
// ===========================================================================

TEST_CASE("pfilter/voxel_dedup_reduces_and_is_deterministic") {
  Rng rng(31);
  std::vector<PointVertex> in;
  for (int i = 0; i < 200000; ++i) {
    PointVertex p;
    p.x = static_cast<float>(rng.uniform() * 2.0);
    p.y = static_cast<float>(rng.uniform() * 2.0);
    p.z = static_cast<float>(rng.uniform() * 0.5);
    p.r = p.g = p.b = 100;
    p.a = 255;
    in.push_back(p);
  }
  VoxelDedupConfig cfg;
  cfg.voxel_size_m = 0.05;
  std::vector<PointVertex> a, b;
  const std::size_t na = voxel_downsample(Span<const PointVertex>(in.data(), in.size()), cfg, &a);
  const std::size_t nb = voxel_downsample(Span<const PointVertex>(in.data(), in.size()), cfg, &b);
  MESSAGE("dedup: " << in.size() << " -> " << na << " points at 5 cm");
  CHECK(na == nb);
  CHECK(na < in.size() / 4);
  CHECK(na > 0);
  // Bit-identical, in order: the output order is insertion order of each
  // voxel's first point, never hash order.
  REQUIRE(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(PointVertex)) == 0);

  // Every survivor must lie inside a voxel of the input's bounding box.
  for (const PointVertex& p : a) {
    CHECK(p.x >= -0.05f);
    CHECK(p.x <= 2.05f);
  }
}

TEST_CASE("pfilter/streaming_accumulator_matches_the_batch_dedup") {
  Rng rng(77);
  std::vector<PointVertex> in;
  for (int i = 0; i < 50000; ++i) {
    PointVertex p;
    p.x = static_cast<float>(rng.gauss());
    p.y = static_cast<float>(rng.gauss());
    p.z = static_cast<float>(rng.gauss() * 0.3);
    p.r = p.g = p.b = static_cast<std::uint8_t>(i & 0xFF);
    p.a = 255;
    in.push_back(p);
  }
  VoxelDedupConfig cfg;
  cfg.voxel_size_m = 0.1;
  std::vector<PointVertex> batch;
  voxel_downsample(Span<const PointVertex>(in.data(), in.size()), cfg, &batch);

  VoxelAccumulator acc(0.1, true);
  acc.add(Span<const PointVertex>(in.data(), in.size()));
  std::vector<PointVertex> streamed;
  acc.extract(&streamed);

  MESSAGE("accumulator: " << acc.points_seen() << " in, " << acc.voxel_count() << " voxels");
  REQUIRE(streamed.size() == batch.size());
  CHECK(std::memcmp(streamed.data(), batch.data(), batch.size() * sizeof(PointVertex)) == 0);
}

TEST_CASE("pfilter/outlier_filter_removes_speckle_and_keeps_the_surface") {
  // A 6 x 6 m plane at 2 cm spacing, plus 400 points scattered through the
  // volume above it. The filter must delete the scatter and keep the plane.
  std::vector<PointVertex> in;
  for (int i = 0; i < 300; ++i) {
    for (int j = 0; j < 300; ++j) {
      PointVertex p;
      p.x = static_cast<float>(i * 0.02);
      p.y = static_cast<float>(j * 0.02);
      p.z = 0.f;
      p.r = p.g = p.b = 200;
      p.a = 255;
      in.push_back(p);
    }
  }
  const std::size_t surface = in.size();
  Rng rng(99);
  for (int i = 0; i < 400; ++i) {
    PointVertex p;
    p.x = static_cast<float>(rng.uniform() * 6.0);
    p.y = static_cast<float>(rng.uniform() * 6.0);
    p.z = static_cast<float>(1.0 + rng.uniform() * 4.0);
    p.r = p.g = p.b = 10;
    p.a = 255;
    in.push_back(p);
  }
  OutlierFilterConfig cfg;
  cfg.neighbors = 8;
  cfg.std_dev_mul = 1.5;
  cfg.search_radius_m = 0.4;
  std::vector<PointVertex> out;
  OutlierFilterStats st;
  CHECK(statistical_outlier_filter(Span<const PointVertex>(in.data(), in.size()), cfg, 0.02, &out,
                                   &st)
            .ok());
  std::size_t kept_surface = 0, kept_speckle = 0;
  for (const PointVertex& p : out) {
    if (p.z < 0.5f) {
      ++kept_surface;
    } else {
      ++kept_speckle;
    }
  }
  MESSAGE("outliers: in " << st.in << ", kept " << st.kept << " (isolated "
                          << st.removed_isolated << ", statistical " << st.removed_statistical
                          << "), mean " << (st.mean_distance_m * 1000.0) << " mm, sigma "
                          << (st.std_dev_m * 1000.0) << " mm");
  MESSAGE("outliers: surface kept " << kept_surface << "/" << surface << ", speckle kept "
                                    << kept_speckle << "/400");
  CHECK(kept_speckle < 40);                 // >90% of the scatter is gone
  CHECK(kept_surface > surface * 95 / 100); // and the surface survives
  CHECK(st.removed_isolated + st.removed_statistical == in.size() - out.size());

  // Disabled is a pass-through, not a no-op with a different answer.
  OutlierFilterConfig off;
  off.enabled = false;
  std::vector<PointVertex> all;
  CHECK(statistical_outlier_filter(Span<const PointVertex>(in.data(), in.size()), off, 0.02, &all)
            .ok());
  CHECK(all.size() == in.size());
}

// ===========================================================================
// ploop — detection + verification + optimization, against truth
// ===========================================================================

TEST_CASE("ploop/loop_closure_cuts_the_ate") {
  // THE quantitative claim of this task. A circular walk through the room
  // returning to its start, sampled as 44 keyframes:
  //
  //   truth      the analytic trajectory
  //   odometry   truth corrupted by a drift that accumulates per keyframe —
  //              a small yaw bias plus a small translation bias, which is
  //              exactly the shape of real LIO drift and is what makes the
  //              end of the loop miss its start by ~1 m
  //
  // The pipeline's three loop stages then run on it: Scan Context over the
  // keyframe clouds, point-to-plane ICP against the local submap, and the
  // pose graph. ATE is measured against truth, before and after.
  Room room;
  const int nk = 44;
  const double radius = 6.5;

  std::vector<PoseNode> truth(nk), odom(nk);
  std::vector<std::vector<PointVertex>> clouds(nk);
  for (int i = 0; i < nk; ++i) {
    const double u = static_cast<double>(i) / static_cast<double>(nk - 1) * 6.283185307179586;
    double q[4];
    quat_from_rpy(0.0, 0.0, u, q);
    const double p[3] = {radius * std::sin(u), radius * (1.0 - std::cos(u)), 1.4};
    for (int k = 0; k < 4; ++k) truth[i].q[k] = q[k];
    for (int k = 0; k < 3; ++k) truth[i].p[k] = p[k];
    clouds[i] = scan_room(room, q, p, 500u + i, 17000, 0.01);
    REQUIRE(clouds[i].size() > 3000);
  }

  // Integrate the drifted odometry: each true relative transform gets a small
  // constant error, which accumulates the way real drift does.
  Rng rng(4242);
  for (int k = 0; k < 4; ++k) odom[0].q[k] = truth[0].q[k];
  for (int k = 0; k < 3; ++k) odom[0].p[k] = truth[0].p[k];
  for (int i = 1; i < nk; ++i) {
    double qz[4], pz[3];
    pbetween(truth[i - 1].q, truth[i - 1].p, truth[i].q, truth[i].p, qz, pz);
    double qerr[4];
    quat_from_rpy(0.0008 * rng.gauss(), 0.0008 * rng.gauss(), 0.010 + 0.001 * rng.gauss(), qerr);
    const double perr[3] = {0.004 + 0.002 * rng.gauss(), 0.002 * rng.gauss(),
                            0.001 * rng.gauss()};
    double qzn[4], pzn[3];
    pcompose(qz, pz, qerr, perr, qzn, pzn);
    pcompose(odom[i - 1].q, odom[i - 1].p, qzn, pzn, odom[i].q, odom[i].p);
  }

  const AteResult before = absolute_trajectory_error(odom, truth);
  MESSAGE("ATE before: rms " << before.rms_m << " m, max " << before.max_m << " m, final "
                             << before.final_m << " m");
  REQUIRE(before.final_m > 0.5);  // the drift is real, not decorative

  // --- stage 2: candidates ------------------------------------------------
  ScanContextConfig sc;
  sc.min_index_gap = 12;
  sc.min_time_gap_s = 0.0;
  ScanContextDb db(sc);
  for (int i = 0; i < nk; ++i) {
    db.add(Span<const PointVertex>(clouds[i].data(), clouds[i].size()),
           static_cast<std::int64_t>(i) * 200'000'000LL);
  }

  IcpConfig icfg;
  LoopAcceptConfig acc;
  acc.min_inliers = 200;
  acc.min_inlier_ratio = 0.25;

  struct Edge {
    std::uint32_t from, to;
    double q[4], p[3];
    double rms;
  };
  std::vector<Edge> edges;
  std::uint32_t candidates = 0;
  for (int qi = 0; qi < nk; ++qi) {
    const ScanContextMatch m = db.query(static_cast<std::uint32_t>(qi));
    if (!m.found) continue;
    ++candidates;
    // Submap: +-4 keyframes around the match, in the match's body frame,
    // assembled with the DRIFTED odometry (locally consistent, which is all a
    // local submap needs).
    const int lo = std::max(0, static_cast<int>(m.index) - 4);
    const int hi = std::min(nk - 1, static_cast<int>(m.index) + 4);
    std::vector<PointVertex> submap;
    for (int k = lo; k <= hi; ++k) {
      double qr[4], pr[3];
      pbetween(odom[m.index].q, odom[m.index].p, odom[k].q, odom[k].p, qr, pr);
      for (const PointVertex& v : clouds[k]) {
        const double w[3] = {v.x, v.y, v.z};
        double o[3];
        qrot(qr, w, o);
        PointVertex n = v;
        n.x = static_cast<float>(o[0] + pr[0]);
        n.y = static_cast<float>(o[1] + pr[1]);
        n.z = static_cast<float>(o[2] + pr[2]);
        submap.push_back(n);
      }
    }
    std::vector<PointVertex> down;
    VoxelDedupConfig dcfg;
    dcfg.voxel_size_m = 0.2;
    voxel_downsample(Span<const PointVertex>(submap.data(), submap.size()), dcfg, &down);

    const double half = 0.5 * m.yaw_rad;
    const double init_q[4] = {0.0, 0.0, std::sin(half), std::cos(half)};
    const double init_p[3] = {0.0, 0.0, 0.0};
    const IcpResult r = icp_point_to_plane(
        Span<const PointVertex>(clouds[qi].data(), clouds[qi].size()),
        Span<const PointVertex>(down.data(), down.size()), init_q, init_p, icfg);
    const char* reason = nullptr;
    const bool ok = loop_is_acceptable(r, acc, &reason);
    MESSAGE("candidate " << qi << " -> " << m.index << ": sc " << m.distance << ", icp rms "
                         << (r.rms_m * 100.0) << " cm, inliers " << r.inliers << " ("
                         << (r.inlier_ratio * 100.0) << "%), " << std::string(reason));
    if (!ok) continue;
    Edge e;
    e.from = m.index;
    e.to = static_cast<std::uint32_t>(qi);
    for (int k = 0; k < 4; ++k) e.q[k] = r.q[k];
    for (int k = 0; k < 3; ++k) e.p[k] = r.p[k];
    e.rms = r.rms_m;
    edges.push_back(e);
  }
  MESSAGE("loops: " << candidates << " candidates, " << edges.size() << " accepted");
  REQUIRE(candidates > 0);
  REQUIRE(!edges.empty());

  // --- stage 3: the graph -------------------------------------------------
  PoseGraph g;
  for (int i = 0; i < nk; ++i) g.add_node(odom[i].q, odom[i].p);
  CHECK(g.set_fixed(0, true).ok());
  for (int i = 1; i < nk; ++i) {
    double qz[4], pz[3];
    pbetween(odom[i - 1].q, odom[i - 1].p, odom[i].q, odom[i].p, qz, pz);
    CHECK(g.add_between(i - 1, i, qz, pz, 0.5 * se3::kDegToRad, 0.02).ok());
  }
  for (const Edge& e : edges) {
    CHECK(g.add_between(e.from, e.to, e.q, e.p, 1.0 * se3::kDegToRad,
                        std::max(0.05, e.rms), 2.0, true)
              .ok());
  }
  Result<PoseGraphSummary> res = g.optimize();
  REQUIRE(res.ok());
  MESSAGE("graph: " << res.value().variables << " variables, chi2 " << res.value().initial_chi2
                    << " -> " << res.value().final_chi2 << " in " << res.value().iterations
                    << " iterations (" << res.value().rejected_steps << " rejected)");

  const AteResult after = absolute_trajectory_error(g.nodes(), truth);
  MESSAGE("ATE after:  rms " << after.rms_m << " m, max " << after.max_m << " m, final "
                             << after.final_m << " m");
  MESSAGE("ATE improvement: " << (before.rms_m / after.rms_m) << "x (rms), "
                              << (before.final_m / after.final_m) << "x (final)");

  // THE assertion. Not "did not get worse" — a real improvement, quantified.
  CHECK(after.rms_m < before.rms_m);
  CHECK(after.rms_m < before.rms_m * 0.5);
  CHECK(after.final_m < before.final_m * 0.35);
  CHECK(after.max_m < before.max_m);
  CHECK(g.loop_count() == edges.size());

  // The control: the SAME graph with the loop edges removed cannot improve
  // anything, because the odometry chain alone is already its own optimum.
  PoseGraph plain;
  for (int i = 0; i < nk; ++i) plain.add_node(odom[i].q, odom[i].p);
  CHECK(plain.set_fixed(0, true).ok());
  for (int i = 1; i < nk; ++i) {
    double qz[4], pz[3];
    pbetween(odom[i - 1].q, odom[i - 1].p, odom[i].q, odom[i].p, qz, pz);
    CHECK(plain.add_between(i - 1, i, qz, pz, 0.5 * se3::kDegToRad, 0.02).ok());
  }
  Result<PoseGraphSummary> pres = plain.optimize();
  REQUIRE(pres.ok());
  const AteResult control = absolute_trajectory_error(plain.nodes(), truth);
  MESSAGE("control (no loop edges): rms " << control.rms_m << " m, final " << control.final_m
                                          << " m");
  CHECK(std::fabs(control.rms_m - before.rms_m) < 1e-6);
  CHECK(after.rms_m < control.rms_m * 0.5);
}

// ===========================================================================
// post — PostSlamPipeline, end to end from a .lscan
// ===========================================================================

namespace {

PostConfig fast_synth_config() {
  PostConfig cfg;
  cfg.keyframe_translation_m = 0.6;
  cfg.keyframe_rotation_deg = 12.0;
  cfg.keyframe_voxel_m = 0.25;
  cfg.max_points_per_keyframe = 6000;
  cfg.scan_context.min_index_gap = 8;
  cfg.scan_context.min_time_gap_s = 3.0;
  cfg.loop_submap_half_span = 4;
  cfg.dedup.voxel_size_m = 0.05;
  cfg.outlier.enabled = true;
  cfg.outlier.std_dev_mul = 2.0;
  cfg.progress_chunk_interval = 256;
  return cfg;
}

}  // namespace

TEST_CASE("post/runs_end_to_end_from_a_recording") {
  const std::string dir = make_temp_dir("synth");
  TempDirGuard guard(dir);
  SynthConfig sc;
  const SynthResult syn = write_synthetic_lscan(dir, sc);
  MESSAGE("synthetic .lscan: " << syn.point_packets << " point packets, " << syn.imu_packets
                               << " IMU, " << syn.points << " points");
  REQUIRE(syn.point_packets > 500);

  PostConfig cfg = fast_synth_config();
  PostSlamPipeline pipe(cfg);

  std::vector<PostStage> stages_seen;
  float last_fraction = -1.f;
  bool monotone = true;
  pipe.set_progress_callback([&](const PostProgress& p) {
    if (stages_seen.empty() || stages_seen.back() != p.stage) stages_seen.push_back(p.stage);
    if (p.fraction < last_fraction - 1e-6f) monotone = false;
    last_fraction = p.fraction;
    CHECK(p.label != nullptr);
  });

  const double t0 = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
  const Status st = pipe.run(dir);
  const double t1 = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
  REQUIRE(st.ok());

  const PostStats& s = pipe.stats();
  MESSAGE("post: chunks " << s.chunks_read << " (" << s.point_chunks << " point, " << s.imu_chunks
                          << " imu), points decoded " << s.points_decoded);
  MESSAGE("post: keyframes " << s.keyframes << " (" << s.keyframe_points << " points), poses "
                             << s.odom_poses << ", path " << s.trajectory_length_m << " m");
  MESSAGE("post: loop candidates " << s.loop_candidates << ", accepted " << s.loops_accepted
                                   << ", rejected " << s.loops_rejected);
  MESSAGE("post: graph " << s.graph.variables << " vars, bandwidth " << s.graph.bandwidth_blocks
                         << ", chi2 " << s.graph.initial_chi2 << " -> " << s.graph.final_chi2
                         << ", keyframe shift rms " << s.keyframe_shift_rms_m << " m / max "
                         << s.keyframe_shift_max_m << " m");
  MESSAGE("post: reintegrated " << s.reintegrated_points << " points (unposed " << s.unposed_points
                                << ") -> dedup " << s.dedup_points << " -> final "
                                << s.final_points);
  MESSAGE("post: ms odom " << s.ms_odometry << ", loops " << s.ms_loops << ", opt "
                           << s.ms_optimize << ", reint " << s.ms_reintegrate << ", filter "
                           << s.ms_filter << ", total " << s.ms_total << " (wall "
                           << (t1 - t0) << ")");

  CHECK(pipe.stage() == PostStage::kDone);
  CHECK(pipe.progress() == doctest::Approx(1.0f));
  CHECK(monotone);
  // Every stage of §3.3 must have been reported, in order.
  CHECK(stages_seen.size() >= 6);
  CHECK(stages_seen.front() == PostStage::kOpening);
  CHECK(stages_seen.back() == PostStage::kDone);
  for (std::size_t i = 1; i < stages_seen.size(); ++i) {
    CHECK(static_cast<int>(stages_seen[i]) > static_cast<int>(stages_seen[i - 1]));
  }
  CHECK(std::string(to_string(PostStage::kReintegration)) == "re-integration");

  CHECK(s.point_chunks == syn.point_packets);
  CHECK(s.imu_chunks == syn.imu_packets);
  CHECK(s.malformed_chunks == 0);
  CHECK(s.keyframes >= 8);
  CHECK(s.lio.initialized);
  CHECK_FALSE(s.lio.diverged);
  CHECK(s.reintegrated_points > 100000);
  CHECK(s.dedup_points > 10000);
  CHECK(s.final_points > 0);
  CHECK(s.final_points <= s.dedup_points);
  CHECK(s.buffer_overflow_points == 0);
  CHECK(pipe.final_cloud().size() == s.final_points);
  CHECK(pipe.out_store().total_points() == s.final_points);
  CHECK(pipe.out_store().page_count() >= 1);

  // The trajectory must be a real one and every published point finite.
  CHECK(pipe.trajectory().size() == s.odom_poses);
  CHECK(s.trajectory_length_m > 10.0);
  std::uint64_t checked = 0;
  double bounds_lo[3] = {1e30, 1e30, 1e30}, bounds_hi[3] = {-1e30, -1e30, -1e30};
  for (const PointVertex& p : pipe.final_cloud()) {
    REQUIRE(std::isfinite(p.x));
    REQUIRE(std::isfinite(p.y));
    REQUIRE(std::isfinite(p.z));
    bounds_lo[0] = std::min(bounds_lo[0], static_cast<double>(p.x));
    bounds_lo[1] = std::min(bounds_lo[1], static_cast<double>(p.y));
    bounds_lo[2] = std::min(bounds_lo[2], static_cast<double>(p.z));
    bounds_hi[0] = std::max(bounds_hi[0], static_cast<double>(p.x));
    bounds_hi[1] = std::max(bounds_hi[1], static_cast<double>(p.y));
    bounds_hi[2] = std::max(bounds_hi[2], static_cast<double>(p.z));
    ++checked;
  }
  MESSAGE("post: final cloud bounds [" << bounds_lo[0] << " " << bounds_lo[1] << " "
                                       << bounds_lo[2] << "] .. [" << bounds_hi[0] << " "
                                       << bounds_hi[1] << " " << bounds_hi[2] << "]");
  CHECK(checked > 0);
  // The room is 24 x 18 x 3.5 m centred on the origin, and the scanner walked
  // a 13 m circle inside it. The reconstruction must land in that box, not in
  // a diverged one.
  CHECK(bounds_hi[0] - bounds_lo[0] < 40.0);
  CHECK(bounds_hi[1] - bounds_lo[1] < 40.0);
  CHECK(bounds_hi[2] < 12.0);
}

TEST_CASE("post/two_runs_are_bit_identical") {
  const std::string dir = make_temp_dir("determinism");
  TempDirGuard guard(dir);
  SynthConfig sc;
  sc.duration_s = 6.0;
  (void)write_synthetic_lscan(dir, sc);

  PostConfig cfg = fast_synth_config();
  PostSlamPipeline a(cfg), b(cfg);
  REQUIRE(a.run(dir).ok());
  REQUIRE(b.run(dir).ok());

  MESSAGE("determinism: " << a.final_cloud().size() << " vs " << b.final_cloud().size()
                          << " points, " << a.keyframes().size() << " vs "
                          << b.keyframes().size() << " keyframes");
  REQUIRE(a.keyframes().size() == b.keyframes().size());
  REQUIRE(a.final_cloud().size() == b.final_cloud().size());

  // Bit-identical: every keyframe pose (odometry AND optimized), and every
  // point of the final cloud, compared as raw bytes.
  for (std::size_t i = 0; i < a.keyframes().size(); ++i) {
    const Keyframe& ka = a.keyframes()[i];
    const Keyframe& kb = b.keyframes()[i];
    CHECK(ka.t_ns == kb.t_ns);
    for (int k = 0; k < 4; ++k) CHECK(ka.q[k] == kb.q[k]);
    for (int k = 0; k < 3; ++k) CHECK(ka.p[k] == kb.p[k]);
    for (int k = 0; k < 4; ++k) CHECK(ka.q_opt[k] == kb.q_opt[k]);
    for (int k = 0; k < 3; ++k) CHECK(ka.p_opt[k] == kb.p_opt[k]);
    REQUIRE(ka.points.size() == kb.points.size());
  }
  CHECK(std::memcmp(a.final_cloud().data(), b.final_cloud().data(),
                    a.final_cloud().size() * sizeof(PointVertex)) == 0);
  CHECK(a.stats().graph.final_chi2 == b.stats().graph.final_chi2);
  CHECK(a.stats().loops_accepted == b.stats().loops_accepted);
  CHECK(a.stats().dedup_points == b.stats().dedup_points);
  CHECK(a.stats().reintegrated_points == b.stats().reintegrated_points);
}

TEST_CASE("post/cancellation_stops_every_stage") {
  const std::string dir = make_temp_dir("cancel");
  TempDirGuard guard(dir);
  SynthConfig sc;
  sc.duration_s = 6.0;
  (void)write_synthetic_lscan(dir, sc);
  const PostConfig cfg = fast_synth_config();

  // 1. Cancelled before it starts.
  {
    PostSlamPipeline pipe(cfg);
    pipe.cancel();
    const Status st = pipe.run(dir);
    CHECK_FALSE(st.ok());
    CHECK(st.error() == ScanError::kCancelled);
    CHECK(pipe.stage() == PostStage::kCancelled);
  }

  // 2. Cancelled from the progress callback, once inside each stage. The
  //    pipeline must unwind with kCancelled from whichever stage it was in —
  //    including the ones that are not simple streaming loops (the graph
  //    solve, the outlier filter).
  const PostStage targets[] = {PostStage::kOdometry, PostStage::kLoopDetection,
                               PostStage::kReintegration, PostStage::kFiltering};
  for (PostStage target : targets) {
    PostSlamPipeline pipe(cfg);
    PostStage stopped_in = PostStage::kIdle;
    pipe.set_progress_callback([&pipe, &stopped_in, target](const PostProgress& p) {
      if (p.stage == target && p.stage_fraction > 0.2f && stopped_in == PostStage::kIdle) {
        stopped_in = p.stage;
        pipe.cancel();
      }
    });
    const Status st = pipe.run(dir);
    MESSAGE("cancel in " << std::string(to_string(target)) << ": status "
                         << std::string(error_str(st.error())) << ", stage "
                         << std::string(to_string(pipe.stage())) << ", progress "
                         << pipe.progress());
    if (stopped_in == PostStage::kIdle) {
      // The stage never reported past 20% (it can be a single report for a
      // tiny capture). Nothing to assert about cancellation then.
      continue;
    }
    CHECK_FALSE(st.ok());
    CHECK(st.error() == ScanError::kCancelled);
    CHECK(pipe.stage() == PostStage::kCancelled);
    CHECK(pipe.progress() < 1.0f);
  }

  // 3. An external token — the A15 path.
  {
    CancelToken token;
    PostSlamPipeline pipe(cfg);
    pipe.set_cancel_token(&token);
    pipe.set_progress_callback([&token](const PostProgress& p) {
      if (p.stage == PostStage::kOdometry && p.stage_fraction > 0.3f) token.cancel();
    });
    const Status st = pipe.run(dir);
    CHECK_FALSE(st.ok());
    CHECK(st.error() == ScanError::kCancelled);
    CHECK(token.cancelled());
    token.reset();
    CHECK_FALSE(token.cancelled());
  }
}

TEST_CASE("post/rejects_a_recording_with_no_mid360_data") {
  const std::string dir = make_temp_dir("empty");
  TempDirGuard guard(dir);
  {
    lscan::FileRecordWriter w;
    REQUIRE(w.open(dir).ok());
    const std::uint8_t note[4] = {'h', 'i', '!', '\n'};
    REQUIRE(w.write_chunk(lscan::ChunkType::kSessionNote, 1000, ByteSpan(note, 4)).ok());
    REQUIRE(w.close().ok());
  }
  PostSlamPipeline pipe(fast_synth_config());
  const Status st = pipe.run(dir);
  CHECK_FALSE(st.ok());
  CHECK(st.error() == ScanError::kNotFound);
  CHECK(pipe.stage() == PostStage::kFailed);
}

TEST_CASE("post/real_mid360_capture") {
  const std::string src = find_fixture();
  if (src.empty()) {
    MESSAGE("SKIPPED: outdoor_imu_ccby_6s.livoxdump not found (set SCANENGINE_LIVOXDUMP)");
    return;
  }
  const std::string dir = make_temp_dir("real");
  TempDirGuard guard(dir);
  std::uint64_t records = 0;
  REQUIRE(transcode_livoxdump(src, dir, &records));
  MESSAGE("real: transcoded " << records << " datagrams from " << src << " into a .lscan");
  REQUIRE(records > 10000);

  // The live decimation budget, not full density: docs/A6-lio.md §8 measured
  // 54.8 ms per scan undecimated against 14.7 ms at 40k pts/s, for a
  // trajectory that differs by 0.6%. Full density is what the FINAL CLOUD is
  // built from (the re-integration pass below uses every point regardless);
  // paying 4x in the odometry for 0.6% is not what this smoke test is for.
  // A production run leaves live_points_per_sec at 0.
  PostConfig cfg;
  cfg.lio.live_points_per_sec = 40000;
  cfg.lio.max_points_per_scan = 12000;
  cfg.lio.max_range_m = 60.0f;
  cfg.keyframe_voxel_m = 0.3;
  cfg.max_points_per_keyframe = 5000;
  cfg.dedup.voxel_size_m = 0.10;
  cfg.outlier.enabled = true;
  cfg.outlier.std_dev_mul = 2.0;

  PostSlamPipeline pipe(cfg);
  REQUIRE(pipe.run(dir).ok());
  const PostStats& s = pipe.stats();

  MESSAGE("real: chunks " << s.chunks_read << " (" << s.point_chunks << " point / " << s.imu_chunks
                          << " imu), pre-clock " << s.nonmonotonic_packets << ", malformed "
                          << s.malformed_chunks);
  MESSAGE("real: filter kept " << s.filter.kept << "/" << s.filter.seen << " ("
                               << (s.filter.keep_fraction() * 100.0) << "%)");
  MESSAGE("real: scans " << s.lio.scans << " (skipped " << s.lio.scans_skipped << "), |g| "
                         << s.lio.gravity_m_s2 << ", residual rms "
                         << (s.lio.residual_rms_m * 100.0) << " cm");
  MESSAGE("real: keyframes " << s.keyframes << ", path " << s.trajectory_length_m << " m");
  MESSAGE("real: loop candidates " << s.loop_candidates << ", accepted " << s.loops_accepted);
  MESSAGE("real: graph " << s.graph.variables << " vars, bandwidth " << s.graph.bandwidth_blocks
                         << ", envelope " << s.graph.envelope_scalars << ", chi2 "
                         << s.graph.initial_chi2 << " -> " << s.graph.final_chi2);
  MESSAGE("real: reintegrated " << s.reintegrated_points << " (unposed " << s.unposed_points
                                << ") -> dedup " << s.dedup_points << " -> final "
                                << s.final_points);
  MESSAGE("real: outliers removed " << s.outlier.removed_isolated << " isolated + "
                                    << s.outlier.removed_statistical << " statistical");
  MESSAGE("real: ms odom " << s.ms_odometry << ", loops " << s.ms_loops << ", opt "
                           << s.ms_optimize << ", reint " << s.ms_reintegrate << ", filter "
                           << s.ms_filter << ", TOTAL " << s.ms_total << " ms");

  // There is no ground truth here, so the assertions are the ones that can be
  // made without it — the same set docs/A6-lio.md §7.2 settled on.
  CHECK(s.point_chunks > 12000);
  CHECK(s.imu_chunks == 1200);
  CHECK(s.malformed_chunks == 0);
  CHECK(s.lio.initialized);
  CHECK_FALSE(s.lio.diverged);
  CHECK(s.lio.gravity_m_s2 > 9.4);
  CHECK(s.lio.gravity_m_s2 < 10.2);
  CHECK(s.trajectory_length_m > 20.0);
  CHECK(s.trajectory_length_m < 60.0);
  CHECK(s.keyframes > 20);
  CHECK(s.reintegrated_points > 300000);
  CHECK(s.dedup_points > 10000);
  CHECK(s.final_points > 0);
  CHECK(s.final_points <= s.dedup_points);
  CHECK(s.store_append_failures == 0);
  // A 6-second vehicle-mounted run down a road does not revisit anything.
  // Reporting zero accepted loops is the CORRECT answer here, and a pipeline
  // that invented one would be the bug.
  CHECK(s.loops_accepted == 0);
  for (const PointVertex& p : pipe.final_cloud()) {
    REQUIRE(std::isfinite(p.x));
    REQUIRE(std::isfinite(p.y));
    REQUIRE(std::isfinite(p.z));
  }
}
