// test_lio.cpp — task A6: live lidar-inertial odometry.
//
// Five groups, in increasing order of how much they can lie to you:
//
//   liomath/*   the hand-rolled linear algebra, against closed forms.
//   ivox/*      the incremental voxel map: capacity, knn, determinism, trim.
//   liopose/*   LioPoseSource as a poses/PoseSource.
//   eskf/*      the filter alone, driven by synthesised IMU with no lidar.
//   lio/*       the whole pipeline against a synthetic room with a known
//               trajectory (ATE, drift, gravity, determinism, budget), and
//               against a REAL Mid-360 capture.
//
// The synthetic room is the only place an accuracy number can be checked
// against truth, because it is the only place truth exists. The real capture
// is the only place the wire format, the A4 clock mapping, the no-return
// fraction, the tag histogram and the actual scan pattern are exercised at
// once — and it has no ground truth, so it asserts on self-consistency
// (finite, non-divergent, gravity where gravity should be) and reports.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"

#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/slam/eskf.h"
#include "scanengine/slam/ivox.h"
#include "scanengine/slam/lio.h"
#include "scanengine/timesync/imu_ingest.h"
#include "scanengine/timesync/offset_estimator.h"

// The odometry's own math, unit-tested directly. It is internal to src/slam/
// (DESIGN §1), and a test is not a module.
#include "../src/slam/lio_math.h"

using namespace scanengine;
namespace sd = scanengine::slam_detail;

namespace {

// --- deterministic noise --------------------------------------------------
//
// xorshift64 and a Box-Muller pair. Deliberately NOT <random>: the standard
// does not specify the output of its distributions, so the five CI legs would
// disagree on every number in this file (the same reason test_timesync.cpp
// rolls its own).
struct Rng {
  std::uint64_t s = 0x9E3779B97F4A7C15ull;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double uniform() {  // (0, 1)
    return (static_cast<double>(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0);
  }
  double gauss() {
    const double u1 = uniform(), u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

// --- the synthetic room ---------------------------------------------------

struct Aabb {
  double lo[3];
  double hi[3];
};

// Ray/box, slab method. `exit_side` picks the far intersection (the inner
// surface of the room we are standing in) rather than the near one.
bool ray_box(const sd::Vec3& o, const sd::Vec3& d, const Aabb& b, bool exit_side, double* t_out) {
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

struct Room {
  Aabb shell{{-5.0, -4.0, 0.0}, {5.0, 4.0, 3.0}};
  std::vector<Aabb> pillars;

  Room() {
    pillars.push_back(Aabb{{-2.2, -0.6, 0.0}, {-1.8, 0.6, 3.0}});
    pillars.push_back(Aabb{{1.8, 0.8, 0.0}, {2.2, 2.0, 3.0}});
    pillars.push_back(Aabb{{-0.8, -3.2, 0.0}, {0.8, -2.6, 1.1}});  // a bench
  }

  // Distance to the first surface along `d` from `o`, or -1 for a miss.
  double cast(const sd::Vec3& o, const sd::Vec3& d) const {
    double best = -1.0;
    double t;
    if (ray_box(o, d, shell, true, &t)) best = t;
    for (const Aabb& p : pillars) {
      if (ray_box(o, d, p, false, &t) && t > 1e-6 && (best < 0.0 || t < best)) best = t;
    }
    return best;
  }
};

// --- the ground-truth trajectory -----------------------------------------
//
// Stationary for the first 0.6 s (the ESKF's static-init window), then a
// figure-eight with a gentle rise and a +-0.5 rad yaw sweep. The time warp
// u(t) is C^1 with u'(t_s) = 0, so both the accelerometer and the gyro see a
// continuous signal at the moment the scanner is picked up — a step there
// would be testing the filter's reaction to a physically impossible input.
struct Traj {
  static constexpr double kStill = 0.6;
  static constexpr double kRamp = 1.2;
  static constexpr double kOmega = 0.62831853071795862;  // 2*pi / 10 s
  static constexpr double kZ0 = 1.5;

  static double warp(double t) {
    const double e = t - kStill;
    if (e <= 0.0) return 0.0;
    if (e < kRamp) return e * e / (2.0 * kRamp);
    return e - kRamp * 0.5;
  }

  static void pose(double t, sd::Vec3* p, sd::Mat3* R) {
    const double u = warp(t) * kOmega;
    *p = sd::Vec3(1.6 * std::sin(u), 1.1 * std::sin(2.0 * u), kZ0 + 0.12 * (1.0 - std::cos(u)));
    const double yaw = 0.5 * std::sin(u);
    *R = sd::so3_exp(sd::Vec3(0, 0, yaw));
  }
};

struct ImuTruth {
  sd::Vec3 gyro;   // body, rad/s
  sd::Vec3 accel;  // body specific force, m/s^2
};

// Central differences on the analytic trajectory. h = 1e-4 s gives ~1e-8
// truncation error in double precision, which is four orders below the noise
// the samples are then corrupted with.
ImuTruth imu_truth(double t) {
  const double h = 1e-4;
  sd::Vec3 p0, p1, p2;
  sd::Mat3 R0, R1, R2;
  Traj::pose(t - h, &p0, &R0);
  Traj::pose(t, &p1, &R1);
  Traj::pose(t + h, &p2, &R2);
  const sd::Vec3 acc_w = (p2 - p1 * 2.0 + p0) * (1.0 / (h * h));
  const sd::Mat3 Rdot = (R2 - R0) * (1.0 / (2.0 * h));
  const sd::Mat3 W = sd::transpose(R1) * Rdot;
  ImuTruth out;
  out.gyro = sd::Vec3(W(2, 1), W(0, 2), W(1, 0));
  const sd::Vec3 g(0.0, 0.0, -9.80665);
  out.accel = sd::mul_transpose(R1, acc_w - g);
  return out;
}

// --- fixture discovery ----------------------------------------------------
//
// The real capture lives in the spike tree, and A6 may not add a CMake
// variable to point at it. __FILE__ is the source path the compiler was
// given, so walking up from it finds the repo without a build-system change;
// SCANENGINE_LIVOXDUMP overrides for an out-of-tree copy.
std::string find_fixture() {
  if (const char* env = std::getenv("SCANENGINE_LIVOXDUMP")) {
    if (env[0] != '\0') return env;
  }
  const char* rel = "spikes/s2-mid360-sim/fixtures/outdoor_imu_ccby_6s.livoxdump";
  std::string here = __FILE__;  // .../engine/tests/test_lio.cpp
  const std::size_t cut = here.rfind("engine/tests/");
  if (cut != std::string::npos) {
    const std::string root = here.substr(0, cut);
    std::string cand = root + rel;
    if (std::FILE* f = std::fopen(cand.c_str(), "rb")) {
      std::fclose(f);
      return cand;
    }
  }
  const char* ups[] = {"", "../", "../../", "../../../", "../../../../"};
  for (const char* up : ups) {
    std::string cand = std::string(up) + rel;
    if (std::FILE* f = std::fopen(cand.c_str(), "rb")) {
      std::fclose(f);
      return cand;
    }
  }
  return std::string();
}

// One record of the .livoxdump container documented in
// tools/remote-capture/capture_mid360.py.
struct DumpRecord {
  std::uint64_t t_ns = 0;
  std::uint16_t port = 0;
  std::vector<std::uint8_t> payload;
};

bool read_livoxdump(const std::string& path, std::vector<DumpRecord>* out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  auto rd = [&](void* dst, std::size_t n) { return std::fread(dst, 1, n, f) == n; };
  char magic[8];
  std::uint16_t version = 0, num_ports = 0;
  if (!rd(magic, 8) || std::memcmp(magic, "LX360CAP", 8) != 0) {
    std::fclose(f);
    return false;
  }
  if (!rd(&version, 2) || !rd(&num_ports, 2) || num_ports == 0 || num_ports > 16) {
    std::fclose(f);
    return false;
  }
  std::vector<std::uint32_t> ports(num_ports);
  if (!rd(ports.data(), 4u * num_ports)) {
    std::fclose(f);
    return false;
  }
  for (;;) {
    std::uint64_t t = 0;
    std::uint16_t idx = 0;
    std::uint32_t len = 0;
    if (!rd(&t, 8) || !rd(&idx, 2) || !rd(&len, 4)) break;  // truncated tail: stop, per the format
    if (idx >= num_ports || len > 65535) break;
    DumpRecord r;
    r.t_ns = t;
    r.port = static_cast<std::uint16_t>(ports[idx]);
    r.payload.resize(len);
    if (len != 0 && !rd(r.payload.data(), len)) break;
    out->push_back(std::move(r));
  }
  std::fclose(f);
  return true;
}

}  // namespace

// ===========================================================================
// liomath — the hand-rolled algebra
// ===========================================================================

TEST_CASE("liomath/so3_exp_log_roundtrip") {
  Rng rng(12345);
  double worst = 0.0;
  for (int i = 0; i < 400; ++i) {
    sd::Vec3 w(rng.gauss(), rng.gauss(), rng.gauss());
    const double scale = (i < 100) ? 1e-9 : (i < 200 ? 1e-4 : 1.0);
    w = w * scale;
    if (sd::norm(w) > 3.0) w = w * (3.0 / sd::norm(w));
    const sd::Mat3 R = sd::so3_exp(w);
    // Still on SO(3)?
    const sd::Mat3 I = sd::transpose(R) * R;
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) {
        CHECK(std::fabs(I(a, b) - (a == b ? 1.0 : 0.0)) < 1e-12);
      }
    }
    const sd::Vec3 back = sd::so3_log(R);
    worst = std::max(worst, sd::norm(back - w));
  }
  MESSAGE("so3 exp/log worst round-trip error: " << worst << " rad");
  CHECK(worst < 1e-9);
}

TEST_CASE("liomath/so3_log_near_pi") {
  // The branch that exists only for the theta -> pi degeneracy.
  for (int axis = 0; axis < 3; ++axis) {
    sd::Vec3 w;
    w[axis] = 3.14159265358979 - 1e-9;
    const sd::Vec3 back = sd::so3_log(sd::so3_exp(w));
    CHECK(std::fabs(sd::norm(back) - sd::norm(w)) < 1e-6);
    CHECK(std::fabs(std::fabs(sd::dot(back, w)) - sd::norm(back) * sd::norm(w)) < 1e-6);
  }
}

TEST_CASE("liomath/quaternion_roundtrip") {
  Rng rng(999);
  for (int i = 0; i < 200; ++i) {
    const sd::Vec3 w(rng.gauss(), rng.gauss(), rng.gauss());
    const sd::Mat3 R = sd::so3_exp(w);
    double q[4];
    sd::mat3_to_quat(R, q);
    const sd::Mat3 R2 = sd::quat_to_mat3(q);
    for (int k = 0; k < 9; ++k) CHECK(std::fabs(R.m[k] - R2.m[k]) < 1e-12);
  }
}

TEST_CASE("liomath/rotation_between_including_antiparallel") {
  const sd::Vec3 z(0, 0, 1);
  Rng rng(4242);
  for (int i = 0; i < 200; ++i) {
    sd::Vec3 a(rng.gauss(), rng.gauss(), rng.gauss());
    if (sd::norm(a) < 1e-6) continue;
    a = a * (1.0 / sd::norm(a));
    const sd::Mat3 R = sd::rotation_between(a, z);
    const sd::Vec3 got = R * a;
    CHECK(sd::norm(got - z) < 1e-12);
  }
  // Exactly anti-parallel: the degenerate branch.
  const sd::Mat3 R = sd::rotation_between(sd::Vec3(0, 0, -1), z);
  CHECK(sd::norm(R * sd::Vec3(0, 0, -1) - z) < 1e-9);
}

TEST_CASE("liomath/ldlt_solve_and_inverse") {
  Rng rng(7);
  const int n = 18;
  std::vector<double> A(n * n, 0.0), M(n * n, 0.0);
  // A = M^T M + 0.5 I: symmetric positive definite by construction.
  for (int i = 0; i < n * n; ++i) M[i] = rng.gauss();
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      double s = (i == j) ? 0.5 : 0.0;
      for (int k = 0; k < n; ++k) s += M[k * n + i] * M[k * n + j];
      A[i * n + j] = s;
    }
  }
  std::vector<double> x_true(n), b(n, 0.0);
  for (int i = 0; i < n; ++i) x_true[i] = rng.gauss();
  for (int i = 0; i < n; ++i) {
    for (int k = 0; k < n; ++k) b[i] += A[i * n + k] * x_true[k];
  }

  std::vector<double> fact = A, x = b;
  REQUIRE(sd::ldlt_factor(fact.data(), n));
  sd::ldlt_solve_inplace(fact.data(), n, x.data());
  double worst = 0.0;
  for (int i = 0; i < n; ++i) worst = std::max(worst, std::fabs(x[i] - x_true[i]));
  MESSAGE("ldlt solve worst component error: " << worst);
  CHECK(worst < 1e-8);

  std::vector<double> inv(n * n), scratch(n * n);
  REQUIRE(sd::ldlt_inverse(A.data(), n, inv.data(), scratch.data()));
  double off = 0.0;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      double s = 0.0;
      for (int k = 0; k < n; ++k) s += A[i * n + k] * inv[k * n + j];
      off = std::max(off, std::fabs(s - (i == j ? 1.0 : 0.0)));
    }
  }
  MESSAGE("ldlt inverse worst |A*Ainv - I|: " << off);
  CHECK(off < 1e-8);
  // Symmetry is enforced, not merely likely.
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) CHECK(inv[i * n + j] == inv[j * n + i]);
  }

  // A singular matrix must be reported, not silently "solved".
  std::vector<double> sing(n * n, 0.0);
  CHECK_FALSE(sd::ldlt_factor(sing.data(), n));
}

TEST_CASE("liomath/plane_fit_normal_and_planarity") {
  // Five points on the plane z = 0.25x + 0.5, plus 1 cm of noise.
  const sd::Vec3 truth_n = sd::Vec3(-0.25, 0.0, 1.0) * (1.0 / std::sqrt(1.0625));
  Rng rng(31337);
  sd::Vec3 pts[5];
  sd::Vec3 c;
  for (int i = 0; i < 5; ++i) {
    const double x = -0.2 + 0.1 * i, y = 0.15 * ((i % 3) - 1);
    pts[i] = sd::Vec3(x, y, 0.25 * x + 0.5 + 0.01 * rng.gauss());
    c += pts[i];
  }
  c = c * 0.2;
  sd::Mat3 C = sd::Mat3::zero();
  for (int i = 0; i < 5; ++i) {
    const sd::Vec3 d = pts[i] - c;
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) C(a, b) += d[a] * d[b];
    }
  }
  sd::Vec3 n;
  double eig[3];
  REQUIRE(sd::sym3_smallest_eigenvector(C, &n, eig));
  const double align = std::fabs(sd::dot(n, truth_n));
  MESSAGE("plane fit |n . n_true| = " << align << ", eig = " << eig[0] << " " << eig[1] << " "
                                      << eig[2]);
  // 5 points spanning 0.4 m with 1 cm of noise place the normal to ~7 deg —
  // which is the honest precision of a single 5-point patch, and the reason
  // the update pools thousands of them rather than trusting any one.
  CHECK(align > 0.98);
  CHECK(eig[0] <= eig[1]);
  CHECK(eig[1] <= eig[2]);

  // Same geometry at 1 mm noise: the estimator itself is unbiased.
  {
    Rng r2(31337);
    sd::Vec3 q[5];
    sd::Vec3 qc;
    for (int i = 0; i < 5; ++i) {
      const double x = -0.2 + 0.1 * i, y = 0.15 * ((i % 3) - 1);
      q[i] = sd::Vec3(x, y, 0.25 * x + 0.5 + 0.001 * r2.gauss());
      qc += q[i];
    }
    qc = qc * 0.2;
    sd::Mat3 Q = sd::Mat3::zero();
    for (int i = 0; i < 5; ++i) {
      const sd::Vec3 d = q[i] - qc;
      for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) Q(a, b) += d[a] * d[b];
      }
    }
    sd::Vec3 qn;
    double qe[3];
    REQUIRE(sd::sym3_smallest_eigenvector(Q, &qn, qe));
    MESSAGE("plane fit at 1 mm noise: |n . n_true| = " << std::fabs(sd::dot(qn, truth_n)));
    CHECK(std::fabs(sd::dot(qn, truth_n)) > 0.999);
  }

  // Collinear points: the planarity ratio must NOT look planar.
  sd::Mat3 L = sd::Mat3::zero();
  sd::Vec3 lc;
  sd::Vec3 lp[5];
  for (int i = 0; i < 5; ++i) {
    lp[i] = sd::Vec3(0.1 * i, 0.2 * i, 0.05 * i);
    lc += lp[i];
  }
  lc = lc * 0.2;
  for (int i = 0; i < 5; ++i) {
    const sd::Vec3 d = lp[i] - lc;
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) L(a, b) += d[a] * d[b];
    }
  }
  sd::Vec3 ln;
  double leig[3];
  if (sd::sym3_smallest_eigenvector(L, &ln, leig)) {
    CHECK(leig[1] < 1e-9 * leig[2] + 1e-12);  // no second in-plane direction
  }
}

// ===========================================================================
// ivox — the incremental voxel map
// ===========================================================================

TEST_CASE("ivox/insert_capacity_and_voxel_cap") {
  IVoxConfig cfg;
  cfg.voxel_size_m = 0.5;
  cfg.max_points_per_voxel = 4;
  cfg.max_voxels = 3;
  IVox m(cfg);

  // Five points in one voxel: four stored, the fifth refused.
  int stored = 0;
  for (int i = 0; i < 5; ++i) stored += m.insert(0.1 + 0.01 * i, 0.1, 0.1) ? 1 : 0;
  CHECK(stored == 4);
  CHECK(m.point_count() == 4);
  CHECK(m.voxel_count() == 1);

  CHECK(m.insert(10.0, 0.0, 0.0));
  CHECK(m.insert(20.0, 0.0, 0.0));
  CHECK(m.voxel_count() == 3);
  CHECK_FALSE(m.insert(30.0, 0.0, 0.0));  // max_voxels
  CHECK(m.voxel_count() == 3);

  CHECK_FALSE(m.insert(std::nan(""), 0.0, 0.0));
}

TEST_CASE("ivox/knn_is_correct_and_ordered") {
  IVoxConfig cfg;
  cfg.voxel_size_m = 1.0;
  cfg.max_points_per_voxel = 32;
  IVox m(cfg);
  Rng rng(2024);
  std::vector<sd::Vec3> all;
  for (int i = 0; i < 3000; ++i) {
    sd::Vec3 p(rng.uniform() * 4.0 - 2.0, rng.uniform() * 4.0 - 2.0, rng.uniform() * 4.0 - 2.0);
    // Store what the map stores: it keeps float triples, so the brute-force
    // reference must be rounded the same way or the distances differ at 1e-7.
    const sd::Vec3 pf(static_cast<double>(static_cast<float>(p[0])),
                      static_cast<double>(static_cast<float>(p[1])),
                      static_cast<double>(static_cast<float>(p[2])));
    if (m.insert(p[0], p[1], p[2])) all.push_back(pf);
  }
  REQUIRE(all.size() > 500);

  const double q[3] = {0.13, -0.27, 0.41};
  double out[IVox::kMaxK * 3];
  const std::size_t got = m.knn(q, 5, 1.0, out);
  REQUIRE(got == 5);

  // Brute force over everything the map actually stored.
  std::vector<double> d2;
  for (const sd::Vec3& p : all) {
    const double dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    const double r2 = dx * dx + dy * dy + dz * dz;
    if (r2 <= 1.0) d2.push_back(r2);
  }
  std::sort(d2.begin(), d2.end());
  for (std::size_t i = 0; i < got; ++i) {
    const double dx = out[i * 3 + 0] - q[0], dy = out[i * 3 + 1] - q[1], dz = out[i * 3 + 2] - q[2];
    const double r2 = dx * dx + dy * dy + dz * dz;
    CHECK(std::fabs(r2 - d2[i]) < 1e-9);  // same distance as the i-th true nearest
    if (i > 0) {
      const double px = out[(i - 1) * 3 + 0] - q[0], py = out[(i - 1) * 3 + 1] - q[1],
                   pz = out[(i - 1) * 3 + 2] - q[2];
      CHECK(r2 >= px * px + py * py + pz * pz - 1e-15);  // ordered
    }
  }

  // Nothing within a tiny radius.
  const double far_q[3] = {100.0, 100.0, 100.0};
  CHECK(m.knn(far_q, 5, 1.0, out) == 0);
  CHECK(m.knn(q, IVox::kMaxK + 1, 1.0, out) == 0);  // k above the ceiling
}

TEST_CASE("ivox/knn_is_deterministic") {
  IVoxConfig cfg;
  IVox a(cfg), b(cfg);
  Rng r1(88), r2(88);
  for (int i = 0; i < 5000; ++i) {
    const double x = r1.uniform() * 6.0, y = r1.uniform() * 6.0, z = r1.uniform() * 6.0;
    const double x2 = r2.uniform() * 6.0, y2 = r2.uniform() * 6.0, z2 = r2.uniform() * 6.0;
    a.insert(x, y, z);
    b.insert(x2, y2, z2);
  }
  REQUIRE(a.point_count() == b.point_count());
  Rng rq(5);
  for (int i = 0; i < 400; ++i) {
    const double q[3] = {rq.uniform() * 6.0, rq.uniform() * 6.0, rq.uniform() * 6.0};
    double oa[IVox::kMaxK * 3], ob[IVox::kMaxK * 3];
    const std::size_t na = a.knn(q, 5, 0.7, oa);
    const std::size_t nb = b.knn(q, 5, 0.7, ob);
    REQUIRE(na == nb);
    for (std::size_t k = 0; k < na * 3; ++k) CHECK(oa[k] == ob[k]);  // bit-identical
  }
}

TEST_CASE("ivox/trim_drops_distant_voxels") {
  IVoxConfig cfg;
  cfg.voxel_size_m = 1.0;
  IVox m(cfg);
  for (int i = 0; i < 100; ++i) m.insert(static_cast<double>(i), 0.0, 0.0);
  CHECK(m.voxel_count() == 100);
  const double c[3] = {0.0, 0.0, 0.0};
  const std::size_t dropped = m.trim(c, 10.0);
  CHECK(dropped > 80);
  CHECK(m.voxel_count() == 100 - dropped);
  CHECK(m.point_count() == m.voxel_count());
}

// ===========================================================================
// liopose — LioPoseSource as a PoseSource
// ===========================================================================

TEST_CASE("liopose/interpolates_and_reports_range") {
  LioPoseSource ps(8);
  Pose out;
  CHECK(ps.pose_at(0, &out).error() == ScanError::kNotFound);

  for (int i = 0; i < 5; ++i) {
    Pose p;
    p.t_mono_ns = i * 100'000'000LL;
    p.position[0] = i * 2.0;
    const sd::Mat3 R = sd::so3_exp(sd::Vec3(0, 0, 0.2 * i));
    sd::mat3_to_quat(R, p.orientation);
    p.quality = PoseQuality::kGood;
    CHECK(ps.push_pose(p).ok());
  }
  CHECK(ps.size() == 5);

  CHECK(ps.pose_at(-1, &out).error() == ScanError::kNotFound);
  CHECK(ps.pose_at(500'000'000LL, &out).error() == ScanError::kAgain);

  REQUIRE(ps.pose_at(150'000'000LL, &out).ok());
  CHECK(std::fabs(out.position[0] - 3.0) < 1e-9);
  const sd::Vec3 ang = sd::so3_log(sd::quat_to_mat3(out.orientation));
  CHECK(std::fabs(ang[2] - 0.3) < 1e-6);

  REQUIRE(ps.pose_at(200'000'000LL, &out).ok());
  CHECK(std::fabs(out.position[0] - 4.0) < 1e-12);  // exactly on a knot

  // Trajectory length survives the ring wrapping.
  CHECK(std::fabs(ps.trajectory_length_m() - 8.0) < 1e-9);
  for (int i = 5; i < 12; ++i) {
    Pose p;
    p.t_mono_ns = i * 100'000'000LL;
    p.position[0] = i * 2.0;
    CHECK(ps.push_pose(p).ok());
  }
  CHECK(ps.size() == 8);
  CHECK(std::fabs(ps.trajectory_length_m() - 22.0) < 1e-9);

  Pose old;
  old.t_mono_ns = 0;
  CHECK(ps.push_pose(old).error() == ScanError::kInvalidArgument);
}

TEST_CASE("liopose/callback_fires_on_the_pushing_thread") {
  LioPoseSource ps;
  int hits = 0;
  ps.set_callback([&hits](const Pose&) { ++hits; });
  Pose p;
  p.t_mono_ns = 1;
  CHECK(ps.push_pose(p).ok());
  CHECK(hits == 1);
  CHECK(ps.stream() == StreamId::kLidarMid360);
  CHECK(std::string(ps.name()) == "lio");
}

// ===========================================================================
// eskf — the filter alone
// ===========================================================================

TEST_CASE("eskf/static_init_aligns_gravity_to_z") {
  // Scanner tilted 20 deg about x and 10 deg about y while at rest.
  const sd::Mat3 R_true = sd::so3_exp(sd::Vec3(0.349, 0.175, 0.0));
  const sd::Vec3 g_w(0, 0, -9.80665);
  const sd::Vec3 f_body = sd::mul_transpose(R_true, -g_w);

  Eskf f;
  const double gyro[3] = {0.001, -0.002, 0.0005};
  const double acc[3] = {f_body[0], f_body[1], f_body[2]};
  REQUIRE(f.init_from_static(0, gyro, acc).ok());
  CHECK(f.initialized());

  // The estimated world frame must put gravity on -Z.
  const sd::Mat3 R_est = sd::quat_to_mat3(f.state().q);
  const sd::Vec3 up = R_est * sd::Vec3(acc[0], acc[1], acc[2]);
  CHECK(std::fabs(up[0]) < 1e-9);
  CHECK(std::fabs(up[1]) < 1e-9);
  CHECK(up[2] > 9.7);
  CHECK(std::fabs(f.state().bg[0] - 0.001) < 1e-12);

  // Degenerate input is refused rather than producing a nonsense frame.
  Eskf f2;
  const double zero[3] = {0, 0, 0};
  CHECK(f2.init_from_static(0, gyro, zero).error() == ScanError::kInvalidArgument);
}

TEST_CASE("eskf/static_imu_produces_bounded_drift") {
  Eskf f;
  const double gyro0[3] = {0, 0, 0};
  const double acc0[3] = {0, 0, 9.80665};
  REQUIRE(f.init_from_static(0, gyro0, acc0).ok());
  // 10 s of perfectly still IMU: dead reckoning has nothing to integrate, so
  // position must stay at the origin to within numerical noise.
  for (int i = 1; i <= 2000; ++i) {
    const Status s = f.propagate(i * 5'000'000LL, gyro0, acc0);
    CHECK(s.ok());
  }
  const EskfState& x = f.state();
  MESSAGE("static 10 s drift: " << std::sqrt(x.p[0] * x.p[0] + x.p[1] * x.p[1] + x.p[2] * x.p[2])
                                << " m");
  CHECK(std::fabs(x.p[0]) < 1e-9);
  CHECK(std::fabs(x.p[1]) < 1e-9);
  CHECK(std::fabs(x.p[2]) < 1e-9);
  CHECK_FALSE(f.diverged());
  CHECK(f.cov()[0] >= 0.0);
}

TEST_CASE("eskf/propagation_tracks_a_known_trajectory") {
  // Pure dead reckoning over the synthetic trajectory with noiseless IMU:
  // this is the propagation model checked against the same analytic motion
  // the full pipeline is later asked to recover.
  Eskf f;
  EskfConfig cfg;
  EskfState s0;
  s0.t_ns = 0;
  s0.p[2] = Traj::kZ0;
  s0.g[2] = -9.80665;
  f.init(s0);

  const double dt = 1.0 / 200.0;
  double worst = 0.0;
  for (int i = 1; i <= 400; ++i) {  // 2 s
    const double t = i * dt;
    const ImuTruth m = imu_truth(t);
    const double g[3] = {m.gyro[0], m.gyro[1], m.gyro[2]};
    const double a[3] = {m.accel[0], m.accel[1], m.accel[2]};
    CHECK(f.propagate(static_cast<std::int64_t>(t * 1e9), g, a).ok());
    sd::Vec3 pt;
    sd::Mat3 Rt;
    Traj::pose(t, &pt, &Rt);
    const sd::Vec3 err = sd::Vec3(f.state().p) - pt;
    worst = std::max(worst, sd::norm(err));
  }
  MESSAGE("open-loop IMU integration error over 2 s: " << worst << " m");
  CHECK(worst < 0.02);  // Euler integration at 200 Hz, no lidar at all
  CHECK_FALSE(f.diverged());
}

TEST_CASE("eskf/boxplus_boxminus_are_inverses") {
  Eskf f;
  EskfState s0;
  s0.t_ns = 0;
  const sd::Mat3 R0 = sd::so3_exp(sd::Vec3(0.2, -0.3, 0.1));
  sd::mat3_to_quat(R0, s0.q);
  s0.p[0] = 1.0;
  s0.v[1] = -0.5;
  f.init(s0);

  double dx[kEskfDim];
  Rng rng(5150);
  for (int i = 0; i < kEskfDim; ++i) dx[i] = 0.05 * rng.gauss();
  f.boxplus(dx);
  double back[kEskfDim];
  f.boxminus(s0, back);
  for (int i = 0; i < kEskfDim; ++i) CHECK(std::fabs(back[i] - dx[i]) < 1e-9);
}

TEST_CASE("eskf/long_gap_is_reported_not_integrated") {
  Eskf f;
  const double gyro0[3] = {0, 0, 0};
  const double acc0[3] = {0, 0, 9.80665};
  REQUIRE(f.init_from_static(0, gyro0, acc0).ok());
  const double acc1[3] = {5.0, 0, 9.80665};
  CHECK(f.propagate(2'000'000'000LL, gyro0, acc1).error() == ScanError::kTimeout);
  CHECK(std::fabs(f.state().p[0]) < 1e-12);  // nothing was invented
  CHECK(f.state().t_ns == 2'000'000'000LL);  // but the clock re-anchored
  CHECK(f.propagate(1'000'000'000LL, gyro0, acc0).error() == ScanError::kAgain);
}

// ===========================================================================
// lio — the whole pipeline
// ===========================================================================

namespace {

struct SynthResult {
  double ate_rms_m = 0.0;
  double ate_max_m = 0.0;
  double final_drift_m = 0.0;
  double rot_rms_deg = 0.0;
  double gravity = 0.0;
  double gravity_tilt_deg = 0.0;
  LioStats stats{};
  std::size_t map_pages = 0;
  std::uint64_t map_store_points = 0;
};

// Drive `lio` with a synthetic Mid-360: 96-point datagrams every 480 us
// (2,083/s = 200k pts/s, exactly the rate A3 measured), 200 Hz IMU, over
// `seconds` of the analytic trajectory.
SynthResult run_synthetic(LioOdometry& lio, double seconds, std::uint64_t seed,
                          double range_sigma_m, double gyro_sigma, double accel_sigma,
                          const double gyro_bias[3], const double accel_bias[3]) {
  const Room room;
  Rng rng(seed);

  const std::int64_t t0 = 1'000'000'000LL;  // arbitrary non-zero engine epoch
  const double imu_dt = 1.0 / 200.0;
  const double pkt_dt = 480e-6;
  const int pts_per_pkt = 96;

  std::vector<PointVertex> batch;
  batch.reserve(pts_per_pkt);

  std::size_t point_index = 0;
  double next_imu = 0.0;
  double next_pkt = 0.0;
  const double golden_a = 0.7548776662466927;  // plastic-number low-discrepancy pair
  const double golden_b = 0.5698402909980532;

  const double el_min = -7.0 * 3.14159265358979 / 180.0;
  const double el_max = 52.0 * 3.14159265358979 / 180.0;

  while (next_imu < seconds || next_pkt < seconds) {
    if (next_imu <= next_pkt) {
      const ImuTruth m = imu_truth(next_imu);
      const float gy[3] = {static_cast<float>(m.gyro[0] + gyro_bias[0] + gyro_sigma * rng.gauss()),
                           static_cast<float>(m.gyro[1] + gyro_bias[1] + gyro_sigma * rng.gauss()),
                           static_cast<float>(m.gyro[2] + gyro_bias[2] + gyro_sigma * rng.gauss())};
      const float ac[3] = {
          static_cast<float>(m.accel[0] + accel_bias[0] + accel_sigma * rng.gauss()),
          static_cast<float>(m.accel[1] + accel_bias[1] + accel_sigma * rng.gauss()),
          static_cast<float>(m.accel[2] + accel_bias[2] + accel_sigma * rng.gauss())};
      const Status s = lio.push_imu(t0 + static_cast<std::int64_t>(next_imu * 1e9), gy, ac);
      REQUIRE(s.ok());
      next_imu += imu_dt;
    } else {
      sd::Vec3 p;
      sd::Mat3 R;
      Traj::pose(next_pkt, &p, &R);
      batch.clear();
      for (int i = 0; i < pts_per_pkt; ++i) {
        const double f1 = static_cast<double>(point_index) * golden_a;
        const double f2 = static_cast<double>(point_index) * golden_b;
        const double u1 = f1 - std::floor(f1);
        const double u2 = f2 - std::floor(f2);
        ++point_index;
        const double az = 6.283185307179586 * u1;
        const double el = el_min + (el_max - el_min) * u2;
        const sd::Vec3 d_body(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az),
                              std::sin(el));
        const sd::Vec3 d_world = R * d_body;
        const double rng_m = room.cast(p, d_world);
        if (rng_m < 0.2) continue;
        const double r = rng_m + range_sigma_m * rng.gauss();
        PointVertex pv;
        pv.x = static_cast<float>(d_body[0] * r);
        pv.y = static_cast<float>(d_body[1] * r);
        pv.z = static_cast<float>(d_body[2] * r);
        const int refl = 40 + static_cast<int>(180.0 * u2);
        pv.r = pv.g = pv.b = static_cast<std::uint8_t>(refl);
        pv.a = 255;
        batch.push_back(pv);
      }
      if (!batch.empty()) {
        const Status s = lio.push_points(
            Span<const PointVertex>(batch.data(), batch.size()),
            t0 + static_cast<std::int64_t>(next_pkt * 1e9));
        REQUIRE(s.ok());
      }
      next_pkt += pkt_dt;
    }
  }
  REQUIRE(lio.flush().ok());

  // --- score against truth ------------------------------------------------
  SynthResult out;
  out.stats = lio.stats();
  double sum2 = 0.0, rot2 = 0.0;
  std::size_t n = 0;
  sd::Vec3 p_ref;
  sd::Mat3 R_ref;
  Traj::pose(0.0, &p_ref, &R_ref);

  const LioPoseSource& ps = lio.poses();
  for (double t = Traj::kStill; t < seconds; t += 0.1) {
    Pose got;
    const Status s = ps.pose_at(t0 + static_cast<std::int64_t>(t * 1e9), &got);
    if (!s.ok()) continue;
    sd::Vec3 pt;
    sd::Mat3 Rt;
    Traj::pose(t, &pt, &Rt);
    // The LIO frame's origin is its first pose; ground truth's is the room.
    const sd::Vec3 truth = pt - p_ref;
    const sd::Vec3 est(got.position);
    const double e = sd::norm(est - truth);
    sum2 += e * e;
    ++n;
    out.ate_max_m = std::max(out.ate_max_m, e);
    out.final_drift_m = e;
    const sd::Mat3 R_est = sd::quat_to_mat3(got.orientation);
    const double ang = sd::norm(sd::so3_log(sd::transpose(Rt) * R_est));
    rot2 += ang * ang;
  }
  if (n > 0) {
    out.ate_rms_m = std::sqrt(sum2 / static_cast<double>(n));
    out.rot_rms_deg = std::sqrt(rot2 / static_cast<double>(n)) * 57.29577951308232;
  }
  const EskfState& x = lio.filter().state();
  out.gravity = std::sqrt(x.g[0] * x.g[0] + x.g[1] * x.g[1] + x.g[2] * x.g[2]);
  if (out.gravity > 0.0) {
    double c = -x.g[2] / out.gravity;
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    out.gravity_tilt_deg = std::acos(c) * 57.29577951308232;
  }
  out.map_pages = lio.map_store().page_count();
  out.map_store_points = lio.map_store().total_points();
  return out;
}

LioConfig synthetic_config() {
  LioConfig cfg;
  cfg.scan_period_s = 0.1;
  cfg.live_points_per_sec = 40000;
  cfg.min_range_m = 0.3f;
  cfg.max_range_m = 30.0f;
  cfg.map.voxel_size_m = 0.4;
  cfg.map.max_points_per_voxel = 20;
  cfg.max_correspondence_m = 0.6;
  cfg.init_imu_samples = 100;
  cfg.eskf.gyro_noise_rad_s = 0.01;
  cfg.eskf.accel_noise_m_s2 = 0.1;
  return cfg;
}

}  // namespace

TEST_CASE("lio/synthetic_room_trajectory") {
  LioOdometry lio(synthetic_config());
  REQUIRE(lio.start().ok());

  const double gbias[3] = {0.004, -0.003, 0.002};
  const double abias[3] = {0.04, -0.03, 0.05};
  const SynthResult r = run_synthetic(lio, 8.0, 20260815ull, 0.01, 0.002, 0.02, gbias, abias);

  MESSAGE("synthetic: ATE rms " << r.ate_rms_m << " m, max " << r.ate_max_m << " m, final "
                                << r.final_drift_m << " m, rot rms " << r.rot_rms_deg << " deg");
  MESSAGE("synthetic: |g| " << r.gravity << " m/s^2, tilt " << r.gravity_tilt_deg << " deg");
  MESSAGE("synthetic: scans " << r.stats.scans << " (skipped " << r.stats.scans_skipped
                              << "), points in " << r.stats.points_in << " kept "
                              << r.stats.points_kept << " mapped " << r.stats.points_mapped);
  MESSAGE("synthetic: residual rms " << (r.stats.residual_rms_m * 100.0) << " cm");
  MESSAGE("synthetic: per-scan ms mean " << r.stats.scan_ms_mean << " p50 " << r.stats.scan_ms_p50
                                         << " p95 " << r.stats.scan_ms_p95 << " max "
                                         << r.stats.scan_ms_max);
  MESSAGE("synthetic: map " << r.stats.map_voxels << " voxels / " << r.stats.map_points
                            << " points; store " << r.map_store_points << " points in "
                            << r.map_pages << " pages");
  const double scans_d = static_cast<double>(r.stats.scans);
  MESSAGE("synthetic: mean residuals/scan "
          << static_cast<double>(r.stats.residuals_total) / scans_d << ", mean iterations/scan "
          << static_cast<double>(r.stats.iterations_total) / scans_d << ", budget used "
          << r.stats.cpu_budget_used);

  CHECK(r.stats.initialized);
  CHECK_FALSE(r.stats.diverged);
  CHECK(r.stats.scans >= 70);
  // The decimation budget: 40k pts/s of 200k, +-15% (integer strides cannot
  // hit an arbitrary ratio exactly).
  const double kept_rate = static_cast<double>(r.stats.points_kept) / 8.0;
  MESSAGE("synthetic: decimated input rate " << kept_rate << " pts/s");
  CHECK(kept_rate > 30000.0);
  CHECK(kept_rate < 48000.0);

  CHECK(r.ate_rms_m < 0.15);
  CHECK(r.ate_max_m < 0.30);
  CHECK(r.rot_rms_deg < 2.0);
  CHECK(r.gravity > 9.5);
  CHECK(r.gravity < 10.1);
  CHECK(r.gravity_tilt_deg < 3.0);
  CHECK(r.map_store_points > 10000);
  CHECK(r.stats.residual_rms_m < 0.05);
  CHECK(r.stats.store_appends_failed == 0);
  CHECK(lio.stop().ok());
}

TEST_CASE("lio/the_lidar_is_what_removes_the_drift") {
  // The control for the accuracy claim above. Identical input, identical
  // filter, identical IMU — but min_correspondences set past anything a scan
  // can supply, so the update never fires and the same code path becomes
  // pure IMU dead reckoning. If the lidar were not doing the work, the two
  // ATE numbers would be the same.
  const double gbias[3] = {0.004, -0.003, 0.002};
  const double abias[3] = {0.04, -0.03, 0.05};

  LioConfig blind = synthetic_config();
  blind.min_correspondences = 100000000u;
  LioOdometry no_lidar(blind);
  REQUIRE(no_lidar.start().ok());
  const SynthResult rb = run_synthetic(no_lidar, 8.0, 20260815ull, 0.01, 0.002, 0.02, gbias, abias);

  LioOdometry with_lidar(synthetic_config());
  REQUIRE(with_lidar.start().ok());
  const SynthResult rw =
      run_synthetic(with_lidar, 8.0, 20260815ull, 0.01, 0.002, 0.02, gbias, abias);

  MESSAGE("dead-reckoning-only ATE rms " << rb.ate_rms_m << " m (final " << rb.final_drift_m
                                         << " m) vs LIO " << rw.ate_rms_m << " m (final "
                                         << rw.final_drift_m << " m)");
  CHECK(rb.stats.scans_skipped == rb.stats.scans);  // the update really never ran
  CHECK(rb.ate_rms_m > 10.0 * rw.ate_rms_m);
  CHECK(no_lidar.stop().ok());
  CHECK(with_lidar.stop().ok());
}

TEST_CASE("lio/is_deterministic") {
  const LioConfig cfg = synthetic_config();
  const double gbias[3] = {0.004, -0.003, 0.002};
  const double abias[3] = {0.04, -0.03, 0.05};

  LioOdometry a(cfg), b(cfg);
  REQUIRE(a.start().ok());
  REQUIRE(b.start().ok());
  const SynthResult ra = run_synthetic(a, 3.0, 777ull, 0.01, 0.002, 0.02, gbias, abias);
  const SynthResult rb = run_synthetic(b, 3.0, 777ull, 0.01, 0.002, 0.02, gbias, abias);

  Pose pa, pb;
  REQUIRE(a.current_pose(&pa));
  REQUIRE(b.current_pose(&pb));
  for (int i = 0; i < 3; ++i) CHECK(pa.position[i] == pb.position[i]);
  for (int i = 0; i < 4; ++i) CHECK(pa.orientation[i] == pb.orientation[i]);
  CHECK(ra.stats.points_kept == rb.stats.points_kept);
  CHECK(ra.stats.points_mapped == rb.stats.points_mapped);
  CHECK(ra.stats.map_points == rb.stats.map_points);
  CHECK(ra.stats.map_voxels == rb.stats.map_voxels);
  CHECK(ra.map_store_points == rb.map_store_points);
  CHECK(a.stop().ok());
  CHECK(b.stop().ok());
}

TEST_CASE("lio/internal_thread_matches_inline") {
  // The odometry thread must not change the answer — only who computes it.
  LioConfig cfg = synthetic_config();
  const double gbias[3] = {0.004, -0.003, 0.002};
  const double abias[3] = {0.04, -0.03, 0.05};

  LioOdometry inline_lio(cfg);
  REQUIRE(inline_lio.start().ok());
  const SynthResult ri = run_synthetic(inline_lio, 3.0, 31415ull, 0.01, 0.002, 0.02, gbias, abias);

  cfg.internal_thread = true;
  LioOdometry threaded(cfg);
  REQUIRE(threaded.start().ok());
  const SynthResult rt = run_synthetic(threaded, 3.0, 31415ull, 0.01, 0.002, 0.02, gbias, abias);

  Pose pi, pt;
  REQUIRE(inline_lio.current_pose(&pi));
  REQUIRE(threaded.current_pose(&pt));
  MESSAGE("thread vs inline final position delta: "
          << sd::norm(sd::Vec3(pi.position) - sd::Vec3(pt.position)) << " m");
  for (int i = 0; i < 3; ++i) CHECK(pi.position[i] == pt.position[i]);
  CHECK(ri.stats.map_points == rt.stats.map_points);
  CHECK(inline_lio.stop().ok());
  CHECK(threaded.stop().ok());
}

TEST_CASE("lio/external_page_store_receives_the_map") {
  PageStore store;
  LioConfig cfg = synthetic_config();
  cfg.map_store = &store;
  LioOdometry lio(cfg);
  REQUIRE(lio.start().ok());
  const double zero[3] = {0, 0, 0};
  const SynthResult r = run_synthetic(lio, 2.5, 6060ull, 0.01, 0.002, 0.02, zero, zero);
  (void)r;
  CHECK(store.total_points() > 5000);
  REQUIRE(store.page_count() >= 1);
  const PageView v = store.page_view(store.page_ids().front());
  REQUIRE(v.valid());
  CHECK(v.stream == cfg.map_stream);
  // The published cloud is the room, in world coordinates, and the room is
  // 10 x 8 x 3 m centred on the origin with the scanner starting at z = 1.5.
  CHECK(v.bounds_min[0] > -6.0);
  CHECK(v.bounds_max[0] < 6.0);
  CHECK(v.bounds_min[2] > -2.5);
  CHECK(v.bounds_max[2] < 2.5);
  for (std::uint32_t i = 0; i < v.count; ++i) {
    REQUIRE(std::isfinite(v.data[i].x));
    REQUIRE(std::isfinite(v.data[i].y));
    REQUIRE(std::isfinite(v.data[i].z));
  }
  CHECK(lio.stop().ok());
}

TEST_CASE("lio/survives_a_lidar_dropout") {
  // IMU keeps coming, points stop. The odometry must dead-reckon rather than
  // stall, and must recover when the lidar returns.
  LioConfig cfg = synthetic_config();
  LioOdometry lio(cfg);
  REQUIRE(lio.start().ok());
  const double zero[3] = {0, 0, 0};
  const SynthResult r1 = run_synthetic(lio, 2.0, 4321ull, 0.01, 0.002, 0.02, zero, zero);
  const std::uint64_t scans_before = r1.stats.scans;

  // 1 s of IMU only, continuing the same trajectory.
  const std::int64_t t0 = 1'000'000'000LL;
  for (int i = 400; i < 600; ++i) {
    const double t = i / 200.0;
    const ImuTruth m = imu_truth(t);
    const float gy[3] = {static_cast<float>(m.gyro[0]), static_cast<float>(m.gyro[1]),
                         static_cast<float>(m.gyro[2])};
    const float ac[3] = {static_cast<float>(m.accel[0]), static_cast<float>(m.accel[1]),
                         static_cast<float>(m.accel[2])};
    REQUIRE(lio.push_imu(t0 + static_cast<std::int64_t>(t * 1e9), gy, ac).ok());
  }
  REQUIRE(lio.flush().ok());
  const LioStats after = lio.stats();
  MESSAGE("dropout: scans " << scans_before << " -> " << after.scans << ", skipped "
                            << after.scans_skipped);
  CHECK(after.scans > scans_before);      // dead reckoning kept producing poses
  CHECK(after.scans_skipped >= 1);        // and honestly reported them as un-updated
  CHECK_FALSE(after.diverged);
  Pose p;
  REQUIRE(lio.current_pose(&p));
  CHECK(p.quality == PoseQuality::kFair);  // not kGood: no lidar backed it
  CHECK(lio.stop().ok());
}

// ===========================================================================
// lio/real — a genuine Mid-360 capture, end to end
// ===========================================================================

TEST_CASE("lio/real_mid360_capture") {
  const std::string path = find_fixture();
  if (path.empty()) {
    MESSAGE("SKIPPED: outdoor_imu_ccby_6s.livoxdump not found (set SCANENGINE_LIVOXDUMP)");
    return;
  }
  std::vector<DumpRecord> recs;
  REQUIRE(read_livoxdump(path, &recs));
  MESSAGE("real: " << path << " -> " << recs.size() << " datagrams");
  REQUIRE(recs.size() > 10000);

  // A4, exactly as docs/A4-timesync.md §7 prescribes — with one deliberate
  // departure: BOTH the point stream and the IMU are fed into the
  // kLidarMid360 estimator rather than getting one each. The Mid-360 stamps
  // both from the same device clock (A3 §4), so a second estimator would add
  // a few milliseconds of independent estimation noise BETWEEN the two
  // streams, which is exactly the quantity the deskew is sensitive to.
  TimeSync ts;
  ImuIngest imu(ts, StreamId::kLidarMid360);

  LioConfig cfg;
  cfg.scan_period_s = 0.1;
  cfg.live_points_per_sec = 40000;
  cfg.min_range_m = 0.5f;
  cfg.max_range_m = 60.0f;
  cfg.map.voxel_size_m = 0.5;
  cfg.max_correspondence_m = 1.0;
  cfg.init_imu_samples = 100;
  LioOdometry lio(cfg);
  REQUIRE(lio.start().ok());

  mid360::PointFilterConfig filter;  // A3 defaults: no-returns and spatial-noise tags out
  mid360::FilterStats fstats;
  mid360::LossTracker loss;

  std::uint64_t point_packets = 0, imu_packets = 0, bad_packets = 0, nonmono = 0;
  std::uint64_t points_decoded = 0;
  std::uint64_t last_dev_ts = 0;
  bool have_dev_ts = false;
  std::vector<PointVertex> batch;
  batch.reserve(mid360::kPointsPerPacket);

  for (const DumpRecord& rec : recs) {
    const mid360::PacketView v = mid360::parse_packet(rec.payload.data(), rec.payload.size());
    if (!v.valid()) {
      ++bad_packets;
      continue;
    }
    std::uint64_t dev_ts = 0;  // memcpy: the datagram's 8-byte field is not aligned
    std::memcpy(&dev_ts, &v.header->timestamp, sizeof(dev_ts));
    if (v.header->data_type == mid360::kDataTypeImu) {
      const mid360::ImuRaw* raw = reinterpret_cast<const mid360::ImuRaw*>(v.payload);
      const float gyro[3] = {raw->gyro_x, raw->gyro_y, raw->gyro_z};
      const float acc_g[3] = {raw->acc_x, raw->acc_y, raw->acc_z};
      const ImuSample s = imu.add_g(static_cast<std::int64_t>(dev_ts),
                                    TimePoint{static_cast<std::int64_t>(rec.t_ns)}, gyro, acc_g);
      REQUIRE(lio.push_imu(s.t_engine_ns, s.gyro_rad_s, s.accel_m_s2).ok());
      ++imu_packets;
      continue;
    }
    if (v.header->data_type != mid360::kDataTypeCartesianHigh) continue;

    // Loss accounting is per DATAGRAM and must see every one of them,
    // including the ones the clock filter below refuses — skipping 150
    // packets here would be reported as 150 lost packets, which is exactly
    // the kind of number that gets escalated as a network fault.
    std::uint32_t lost = 0;
    (void)loss.observe(v.header->udp_cnt, &lost);

    // The first 150 datagrams of this capture carry timestamp 0 while
    // arrival advances 72 ms — the device clock had not started. Feeding
    // those to the offset estimator would poison the mapping, and they are
    // exactly what a driver's "device stamp went backwards" rule discards.
    if (have_dev_ts && dev_ts <= last_dev_ts) {
      ++nonmono;
      continue;
    }
    last_dev_ts = dev_ts;
    have_dev_ts = true;

    ts.add_pair(StreamId::kLidarMid360, static_cast<std::int64_t>(dev_ts),
                TimePoint{static_cast<std::int64_t>(rec.t_ns)});
    const TimeModel model = ts.model(StreamId::kLidarMid360);

    const mid360::CartesianHigh* pts =
        reinterpret_cast<const mid360::CartesianHigh*>(v.payload);
    batch.clear();
    for (std::uint32_t i = 0; i < v.point_count; ++i) {
      if (!mid360::point_passes(pts[i], filter, &fstats)) continue;
      PointVertex pv;
      pv.x = static_cast<float>(pts[i].x) * 1e-3f;
      pv.y = static_cast<float>(pts[i].y) * 1e-3f;
      pv.z = static_cast<float>(pts[i].z) * 1e-3f;
      pv.r = pv.g = pv.b = pts[i].reflectivity;
      pv.a = 255;
      batch.push_back(pv);
    }
    points_decoded += batch.size();
    if (!batch.empty()) {
      REQUIRE(lio.push_points(Span<const PointVertex>(batch.data(), batch.size()),
                              model.apply(static_cast<std::int64_t>(dev_ts)))
                  .ok());
    }
    ++point_packets;
  }
  REQUIRE(lio.flush().ok());

  const LioStats st = lio.stats();
  const OffsetEstimate est = ts.estimate(StreamId::kLidarMid360);

  MESSAGE("real: point packets " << point_packets << ", IMU " << imu_packets << ", bad "
                                 << bad_packets << ", pre-clock " << nonmono);
  MESSAGE("real: loss " << loss.lost() << " (" << (loss.loss_fraction() * 100.0) << "%), dup "
                        << loss.duplicates() << ", resets " << loss.resets());
  MESSAGE("real: filter kept " << fstats.kept << "/" << fstats.seen << " ("
                               << (fstats.keep_fraction() * 100.0) << "%), no-return "
                               << fstats.dropped_no_return << ", tag " << fstats.dropped_tag);
  MESSAGE("real: sync offset " << (est.offset_ns * 1e-6) << " ms, jitter " << (est.jitter_ns * 1e-6)
                               << " ms, converged " << est.converged << ", quality "
                               << std::string(to_string(sync_quality(est))));
  MESSAGE("real: points decoded " << points_decoded << ", kept " << st.points_kept << ", mapped "
                                  << st.points_mapped);
  const double rscans = static_cast<double>(st.scans);
  MESSAGE("real: scans " << st.scans << " (skipped " << st.scans_skipped
                         << "), mean residuals/scan " << (st.residuals_total / rscans)
                         << ", mean iterations/scan " << (st.iterations_total / rscans));
  MESSAGE("real: residual rms " << (st.residual_rms_m * 100.0) << " cm, points late "
                                << st.points_late << ", store appends failed "
                                << st.store_appends_failed);
  MESSAGE("real: per-scan ms mean " << st.scan_ms_mean << " p50 " << st.scan_ms_p50 << " p95 "
                                    << st.scan_ms_p95 << " max " << st.scan_ms_max);
  Pose last_pose;
  REQUIRE(lio.current_pose(&last_pose));
  const double disp = std::sqrt(last_pose.position[0] * last_pose.position[0] +
                                last_pose.position[1] * last_pose.position[1] +
                                last_pose.position[2] * last_pose.position[2]);
  MESSAGE("real: path length " << st.trajectory_length_m << " m, net displacement " << disp
                               << " m, |g| " << st.gravity_m_s2 << " m/s^2, final |v| "
                               << st.speed_m_s << " m/s");
  MESSAGE("real: map " << st.map_voxels << " voxels / " << st.map_points << " points; store "
                       << lio.map_store().total_points() << " points in "
                       << lio.map_store().page_count() << " pages");
  MESSAGE("real: CPU budget used " << st.cpu_budget_used << " of " << cfg.cpu_budget_cores
                                   << " cores");

  // --- wire-level sanity (these are the A3/S2 numbers, re-measured here) ---
  CHECK(point_packets > 12000);
  CHECK(imu_packets == 1200);
  CHECK(bad_packets == 0);
  // Two datagrams are genuinely missing from this capture (0.016%), well
  // under A3's 1% degradation threshold. Asserting on the FRACTION rather
  // than on zero is the honest form: the capture is what it is.
  CHECK(loss.loss_fraction() < 0.001);
  CHECK(fstats.keep_fraction() > 0.5);
  CHECK(fstats.dropped_no_return > 100000);

  // --- odometry sanity ----------------------------------------------------
  CHECK(st.initialized);
  CHECK_FALSE(st.diverged);
  CHECK(st.scans >= 50);
  CHECK(st.residuals_last > 0);
  // The fit must sit at scene scale, not at "somewhere near a wall". 10.7 cm
  // measured, and the tolerance is deliberately set well above it rather than
  // just above it: this is an OUTDOOR capture at 6 m/s where the "planes" are
  // road, kerb, foliage and building faces at a 15 m mean range, so the
  // residual is dominated by how planar the world actually is, not by the
  // sensor. (A sweep of max_correspondence_m over {0.3, 0.5, 1.0} m gives
  // {3.2, 8.2, 10.7} cm — and the 3.2 cm setting is the WORST of the three,
  // because at 400 correspondences per scan the update stops constraining
  // the filter and the final speed doubles to 12.6 m/s. A small residual on
  // a small number of hand-picked matches is not a better fit.)
  CHECK(st.residual_rms_m > 0.0);
  CHECK(st.residual_rms_m < 0.25);
  CHECK(st.store_appends_failed == 0);
  CHECK(st.scans_skipped * 2 < st.scans);  // most scans got a real lidar update
  CHECK(st.points_mapped > 20000);
  CHECK(lio.map_store().total_points() > 20000);

  // Gravity is the one physical quantity we know without ground truth.
  CHECK(st.gravity_m_s2 > 9.4);
  CHECK(st.gravity_m_s2 < 10.2);

  // A 6-second handheld/vehicle clip cannot have travelled 100 m, and a
  // diverged filter always does.
  CHECK(st.trajectory_length_m >= 0.0);
  CHECK(st.trajectory_length_m < 60.0);
  CHECK(st.speed_m_s < 20.0);

  // Every pose finite, monotone in time, and every published map point finite.
  Pose prev;
  bool have_prev = false;
  const LioPoseSource& ps = lio.poses();
  for (std::int64_t t = st.t_last_ns - 5'000'000'000LL; t <= st.t_last_ns; t += 100'000'000LL) {
    Pose p;
    if (!ps.pose_at(t, &p).ok()) continue;
    for (int i = 0; i < 3; ++i) REQUIRE(std::isfinite(p.position[i]));
    for (int i = 0; i < 4; ++i) REQUIRE(std::isfinite(p.orientation[i]));
    if (have_prev) CHECK(p.t_mono_ns >= prev.t_mono_ns);
    prev = p;
    have_prev = true;
  }
  CHECK(have_prev);

  std::uint64_t checked = 0;
  for (PageId id : lio.map_store().page_ids()) {
    const PageView v = lio.map_store().page_view(id);
    for (std::uint32_t i = 0; i < v.count; ++i) {
      REQUIRE(std::isfinite(v.data[i].x));
      REQUIRE(std::isfinite(v.data[i].y));
      REQUIRE(std::isfinite(v.data[i].z));
      ++checked;
    }
  }
  CHECK(checked == lio.map_store().total_points());
  CHECK(lio.stop().ok());
}
