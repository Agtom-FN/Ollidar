// eskf.cpp — implementation of include/scanengine/slam/eskf.h.
#include "scanengine/slam/eskf.h"

#include <cmath>
#include <cstring>

#include "lio_math.h"

namespace scanengine {

using slam_detail::Mat3;
using slam_detail::Vec3;

namespace {
constexpr int N = kEskfDim;

// Error-state block offsets. Mirrors the table in eskf.h.
constexpr int kP = 0, kR = 3, kV = 6, kBG = 9, kBA = 12, kG = 15;

inline void set_block3(double* M, int r, int c, const Mat3& b) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) M[(r + i) * N + (c + j)] = b(i, j);
}
inline void add_block3(double* M, int r, int c, const Mat3& b) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) M[(r + i) * N + (c + j)] += b(i, j);
}
inline Mat3 scaled_identity(double s) {
  Mat3 m = Mat3::zero();
  m(0, 0) = s;
  m(1, 1) = s;
  m(2, 2) = s;
  return m;
}
}  // namespace

struct Eskf::Impl {
  EskfConfig cfg;
  EskfState x;
  Mat3 R = Mat3::identity();  // cached rotation; q is derived from it
  double P[N * N] = {};
  double tmp_a[N * N] = {};
  double tmp_b[N * N] = {};
  bool init = false;

  void sync_quat() { slam_detail::mat3_to_quat(R, x.q); }
};

Eskf::Eskf(const EskfConfig& cfg) : impl_(new Impl()) {
  impl_->cfg = cfg;
  impl_->x.g[2] = -cfg.gravity_m_s2;
}

Eskf::~Eskf() = default;

const EskfConfig& Eskf::config() const { return impl_->cfg; }
const EskfState& Eskf::state() const { return impl_->x; }
bool Eskf::initialized() const { return impl_->init; }
double* Eskf::cov() { return impl_->P; }
const double* Eskf::cov() const { return impl_->P; }

void Eskf::init(const EskfState& s) {
  impl_->x = s;
  impl_->R = slam_detail::orthonormalize(slam_detail::quat_to_mat3(s.q));
  impl_->sync_quat();
  const EskfConfig& c = impl_->cfg;
  std::memset(impl_->P, 0, sizeof(impl_->P));
  const double d[6] = {c.init_sigma_p_m,     c.init_sigma_rot_rad, c.init_sigma_v_m_s,
                       c.init_sigma_bg_rad_s, c.init_sigma_ba_m_s2, c.init_sigma_g_m_s2};
  for (int b = 0; b < 6; ++b) {
    for (int i = 0; i < 3; ++i) {
      const int k = b * 3 + i;
      impl_->P[k * N + k] = d[b] * d[b];
    }
  }
  impl_->init = true;
}

Status Eskf::init_from_static(std::int64_t t_ns, const double mean_gyro[3],
                              const double mean_accel[3]) {
  const Vec3 a(mean_accel);
  const double an = slam_detail::norm(a);
  if (!(an > 1.0) || !std::isfinite(an)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "eskf: static-init accel magnitude %.4f m/s^2 is not gravity", an);
  }
  // At rest the accelerometer measures the specific force -R^T g. Rotating
  // the measured direction onto +Z makes the world frame z-up and leaves the
  // magnitude mismatch (|a| vs 9.80665) to be absorbed by the accel bias
  // rather than baked into the gravity vector.
  const Mat3 R0 = slam_detail::rotation_between(a, Vec3(0, 0, 1));

  EskfState s;
  s.t_ns = t_ns;
  slam_detail::mat3_to_quat(R0, s.q);
  for (int i = 0; i < 3; ++i) {
    s.p[i] = 0.0;
    s.v[i] = 0.0;
    s.bg[i] = mean_gyro[i];
    s.ba[i] = 0.0;
    s.g[i] = 0.0;
  }
  s.g[2] = -impl_->cfg.gravity_m_s2;
  init(s);
  return kOkStatus;
}

void Eskf::set_state(const EskfState& s) {
  impl_->x = s;
  impl_->R = slam_detail::orthonormalize(slam_detail::quat_to_mat3(s.q));
  impl_->sync_quat();
}

Status Eskf::propagate(std::int64_t t_ns, const double gyro[3], const double accel[3]) {
  Impl& s = *impl_;
  if (!s.init) return ScanError::kInvalidState;
  if (t_ns <= s.x.t_ns) return ScanError::kAgain;

  double dt_total = static_cast<double>(t_ns - s.x.t_ns) * 1e-9;
  if (dt_total > s.cfg.max_gap_s) {
    // Longer than the filter is willing to invent. Re-anchor and say so; the
    // caller decides whether that is a stream gap (fine) or a fault.
    s.x.t_ns = t_ns;
    return ScanError::kTimeout;
  }

  const Vec3 w_m(gyro), a_m(accel);
  const Vec3 bg(s.x.bg), ba(s.x.ba);
  const Vec3 w = w_m - bg;
  const Vec3 a = a_m - ba;

  // Split long gaps: the covariance propagation is first-order in dt, and
  // 20 ms is already generous for a 200 Hz IMU (it only bites when a packet
  // is lost).
  int steps = 1;
  if (dt_total > s.cfg.max_step_s) {
    steps = static_cast<int>(std::ceil(dt_total / s.cfg.max_step_s));
    if (steps > 64) steps = 64;
  }
  const double dt = dt_total / static_cast<double>(steps);

  for (int step = 0; step < steps; ++step) {
    const Mat3 R = s.R;
    const Vec3 acc_w = R * a + Vec3(s.x.g);

    // --- nominal ---------------------------------------------------------
    for (int i = 0; i < 3; ++i) {
      s.x.p[i] += s.x.v[i] * dt + 0.5 * acc_w[i] * dt * dt;
      s.x.v[i] += acc_w[i] * dt;
    }
    const Vec3 dtheta = w * dt;
    s.R = slam_detail::orthonormalize(R * slam_detail::so3_exp(dtheta));

    // --- F ---------------------------------------------------------------
    double* F = s.tmp_a;
    std::memset(F, 0, sizeof(double) * N * N);
    for (int i = 0; i < N; ++i) F[i * N + i] = 1.0;
    set_block3(F, kP, kV, scaled_identity(dt));
    set_block3(F, kR, kR, slam_detail::transpose(slam_detail::so3_exp(dtheta)));
    set_block3(F, kR, kBG, slam_detail::so3_jr(dtheta) * (-dt));
    set_block3(F, kV, kR, (R * slam_detail::hat(a)) * (-dt));
    set_block3(F, kV, kBA, R * (-dt));
    set_block3(F, kV, kG, scaled_identity(dt));

    // --- P <- F P F^T + Q ------------------------------------------------
    // FP into tmp_b, then (FP) F^T back into P. Fixed loop order, so the
    // reduction is bit-reproducible across builds.
    double* FP = s.tmp_b;
    for (int i = 0; i < N; ++i) {
      for (int j = 0; j < N; ++j) {
        double acc = 0.0;
        for (int k = 0; k < N; ++k) acc += F[i * N + k] * s.P[k * N + j];
        FP[i * N + j] = acc;
      }
    }
    for (int i = 0; i < N; ++i) {
      for (int j = 0; j < N; ++j) {
        double acc = 0.0;
        for (int k = 0; k < N; ++k) acc += FP[i * N + k] * F[j * N + k];
        s.P[i * N + j] = acc;
      }
    }

    const double qg = s.cfg.gyro_noise_rad_s * s.cfg.gyro_noise_rad_s * dt * dt;
    const double qa = s.cfg.accel_noise_m_s2 * s.cfg.accel_noise_m_s2 * dt * dt;
    const double qbg = s.cfg.gyro_bias_rw_rad_s2 * s.cfg.gyro_bias_rw_rad_s2 * dt;
    const double qba = s.cfg.accel_bias_rw_m_s3 * s.cfg.accel_bias_rw_m_s3 * dt;
    add_block3(s.P, kR, kR, scaled_identity(qg));
    add_block3(s.P, kV, kV, scaled_identity(qa));
    add_block3(s.P, kBG, kBG, scaled_identity(qbg));
    add_block3(s.P, kBA, kBA, scaled_identity(qba));
    // Position and gravity get no process noise of their own — position is
    // driven entirely by velocity and gravity is a constant of the session.
    // A floor keeps P strictly positive definite so its inverse (which the
    // iterated update needs) always exists.
    for (int i = 0; i < 3; ++i) {
      s.P[(kP + i) * N + (kP + i)] += 1e-12;
      s.P[(kG + i) * N + (kG + i)] += 1e-14;
    }

    // Symmetrize: F P F^T is symmetric in exact arithmetic and drifts by
    // ~1e-18 per step in practice. Cheap to fix, expensive to debug later.
    for (int i = 0; i < N; ++i) {
      for (int j = i + 1; j < N; ++j) {
        const double v = 0.5 * (s.P[i * N + j] + s.P[j * N + i]);
        s.P[i * N + j] = v;
        s.P[j * N + i] = v;
      }
    }
  }

  s.x.t_ns = t_ns;
  s.sync_quat();
  return kOkStatus;
}

void Eskf::boxplus(const double dx[kEskfDim]) {
  Impl& s = *impl_;
  for (int i = 0; i < 3; ++i) {
    s.x.p[i] += dx[kP + i];
    s.x.v[i] += dx[kV + i];
    s.x.bg[i] += dx[kBG + i];
    s.x.ba[i] += dx[kBA + i];
    s.x.g[i] += dx[kG + i];
  }
  s.R = slam_detail::orthonormalize(s.R * slam_detail::so3_exp(Vec3(dx + kR)));
  s.sync_quat();
}

void Eskf::boxminus(const EskfState& other, double dx[kEskfDim]) const {
  const Impl& s = *impl_;
  for (int i = 0; i < 3; ++i) {
    dx[kP + i] = s.x.p[i] - other.p[i];
    dx[kV + i] = s.x.v[i] - other.v[i];
    dx[kBG + i] = s.x.bg[i] - other.bg[i];
    dx[kBA + i] = s.x.ba[i] - other.ba[i];
    dx[kG + i] = s.x.g[i] - other.g[i];
  }
  const Mat3 Ro = slam_detail::quat_to_mat3(other.q);
  const Vec3 d = slam_detail::so3_log(slam_detail::transpose(Ro) * s.R);
  for (int i = 0; i < 3; ++i) dx[kR + i] = d[i];
}

bool Eskf::diverged(double max_speed_m_s) const {
  const EskfState& x = impl_->x;
  const double* arrays[5] = {x.p, x.v, x.bg, x.ba, x.g};
  for (const double* arr : arrays) {
    for (int i = 0; i < 3; ++i) {
      if (!std::isfinite(arr[i])) return true;
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(x.q[i])) return true;
  }
  for (int i = 0; i < N; ++i) {
    if (!std::isfinite(impl_->P[i * N + i]) || impl_->P[i * N + i] < 0.0) return true;
  }
  const double sp = std::sqrt(x.v[0] * x.v[0] + x.v[1] * x.v[1] + x.v[2] * x.v[2]);
  return sp > max_speed_m_s;
}

}  // namespace scanengine
