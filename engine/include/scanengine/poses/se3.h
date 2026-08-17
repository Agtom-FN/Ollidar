// se3.h — the small rigid-transform vocabulary the pose/pushbroom/calibration
// code shares.
//
// Deliberately dependency-free and expressed over PLAIN ARRAYS:
//
//   * a rotation is a unit quaternion `double q[4]` in (x, y, z, w) order —
//     the same order poses/pose_source.h's Pose::orientation already uses and
//     the same order core/event.h's PoseUpdatePayload mirrors across the C
//     ABI, so nothing has to be re-ordered at a boundary;
//   * a rigid transform is a ROW-MAJOR 4x4 `double m[16]`, matching the
//     `double transform[16]` / `double camera_from_lidar[16]` seams already
//     declared in slam/slam.h, color/colorize.h and merge/merge.h;
//   * a 3x3 rotation matrix is a row-major `double R[9]`.
//
// Naming: `a_from_b` transforms a point expressed in frame b into frame a,
// i.e. `X_a = a_from_b * X_b`. Compositions read left to right:
// `world_from_lidar = world_from_phone * phone_from_lidar`.
//
// Why no Eigen here: this header is included by public headers, and every
// operation it needs is a 3-vector or a 3x3/4x4 product. Keeping it plain
// keeps `ENGINE_WITH_EIGEN=OFF` a working configuration and keeps the C-ABI
// mirror of these types a memcpy. See engine/docs/A8-pushbroom.md §2 for the
// full "no Ceres, no Eigen" rationale.
//
// Owner: A8.
#ifndef SCANENGINE_POSES_SE3_H
#define SCANENGINE_POSES_SE3_H

#include <cmath>
#include <cstddef>

namespace scanengine {
namespace se3 {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;

// --- 3-vectors -------------------------------------------------------------

inline double dot3(const double a[3], const double b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline void cross3(const double a[3], const double b[3], double out[3]) {
  const double x = a[1] * b[2] - a[2] * b[1];
  const double y = a[2] * b[0] - a[0] * b[2];
  const double z = a[0] * b[1] - a[1] * b[0];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

inline double norm3(const double a[3]) { return std::sqrt(dot3(a, a)); }

inline bool normalize3(double a[3]) {
  const double n = norm3(a);
  if (!(n > 0.0)) return false;
  a[0] /= n;
  a[1] /= n;
  a[2] /= n;
  return true;
}

// --- quaternions (x, y, z, w) ---------------------------------------------

inline void quat_identity(double q[4]) {
  q[0] = q[1] = q[2] = 0.0;
  q[3] = 1.0;
}

inline bool quat_normalize(double q[4]) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(n > 0.0)) {
    quat_identity(q);
    return false;
  }
  q[0] /= n;
  q[1] /= n;
  q[2] /= n;
  q[3] /= n;
  return true;
}

// Hamilton product, (x, y, z, w) order: `out` = `a` then `b` applied in `a`'s
// frame, i.e. `R(out) = R(a) * R(b)`. Aliasing-safe.
//
// ROUND 9 added these two: until then the engine's only quaternion multiply
// lived in an anonymous namespace inside `slam/post/post_pipeline.cpp` and was
// not reachable from anywhere else, so the IMU densifier would have been the
// third hand-rolled copy.
inline void quat_mul(const double a[4], const double b[4], double out[4]) {
  const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

inline void quat_conj(const double q[4], double out[4]) {
  out[0] = -q[0];
  out[1] = -q[1];
  out[2] = -q[2];
  out[3] = q[3];
}

// Axis-angle (rotation vector, radians) -> quaternion, and back. Series-
// expanded near zero, which is the case that matters here: a gyro step at
// 400 Hz is a few milliradians and the naive `sin(t)/t` loses precision.
inline void quat_from_rotvec(const double w[3], double q[4]) {
  const double t2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
  const double t = std::sqrt(t2);
  double s;  // sin(t/2)/t
  if (t < 1e-8) {
    s = 0.5 - t2 / 48.0;  // 1/2 - t^2/48 + ...
    q[3] = 1.0 - t2 / 8.0;
  } else {
    s = std::sin(0.5 * t) / t;
    q[3] = std::cos(0.5 * t);
  }
  q[0] = w[0] * s;
  q[1] = w[1] * s;
  q[2] = w[2] * s;
}

inline void quat_to_rotvec(const double q[4], double w[3]) {
  double n[4] = {q[0], q[1], q[2], q[3]};
  quat_normalize(n);
  if (n[3] < 0.0) {  // shortest arc
    for (int i = 0; i < 4; ++i) n[i] = -n[i];
  }
  const double v = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  double k;  // angle / |v|
  if (v < 1e-8) {
    k = 2.0 + (2.0 / 3.0) * v * v;  // 2*asin(v)/v expanded
  } else {
    k = 2.0 * std::atan2(v, n[3]) / v;
  }
  w[0] = n[0] * k;
  w[1] = n[1] * k;
  w[2] = n[2] * k;
}

// Shortest-arc spherical linear interpolation. `u` is clamped to [0, 1].
//
// The sign fix (`if (cos < 0) negate b`) is not cosmetic: a quaternion and its
// negation are the same rotation, and ARCore/GTSAM/Eigen all hand out either
// sign freely. Without it a 179-degree-apart pair interpolates the long way
// round and a pushbroom sweep folds a wall inside out for one sample interval.
inline void quat_slerp(const double a[4], const double b[4], double u, double out[4]) {
  if (u <= 0.0) {
    for (int i = 0; i < 4; ++i) out[i] = a[i];
    quat_normalize(out);
    return;
  }
  if (u >= 1.0) {
    for (int i = 0; i < 4; ++i) out[i] = b[i];
    quat_normalize(out);
    return;
  }
  double cos_half = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  double bb[4] = {b[0], b[1], b[2], b[3]};
  if (cos_half < 0.0) {
    cos_half = -cos_half;
    for (int i = 0; i < 4; ++i) bb[i] = -bb[i];
  }
  double wa, wb;
  if (cos_half > 0.9995) {
    // Nearly parallel: lerp + renormalize. Numerically better than slerp here
    // and the angular error is below 1e-7 rad at this threshold.
    wa = 1.0 - u;
    wb = u;
  } else {
    const double half = std::acos(cos_half > 1.0 ? 1.0 : cos_half);
    const double s = std::sin(half);
    wa = std::sin((1.0 - u) * half) / s;
    wb = std::sin(u * half) / s;
  }
  for (int i = 0; i < 4; ++i) out[i] = wa * a[i] + wb * bb[i];
  quat_normalize(out);
}

// Row-major 3x3.
inline void quat_to_matrix(const double q[4], double R[9]) {
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  const double xx = x * x, yy = y * y, zz = z * z;
  const double xy = x * y, xz = x * z, yz = y * z;
  const double wx = w * x, wy = w * y, wz = w * z;
  R[0] = 1.0 - 2.0 * (yy + zz);
  R[1] = 2.0 * (xy - wz);
  R[2] = 2.0 * (xz + wy);
  R[3] = 2.0 * (xy + wz);
  R[4] = 1.0 - 2.0 * (xx + zz);
  R[5] = 2.0 * (yz - wx);
  R[6] = 2.0 * (xz - wy);
  R[7] = 2.0 * (yz + wx);
  R[8] = 1.0 - 2.0 * (xx + yy);
}

// Shepperd's method: pick the largest of the four diagonal combinations so the
// square root never loses precision near a 180-degree rotation.
inline void matrix_to_quat(const double R[9], double q[4]) {
  const double trace = R[0] + R[4] + R[8];
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    q[3] = 0.25 * s;
    q[0] = (R[7] - R[5]) / s;
    q[1] = (R[2] - R[6]) / s;
    q[2] = (R[3] - R[1]) / s;
  } else if (R[0] > R[4] && R[0] > R[8]) {
    const double s = std::sqrt(1.0 + R[0] - R[4] - R[8]) * 2.0;
    q[3] = (R[7] - R[5]) / s;
    q[0] = 0.25 * s;
    q[1] = (R[1] + R[3]) / s;
    q[2] = (R[2] + R[6]) / s;
  } else if (R[4] > R[8]) {
    const double s = std::sqrt(1.0 + R[4] - R[0] - R[8]) * 2.0;
    q[3] = (R[2] - R[6]) / s;
    q[0] = (R[1] + R[3]) / s;
    q[1] = 0.25 * s;
    q[2] = (R[5] + R[7]) / s;
  } else {
    const double s = std::sqrt(1.0 + R[8] - R[0] - R[4]) * 2.0;
    q[3] = (R[3] - R[1]) / s;
    q[0] = (R[2] + R[6]) / s;
    q[1] = (R[5] + R[7]) / s;
    q[2] = 0.25 * s;
  }
  quat_normalize(q);
}

// --- SO(3) exp / log -------------------------------------------------------

// Rodrigues. `w` is an axis-angle vector in radians.
inline void so3_exp(const double w[3], double R[9]) {
  const double theta = norm3(w);
  if (theta < 1e-12) {
    R[0] = 1.0; R[1] = -w[2]; R[2] = w[1];
    R[3] = w[2]; R[4] = 1.0; R[5] = -w[0];
    R[6] = -w[1]; R[7] = w[0]; R[8] = 1.0;
    return;
  }
  const double kx = w[0] / theta, ky = w[1] / theta, kz = w[2] / theta;
  const double c = std::cos(theta), s = std::sin(theta), v = 1.0 - c;
  R[0] = kx * kx * v + c;
  R[1] = kx * ky * v - kz * s;
  R[2] = kx * kz * v + ky * s;
  R[3] = ky * kx * v + kz * s;
  R[4] = ky * ky * v + c;
  R[5] = ky * kz * v - kx * s;
  R[6] = kz * kx * v - ky * s;
  R[7] = kz * ky * v + kx * s;
  R[8] = kz * kz * v + c;
}

inline void so3_log(const double R[9], double w[3]) {
  const double trace = R[0] + R[4] + R[8];
  double c = 0.5 * (trace - 1.0);
  if (c > 1.0) c = 1.0;
  if (c < -1.0) c = -1.0;
  const double theta = std::acos(c);
  if (theta < 1e-9) {
    w[0] = 0.5 * (R[7] - R[5]);
    w[1] = 0.5 * (R[2] - R[6]);
    w[2] = 0.5 * (R[3] - R[1]);
    return;
  }
  if (theta > kPi - 1e-6) {
    // Near pi the antisymmetric part vanishes; recover the axis from the
    // symmetric part instead (same branch problem matrix_to_quat solves).
    double q[4];
    matrix_to_quat(R, q);
    double v[3] = {q[0], q[1], q[2]};
    const double n = norm3(v);
    const double ang = 2.0 * std::atan2(n, q[3] < 0.0 ? -q[3] : q[3]);
    if (n > 0.0) {
      const double sgn = q[3] < 0.0 ? -1.0 : 1.0;
      for (int i = 0; i < 3; ++i) w[i] = sgn * v[i] / n * ang;
    } else {
      w[0] = w[1] = w[2] = 0.0;
    }
    return;
  }
  const double k = theta / (2.0 * std::sin(theta));
  w[0] = k * (R[7] - R[5]);
  w[1] = k * (R[2] - R[6]);
  w[2] = k * (R[3] - R[1]);
}

// Geodesic angle between two rotations, in degrees:
//   theta = acos((trace(Ra^T Rb) - 1) / 2)
inline double rot_angle_deg(const double Ra[9], const double Rb[9]) {
  double t = 0.0;
  for (int i = 0; i < 3; ++i) {
    // (Ra^T Rb)[i][i] = sum_j Ra[j][i] * Rb[j][i]
    for (int j = 0; j < 3; ++j) t += Ra[j * 3 + i] * Rb[j * 3 + i];
  }
  double c = 0.5 * (t - 1.0);
  if (c > 1.0) c = 1.0;
  if (c < -1.0) c = -1.0;
  return std::acos(c) * kRadToDeg;
}

// --- 4x4 row-major rigid transforms ---------------------------------------

inline void mat4_identity(double m[16]) {
  for (int i = 0; i < 16; ++i) m[i] = 0.0;
  m[0] = m[5] = m[10] = m[15] = 1.0;
}

inline void mat4_from_rt(const double R[9], const double t[3], double m[16]) {
  m[0] = R[0]; m[1] = R[1]; m[2] = R[2];  m[3] = t[0];
  m[4] = R[3]; m[5] = R[4]; m[6] = R[5];  m[7] = t[1];
  m[8] = R[6]; m[9] = R[7]; m[10] = R[8]; m[11] = t[2];
  m[12] = 0.0; m[13] = 0.0; m[14] = 0.0;  m[15] = 1.0;
}

inline void mat4_get_rt(const double m[16], double R[9], double t[3]) {
  R[0] = m[0]; R[1] = m[1]; R[2] = m[2];
  R[3] = m[4]; R[4] = m[5]; R[5] = m[6];
  R[6] = m[8]; R[7] = m[9]; R[8] = m[10];
  t[0] = m[3]; t[1] = m[7]; t[2] = m[11];
}

inline void mat4_from_quat_pos(const double q[4], const double p[3], double m[16]) {
  double R[9];
  quat_to_matrix(q, R);
  mat4_from_rt(R, p, m);
}

// out = a * b. Safe when `out` aliases `a` or `b`.
inline void mat4_mul(const double a[16], const double b[16], double out[16]) {
  double tmp[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double s = 0.0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      tmp[r * 4 + c] = s;
    }
  }
  for (int i = 0; i < 16; ++i) out[i] = tmp[i];
}

// Inverse of a RIGID transform: [R|t]^-1 = [R^T | -R^T t]. Not a general
// 4x4 inverse — callers must not pass a scaled/sheared matrix.
inline void mat4_inverse_rigid(const double m[16], double out[16]) {
  double R[9], t[3];
  mat4_get_rt(m, R, t);
  double Rt[9] = {R[0], R[3], R[6], R[1], R[4], R[7], R[2], R[5], R[8]};
  double nt[3];
  for (int i = 0; i < 3; ++i) nt[i] = -(Rt[i * 3 + 0] * t[0] + Rt[i * 3 + 1] * t[1] + Rt[i * 3 + 2] * t[2]);
  mat4_from_rt(Rt, nt, out);
}

inline void mat4_apply(const double m[16], const double p[3], double out[3]) {
  const double x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
  const double y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
  const double z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

// Rotate only (no translation) — used for normals and directions.
inline void mat4_rotate(const double m[16], const double v[3], double out[3]) {
  const double x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
  const double y = m[4] * v[0] + m[5] * v[1] + m[6] * v[2];
  const double z = m[8] * v[0] + m[9] * v[1] + m[10] * v[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

// True when `m` is a plausible rigid transform: finite, orthonormal rotation,
// bottom row (0,0,0,1). Guards every double[16] that crosses an API boundary
// (a JNI caller with a column-major matrix would otherwise silently produce a
// mirrored cloud).
inline bool mat4_is_rigid(const double m[16], double tol = 1e-6) {
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(m[i])) return false;
  }
  if (std::fabs(m[12]) > tol || std::fabs(m[13]) > tol || std::fabs(m[14]) > tol ||
      std::fabs(m[15] - 1.0) > tol) {
    return false;
  }
  double R[9], t[3];
  mat4_get_rt(m, R, t);
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) s += R[k * 3 + i] * R[k * 3 + j];
      const double want = (i == j) ? 1.0 : 0.0;
      if (std::fabs(s - want) > tol) return false;
    }
  }
  // det > 0 rules out a reflection (which passes the orthonormality test).
  const double det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) +
                     R[2] * (R[3] * R[7] - R[4] * R[6]);
  return det > 0.0;
}

// Errors between two rigid transforms, in the units the S6 report uses.
inline void transform_error(const double a[16], const double b[16], double* rot_deg,
                            double* trans_mm) {
  double Ra[9], ta[3], Rb[9], tb[3];
  mat4_get_rt(a, Ra, ta);
  mat4_get_rt(b, Rb, tb);
  if (rot_deg != nullptr) *rot_deg = rot_angle_deg(Ra, Rb);
  if (trans_mm != nullptr) {
    const double d[3] = {ta[0] - tb[0], ta[1] - tb[1], ta[2] - tb[2]};
    *trans_mm = norm3(d) * 1000.0;
  }
}

}  // namespace se3
}  // namespace scanengine

#endif  // SCANENGINE_POSES_SE3_H
