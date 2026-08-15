#include "app/SyntheticMid360.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>
#include <cstring>
#include <vector>

#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"

// Ported from engine/tests/test_post.cpp's write_synthetic_lscan() and its
// helpers (Room, LoopTraj, imu_truth, mid360_ray, put_header, Rng, the small
// quaternion helpers) — see SyntheticMid360.h for why this is a port and not
// a shared header. engine/ is read-only for this task.
namespace lidarscan {
namespace {

using namespace scanengine;

// xorshift64 + Box-Muller — deliberately not <random>, whose distributions
// are unspecified across implementations (the same reason the engine's own
// tests roll their own).
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double uniform() {
    return (static_cast<double>(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0);
  }
  double gauss() {
    const double u1 = uniform(), u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

void qrot(const double q[4], const double v[3], double o[3]) {
  double R[9];
  se3::quat_to_matrix(q, R);
  o[0] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
  o[1] = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
  o[2] = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
}
void quat_from_rpy(double roll, double pitch, double yaw, double q[4]) {
  const double w[3] = {roll, pitch, yaw};
  double R[9];
  se3::so3_exp(w, R);
  se3::matrix_to_quat(R, q);
}

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

// A 24 x 18 x 3.5 m hall with six pillars, two low benches and a cabinet —
// asymmetric on purpose, so Scan Context's yaw search has something to
// disambiguate against (a rotationally symmetric room makes it ambiguous).
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
    solids.push_back(Aabb{{-10.5, -1.5, 0.0}, {-9.0, 1.5, 1.2}});
    solids.push_back(Aabb{{8.0, -3.0, 0.0}, {11.0, -2.0, 0.8}});
    solids.push_back(Aabb{{-3.0, 6.0, 0.0}, {-1.0, 8.0, 2.2}});
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

void mid360_ray(std::uint64_t i, double dir[3]) {
  const double kGolden = 2.39996322972865332;
  const double az = std::fmod(static_cast<double>(i) * kGolden, 6.283185307179586);
  const double u = std::fmod(static_cast<double>(i) * 0.0173, 1.0);
  const double el = (-7.0 + 59.0 * u) * se3::kDegToRad;
  dir[0] = std::cos(el) * std::cos(az);
  dir[1] = std::cos(el) * std::sin(az);
  dir[2] = std::sin(el);
}

// Stationary for 0.6 s (the ESKF static-init window), then one full circle so
// the scanner returns to its own starting pose — a loop, not a tour.
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

}  // namespace

SyntheticMid360Result buildSyntheticMid360Project(const QString& dir, double duration_s) {
  SyntheticMid360Result out;

  const double points_per_sec = 20000.0;  // scaled down from the real 200k so a
                                           // full-density post run stays interactive
  const double imu_hz = 200.0;
  const double loop_radius_m = 6.0;
  const double range_noise_m = 0.01;
  const double gyro_bias = 0.004, accel_bias = 0.05, gyro_noise = 0.002, accel_noise = 0.02;

  QDir().mkpath(QFileInfo(dir).absolutePath());

  Room room;
  LoopTraj tr;
  tr.radius = loop_radius_m;
  tr.duration = duration_s;
  Rng rng(0xA7A7A7A7ull);

  lscan::FileRecordWriter w;
  const auto open_st = w.open(dir.toStdString());
  if (!open_st.ok()) {
    out.error = QString("FileRecordWriter::open(%1): %2")
                    .arg(dir, scanengine::error_str(open_st.error()));
    return out;
  }
  w.set_profile("survey");
  w.add_sensor("mid360-sim", "lidar", "Livox Mid-360 (synthetic)");

  const std::int64_t t0 = 1'700'000'000'000'000'000LL;
  const double dt_pkt = static_cast<double>(mid360::kPointsPerPacket) / points_per_sec;
  const double dt_imu = 1.0 / imu_hz;

  std::vector<std::uint8_t> pbuf(mid360::kPointPacketBytes);
  std::vector<std::uint8_t> ibuf(mid360::kImuPacketBytes);
  std::uint16_t udp_cnt = 0;
  std::uint64_t ray_seq = 0;

  double t_pkt = 0.0, t_imu = 0.0;
  while (t_pkt < duration_s || t_imu < duration_s) {
    const bool do_imu = t_imu <= t_pkt;
    if (do_imu) {
      double g[3], a[3];
      imu_truth(tr, t_imu, g, a);
      const float gyro[3] = {static_cast<float>(g[0] + gyro_bias + gyro_noise * rng.gauss()),
                             static_cast<float>(g[1] + gyro_bias + gyro_noise * rng.gauss()),
                             static_cast<float>(g[2] + gyro_bias + gyro_noise * rng.gauss())};
      const float acc[3] = {
          static_cast<float>((a[0] + accel_bias + accel_noise * rng.gauss()) / 9.80665),
          static_cast<float>((a[1] + accel_bias + accel_noise * rng.gauss()) / 9.80665),
          static_cast<float>((a[2] + accel_bias + accel_noise * rng.gauss()) / 9.80665)};
      const std::int64_t dev = static_cast<std::int64_t>(t_imu * 1e9) + 1;
      put_header(ibuf.data(), static_cast<std::uint16_t>(mid360::kImuPacketBytes), 1, udp_cnt++,
                 mid360::kDataTypeImu, static_cast<std::uint64_t>(dev));
      mid360::ImuRaw raw{gyro[0], gyro[1], gyro[2], acc[0], acc[1], acc[2]};
      std::memcpy(ibuf.data() + sizeof(mid360::DataHeader), &raw, sizeof(raw));
      const auto st = w.write_chunk(lscan::ChunkType::kMid360Imu, t0 + dev,
                                    ByteSpan(ibuf.data(), ibuf.size()));
      if (!st.ok()) {
        out.error = QString("write_chunk(imu): %1").arg(scanengine::error_str(st.error()));
        return out;
      }
      ++out.imu_packets;
      t_imu += dt_imu;
      continue;
    }
    double q[4], p[3];
    tr.pose(t_pkt, q, p);
    put_header(pbuf.data(), static_cast<std::uint16_t>(mid360::kPointPacketBytes),
               mid360::kPointsPerPacket, udp_cnt++, mid360::kDataTypeCartesianHigh,
               static_cast<std::uint64_t>(static_cast<std::int64_t>(t_pkt * 1e9) + 1));
    auto* pts = reinterpret_cast<mid360::CartesianHigh*>(pbuf.data() + sizeof(mid360::DataHeader));
    for (std::uint32_t i = 0; i < mid360::kPointsPerPacket; ++i) {
      double d_body[3];
      mid360_ray(ray_seq++, d_body);
      double d_world[3];
      qrot(q, d_body, d_world);
      const double t = room.cast(p, d_world);
      if (t <= 0.3 || t > 45.0) {
        pts[i] = mid360::CartesianHigh{0, 0, 0, 0, 0};
        continue;
      }
      const double r = t + range_noise_m * rng.gauss();
      pts[i].x = static_cast<std::int32_t>(d_body[0] * r * 1000.0);
      pts[i].y = static_cast<std::int32_t>(d_body[1] * r * 1000.0);
      pts[i].z = static_cast<std::int32_t>(d_body[2] * r * 1000.0);
      pts[i].reflectivity = 120;
      pts[i].tag = 0;
      ++out.points;
    }
    const auto st = w.write_chunk(lscan::ChunkType::kMid360Points,
                                  t0 + static_cast<std::int64_t>(t_pkt * 1e9) + 1,
                                  ByteSpan(pbuf.data(), pbuf.size()));
    if (!st.ok()) {
      out.error = QString("write_chunk(points): %1").arg(scanengine::error_str(st.error()));
      return out;
    }
    ++out.point_packets;
    t_pkt += dt_pkt;
  }

  const auto close_st = w.close();
  if (!close_st.ok()) {
    out.error = QString("FileRecordWriter::close: %1").arg(scanengine::error_str(close_st.error()));
    return out;
  }

  out.ok = true;
  out.duration_s = duration_s;
  return out;
}

}  // namespace lidarscan
