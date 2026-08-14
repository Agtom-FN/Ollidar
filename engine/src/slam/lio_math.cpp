// lio_math.cpp — the only out-of-line part of src/slam/lio_math.h.
#include "lio_math.h"

#include <utility>

namespace scanengine {
namespace slam_detail {

namespace {
constexpr double kPi = 3.14159265358979323846;

inline double det3(const Mat3& m) {
  return m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) -
         m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
         m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
}

inline void sort3(double e[3]) {
  if (e[0] > e[1]) std::swap(e[0], e[1]);
  if (e[1] > e[2]) std::swap(e[1], e[2]);
  if (e[0] > e[1]) std::swap(e[0], e[1]);
}
}  // namespace

bool sym3_eigenvalues(const Mat3& a, double e[3]) {
  const double a00 = a(0, 0), a11 = a(1, 1), a22 = a(2, 2);
  const double a01 = a(0, 1), a02 = a(0, 2), a12 = a(1, 2);
  const double p1 = a01 * a01 + a02 * a02 + a12 * a12;
  const double q = (a00 + a11 + a22) / 3.0;
  const double scale = std::fabs(q) + 1.0;
  if (p1 <= 1e-24 * scale * scale) {
    e[0] = a00;
    e[1] = a11;
    e[2] = a22;
    sort3(e);
    return std::isfinite(e[0]) && std::isfinite(e[2]);
  }
  const double p2 =
      (a00 - q) * (a00 - q) + (a11 - q) * (a11 - q) + (a22 - q) * (a22 - q) + 2.0 * p1;
  const double p = std::sqrt(p2 / 6.0);
  if (!(p > 0.0)) return false;
  Mat3 b = a;
  b(0, 0) -= q;
  b(1, 1) -= q;
  b(2, 2) -= q;
  for (int i = 0; i < 9; ++i) b.m[i] /= p;
  double r = det3(b) * 0.5;
  if (r > 1.0) r = 1.0;
  if (r < -1.0) r = -1.0;
  const double phi = std::acos(r) / 3.0;
  const double e_max = q + 2.0 * p * std::cos(phi);
  const double e_min = q + 2.0 * p * std::cos(phi + 2.0 * kPi / 3.0);
  e[0] = e_min;
  e[1] = 3.0 * q - e_max - e_min;
  e[2] = e_max;
  return std::isfinite(e[0]) && std::isfinite(e[2]);
}

bool sym3_smallest_eigenvector(const Mat3& a, Vec3* n, double e[3]) {
  double eig[3];
  if (!sym3_eigenvalues(a, eig)) return false;
  if (e != nullptr) {
    e[0] = eig[0];
    e[1] = eig[1];
    e[2] = eig[2];
  }
  // (A - lambda_min I) has rank 2; the null direction is the cross product of
  // any two independent rows. Take the pair with the largest cross product so
  // the choice is both stable and deterministic.
  Mat3 b = a;
  b(0, 0) -= eig[0];
  b(1, 1) -= eig[0];
  b(2, 2) -= eig[0];
  const Vec3 r0(b.m[0], b.m[1], b.m[2]);
  const Vec3 r1(b.m[3], b.m[4], b.m[5]);
  const Vec3 r2(b.m[6], b.m[7], b.m[8]);
  const Vec3 c[3] = {cross(r0, r1), cross(r0, r2), cross(r1, r2)};
  int best = 0;
  double best_n2 = norm2(c[0]);
  for (int i = 1; i < 3; ++i) {
    const double n2 = norm2(c[i]);
    if (n2 > best_n2) {
      best_n2 = n2;
      best = i;
    }
  }
  if (!(best_n2 > 1e-30)) return false;
  Vec3 v = c[best] * (1.0 / std::sqrt(best_n2));
  // Fix the sign deterministically: the largest-magnitude component is made
  // positive. Which way a plane normal points is arbitrary, and the residual
  // is signed, so an unstable sign would flip residuals between iterations.
  int k = 0;
  for (int i = 1; i < 3; ++i) {
    if (std::fabs(v[i]) > std::fabs(v[k])) k = i;
  }
  if (v[k] < 0.0) v = -v;
  *n = v;
  return true;
}

bool ldlt_factor(double* a, int n) {
  for (int j = 0; j < n; ++j) {
    double d = a[j * n + j];
    for (int k = 0; k < j; ++k) {
      const double l = a[j * n + k];
      d -= l * l * a[k * n + k];
    }
    if (!(d > 0.0) || !std::isfinite(d)) return false;
    a[j * n + j] = d;
    for (int i = j + 1; i < n; ++i) {
      double s = a[i * n + j];
      for (int k = 0; k < j; ++k) s -= a[i * n + k] * a[j * n + k] * a[k * n + k];
      a[i * n + j] = s / d;
    }
  }
  return true;
}

void ldlt_solve_inplace(const double* a, int n, double* b) {
  for (int i = 0; i < n; ++i) {  // L y = b
    double s = b[i];
    for (int k = 0; k < i; ++k) s -= a[i * n + k] * b[k];
    b[i] = s;
  }
  for (int i = 0; i < n; ++i) b[i] /= a[i * n + i];  // D z = y
  for (int i = n - 1; i >= 0; --i) {                 // L^T x = z
    double s = b[i];
    for (int k = i + 1; k < n; ++k) s -= a[k * n + i] * b[k];
    b[i] = s;
  }
}

bool ldlt_inverse(const double* a, int n, double* inv, double* scratch) {
  if (n <= 0 || n > 64) return false;  // `col` below is a fixed 64-slot frame
  for (int i = 0; i < n * n; ++i) scratch[i] = a[i];
  if (!ldlt_factor(scratch, n)) return false;
  // One column of the inverse per unit basis vector. n=18 here, so the
  // O(n^3) cost is ~6k flops and runs a handful of times per scan.
  for (int c = 0; c < n; ++c) {
    double col[64];
    for (int i = 0; i < n; ++i) col[i] = (i == c) ? 1.0 : 0.0;
    ldlt_solve_inplace(scratch, n, col);
    for (int i = 0; i < n; ++i) inv[i * n + c] = col[i];
  }
  // Force exact symmetry: the inverse of a symmetric matrix is symmetric,
  // and letting rounding break that lets a covariance drift indefinite.
  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      const double v = 0.5 * (inv[i * n + j] + inv[j * n + i]);
      inv[i * n + j] = v;
      inv[j * n + i] = v;
    }
  }
  return true;
}

}  // namespace slam_detail
}  // namespace scanengine
