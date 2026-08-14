// test_mount_calib.cpp — A8: the planar-checkerboard mount-extrinsics solver.
//
// The scene generator below is a C++ port of the S6 spike's Python simulation
// (spikes/s6-calibration/sim/{geom,rig,targets}.py): the same camera model,
// the same two ground-truth mounts, the same R3 low-discrepancy wizard pose
// schedule, the same board sizes, and the same 2-D "line of returns" vs 3-D
// "patch of returns" observation models. That is deliberate — it makes the
// tables this file prints directly comparable with REPORT.md's T1–T4, so a
// regression in the shipped solver shows up as a disagreement with the study
// that sized the feature.
//
// Two departures, both stated in engine/docs/A8-pushbroom.md §4:
//  * the camera side is modelled as "checkerboard PnP plane + small noise"
//    (0.02 deg of normal error, 0.1 mm of offset error) rather than by
//    re-running a PnP solve. Those are the numbers a 48-corner A1 board at
//    1.5 m with 0.3 px corner noise supports, and they have to be that small:
//    S6's own noise sweep (T3) is linear all the way down to 5 mm with no
//    floor, so the camera term must sit well below it. At 0.5 mm of offset
//    error the sweep visibly floors out at the low end and stops matching T3;
//  * randomness is a fixed-seed xorshift64 + Box-Muller rather than numpy, so
//    the five CI legs agree — the same rule test_timesync.cpp follows.
//
// Included first and alone, so this file also serves as the self-containment
// check for the header (test_headers.cpp is not A8's to edit).
#include "scanengine/slam/pushbroom/mount_calibration.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/poses/se3.h"

using namespace scanengine;

namespace {

constexpr double kPi = se3::kPi;
constexpr double kDeg = kPi / 180.0;

// --------------------------------------------------------------------- RNG

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

  std::uint64_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    return s_;
  }
  double uniform01() {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform01(); }
  double normal(double mu, double sigma) {
    // Box-Muller. <random>'s distributions are not specified bit-for-bit by
    // the standard, so they cannot be used in a cross-platform test.
    const double u1 = std::max(uniform01(), 1e-300);
    const double u2 = uniform01();
    return mu + sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
  }
  void normal3(double sigma, double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = normal(0.0, sigma);
  }

 private:
  std::uint64_t s_;
};

// ------------------------------------------------------------------ geometry

// scipy's Rotation.from_euler("xyz", deg, degrees=True): EXTRINSIC rotations
// about the fixed x, then y, then z axes, i.e. R = Rz(c) Ry(b) Rx(a).
void euler_xyz_deg(const double d[3], double R[9]) {
  const double a = d[0] * kDeg, b = d[1] * kDeg, c = d[2] * kDeg;
  const double ca = std::cos(a), sa = std::sin(a);
  const double cb = std::cos(b), sb = std::sin(b);
  const double cc = std::cos(c), sc = std::sin(c);
  const double Rx[9] = {1, 0, 0, 0, ca, -sa, 0, sa, ca};
  const double Ry[9] = {cb, 0, sb, 0, 1, 0, -sb, 0, cb};
  const double Rz[9] = {cc, -sc, 0, sc, cc, 0, 0, 0, 1};
  double t[9];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += Ry[i * 3 + k] * Rx[k * 3 + j];
      t[i * 3 + j] = s;
    }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += Rz[i * 3 + k] * t[k * 3 + j];
      R[i * 3 + j] = s;
    }
}

void mat3_mul(const double a[9], const double b[9], double out[9]) {
  double t[9];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += a[i * 3 + k] * b[k * 3 + j];
      t[i * 3 + j] = s;
    }
  for (int i = 0; i < 9; ++i) out[i] = t[i];
}

void mat3_col(const double R[9], int c, double out[3]) {
  out[0] = R[0 * 3 + c];
  out[1] = R[1 * 3 + c];
  out[2] = R[2 * 3 + c];
}

void mat3_apply(const double R[9], const double v[3], double out[3]) {
  const double x = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
  const double y = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
  const double z = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

// S6 geom.py look_at: world<-camera for a camera at `eye` aimed at `target`.
// Camera z is the viewing direction, y points down in the image.
void look_at(const double eye[3], const double target[3], double roll_deg,
             const double world_up[3], double m[16]) {
  double z[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  se3::normalize3(z);
  double up[3] = {world_up[0], world_up[1], world_up[2]};
  if (std::fabs(se3::dot3(up, z)) > 0.98) {
    up[0] = 0.0;
    up[1] = 1.0;
    up[2] = 0.0;
  }
  double neg_up[3] = {-up[0], -up[1], -up[2]};
  double x[3];
  se3::cross3(z, neg_up, x);
  se3::normalize3(x);
  double y[3];
  se3::cross3(z, x, y);
  const double R[9] = {x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]};
  const double w[3] = {0.0, 0.0, roll_deg * kDeg};
  double Rr[9], Rout[9];
  se3::so3_exp(w, Rr);
  mat3_mul(R, Rr, Rout);
  se3::mat4_from_rt(Rout, eye, m);
}

// ------------------------------------------------------------------- targets

struct Board {
  int cols, rows;
  double square_m, margin_m;
  const char* label;
  double width_m() const { return (cols - 1) * square_m + 2 * margin_m; }
  double height_m() const { return (rows - 1) * square_m + 2 * margin_m; }
};

const Board kBoardA1{8, 6, 0.100, 0.050, "A1 checkerboard (0.80 x 0.60 m)"};
const Board kBoardXL{9, 7, 0.130, 0.060, "XL board (1.16 x 0.90 m)"};

struct LidarSpec {
  const char* name;
  bool two_d;
  double range_sigma_m;
  double ang_res_deg;
  double dwell_s;
  std::size_t max_target_pts;
};

const LidarSpec kD6{"COIN-D6", true, 0.030, 0.9, 1.5, 250};
const LidarSpec kMid360{"Livox Mid-360", false, 0.020, 0.4, 1.0, 600};

// S6 rig.py's two ground-truth mounts, including the few degrees of bracket
// misalignment that the wizard exists to recover.
void mount_d6(double m[16]) {
  const double R_nom[9] = {0, 0, 1, 0, -1, 0, 1, 0, 0};  // columns: +z, -y, +x
  const double e[3] = {1.5, -2.0, 0.8};
  double Re[9], R[9];
  euler_xyz_deg(e, Re);
  mat3_mul(R_nom, Re, R);
  const double t[3] = {0.015, 0.150, -0.030};
  se3::mat4_from_rt(R, t, m);
}

void mount_mid360(double m[16]) {
  const double R_nom[9] = {0, -1, 0, 0, 0, -1, 1, 0, 0};  // columns: +z, -x, -y
  const double e_pitch[3] = {15.0, 0.0, 0.0};
  const double e_misalign[3] = {0.9, 1.3, -0.6};
  double Rp[9], Rm[9], tmp[9], R[9];
  euler_xyz_deg(e_pitch, Rp);
  euler_xyz_deg(e_misalign, Rm);
  mat3_mul(Rp, R_nom, tmp);
  mat3_mul(tmp, Rm, R);
  const double t[3] = {0.0, 0.100, -0.045};
  se3::mat4_from_rt(R, t, m);
}

// The prescribed viewpoints: an R3 low-discrepancy sweep of azimuth,
// elevation and ROLL. Roll is not optional — S6 T2 shows translation-only
// poses roughly double the error for both sensors, and for the 2-D sensor
// rolling the phone is the ONLY thing that changes the direction of its scan
// line across the target.
void wizard_slot(int i, int n, bool diverse, double out[3]) {
  if (diverse) {
    const double a[3] = {0.8191725134, 0.6710436067, 0.5497004779};
    for (int k = 0; k < 3; ++k) {
      double u = 0.5 + static_cast<double>(i + 1) * a[k];
      u -= std::floor(u);
      out[k] = u;
    }
    out[0] = -38.0 + 76.0 * out[0];
    out[1] = -24.0 + 50.0 * out[1];
    out[2] = -60.0 + 120.0 * out[2];
  } else {
    // "Sideways steps only, phone upright" — the lazy-wizard failure mode.
    out[0] = (n > 1) ? -16.0 + 32.0 * static_cast<double>(i) / (n - 1) : 0.0;
    out[1] = 4.0;
    out[2] = 0.0;
  }
}

void pose_from_slot(const double slot[3], Rng& rng, double dist_lo, double dist_hi,
                    double T_wc[16]) {
  const double a = (slot[0] + rng.normal(0, 5.0)) * kDeg;
  const double e = (slot[1] + rng.normal(0, 5.0)) * kDeg;
  const double r = slot[2] + rng.normal(0, 5.0);
  const double d = rng.uniform(dist_lo, dist_hi);
  const double eye[3] = {d * std::sin(a) * std::cos(e), -d * std::sin(e),
                         d * std::cos(a) * std::cos(e)};
  const double aim[3] = {rng.normal(0, 0.05), rng.normal(0, 0.05), 0.0};
  const double world_up[3] = {0.0, -1.0, 0.0};
  look_at(eye, aim, r, world_up, T_wc);
}

// --------------------------------------------------- camera-side observation
//
// The board plane as the camera measures it: n . X = d in the camera frame.
// Returns false when the board is not usably detectable from this pose (not
// fully in frame with a 40 px margin, or a mean incidence past 62 degrees at
// which corner detection collapses) — the same two rejections WIZARD.md
// screen 2 turns into live "move back" / "too side-on" chips.
constexpr double kCamPlaneNormalSigmaDeg = 0.02;
constexpr double kCamPlaneOffsetSigmaM = 0.0001;

bool observe_board_camera(const double T_wc[16], const Board& board, const CalibCamera& cam,
                          Rng& rng, double n_out[3], double* d_out) {
  double T_cw[16];
  se3::mat4_inverse_rigid(T_wc, T_cw);

  const double hx = (board.cols - 1) * board.square_m * 0.5;
  const double hy = (board.rows - 1) * board.square_m * 0.5;
  const double corners[4][3] = {{-hx, -hy, 0}, {hx, -hy, 0}, {-hx, hy, 0}, {hx, hy, 0}};

  double R_cw[9], t_cw[3];
  se3::mat4_get_rt(T_cw, R_cw, t_cw);
  double n_true[3];
  const double z_axis[3] = {0, 0, 1};
  mat3_apply(R_cw, z_axis, n_true);

  double incidence_sum = 0.0;
  for (const auto& c : corners) {
    double X[3];
    se3::mat4_apply(T_cw, c, X);
    if (!(X[2] > 0.1)) return false;
    const double u = cam.fx * X[0] / X[2] + cam.cx;
    const double v = cam.fy * X[1] / X[2] + cam.cy;
    if (u < 40.0 || u > static_cast<double>(cam.width) - 40.0) return false;
    if (v < 40.0 || v > static_cast<double>(cam.height) - 40.0) return false;
    double view[3] = {X[0], X[1], X[2]};
    se3::normalize3(view);
    double c_ang = std::fabs(se3::dot3(view, n_true));
    if (c_ang > 1.0) c_ang = 1.0;
    incidence_sum += std::acos(c_ang) / kDeg;
  }
  if (incidence_sum / 4.0 > 62.0) return false;

  double n[3] = {n_true[0], n_true[1], n_true[2]};
  double d = se3::dot3(n, t_cw);
  if (d < 0) {
    for (int i = 0; i < 3; ++i) n[i] = -n[i];
    d = -d;
  }
  double w[3];
  rng.normal3(kCamPlaneNormalSigmaDeg * kDeg, w);
  double Rn[9], nn[3];
  se3::so3_exp(w, Rn);
  mat3_apply(Rn, n, nn);
  se3::normalize3(nn);
  for (int i = 0; i < 3; ++i) n_out[i] = nn[i];
  *d_out = d + rng.normal(0, kCamPlaneOffsetSigmaM);
  return true;
}

// ---------------------------------------------------- lidar-side observation

bool clip_line_to_rect(const double base[3], const double dir[3], double hw, double hh,
                       double* t_lo, double* t_hi) {
  double lo = -1e9, hi = 1e9;
  const double half[2] = {hw, hh};
  for (int ax = 0; ax < 2; ++ax) {
    const double b = base[ax], dv = dir[ax];
    if (std::fabs(dv) < 1e-9) {
      if (std::fabs(b) > half[ax]) return false;
      continue;
    }
    const double a = (-half[ax] - b) / dv;
    const double c = (half[ax] - b) / dv;
    lo = std::max(lo, std::min(a, c));
    hi = std::min(hi, std::max(a, c));
  }
  if (hi - lo <= 0.05) return false;
  *t_lo = lo;
  *t_hi = hi;
  return true;
}

// Returns the lidar-frame returns that land on the board, with radial range
// noise. `plane_z_offset` displaces the surface the LIDAR actually hit while
// leaving the camera's reported plane at z = 0 — that is the "board is not
// clear of the wall, so segmentation grabbed the wall" failure the wizard
// screen-1 rule exists to prevent, and it is what the corrupted-capture test
// injects.
bool observe_board_lidar(const double T_wc[16], const double T_cl[16], const LidarSpec& spec,
                         const Board& board, double sigma_range, double plane_z_offset,
                         Rng& rng, std::vector<double>* out) {
  double T_wl[16], T_lw[16];
  se3::mat4_mul(T_wc, T_cl, T_wl);
  se3::mat4_inverse_rigid(T_wl, T_lw);

  const double hw = board.width_m() / 2 - 0.025;
  const double hh = board.height_m() / 2 - 0.025;
  if (hw <= 0 || hh <= 0) return false;

  double R_wl[9], o[3];
  se3::mat4_get_rt(T_wl, R_wl, o);

  std::vector<double> P_w;
  if (spec.two_d) {
    // The scan plane is lidar z = 0; intersect it with the board plane.
    double e1[3], e2[3];
    mat3_col(R_wl, 0, e1);
    mat3_col(R_wl, 1, e2);
    const double nz[2] = {e1[2], e2[2]};
    const double nz2 = nz[0] * nz[0] + nz[1] * nz[1];
    if (std::sqrt(nz2) < 1e-6) return false;  // scan plane parallel to the board
    const double oz = o[2] - plane_z_offset;
    const double base_ab[2] = {-oz * nz[0] / nz2, -oz * nz[1] / nz2};
    double base_w[3];
    for (int i = 0; i < 3; ++i) base_w[i] = o[i] + base_ab[0] * e1[i] + base_ab[1] * e2[i];
    const double inv = 1.0 / std::sqrt(nz2);
    const double dab[2] = {-nz[1] * inv, nz[0] * inv};
    double dir_w[3];
    for (int i = 0; i < 3; ++i) dir_w[i] = dab[0] * e1[i] + dab[1] * e2[i];
    se3::normalize3(dir_w);
    // Clipping happens in the board's own x/y, so shift the base into it.
    double base_rel[3] = {base_w[0], base_w[1], base_w[2] - plane_z_offset};
    double t0 = 0, t1 = 0;
    if (!clip_line_to_rect(base_rel, dir_w, hw, hh, &t0, &t1)) return false;

    double mid[3];
    for (int i = 0; i < 3; ++i) mid[i] = base_w[i] + 0.5 * (t0 + t1) * dir_w[i];
    const double dmid[3] = {mid[0] - o[0], mid[1] - o[1], mid[2] - o[2]};
    const double rmid = se3::norm3(dmid);
    const double step = 2.0 * rmid * std::tan(spec.ang_res_deg * kDeg / 2.0);
    int n_line = static_cast<int>((t1 - t0) / std::max(step, 1e-4));
    n_line = std::max(3, std::min(n_line, static_cast<int>(spec.max_target_pts)));
    // 10 Hz rotation x dwell: the same profile is re-observed that many times.
    const int reps = std::max(1, static_cast<int>(spec.dwell_s * 10));
    for (int r = 0; r < reps && P_w.size() / 3 < spec.max_target_pts; ++r) {
      const double jitter = rng.uniform(-step / 2, step / 2);
      for (int i = 0; i < n_line && P_w.size() / 3 < spec.max_target_pts; ++i) {
        double t = t0 + (t1 - t0) * static_cast<double>(i) / std::max(1, n_line - 1) + jitter;
        t = std::min(std::max(t, t0), t1);
        for (int k = 0; k < 3; ++k) P_w.push_back(base_w[k] + t * dir_w[k]);
      }
    }
  } else {
    for (std::size_t i = 0; i < spec.max_target_pts; ++i) {
      P_w.push_back(rng.uniform(-hw, hw));
      P_w.push_back(rng.uniform(-hh, hh));
      P_w.push_back(plane_z_offset);
    }
  }
  if (P_w.size() < 9) return false;

  out->clear();
  out->reserve(P_w.size());
  for (std::size_t i = 0; i < P_w.size() / 3; ++i) {
    double p_l[3];
    se3::mat4_apply(T_lw, &P_w[i * 3], p_l);
    const double r = se3::norm3(p_l);
    if (!(r > 1e-6)) return false;
    const double e = rng.normal(0, sigma_range);
    for (int k = 0; k < 3; ++k) out->push_back(p_l[k] + p_l[k] / r * e);
  }
  return true;
}

// ---------------------------------------------------------------- the session

// The bracket's CAD nominal: what the solve starts from. S6 campaign.py uses
// 4 degrees and 25 mm — a machined or printed phone bracket is good to about
// that, and it is far enough out that a robust kernel applied first stalls.
constexpr double kCadRotErrDeg = 4.0;
constexpr double kCadTransErrMm = 25.0;

void cad_nominal(const double T_true[16], Rng& rng, double out[16], double rot_deg = kCadRotErrDeg,
                 double trans_mm = kCadTransErrMm) {
  double R[9], t[3];
  se3::mat4_get_rt(T_true, R, t);
  double w[3];
  rng.normal3(rot_deg * kDeg, w);
  double dR[9], Rn[9];
  se3::so3_exp(w, dR);
  mat3_mul(R, dR, Rn);
  double dt[3];
  rng.normal3(trans_mm * 1e-3, dt);
  const double tn[3] = {t[0] + dt[0], t[1] + dt[1], t[2] + dt[2]};
  se3::mat4_from_rt(Rn, tn, out);
}

struct TrialSpec {
  const LidarSpec* lidar = &kMid360;
  const Board* board = &kBoardA1;
  int n_poses = 8;
  double sigma_range = -1.0;  // < 0 = the sensor's own
  bool diverse = true;
  bool compute_split_half = true;
  bool robust_first_stage = false;
  bool run_robust_stage = true;
  int corrupt_every = 0;      // 0 = clean; N = every Nth pose hits the wall behind the board
  double corrupt_offset_m = 0.35;
  double cad_rot_deg = kCadRotErrDeg;
  double cad_trans_mm = kCadTransErrMm;
};

struct Trial {
  bool ok = false;
  double rot_deg = 0;
  double trans_mm = 0;
  double px_1m = 0, px_3m = 0, px_8m = 0;
  double split_half_px = -1;
  double sigma_rot_deg = 0;
  double sigma_trans_mm = 0;
  CalibGate gate = CalibGate::kUnknown;
};

Trial run_trial(const TrialSpec& ts, std::uint64_t seed) {
  Rng rng(seed * 0x2545F4914F6CDD1DULL + 0x9E3779B97F4A7C15ULL);
  Trial out;

  double T_cl_true[16];
  if (ts.lidar->two_d) {
    mount_d6(T_cl_true);
  } else {
    mount_mid360(T_cl_true);
  }
  const double sigma = ts.sigma_range >= 0 ? ts.sigma_range : ts.lidar->range_sigma_m;

  MountCalibConfig cfg;
  cfg.compute_split_half = ts.compute_split_half;
  cfg.robust_first_stage = ts.robust_first_stage;
  cfg.run_robust_stage = ts.run_robust_stage;
  cfg.min_observations = 5;
  MountCalibrationSolver solver(cfg);

  for (int i = 0; i < ts.n_poses; ++i) {
    double slot[3];
    wizard_slot(i, ts.n_poses, ts.diverse, slot);
    bool captured = false;
    for (int retry = 0; retry < 6 && !captured; ++retry) {
      double T_wc[16];
      pose_from_slot(slot, rng, 1.1, 1.9, T_wc);
      PlaneObservation obs;
      if (!observe_board_camera(T_wc, *ts.board, cfg.camera, rng, obs.normal, &obs.d)) continue;
      const double z_off =
          (ts.corrupt_every > 0 && (i % ts.corrupt_every) == 0) ? ts.corrupt_offset_m : 0.0;
      if (!observe_board_lidar(T_wc, T_cl_true, *ts.lidar, *ts.board, sigma, z_off, rng,
                               &obs.points_lidar)) {
        continue;
      }
      obs.sigma_m = sigma;
      if (obs.point_count() < 3) continue;
      if (!solver.add_observation(obs).ok()) continue;
      captured = true;
    }
    if (!captured) return out;  // the user could not complete the prescribed pose
  }
  if (solver.observation_count() < static_cast<std::size_t>(ts.n_poses)) return out;

  double cad[16];
  cad_nominal(T_cl_true, rng, cad, ts.cad_rot_deg, ts.cad_trans_mm);
  auto r = solver.solve(cad);
  if (!r.ok()) return out;
  const MountCalibResult& res = r.value();

  se3::transform_error(T_cl_true, res.camera_from_lidar, &out.rot_deg, &out.trans_mm);
  out.px_1m = reprojection_disagreement_px(T_cl_true, res.camera_from_lidar, 1.0, cfg.camera);
  out.px_3m = reprojection_disagreement_px(T_cl_true, res.camera_from_lidar, 3.0, cfg.camera);
  out.px_8m = reprojection_disagreement_px(T_cl_true, res.camera_from_lidar, 8.0, cfg.camera);
  out.split_half_px = res.split_half_px;
  out.sigma_rot_deg = res.sigma_rot_deg;
  out.sigma_trans_mm = res.sigma_trans_mm;
  out.gate = res.gate;
  out.ok = true;
  return out;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const double idx = p * static_cast<double>(v.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(idx);
  const std::size_t hi = std::min(lo + 1, v.size() - 1);
  const double f = idx - static_cast<double>(lo);
  return v[lo] * (1.0 - f) + v[hi] * f;
}

double cv(const std::vector<double>& v) {  // coefficient of variation
  if (v.size() < 2) return 0.0;
  double m = 0.0;
  for (double x : v) m += x;
  m /= static_cast<double>(v.size());
  if (!(std::fabs(m) > 1e-12)) return 0.0;
  double s = 0.0;
  for (double x : v) s += (x - m) * (x - m);
  return std::sqrt(s / static_cast<double>(v.size() - 1)) / std::fabs(m);
}

struct Batch {
  std::vector<double> rot, trans, px1, px3, px8, split, sigma_rot;
  int n_ok = 0, n_fail = 0;
};

Batch run_batch(const TrialSpec& ts, int trials, std::uint64_t seed0 = 1) {
  Batch b;
  for (int i = 0; i < trials; ++i) {
    const Trial t = run_trial(ts, seed0 + static_cast<std::uint64_t>(i) * 7919ULL);
    if (!t.ok) {
      ++b.n_fail;
      continue;
    }
    ++b.n_ok;
    b.rot.push_back(t.rot_deg);
    b.trans.push_back(t.trans_mm);
    b.px1.push_back(t.px_1m);
    b.px3.push_back(t.px_3m);
    b.px8.push_back(t.px_8m);
    if (t.split_half_px >= 0) b.split.push_back(t.split_half_px);
    b.sigma_rot.push_back(t.sigma_rot_deg);
  }
  return b;
}

constexpr int kTrials = 21;

std::string row(const char* label, const Batch& b) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%-34s rot %7.3f deg  trans %7.1f mm  px@3m %8.1f (p90 %8.1f)",
                label, median(b.rot), median(b.trans), median(b.px3), percentile(b.px3, 0.9));
  return std::string(buf);
}

}  // namespace

// ===========================================================================
// Solver mechanics
// ===========================================================================

TEST_CASE("mount_calib/recovers_the_extrinsic_exactly_with_zero_noise") {
  // S6 §2.4's first sanity check: with no noise the residual at ground truth
  // is exactly zero and the solve returns the truth.
  TrialSpec ts;
  ts.lidar = &kMid360;
  ts.n_poses = 8;
  ts.sigma_range = 0.0;
  Rng rng(12345);

  double T_true[16];
  mount_mid360(T_true);
  MountCalibConfig cfg;
  MountCalibrationSolver solver(cfg);
  int captured = 0;
  for (int i = 0; i < 8 && captured < 8; ++i) {
    double slot[3];
    wizard_slot(i, 8, true, slot);
    for (int retry = 0; retry < 8; ++retry) {
      double T_wc[16];
      pose_from_slot(slot, rng, 1.1, 1.9, T_wc);
      PlaneObservation obs;
      Rng clean(999);  // no camera-plane noise either
      if (!observe_board_camera(T_wc, kBoardA1, cfg.camera, clean, obs.normal, &obs.d)) continue;
      // Overwrite with the exact plane: zero noise means zero.
      double T_cw[16], R_cw[9], t_cw[3];
      se3::mat4_inverse_rigid(T_wc, T_cw);
      se3::mat4_get_rt(T_cw, R_cw, t_cw);
      const double z_axis[3] = {0, 0, 1};
      mat3_apply(R_cw, z_axis, obs.normal);
      obs.d = se3::dot3(obs.normal, t_cw);
      if (obs.d < 0) {
        for (int k = 0; k < 3; ++k) obs.normal[k] = -obs.normal[k];
        obs.d = -obs.d;
      }
      if (!observe_board_lidar(T_wc, T_true, kMid360, kBoardA1, 0.0, 0.0, rng,
                               &obs.points_lidar)) {
        continue;
      }
      obs.sigma_m = 0.02;
      if (!solver.add_observation(obs).ok()) continue;
      ++captured;
      break;
    }
  }
  REQUIRE(captured == 8);

  double cad[16];
  cad_nominal(T_true, rng, cad);
  auto r = solver.solve(cad);
  REQUIRE(r.ok());
  double rot = 0, tr = 0;
  se3::transform_error(T_true, r.value().camera_from_lidar, &rot, &tr);
  MESSAGE("zero-noise recovery: " << rot << " deg / " << tr << " mm");
  CHECK(rot < 1e-4);
  CHECK(tr < 1e-3);
  CHECK(r.value().rms_residual_m < 1e-9);
  CHECK(r.value().split_half_px < 0.01);
  CHECK(r.value().gate == CalibGate::kGood);
}

TEST_CASE("mount_calib/rejects_malformed_observations_and_undetermined_captures") {
  MountCalibrationSolver solver;
  PlaneObservation o;
  o.normal[0] = 0.0;
  o.normal[1] = 0.0;
  o.normal[2] = 2.0;  // not a unit vector
  o.d = 1.5;
  o.points_lidar = {0.1, 0.2, 0.3, 0.1, 0.2, 0.4, 0.2, 0.1, 0.3};
  CHECK(solver.add_observation(o).error() == ScanError::kInvalidArgument);

  o.normal[2] = 1.0;
  o.sigma_m = 0.0;
  CHECK(solver.add_observation(o).error() == ScanError::kInvalidArgument);

  o.sigma_m = 0.02;
  o.points_lidar = {0.1, 0.2};  // not a multiple of 3
  CHECK(solver.add_observation(o).error() == ScanError::kInvalidArgument);

  o.points_lidar = {0.1, 0.2, 0.3};  // only one point
  CHECK(solver.add_observation(o).error() == ScanError::kInvalidArgument);

  o.points_lidar = {0.1, 0.2, 0.3, 0.1, 0.2, 0.4, 0.2, 0.1, 0.3};
  REQUIRE(solver.add_observation(o).ok());
  CHECK(solver.observation_count() == 1);

  double ident[16];
  se3::mat4_identity(ident);
  // Two observations cannot determine six unknowns; refuse rather than answer.
  REQUIRE(solver.add_observation(o).ok());
  CHECK(solver.solve(ident).error() == ScanError::kInvalidArgument);

  // A non-rigid initial guess is rejected too — the row/column-major trap.
  REQUIRE(solver.add_observation(o).ok());
  double bad[16];
  se3::mat4_identity(bad);
  bad[0] = 1.5;
  CHECK(solver.solve(bad).error() == ScanError::kInvalidArgument);
}

TEST_CASE("mount_calib/the_robust_stage_earns_its_place_and_never_runs_first") {
  // S6 §2.3 recorded that applying a robust kernel in stage 1 stalled the
  // solve from a 4 deg / 25 mm CAD start: every residual looks like an outlier
  // there, so soft-L1 down-weights the whole problem. This solver keeps the
  // two-stage order for that reason.
  //
  // Measured here, the shipped LM does NOT reproduce the stall, and the reason
  // is structural rather than lucky: Marquardt damping (lambda * diag(H), not
  // lambda * I) makes the step INVARIANT to a uniform rescaling of H and g,
  // and a soft-L1 kernel applied when every residual is large is, to first
  // order, exactly such a uniform rescale. S6's prototype used scipy's
  // trust-region-reflective, which bounds the step in a scaled variable
  // instead, so a global down-weighting does shrink the step and does stall.
  //
  // The order is kept anyway — it costs nothing, it is what the study
  // mandates, and it is the only order whose behaviour is guaranteed if the
  // damping strategy is ever changed. What this test locks down is the part
  // that is load-bearing either way: stage 2 must MEASURABLY improve on a
  // plain L2 fit, and stage 1's kernel must not change the answer.
  TrialSpec two_stage;              // shipped: L2 then robust
  two_stage.lidar = &kMid360;
  two_stage.n_poses = 8;
  two_stage.compute_split_half = false;

  TrialSpec l2_only = two_stage;
  l2_only.run_robust_stage = false;

  TrialSpec robust_first = two_stage;
  robust_first.robust_first_stage = true;

  // A capture with real outliers is where the kernel has to pay for itself.
  // (Both variants are far outside the budget here and would be gated
  // kReject; what is being measured is which one degrades less.)
  TrialSpec dirty_two_stage = two_stage;
  dirty_two_stage.n_poses = 12;
  dirty_two_stage.corrupt_every = 6;
  TrialSpec dirty_l2_only = dirty_two_stage;
  dirty_l2_only.run_robust_stage = false;

  const Batch b_two = run_batch(two_stage, kTrials);
  const Batch b_l2 = run_batch(l2_only, kTrials);
  const Batch b_rf = run_batch(robust_first, kTrials);
  const Batch d_two = run_batch(dirty_two_stage, kTrials);
  const Batch d_l2 = run_batch(dirty_l2_only, kTrials);
  REQUIRE(b_two.n_ok > kTrials / 2);
  REQUIRE(b_l2.n_ok > kTrials / 2);
  REQUIRE(b_rf.n_ok > kTrials / 2);
  REQUIRE(d_two.n_ok > kTrials / 2);
  REQUIRE(d_l2.n_ok > kTrials / 2);

  MESSAGE("clean  L2 -> robust : " << median(b_two.px3) << " px@3m");
  MESSAGE("clean  L2 only      : " << median(b_l2.px3) << " px@3m");
  MESSAGE("clean  robust first : " << median(b_rf.px3) << " px@3m");
  MESSAGE("dirty  L2 -> robust : " << median(d_two.px3) << " px@3m");
  MESSAGE("dirty  L2 only      : " << median(d_l2.px3) << " px@3m");

  // On a purely Gaussian capture a robust kernel costs a little statistical
  // efficiency — that is what it is buying insurance with, and the premium
  // must stay small.
  CHECK(median(b_two.px3) < 1.15 * median(b_l2.px3));
  // The payout: on a capture with mis-segmented poses the kernel is worth
  // several times the whole error budget.
  CHECK(median(d_two.px3) < 0.5 * median(d_l2.px3));
  // Stage 1's kernel choice must not move the answer under this damping — if
  // this ever starts failing, the damping changed and the S6 stall is back.
  CHECK(std::fabs(median(b_rf.px3) - median(b_two.px3)) < 0.05 * median(b_two.px3));
}

// ===========================================================================
// S6 T1 / T3 / T4: accuracy vs pose count, noise and sensor
// ===========================================================================

TEST_CASE("mount_calib/mid360_eight_poses_meet_the_s6_target") {
  // S6 T1: Mid-360, A1 board, 8 poses, 20 mm range noise -> 0.159 deg /
  // 2.6 mm / 4.2 px at 3 m. This is the number the whole wizard is designed
  // around (WIZARD.md §0: better than 0.16 deg and 3 mm).
  TrialSpec ts;
  ts.lidar = &kMid360;
  ts.board = &kBoardA1;
  ts.n_poses = 8;
  const Batch b = run_batch(ts, kTrials);
  REQUIRE(b.n_ok > kTrials / 2);
  MESSAGE(row("mid360 A1 N=8 (S6: 0.159/2.6/4.2)", b));
  CHECK(median(b.rot) < 0.30);
  CHECK(median(b.trans) < 5.0);
  CHECK(median(b.px3) < 8.0);
  // The gate must call a good capture good.
  CHECK(median(b.split) < 12.0);
}

TEST_CASE("mount_calib/accuracy_improves_with_pose_count") {
  // S6 T1's shape: N = 3 is degenerate for the 2-D sensor and poor for the
  // 3-D one; the curve flattens past 8 for the Mid-360.
  const int counts[4] = {3, 5, 8, 12};
  std::vector<double> med;
  for (int n : counts) {
    TrialSpec ts;
    ts.lidar = &kMid360;
    ts.board = &kBoardA1;
    ts.n_poses = n;
    ts.compute_split_half = false;
    const Batch b = run_batch(ts, kTrials);
    REQUIRE(b.n_ok > kTrials / 2);
    char lbl[64];
    std::snprintf(lbl, sizeof(lbl), "mid360 A1 N=%d", n);
    MESSAGE(row(lbl, b));
    med.push_back(median(b.px3));
  }
  CHECK(med[1] < med[0]);        // 5 beats 3
  CHECK(med[2] < med[0]);        // 8 beats 3, decisively
  CHECK(med[2] < 0.6 * med[0]);
  CHECK(med[3] < med[0]);
}

TEST_CASE("mount_calib/d6_needs_a_bench_calibration_not_a_handheld_wizard") {
  // S6 T4. At the D6's specified 30 mm range noise a 12-pose handheld wizard
  // lands far outside the 20.2 px colorization budget; 30-45 tripod poses
  // bring it inside without touching the sensor.
  const int counts[4] = {12, 20, 30, 45};
  std::vector<double> med3;
  for (int n : counts) {
    TrialSpec ts;
    ts.lidar = &kD6;
    ts.board = &kBoardA1;
    ts.n_poses = n;
    const Batch b = run_batch(ts, kTrials);
    REQUIRE(b.n_ok > kTrials / 2);
    char lbl[64];
    std::snprintf(lbl, sizeof(lbl), "d6 A1 N=%d (30 mm noise)", n);
    MESSAGE(row(lbl, b));
    MESSAGE("    split-half gate median " << median(b.split) << " px");
    med3.push_back(median(b.px3));
  }
  // The whole point of the bench procedure: 45 poses must be dramatically
  // better than the 12 a handheld user can manage.
  CHECK(med3[3] < med3[0]);
  CHECK(med3[3] < 0.5 * med3[0]);
  CHECK(med3[1] < med3[0]);

  // And the XL board pays off once the pose count is high (S6 T4: 5.8 px vs
  // 9.0 px at N=45; indistinguishable at N=12-20).
  TrialSpec xl;
  xl.lidar = &kD6;
  xl.board = &kBoardXL;
  xl.n_poses = 45;
  xl.compute_split_half = false;
  const Batch bxl = run_batch(xl, kTrials);
  REQUIRE(bxl.n_ok > kTrials / 2);
  MESSAGE(row("d6 XL N=45 (30 mm noise)", bxl));
}

TEST_CASE("mount_calib/error_scales_with_range_noise") {
  // S6 T3 / action A: the D6 verdict pivots entirely on its real 1-sigma range
  // noise. At 10 mm it behaves like a Mid-360; at 30 mm it does not, and it
  // degrades SUPER-linearly because the 2-D solve is weakly conditioned to
  // begin with.
  const double sigmas[4] = {0.005, 0.010, 0.020, 0.030};
  std::vector<double> med;
  for (double s : sigmas) {
    TrialSpec ts;
    ts.lidar = &kD6;
    ts.board = &kBoardA1;
    ts.n_poses = 12;
    ts.sigma_range = s;
    ts.compute_split_half = false;
    const Batch b = run_batch(ts, kTrials);
    REQUIRE(b.n_ok > kTrials / 2);
    char lbl[64];
    std::snprintf(lbl, sizeof(lbl), "d6 A1 N=12 sigma=%.0f mm", s * 1000.0);
    MESSAGE(row(lbl, b));
    med.push_back(median(b.px3));
  }
  for (std::size_t i = 1; i < med.size(); ++i) CHECK(med[i] > med[i - 1]);
  // Super-linear: 6x the noise costs more than 6x the error.
  CHECK(med[3] / med[0] > 6.0);
}

TEST_CASE("mount_calib/the_wizard_must_instruct_roll") {
  // S6 T2. Poses that vary only in position roughly double the error for both
  // sensors; for the D6, rolling the phone is the ONLY thing that changes the
  // direction of its scan line across the target.
  TrialSpec diverse;
  diverse.lidar = &kD6;
  diverse.board = &kBoardA1;
  diverse.n_poses = 12;
  diverse.compute_split_half = false;

  TrialSpec flat = diverse;
  flat.diverse = false;

  const Batch bd = run_batch(diverse, kTrials);
  const Batch bf = run_batch(flat, kTrials);
  REQUIRE(bd.n_ok > kTrials / 2);
  REQUIRE(bf.n_ok > kTrials / 2);
  MESSAGE(row("d6 N=12 varied az+el+ROLL", bd));
  MESSAGE(row("d6 N=12 sideways steps only", bf));
  CHECK(median(bd.px3) < median(bf.px3));
}

// ===========================================================================
// The quality gate (S6 action G)
// ===========================================================================

TEST_CASE("mount_calib/split_half_gate_rejects_a_corrupted_capture") {
  // A capture where the board was not standing clear of the wall, so lidar
  // segmentation grabbed the wall behind it on some poses (WIZARD.md screen 1
  // rule 1). The camera still reports the board plane; the lidar returns are
  // 35 cm behind it.
  TrialSpec clean;
  clean.lidar = &kMid360;
  clean.board = &kBoardA1;
  clean.n_poses = 12;

  TrialSpec corrupt = clean;
  corrupt.corrupt_every = 3;

  const Batch bc = run_batch(clean, kTrials);
  const Batch bx = run_batch(corrupt, kTrials);
  REQUIRE(bc.n_ok > kTrials / 2);
  REQUIRE(bx.n_ok > kTrials / 2);

  MESSAGE("clean     : split-half " << median(bc.split) << " px, true error "
                                    << median(bc.px3) << " px");
  MESSAGE("corrupted : split-half " << median(bx.split) << " px, true error "
                                    << median(bx.px3) << " px");

  // The gate reads a good capture as good...
  CHECK(median(bc.split) <= 12.0);
  // ...and does not merely scale up on a bad one, it explodes (WIZARD.md §2
  // screen 4: "exactly the behaviour a safety gate wants").
  CHECK(median(bx.split) > 30.0);
  CHECK(median(bx.split) > 4.0 * median(bc.split));

  // Verify the classification the user actually sees.
  int good = 0, reject = 0;
  for (int i = 0; i < kTrials; ++i) {
    const Trial t = run_trial(clean, 1 + static_cast<std::uint64_t>(i) * 7919ULL);
    if (t.ok && t.gate == CalibGate::kGood) ++good;
    const Trial u = run_trial(corrupt, 1 + static_cast<std::uint64_t>(i) * 7919ULL);
    if (u.ok && u.gate == CalibGate::kReject) ++reject;
  }
  MESSAGE("clean captures gated Good: " << good << "/" << kTrials
                                        << ", corrupted gated Reject: " << reject << "/"
                                        << kTrials);
  CHECK(good > kTrials * 2 / 3);
  CHECK(reject > kTrials * 2 / 3);
}

TEST_CASE("mount_calib/solver_covariance_is_not_a_valid_gate") {
  // S6 action G, the reason the gate is split-half and not the covariance:
  // with a PRESCRIBED pose set the linearised covariance is nearly constant
  // from session to session, so it cannot rank a good capture against a bad
  // one. Its measured rank correlation with true error was about 0.1.
  //
  // Here: across sessions that differ only in their noise realisation, the
  // true error varies far more than the reported sigma does.
  TrialSpec ts;
  ts.lidar = &kD6;
  ts.board = &kBoardA1;
  ts.n_poses = 12;
  ts.compute_split_half = false;
  const Batch b = run_batch(ts, kTrials);
  REQUIRE(b.n_ok > kTrials / 2);

  const double cv_true = cv(b.rot);
  const double cv_sigma = cv(b.sigma_rot);
  MESSAGE("true rot error   : median " << median(b.rot) << " deg, CV " << cv_true);
  MESSAGE("reported sigma   : median " << median(b.sigma_rot) << " deg, CV " << cv_sigma);
  CHECK(cv_true > 0.3);
  CHECK(cv_sigma < 0.5 * cv_true);
}

TEST_CASE("mount_calib/reprojection_metric_is_symmetric_and_zero_at_identity") {
  double a[16], b[16];
  mount_mid360(a);
  CalibCamera cam;
  CHECK(reprojection_disagreement_px(a, a, 3.0, cam) < 1e-9);

  // A pure 0.1 deg rotation error costs fx * theta pixels at EVERY range —
  // S6 §6.1's "range-independent" rotation term. fx = 2912 px, so 0.1 deg is
  // about 5.1 px.
  double R[9], t[3];
  se3::mat4_get_rt(a, R, t);
  const double w[3] = {0.1 * kDeg, 0.0, 0.0};
  double dR[9], Rn[9];
  se3::so3_exp(w, dR);
  mat3_mul(dR, R, Rn);
  se3::mat4_from_rt(Rn, t, b);
  const double p1 = reprojection_disagreement_px(a, b, 1.0, cam);
  const double p3 = reprojection_disagreement_px(a, b, 3.0, cam);
  const double p8 = reprojection_disagreement_px(a, b, 8.0, cam);
  MESSAGE("0.1 deg rotation error: " << p1 << " / " << p3 << " / " << p8 << " px at 1/3/8 m");
  CHECK(std::fabs(p3 - 5.08) < 1.0);
  CHECK(std::fabs(p8 - p3) < 0.3);   // range-independent
  CHECK(std::fabs(p1 - p3) < 0.3);

  // A pure 10 mm translation error decays as fx * d / r — S6 §6.1 again.
  double tt[3] = {t[0] + 0.010, t[1], t[2]};
  se3::mat4_from_rt(R, tt, b);
  const double q1 = reprojection_disagreement_px(a, b, 1.0, cam);
  const double q3 = reprojection_disagreement_px(a, b, 3.0, cam);
  MESSAGE("10 mm translation error: " << q1 << " / " << q3 << " px at 1/3 m");
  CHECK(q1 > 2.5 * q3);
}

TEST_CASE("mount_calib/gate_bands_match_the_wizard_design") {
  MountCalibConfig cfg;
  CHECK(classify_gate(-1.0, cfg) == CalibGate::kUnknown);
  CHECK(classify_gate(0.0, cfg) == CalibGate::kGood);
  CHECK(classify_gate(12.0, cfg) == CalibGate::kGood);
  CHECK(classify_gate(12.01, cfg) == CalibGate::kUsable);
  CHECK(classify_gate(30.0, cfg) == CalibGate::kUsable);
  CHECK(classify_gate(30.01, cfg) == CalibGate::kReject);
  CHECK(std::string(to_string(CalibGate::kReject)) == "reject");
}
