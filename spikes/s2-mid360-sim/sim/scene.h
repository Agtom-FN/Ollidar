// S2-sim — synthetic 12 x 8 x 3 m room, sensor trajectory, ray casting and a
// gravity+rotation-consistent IMU model.
//
// The trajectory is analytic so that IMU angular rate and specific force are
// exact derivatives of the same pose used to place the lidar rays — i.e. the
// point cloud and the IMU stream describe one consistent rigid-body motion,
// which is what a lidar-inertial front end (A6) needs to be exercised properly.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace s2sim {

constexpr double kGravity = 9.80665;  // m/s^2

struct Vec3 {
  double x = 0, y = 0, z = 0;
  Vec3() = default;
  Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
};

struct Mat3 {
  double m[3][3];
  Vec3 operator*(const Vec3& v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
  }
  Vec3 Tmul(const Vec3& v) const {  // transpose * v
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
  }
};

// Axis-aligned box. `interior` boxes are hit from the outside (furniture);
// the room shell is hit from the inside.
struct Box {
  Vec3 lo, hi;
  double albedo;
};

struct Pose {
  Vec3 p;         // position, world
  Mat3 R;         // body -> world
  Vec3 v;         // velocity, world
  Vec3 a;         // acceleration, world
  Vec3 omega_b;   // angular rate, body frame (rad/s)
};

// ---------------------------------------------------------------------------
// Room: 12 (x) x 8 (y) x 3 (z) m shell plus a little furniture so the cloud is
// not six bare planes.
// ---------------------------------------------------------------------------
class Room {
 public:
  Room() {
    shell_ = Box{{0, 0, 0}, {12.0, 8.0, 3.0}, 0.55};
    // two structural pillars and a table-height slab
    furniture_.push_back(Box{{3.8, 3.8, 0.0}, {4.2, 4.2, 3.0}, 0.35});
    furniture_.push_back(Box{{8.0, 3.8, 0.0}, {8.4, 4.2, 3.0}, 0.35});
    furniture_.push_back(Box{{5.5, 1.0, 0.0}, {7.5, 2.2, 0.75}, 0.70});
    furniture_.push_back(Box{{1.0, 6.0, 0.0}, {2.2, 7.4, 1.9}, 0.25});
  }

  // Cast a ray from inside the room. Returns range in metres (>0) and the
  // albedo * |cos(incidence)| shading term used for reflectivity.
  bool Cast(const Vec3& o, const Vec3& d, double& range, double& shade) const {
    // Exit distance through the shell (ray starts inside).
    double t_shell = RayExitBox(o, d, shell_);
    double best = t_shell;
    Vec3 best_n = ExitNormal(o, d, shell_, t_shell);
    double best_alb = shell_.albedo;

    for (const Box& b : furniture_) {
      double t;
      Vec3 n;
      if (RayEnterBox(o, d, b, t, n) && t > 1e-3 && t < best) {
        best = t;
        best_n = n;
        best_alb = b.albedo;
      }
    }
    if (!(best > 1e-3) || !std::isfinite(best)) return false;
    range = best;
    double cosi = std::fabs(best_n.x * d.x + best_n.y * d.y + best_n.z * d.z);
    shade = best_alb * std::max(0.05, cosi);
    return true;
  }

 private:
  // Distance at which a ray starting inside `b` leaves it.
  static double RayExitBox(const Vec3& o, const Vec3& d, const Box& b) {
    double t = std::numeric_limits<double>::infinity();
    t = std::min(t, SlabExit(o.x, d.x, b.lo.x, b.hi.x));
    t = std::min(t, SlabExit(o.y, d.y, b.lo.y, b.hi.y));
    t = std::min(t, SlabExit(o.z, d.z, b.lo.z, b.hi.z));
    return t;
  }
  static double SlabExit(double o, double d, double lo, double hi) {
    if (std::fabs(d) < 1e-12) return std::numeric_limits<double>::infinity();
    return d > 0 ? (hi - o) / d : (lo - o) / d;
  }
  static Vec3 ExitNormal(const Vec3& o, const Vec3& d, const Box& b, double t) {
    double tx = SlabExit(o.x, d.x, b.lo.x, b.hi.x);
    double ty = SlabExit(o.y, d.y, b.lo.y, b.hi.y);
    double tz = SlabExit(o.z, d.z, b.lo.z, b.hi.z);
    if (t == tx) return {d.x > 0 ? -1.0 : 1.0, 0, 0};
    if (t == ty) return {0, d.y > 0 ? -1.0 : 1.0, 0};
    (void)tz;
    return {0, 0, d.z > 0 ? -1.0 : 1.0};
  }
  // Standard slab test for a ray entering a box from outside.
  static bool RayEnterBox(const Vec3& o, const Vec3& d, const Box& b, double& t_out, Vec3& n_out) {
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    int axis = 0;
    double sign = 1.0;
    const double ol[3] = {o.x, o.y, o.z};
    const double dl[3] = {d.x, d.y, d.z};
    const double lo[3] = {b.lo.x, b.lo.y, b.lo.z};
    const double hi[3] = {b.hi.x, b.hi.y, b.hi.z};
    for (int i = 0; i < 3; ++i) {
      if (std::fabs(dl[i]) < 1e-12) {
        if (ol[i] < lo[i] || ol[i] > hi[i]) return false;
        continue;
      }
      double inv = 1.0 / dl[i];
      double t1 = (lo[i] - ol[i]) * inv;
      double t2 = (hi[i] - ol[i]) * inv;
      double s = -1.0;
      if (t1 > t2) {
        std::swap(t1, t2);
        s = 1.0;
      }
      if (t1 > tmin) {
        tmin = t1;
        axis = i;
        sign = s;
      }
      tmax = std::min(tmax, t2);
      if (tmin > tmax) return false;
    }
    if (tmin < 0) return false;  // origin inside the furniture box; ignore
    t_out = tmin;
    n_out = Vec3(axis == 0 ? sign : 0.0, axis == 1 ? sign : 0.0, axis == 2 ? sign : 0.0);
    return true;
  }

  Box shell_;
  std::vector<Box> furniture_;
};

// ---------------------------------------------------------------------------
// Analytic sensor trajectory: slow lissajous translation + slow yaw sweep with
// a small roll/pitch wobble, all C^2 so acceleration and angular rate are exact.
// ---------------------------------------------------------------------------
class Trajectory {
 public:
  Pose At(double t) const {
    Pose q;
    const double wx = 2 * M_PI / 97.0, wy = 2 * M_PI / 71.0, wz = 2 * M_PI / 43.0;
    const double Ax = 3.6, Ay = 2.4, Az = 0.10;
    q.p = {6.0 + Ax * std::sin(wx * t), 4.0 + Ay * std::sin(wy * t), 1.25 + Az * std::sin(wz * t)};
    q.v = {Ax * wx * std::cos(wx * t), Ay * wy * std::cos(wy * t), Az * wz * std::cos(wz * t)};
    q.a = {-Ax * wx * wx * std::sin(wx * t), -Ay * wy * wy * std::sin(wy * t),
           -Az * wz * wz * std::sin(wz * t)};

    const double wyaw = 2 * M_PI / 240.0;  // one turn every 4 minutes
    const double wr = 2 * M_PI / 13.0, wp = 2 * M_PI / 17.0;
    const double Ar = 0.035, Ap = 0.045;  // rad
    double roll = Ar * std::sin(wr * t), roll_d = Ar * wr * std::cos(wr * t);
    double pitch = Ap * std::sin(wp * t), pitch_d = Ap * wp * std::cos(wp * t);
    double yaw = wyaw * t, yaw_d = wyaw;

    q.R = FromRpy(roll, pitch, yaw);
    // Body rates from ZYX Euler rates.
    double sr = std::sin(roll), cr = std::cos(roll);
    double sp = std::sin(pitch), cp = std::cos(pitch);
    q.omega_b = {roll_d - yaw_d * sp, pitch_d * cr + yaw_d * cp * sr,
                 -pitch_d * sr + yaw_d * cp * cr};
    return q;
  }

  // Specific force measured by an accelerometer, in body frame, in g.
  static Vec3 SpecificForceG(const Pose& q) {
    Vec3 g_world(0, 0, -kGravity);
    Vec3 f_world = q.a - g_world;
    Vec3 f_body = q.R.Tmul(f_world);
    return f_body * (1.0 / kGravity);
  }

 private:
  static Mat3 FromRpy(double r, double p, double y) {
    double cr = std::cos(r), sr = std::sin(r);
    double cp = std::cos(p), sp = std::sin(p);
    double cy = std::cos(y), sy = std::sin(y);
    Mat3 R{};
    R.m[0][0] = cy * cp;
    R.m[0][1] = cy * sp * sr - sy * cr;
    R.m[0][2] = cy * sp * cr + sy * sr;
    R.m[1][0] = sy * cp;
    R.m[1][1] = sy * sp * sr + cy * cr;
    R.m[1][2] = sy * sp * cr - cy * sr;
    R.m[2][0] = -sp;
    R.m[2][1] = cp * sr;
    R.m[2][2] = cp * cr;
    return R;
  }
};

// ---------------------------------------------------------------------------
// Scan pattern. The real Mid-360 uses a Risley-prism non-repetitive pattern we
// do not have the constants for; this is a two-incommensurate-frequency sweep
// covering the documented 360 deg x [-7, +52] deg FOV without repeating over a
// 10 min run. Documented as a deliberate simplification in REPORT.md.
// ---------------------------------------------------------------------------
inline void ScanDirection(uint64_t point_index, double& az, double& el) {
  // Azimuth: fast sweep; elevation: slower sweep at an irrational frequency
  // ratio so successive frames do not overlay.
  constexpr double kAzRevPerSec = 149.0;
  constexpr double kElSweepPerSec = 92.0 * 0.6180339887498949;  // golden-ratio offset
  constexpr double kRate = 200000.0;
  double t = static_cast<double>(point_index) / kRate;
  az = 2.0 * M_PI * kAzRevPerSec * t;
  double u = std::sin(2.0 * M_PI * kElSweepPerSec * t);
  const double el_mid = (52.0 - 7.0) * 0.5 * M_PI / 180.0;   // +22.5 deg
  const double el_amp = (52.0 + 7.0) * 0.5 * M_PI / 180.0;   //  29.5 deg
  el = el_mid + el_amp * u;
}

}  // namespace s2sim
