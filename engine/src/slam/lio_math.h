// lio_math.h — the small, dependency-free linear algebra the A6 LIO needs.
//
// INTERNAL to src/slam/. Nothing outside this directory may include it
// (DESIGN.md §1: "Nothing in src/ is included across module boundaries").
//
// WHY NOT EIGEN. Eigen is linked and available, and A7 will certainly want
// it. A6 does not: the whole odometry needs exactly four things — 3-vectors,
// 3x3 rotations, an 18x18 symmetric solve/inverse, and SO(3) exp/log. Hand
// rolling them is ~250 lines, and it buys three properties that matter more
// here than convenience:
//
//   1. The engine still builds and the LIO still runs with
//      -DENGINE_WITH_EIGEN=OFF. No #ifdef forest, no second code path that
//      nobody tests.
//   2. Determinism is under our control. Eigen's expression templates pick
//      different vectorized reduction orders per -march / per platform, so
//      "identical input ⇒ identical output" would become an
//      identical-toolchain promise. Every loop below reduces in a fixed
//      index order, so the promise holds across the five CI legs.
//   3. The 18x18 solve is the only non-trivial routine, it runs a handful of
//      times per scan, and an unpivoted LDL^T of a well-conditioned SPD
//      information matrix is both the right algorithm and 40 lines.
//
// Everything is row-major. `Mat3::m[r * 3 + c]`.
#ifndef SCANENGINE_SRC_SLAM_LIO_MATH_H
#define SCANENGINE_SRC_SLAM_LIO_MATH_H

#include <cmath>
#include <cstddef>

namespace scanengine {
namespace slam_detail {

// --- Vec3 -----------------------------------------------------------------

struct Vec3 {
  double v[3] = {0.0, 0.0, 0.0};

  Vec3() = default;
  Vec3(double a, double b, double c) : v{a, b, c} {}
  explicit Vec3(const double* p) : v{p[0], p[1], p[2]} {}

  double& operator[](int i) { return v[i]; }
  double operator[](int i) const { return v[i]; }
  void store(double* p) const {
    p[0] = v[0];
    p[1] = v[1];
    p[2] = v[2];
  }
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
  return Vec3(a[0] + b[0], a[1] + b[1], a[2] + b[2]);
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) {
  return Vec3(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
inline Vec3 operator-(const Vec3& a) { return Vec3(-a[0], -a[1], -a[2]); }
inline Vec3 operator*(const Vec3& a, double s) { return Vec3(a[0] * s, a[1] * s, a[2] * s); }
inline Vec3 operator*(double s, const Vec3& a) { return a * s; }
inline Vec3& operator+=(Vec3& a, const Vec3& b) {
  a[0] += b[0];
  a[1] += b[1];
  a[2] += b[2];
  return a;
}
inline double dot(const Vec3& a, const Vec3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return Vec3(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
}
inline double norm2(const Vec3& a) { return dot(a, a); }
inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline bool finite(const Vec3& a) {
  return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
}

// --- Mat3 -----------------------------------------------------------------

struct Mat3 {
  double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

  static Mat3 identity() { return Mat3(); }
  static Mat3 zero() {
    Mat3 r;
    for (int i = 0; i < 9; ++i) r.m[i] = 0.0;
    return r;
  }
  double& operator()(int r, int c) { return m[r * 3 + c]; }
  double operator()(int r, int c) const { return m[r * 3 + c]; }
};

inline Mat3 operator*(const Mat3& a, const Mat3& b) {
  Mat3 r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      r.m[i * 3 + j] = a.m[i * 3 + 0] * b.m[0 * 3 + j] + a.m[i * 3 + 1] * b.m[1 * 3 + j] +
                       a.m[i * 3 + 2] * b.m[2 * 3 + j];
    }
  }
  return r;
}
inline Vec3 operator*(const Mat3& a, const Vec3& x) {
  return Vec3(a.m[0] * x[0] + a.m[1] * x[1] + a.m[2] * x[2],
              a.m[3] * x[0] + a.m[4] * x[1] + a.m[5] * x[2],
              a.m[6] * x[0] + a.m[7] * x[1] + a.m[8] * x[2]);
}
inline Mat3 operator*(const Mat3& a, double s) {
  Mat3 r;
  for (int i = 0; i < 9; ++i) r.m[i] = a.m[i] * s;
  return r;
}
inline Mat3 operator+(const Mat3& a, const Mat3& b) {
  Mat3 r;
  for (int i = 0; i < 9; ++i) r.m[i] = a.m[i] + b.m[i];
  return r;
}
inline Mat3 operator-(const Mat3& a, const Mat3& b) {
  Mat3 r;
  for (int i = 0; i < 9; ++i) r.m[i] = a.m[i] - b.m[i];
  return r;
}
inline Mat3 transpose(const Mat3& a) {
  Mat3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[j * 3 + i];
  return r;
}
// R^T * x, without materializing R^T.
inline Vec3 mul_transpose(const Mat3& a, const Vec3& x) {
  return Vec3(a.m[0] * x[0] + a.m[3] * x[1] + a.m[6] * x[2],
              a.m[1] * x[0] + a.m[4] * x[1] + a.m[7] * x[2],
              a.m[2] * x[0] + a.m[5] * x[1] + a.m[8] * x[2]);
}
// Skew-symmetric matrix with hat(a) * b == cross(a, b).
inline Mat3 hat(const Vec3& a) {
  Mat3 r = Mat3::zero();
  r(0, 1) = -a[2];
  r(0, 2) = a[1];
  r(1, 0) = a[2];
  r(1, 2) = -a[0];
  r(2, 0) = -a[1];
  r(2, 1) = a[0];
  return r;
}

// Re-orthonormalize by modified Gram-Schmidt on the rows. Called after every
// propagation step: a chain of Exp() products drifts off SO(3) at ~1e-16 per
// step, which over a 30-minute capture at 200 Hz is 3.6e5 steps — small, but
// free to remove and not worth reasoning about later.
inline Mat3 orthonormalize(const Mat3& a) {
  Vec3 r0(a.m[0], a.m[1], a.m[2]);
  Vec3 r1(a.m[3], a.m[4], a.m[5]);
  const double n0 = norm(r0);
  if (!(n0 > 0.0)) return Mat3::identity();
  r0 = r0 * (1.0 / n0);
  r1 = r1 - r0 * dot(r1, r0);
  const double n1 = norm(r1);
  if (!(n1 > 0.0)) return Mat3::identity();
  r1 = r1 * (1.0 / n1);
  const Vec3 r2 = cross(r0, r1);
  Mat3 out;
  out.m[0] = r0[0];
  out.m[1] = r0[1];
  out.m[2] = r0[2];
  out.m[3] = r1[0];
  out.m[4] = r1[1];
  out.m[5] = r1[2];
  out.m[6] = r2[0];
  out.m[7] = r2[1];
  out.m[8] = r2[2];
  return out;
}

// --- SO(3) ----------------------------------------------------------------

// Rodrigues. The small-angle branch is a Taylor expansion, not an
// approximation of convenience: sin(t)/t loses all precision below ~1e-8.
inline Mat3 so3_exp(const Vec3& w) {
  const double t2 = norm2(w);
  const double t = std::sqrt(t2);
  Mat3 K = hat(w);
  double a, b;  // R = I + a*K + b*K*K
  if (t < 1e-8) {
    a = 1.0 - t2 / 6.0;
    b = 0.5 - t2 / 24.0;
  } else {
    a = std::sin(t) / t;
    b = (1.0 - std::cos(t)) / t2;
  }
  Mat3 KK = K * K;
  Mat3 R = Mat3::identity();
  for (int i = 0; i < 9; ++i) R.m[i] += a * K.m[i] + b * KK.m[i];
  return R;
}

inline Vec3 so3_log(const Mat3& R) {
  const double tr = R(0, 0) + R(1, 1) + R(2, 2);
  double c = (tr - 1.0) * 0.5;
  if (c > 1.0) c = 1.0;
  if (c < -1.0) c = -1.0;
  const double theta = std::acos(c);
  const Vec3 u(R(2, 1) - R(1, 2), R(0, 2) - R(2, 0), R(1, 0) - R(0, 1));
  if (theta < 1e-8) return u * 0.5;                    // sin(t)/t -> 1
  if (theta > 3.14159265358979 - 1e-6) {
    // Near pi the antisymmetric part vanishes; recover the axis from the
    // largest diagonal of (R + I)/2 = a*a^T.
    int k = 0;
    double best = R(0, 0);
    if (R(1, 1) > best) { best = R(1, 1); k = 1; }
    if (R(2, 2) > best) { best = R(2, 2); k = 2; }
    Vec3 axis;
    const double d = std::sqrt((R(k, k) + 1.0) * 0.5);
    if (!(d > 0.0)) return Vec3();
    axis[k] = d;
    for (int i = 0; i < 3; ++i) {
      if (i != k) axis[i] = (R(k, i) + R(i, k)) * 0.25 / d;
    }
    const double n = norm(axis);
    if (!(n > 0.0)) return Vec3();
    return axis * (theta / n);
  }
  return u * (theta / (2.0 * std::sin(theta)));
}

// Right Jacobian of SO(3): d Exp(w+dw) ~= Exp(w) Exp(Jr(w) dw).
inline Mat3 so3_jr(const Vec3& w) {
  const double t2 = norm2(w);
  const double t = std::sqrt(t2);
  Mat3 K = hat(w);
  double a, b;  // Jr = I - a*K + b*K*K
  if (t < 1e-6) {
    a = 0.5 - t2 / 24.0;
    b = 1.0 / 6.0 - t2 / 120.0;
  } else {
    a = (1.0 - std::cos(t)) / t2;
    b = (t - std::sin(t)) / (t2 * t);
  }
  Mat3 KK = K * K;
  Mat3 J = Mat3::identity();
  for (int i = 0; i < 9; ++i) J.m[i] += -a * K.m[i] + b * KK.m[i];
  return J;
}

// Rotation matrix -> unit quaternion (x, y, z, w). Shepperd's method: pick
// the branch with the largest denominator so no square root is taken of a
// cancelling difference.
inline void mat3_to_quat(const Mat3& R, double q[4]) {
  const double tr = R(0, 0) + R(1, 1) + R(2, 2);
  if (tr > 0.0) {
    const double s = std::sqrt(tr + 1.0) * 2.0;
    q[3] = 0.25 * s;
    q[0] = (R(2, 1) - R(1, 2)) / s;
    q[1] = (R(0, 2) - R(2, 0)) / s;
    q[2] = (R(1, 0) - R(0, 1)) / s;
  } else if (R(0, 0) > R(1, 1) && R(0, 0) > R(2, 2)) {
    const double s = std::sqrt(1.0 + R(0, 0) - R(1, 1) - R(2, 2)) * 2.0;
    q[3] = (R(2, 1) - R(1, 2)) / s;
    q[0] = 0.25 * s;
    q[1] = (R(0, 1) + R(1, 0)) / s;
    q[2] = (R(0, 2) + R(2, 0)) / s;
  } else if (R(1, 1) > R(2, 2)) {
    const double s = std::sqrt(1.0 + R(1, 1) - R(0, 0) - R(2, 2)) * 2.0;
    q[3] = (R(0, 2) - R(2, 0)) / s;
    q[0] = (R(0, 1) + R(1, 0)) / s;
    q[1] = 0.25 * s;
    q[2] = (R(1, 2) + R(2, 1)) / s;
  } else {
    const double s = std::sqrt(1.0 + R(2, 2) - R(0, 0) - R(1, 1)) * 2.0;
    q[3] = (R(1, 0) - R(0, 1)) / s;
    q[0] = (R(0, 2) + R(2, 0)) / s;
    q[1] = (R(1, 2) + R(2, 1)) / s;
    q[2] = 0.25 * s;
  }
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n > 0.0) {
    for (int i = 0; i < 4; ++i) q[i] /= n;
  }
  if (q[3] < 0.0) {  // canonical hemisphere; makes pose interpolation stable
    for (int i = 0; i < 4; ++i) q[i] = -q[i];
  }
}

inline Mat3 quat_to_mat3(const double q[4]) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(n > 0.0)) return Mat3::identity();
  const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
  Mat3 R;
  R(0, 0) = 1 - 2 * (y * y + z * z);
  R(0, 1) = 2 * (x * y - z * w);
  R(0, 2) = 2 * (x * z + y * w);
  R(1, 0) = 2 * (x * y + z * w);
  R(1, 1) = 1 - 2 * (x * x + z * z);
  R(1, 2) = 2 * (y * z - x * w);
  R(2, 0) = 2 * (x * z - y * w);
  R(2, 1) = 2 * (y * z + x * w);
  R(2, 2) = 1 - 2 * (x * x + y * y);
  return R;
}

// Shortest rotation taking unit vector `from` onto unit vector `to`.
inline Mat3 rotation_between(const Vec3& from, const Vec3& to) {
  const double nf = norm(from), nt = norm(to);
  if (!(nf > 0.0) || !(nt > 0.0)) return Mat3::identity();
  const Vec3 a = from * (1.0 / nf), b = to * (1.0 / nt);
  double c = dot(a, b);
  if (c > 1.0) c = 1.0;
  if (c < -1.0) c = -1.0;
  Vec3 axis = cross(a, b);
  const double s = norm(axis);
  if (s < 1e-12) {
    if (c > 0.0) return Mat3::identity();
    // Anti-parallel: any perpendicular axis, chosen deterministically.
    Vec3 seed = (std::fabs(a[0]) < 0.9) ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    Vec3 perp = cross(a, seed);
    perp = perp * (1.0 / norm(perp));
    return so3_exp(perp * 3.14159265358979);
  }
  return so3_exp(axis * (std::acos(c) / s));
}

// --- plane fitting --------------------------------------------------------
//
// Eigenvalues of a symmetric 3x3, ascending, by the closed-form
// trigonometric solution of the characteristic cubic (Smith 1961). Used for
// the local plane fits the point-to-plane residual needs.
//
// WHY NOT THE FAST-LIO2 PARAMETRIZATION. FAST-LIO2 fits `n . p + 1 = 0` by
// least squares and recovers d = 1/|x|. That is cheaper — one 3x3 normal-
// equation solve, no cubic — but it is singular for any plane through the
// world origin, and the world origin is the scanner's own starting pose,
// which is a place a hand-carried scanner walks back through. A covariance
// eigen-decomposition about the centroid has no such degeneracy and lets the
// planarity ratio (l0/l1) be used as a rejection test for free.
bool sym3_eigenvalues(const Mat3& a, double e[3]);

// Unit eigenvector of the smallest eigenvalue — i.e. the normal of the best
// plane through a centred point set. Returns false if the matrix is
// degenerate enough that no direction is well determined.
bool sym3_smallest_eigenvector(const Mat3& a, Vec3* n, double e[3]);

// --- dense symmetric solve ------------------------------------------------
//
// Unpivoted LDL^T. The matrices this is applied to are information matrices
// (H^T R^-1 H + P^-1) and covariances, both SPD by construction, so pivoting
// buys nothing and costs determinism (a pivot order that depends on rounding
// is exactly the kind of thing that makes two builds disagree).
//
// `a` is n*n row-major and is overwritten: unit-lower L below the diagonal,
// D on it. Returns false if a pivot is not positive — the caller must treat
// that as "the update is not usable", never as "close enough".
bool ldlt_factor(double* a, int n);

// Solves (LDL^T) x = b in place, given a factored `a` from ldlt_factor.
void ldlt_solve_inplace(const double* a, int n, double* b);

// inv = A^-1 for SPD A (n*n row-major). `a` is not modified. `scratch` must
// hold n*n doubles. Returns false if A is not positive definite.
bool ldlt_inverse(const double* a, int n, double* inv, double* scratch);

}  // namespace slam_detail
}  // namespace scanengine

#endif  // SCANENGINE_SRC_SLAM_LIO_MATH_H
