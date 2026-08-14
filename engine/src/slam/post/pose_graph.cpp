// pose_graph.cpp — the hand-rolled SE(3) pose-graph solver (A7).
//
// See include/scanengine/slam/post/pose_graph.h for the GTSAM decision and
// the conventions; this file is the arithmetic.
//
//   residual   e = ( Log(Rz^T Ri^T Rj) ,  Rz^T (Ri^T (pj - pi) - pz) )
//   retraction R <- R Exp(dtheta),  p <- p + R dp
//   normal eq  (J^T W Omega J + lambda diag) delta = -J^T W Omega e
//   solve      reverse-Cuthill-McKee ordering + skyline LDL^T
//
// Every reduction below runs in a fixed index order. That is not style: A6
// promised bit-identical output for identical input across five toolchains
// and A7 has to keep the promise through the optimizer.
#include "scanengine/slam/post/pose_graph.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "scanengine/poses/se3.h"

namespace scanengine {
namespace post {
namespace {

using se3::matrix_to_quat;
using se3::quat_to_matrix;
using se3::so3_exp;
using se3::so3_log;

// --- tiny fixed-size algebra ------------------------------------------------
// Row-major throughout. mat3 is double[9], mat6 is double[36].

void mat3_mul(const double a[9], const double b[9], double out[9]) {
  double t[9];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) s += a[r * 3 + k] * b[k * 3 + c];
      t[r * 3 + c] = s;
    }
  }
  for (int i = 0; i < 9; ++i) out[i] = t[i];
}

void mat3_transpose(const double a[9], double out[9]) {
  double t[9] = {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]};
  for (int i = 0; i < 9; ++i) out[i] = t[i];
}

void mat3_apply(const double a[9], const double v[3], double out[3]) {
  const double x = a[0] * v[0] + a[1] * v[1] + a[2] * v[2];
  const double y = a[3] * v[0] + a[4] * v[1] + a[5] * v[2];
  const double z = a[6] * v[0] + a[7] * v[1] + a[8] * v[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

void mat3_skew(const double v[3], double out[9]) {
  out[0] = 0.0;    out[1] = -v[2]; out[2] = v[1];
  out[3] = v[2];   out[4] = 0.0;   out[5] = -v[0];
  out[6] = -v[1];  out[7] = v[0];  out[8] = 0.0;
}

// Inverse right Jacobian of SO(3):
//   Jr^-1(phi) = I + 0.5 [phi]x + (1/th^2 - (1+cos th)/(2 th sin th)) [phi]x^2
// The bracketed coefficient tends to 1/12 as th -> 0 and is evaluated by its
// series there, because the closed form is 0/0 and then catastrophically
// cancelling for another two decades above that.
void so3_jr_inv(const double phi[3], double out[9]) {
  const double th2 = phi[0] * phi[0] + phi[1] * phi[1] + phi[2] * phi[2];
  const double th = std::sqrt(th2);
  double coeff;
  if (th < 1e-5) {
    // 1/12 + th^2/720 + th^4/30240
    coeff = 1.0 / 12.0 + th2 * (1.0 / 720.0 + th2 * (1.0 / 30240.0));
  } else {
    const double s = std::sin(th);
    const double c = std::cos(th);
    coeff = 1.0 / th2 - (1.0 + c) / (2.0 * th * s);
  }
  double sk[9], sk2[9];
  mat3_skew(phi, sk);
  mat3_mul(sk, sk, sk2);
  for (int i = 0; i < 9; ++i) out[i] = 0.5 * sk[i] + coeff * sk2[i];
  out[0] += 1.0;
  out[4] += 1.0;
  out[8] += 1.0;
}

// Place a 3x3 block at (br, bc) of a 6x6, zeroing nothing else.
inline void mat6_set_block(double m[36], int br, int bc, const double b[9], double scale) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) m[(br + r) * 6 + (bc + c)] = scale * b[r * 3 + c];
  }
}

void mat6_zero(double m[36]) {
  for (int i = 0; i < 36; ++i) m[i] = 0.0;
}

// out = a^T * b, all 6x6.
void mat6_ata_b(const double a[36], const double b[36], double out[36]) {
  for (int r = 0; r < 6; ++r) {
    for (int c = 0; c < 6; ++c) {
      double s = 0.0;
      for (int k = 0; k < 6; ++k) s += a[k * 6 + r] * b[k * 6 + c];
      out[r * 6 + c] = s;
    }
  }
}

void mat6_apply(const double a[36], const double v[6], double out[6]) {
  double t[6];
  for (int r = 0; r < 6; ++r) {
    double s = 0.0;
    for (int k = 0; k < 6; ++k) s += a[r * 6 + k] * v[k];
    t[r] = s;
  }
  for (int i = 0; i < 6; ++i) out[i] = t[i];
}

double vec6_dot(const double a[6], const double b[6]) {
  double s = 0.0;
  for (int i = 0; i < 6; ++i) s += a[i] * b[i];
  return s;
}

// --- the residual and its Jacobians ----------------------------------------

struct BetweenLinear {
  double e[6];
  double Ji[36];
  double Jj[36];
};

void between_linearize(const double qi[4], const double pi[3], const double qj[4],
                       const double pj[3], const double qz[4], const double pz[3],
                       BetweenLinear* out, bool want_jacobians) {
  double Ri[9], Rj[9], Rz[9];
  quat_to_matrix(qi, Ri);
  quat_to_matrix(qj, Rj);
  quat_to_matrix(qz, Rz);

  double RiT[9], RzT[9];
  mat3_transpose(Ri, RiT);
  mat3_transpose(Rz, RzT);

  const double dp[3] = {pj[0] - pi[0], pj[1] - pi[1], pj[2] - pi[2]};
  double d[3];
  mat3_apply(RiT, dp, d);  // d = Ri^T (pj - pi)

  double Rij[9], Re[9];
  mat3_mul(RiT, Rj, Rij);
  mat3_mul(RzT, Rij, Re);  // Re = Rz^T Ri^T Rj

  double e_rot[3];
  so3_log(Re, e_rot);

  const double dm[3] = {d[0] - pz[0], d[1] - pz[1], d[2] - pz[2]};
  double e_trans[3];
  mat3_apply(RzT, dm, e_trans);

  for (int i = 0; i < 3; ++i) {
    out->e[i] = e_rot[i];
    out->e[3 + i] = e_trans[i];
  }
  if (!want_jacobians) return;

  double JrInv[9];
  so3_jr_inv(e_rot, JrInv);

  // d e_rot   / d dtheta_j = Jr^-1(e_rot)
  // d e_trans / d dp_j     = Re
  mat6_zero(out->Jj);
  mat6_set_block(out->Jj, 0, 0, JrInv, 1.0);
  mat6_set_block(out->Jj, 3, 3, Re, 1.0);

  // d e_rot   / d dtheta_i = -Jl^-1(e_rot) Rz^T = -(Jr^-1)^T Rz^T
  // d e_trans / d dtheta_i =  Rz^T [d]x
  // d e_trans / d dp_i     = -Rz^T
  double JrInvT[9], A[9], sk[9], B[9];
  mat3_transpose(JrInv, JrInvT);
  mat3_mul(JrInvT, RzT, A);
  mat3_skew(d, sk);
  mat3_mul(RzT, sk, B);
  mat6_zero(out->Ji);
  mat6_set_block(out->Ji, 0, 0, A, -1.0);
  mat6_set_block(out->Ji, 3, 0, B, 1.0);
  mat6_set_block(out->Ji, 3, 3, RzT, -1.0);
}

struct PriorLinear {
  double e[6];
  double Ji[36];
};

void prior_linearize(const double qi[4], const double pi[3], const double qz[4],
                     const double pz[3], PriorLinear* out, bool want_jacobians) {
  double Ri[9], Rz[9], RzT[9];
  quat_to_matrix(qi, Ri);
  quat_to_matrix(qz, Rz);
  mat3_transpose(Rz, RzT);

  double Re[9];
  mat3_mul(RzT, Ri, Re);
  double e_rot[3];
  so3_log(Re, e_rot);

  const double dp[3] = {pi[0] - pz[0], pi[1] - pz[1], pi[2] - pz[2]};
  double e_trans[3];
  mat3_apply(RzT, dp, e_trans);

  for (int i = 0; i < 3; ++i) {
    out->e[i] = e_rot[i];
    out->e[3 + i] = e_trans[i];
  }
  if (!want_jacobians) return;

  double JrInv[9];
  so3_jr_inv(e_rot, JrInv);
  mat6_zero(out->Ji);
  mat6_set_block(out->Ji, 0, 0, JrInv, 1.0);
  mat6_set_block(out->Ji, 3, 3, Re, 1.0);
}

// Huber: cost(s) = s for s <= d^2, else 2 d sqrt(s) - d^2; the IRLS weight is
// cost'(s) = 1 or d/sqrt(s). d == 0 means "no kernel".
struct RobustTerm {
  double cost;
  double weight;
};

RobustTerm huber(double s, double delta) {
  RobustTerm out{s, 1.0};
  if (delta > 0.0 && s > delta * delta) {
    const double r = std::sqrt(s);
    out.cost = 2.0 * delta * r - delta * delta;
    out.weight = delta / r;
  }
  return out;
}

// --- reverse Cuthill-McKee --------------------------------------------------
//
// Deterministic by construction: the seed of each component is the lowest-index
// node of minimum degree, and each BFS frontier is expanded in (degree, index)
// order. No hash iteration, no std::sort on equal keys without a tiebreak.
std::vector<std::uint32_t> reverse_cuthill_mckee(
    const std::vector<std::vector<std::uint32_t>>& adj) {
  const std::size_t n = adj.size();
  std::vector<std::uint32_t> order;
  order.reserve(n);
  std::vector<char> seen(n, 0);
  std::vector<std::uint32_t> frontier;

  for (;;) {
    // Seed: unvisited, minimum degree, lowest index.
    std::size_t seed = n;
    std::size_t best_deg = 0;
    for (std::size_t v = 0; v < n; ++v) {
      if (seen[v] != 0) continue;
      const std::size_t deg = adj[v].size();
      if (seed == n || deg < best_deg) {
        seed = v;
        best_deg = deg;
      }
    }
    if (seed == n) break;

    std::size_t head = order.size();
    seen[seed] = 1;
    order.push_back(static_cast<std::uint32_t>(seed));
    while (head < order.size()) {
      const std::uint32_t u = order[head++];
      frontier.clear();
      for (std::uint32_t w : adj[u]) {
        if (seen[w] == 0) {
          seen[w] = 1;
          frontier.push_back(w);
        }
      }
      std::sort(frontier.begin(), frontier.end(),
                [&adj](std::uint32_t a, std::uint32_t b) {
                  if (adj[a].size() != adj[b].size()) return adj[a].size() < adj[b].size();
                  return a < b;
                });
      for (std::uint32_t w : frontier) order.push_back(w);
    }
  }
  std::reverse(order.begin(), order.end());
  return order;
}

// --- skyline (profile) symmetric matrix ------------------------------------
//
// Lower triangle, row-wise, each row r storing columns [start[r], r]. Cholesky
// of a symmetric matrix creates no nonzero outside this envelope, so the
// profile computed from the ordering is the exact fill — no symbolic analysis,
// no reallocation during factorization.
class Skyline {
 public:
  void reset(std::vector<std::uint32_t> start) {
    start_ = std::move(start);
    const std::size_t n = start_.size();
    offset_.assign(n + 1, 0);
    std::uint64_t acc = 0;
    for (std::size_t r = 0; r < n; ++r) {
      offset_[r] = acc;
      acc += (r - start_[r]) + 1;
    }
    offset_[n] = acc;
    data_.assign(static_cast<std::size_t>(acc), 0.0);
  }

  void zero() { std::fill(data_.begin(), data_.end(), 0.0); }

  std::size_t rows() const { return start_.size(); }
  std::uint64_t stored() const { return offset_.empty() ? 0 : offset_.back(); }
  std::uint32_t start(std::size_t r) const { return start_[r]; }

  double& at(std::size_t r, std::size_t c) {  // requires start[r] <= c <= r
    return data_[static_cast<std::size_t>(offset_[r]) + (c - start_[r])];
  }
  double at(std::size_t r, std::size_t c) const {
    return data_[static_cast<std::size_t>(offset_[r]) + (c - start_[r])];
  }
  // Zero outside the stored envelope.
  double get(std::size_t r, std::size_t c) const {
    return c < start_[r] ? 0.0 : at(r, c);
  }
  void add(std::size_t r, std::size_t c, double v) { at(r, c) += v; }

  const std::vector<double>& data() const { return data_; }
  std::vector<double>& data() { return data_; }

 private:
  std::vector<std::uint32_t> start_;
  std::vector<std::uint64_t> offset_;
  std::vector<double> data_;
};

// In-place LDL^T over the envelope. Returns false on a non-positive pivot,
// which under Levenberg damping means the caller should raise lambda.
bool skyline_ldlt(Skyline* m, std::vector<double>* d_out, CancelToken* cancel) {
  const std::size_t n = m->rows();
  d_out->assign(n, 0.0);
  std::vector<double>& d = *d_out;
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 4095u) == 0 && cancelled(cancel)) return false;
    const std::size_t si = m->start(i);
    for (std::size_t j = si; j < i; ++j) {
      const std::size_t sj = m->start(j);
      const std::size_t k0 = sj > si ? sj : si;
      double s = m->at(i, j);
      for (std::size_t k = k0; k < j; ++k) s -= m->at(i, k) * d[k] * m->at(j, k);
      m->at(i, j) = s / d[j];
    }
    double s = m->at(i, i);
    for (std::size_t k = si; k < i; ++k) {
      const double lik = m->at(i, k);
      s -= lik * lik * d[k];
    }
    if (!(s > 1e-300) || !std::isfinite(s)) return false;
    d[i] = s;
  }
  return true;
}

void skyline_solve(const Skyline& m, const std::vector<double>& d, const std::vector<double>& b,
                   std::vector<double>* x) {
  const std::size_t n = m.rows();
  x->assign(n, 0.0);
  std::vector<double>& v = *x;
  for (std::size_t i = 0; i < n; ++i) {
    double s = b[i];
    for (std::size_t k = m.start(i); k < i; ++k) s -= m.at(i, k) * v[k];
    v[i] = s;
  }
  for (std::size_t i = 0; i < n; ++i) v[i] /= d[i];
  for (std::size_t ii = n; ii-- > 0;) {
    const double xi = v[ii];
    for (std::size_t k = m.start(ii); k < ii; ++k) v[k] -= m.at(ii, k) * xi;
  }
}

// R <- R Exp(dtheta); p <- p + R_old dp.
void retract(double q[4], double p[3], const double delta[6]) {
  double R[9];
  quat_to_matrix(q, R);
  const double dp[3] = {delta[3], delta[4], delta[5]};
  double world_dp[3];
  mat3_apply(R, dp, world_dp);
  p[0] += world_dp[0];
  p[1] += world_dp[1];
  p[2] += world_dp[2];
  const double dth[3] = {delta[0], delta[1], delta[2]};
  double E[9], Rn[9];
  so3_exp(dth, E);
  mat3_mul(R, E, Rn);
  matrix_to_quat(Rn, q);
}

}  // namespace

// --- public helpers ---------------------------------------------------------

void isotropic_information(double sigma_rot_rad, double sigma_trans_m, double out[36]) {
  for (int i = 0; i < 36; ++i) out[i] = 0.0;
  const double wr = sigma_rot_rad > 0.0 ? 1.0 / (sigma_rot_rad * sigma_rot_rad) : 0.0;
  const double wt = sigma_trans_m > 0.0 ? 1.0 / (sigma_trans_m * sigma_trans_m) : 0.0;
  for (int i = 0; i < 3; ++i) out[i * 6 + i] = wr;
  for (int i = 3; i < 6; ++i) out[i * 6 + i] = wt;
}

void between_error(const double qi[4], const double pi[3], const double qj[4], const double pj[3],
                   const double qz[4], const double pz[3], double out[6]) {
  BetweenLinear lin;
  between_linearize(qi, pi, qj, pj, qz, pz, &lin, false);
  for (int i = 0; i < 6; ++i) out[i] = lin.e[i];
}

AteResult absolute_trajectory_error(const std::vector<PoseNode>& estimate,
                                    const std::vector<PoseNode>& truth) {
  AteResult r;
  const std::size_t n = estimate.size() < truth.size() ? estimate.size() : truth.size();
  if (n == 0) return r;
  double sum = 0.0;
  double last = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double dx = estimate[i].p[0] - truth[i].p[0];
    const double dy = estimate[i].p[1] - truth[i].p[1];
    const double dz = estimate[i].p[2] - truth[i].p[2];
    const double e = std::sqrt(dx * dx + dy * dy + dz * dz);
    sum += e * e;
    if (e > r.max_m) r.max_m = e;
    last = e;
  }
  r.count = n;
  r.rms_m = std::sqrt(sum / static_cast<double>(n));
  r.final_m = last;
  return r;
}

// --- PoseGraph --------------------------------------------------------------

struct PoseGraph::Impl {
  std::vector<PoseNode> nodes;
  std::vector<BetweenFactor> between;
  std::vector<PriorFactor> priors;

  // Scratch reused across iterations so a 5,000-node graph does not
  // reallocate 30 times.
  std::vector<std::uint32_t> var_of_node;  // kInvalidNode for fixed
  std::vector<std::uint32_t> pos_of_var;
  Skyline H;
  std::vector<double> g, delta, diag_d;

  double robust_chi2(const std::vector<PoseNode>& est, bool apply_robust) const {
    double total = 0.0;
    for (const BetweenFactor& f : between) {
      BetweenLinear lin;
      between_linearize(est[f.i].q, est[f.i].p, est[f.j].q, est[f.j].p, f.q, f.p, &lin, false);
      double we[6];
      mat6_apply(f.information, lin.e, we);
      const double s = vec6_dot(lin.e, we);
      total += apply_robust ? huber(s, f.huber_delta).cost : s;
    }
    for (const PriorFactor& f : priors) {
      PriorLinear lin;
      prior_linearize(est[f.i].q, est[f.i].p, f.q, f.p, &lin, false);
      double we[6];
      mat6_apply(f.information, lin.e, we);
      const double s = vec6_dot(lin.e, we);
      total += apply_robust ? huber(s, f.huber_delta).cost : s;
    }
    return total;
  }
};

PoseGraph::PoseGraph() : impl_(new Impl) {}
PoseGraph::~PoseGraph() = default;

std::uint32_t PoseGraph::add_node(const double q[4], const double p[3]) {
  PoseNode n;
  for (int i = 0; i < 4; ++i) n.q[i] = q[i];
  se3::quat_normalize(n.q);
  for (int i = 0; i < 3; ++i) n.p[i] = p[i];
  impl_->nodes.push_back(n);
  return static_cast<std::uint32_t>(impl_->nodes.size() - 1);
}

Status PoseGraph::set_fixed(std::uint32_t index, bool fixed) {
  if (index >= impl_->nodes.size()) {
    return set_last_error(ScanError::kInvalidArgument, "pose_graph: node %u out of range", index);
  }
  impl_->nodes[index].fixed = fixed;
  return kOkStatus;
}

Status PoseGraph::set_node(std::uint32_t index, const double q[4], const double p[3]) {
  if (index >= impl_->nodes.size()) {
    return set_last_error(ScanError::kInvalidArgument, "pose_graph: node %u out of range", index);
  }
  PoseNode& n = impl_->nodes[index];
  for (int i = 0; i < 4; ++i) n.q[i] = q[i];
  se3::quat_normalize(n.q);
  for (int i = 0; i < 3; ++i) n.p[i] = p[i];
  return kOkStatus;
}

Status PoseGraph::add_between(const BetweenFactor& f) {
  const std::size_t n = impl_->nodes.size();
  if (f.i >= n || f.j >= n) {
    return set_last_error(ScanError::kInvalidArgument,
                          "pose_graph: between factor (%u,%u) out of range (%zu nodes)", f.i, f.j,
                          n);
  }
  if (f.i == f.j) {
    return set_last_error(ScanError::kInvalidArgument,
                          "pose_graph: between factor on a single node %u", f.i);
  }
  BetweenFactor copy = f;
  se3::quat_normalize(copy.q);
  impl_->between.push_back(copy);
  return kOkStatus;
}

Status PoseGraph::add_between(std::uint32_t i, std::uint32_t j, const double q[4],
                              const double p[3], double sigma_rot_rad, double sigma_trans_m,
                              double huber_delta, bool loop) {
  BetweenFactor f;
  f.i = i;
  f.j = j;
  for (int k = 0; k < 4; ++k) f.q[k] = q[k];
  for (int k = 0; k < 3; ++k) f.p[k] = p[k];
  isotropic_information(sigma_rot_rad, sigma_trans_m, f.information);
  f.huber_delta = huber_delta;
  f.loop = loop;
  return add_between(f);
}

Status PoseGraph::add_prior(const PriorFactor& f) {
  if (f.i >= impl_->nodes.size()) {
    return set_last_error(ScanError::kInvalidArgument, "pose_graph: prior on node %u out of range",
                          f.i);
  }
  PriorFactor copy = f;
  se3::quat_normalize(copy.q);
  impl_->priors.push_back(copy);
  return kOkStatus;
}

Status PoseGraph::add_position_prior(std::uint32_t i, const double xyz[3], double sigma_m,
                                     double huber_delta) {
  if (!(sigma_m > 0.0)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "pose_graph: position prior sigma must be > 0");
  }
  PriorFactor f;
  f.i = i;
  se3::quat_identity(f.q);  // identity: e_trans is then (p_i - xyz), in metres
  for (int k = 0; k < 3; ++k) f.p[k] = xyz[k];
  // Rotation block stays zero: a position fix says nothing about attitude.
  const double w = 1.0 / (sigma_m * sigma_m);
  for (int k = 3; k < 6; ++k) f.information[k * 6 + k] = w;
  f.huber_delta = huber_delta;
  return add_prior(f);
}

double PoseGraph::chi2() const { return impl_->robust_chi2(impl_->nodes, true); }

std::size_t PoseGraph::node_count() const { return impl_->nodes.size(); }
std::size_t PoseGraph::between_count() const { return impl_->between.size(); }
std::size_t PoseGraph::prior_count() const { return impl_->priors.size(); }
std::size_t PoseGraph::loop_count() const {
  std::size_t n = 0;
  for (const BetweenFactor& f : impl_->between) {
    if (f.loop) ++n;
  }
  return n;
}
const PoseNode& PoseGraph::node(std::uint32_t index) const { return impl_->nodes[index]; }
const std::vector<PoseNode>& PoseGraph::nodes() const { return impl_->nodes; }
const std::vector<BetweenFactor>& PoseGraph::between_factors() const { return impl_->between; }
const std::vector<PriorFactor>& PoseGraph::prior_factors() const { return impl_->priors; }

void PoseGraph::clear() {
  impl_->nodes.clear();
  impl_->between.clear();
  impl_->priors.clear();
}

Result<PoseGraphSummary> PoseGraph::optimize(const PoseGraphOptions& opts, CancelToken* cancel) {
  Impl& im = *impl_;
  PoseGraphSummary sum;
  const std::size_t nn = im.nodes.size();
  if (nn == 0) {
    return set_last_error(ScanError::kInvalidArgument, "pose_graph: no nodes to optimize");
  }

  // --- variables --------------------------------------------------------
  im.var_of_node.assign(nn, kInvalidNode);
  std::uint32_t nv = 0;
  for (std::size_t i = 0; i < nn; ++i) {
    if (!im.nodes[i].fixed) im.var_of_node[i] = nv++;
  }
  sum.variables = nv;
  if (nv == 0) {
    // Everything pinned: legal, and the answer is the input.
    sum.initial_chi2 = sum.final_chi2 = im.robust_chi2(im.nodes, true);
    sum.converged = true;
    return sum;
  }

  // --- adjacency over variables, ordering, envelope ---------------------
  std::vector<std::vector<std::uint32_t>> adj(nv);
  for (const BetweenFactor& f : im.between) {
    const std::uint32_t a = im.var_of_node[f.i];
    const std::uint32_t b = im.var_of_node[f.j];
    if (a == kInvalidNode || b == kInvalidNode) continue;
    adj[a].push_back(b);
    adj[b].push_back(a);
  }
  for (std::vector<std::uint32_t>& l : adj) {
    std::sort(l.begin(), l.end());
    l.erase(std::unique(l.begin(), l.end()), l.end());
  }

  const std::vector<std::uint32_t> order = reverse_cuthill_mckee(adj);
  im.pos_of_var.assign(nv, 0);
  for (std::size_t k = 0; k < order.size(); ++k) im.pos_of_var[order[k]] = static_cast<std::uint32_t>(k);

  std::vector<std::uint32_t> first_block(nv, 0);
  std::uint32_t bandwidth = 0;
  for (std::uint32_t v = 0; v < nv; ++v) {
    const std::uint32_t k = im.pos_of_var[v];
    std::uint32_t lo = k;
    for (std::uint32_t w : adj[v]) {
      const std::uint32_t kw = im.pos_of_var[w];
      if (kw < lo) lo = kw;
    }
    first_block[k] = lo;
    if (k - lo > bandwidth) bandwidth = k - lo;
  }
  sum.bandwidth_blocks = bandwidth;

  const std::size_t n = static_cast<std::size_t>(nv) * 6u;
  std::vector<std::uint32_t> row_start(n, 0);
  for (std::uint32_t k = 0; k < nv; ++k) {
    const std::uint32_t s = first_block[k] * 6u;
    for (int a = 0; a < 6; ++a) row_start[k * 6u + a] = s;
  }
  im.H.reset(std::move(row_start));
  sum.envelope_scalars = im.H.stored();
  im.g.assign(n, 0.0);

  // --- Levenberg-Marquardt ---------------------------------------------
  double lambda = opts.initial_lambda;
  double chi2_cur = im.robust_chi2(im.nodes, true);
  sum.initial_chi2 = chi2_cur;
  sum.final_chi2 = chi2_cur;

  std::vector<PoseNode> trial;
  Skyline damped;
  std::vector<double> rhs;

  const std::uint32_t max_trials = opts.max_iterations * 4u + 8u;
  std::uint32_t trials = 0;
  bool need_assembly = true;

  auto scatter = [&](std::uint32_t var_a, const double Ja[36], std::uint32_t var_b,
                     const double Jb[36], const double Omega[36], double w) {
    // H_ab += Ja^T (w Omega) Jb, into the lower triangle only.
    double WJb[36];
    for (int r = 0; r < 6; ++r) {
      for (int c = 0; c < 6; ++c) {
        double s = 0.0;
        for (int k = 0; k < 6; ++k) s += Omega[r * 6 + k] * Jb[k * 6 + c];
        WJb[r * 6 + c] = w * s;
      }
    }
    double M[36];
    mat6_ata_b(Ja, WJb, M);
    const std::size_t ra = static_cast<std::size_t>(im.pos_of_var[var_a]) * 6u;
    const std::size_t rb = static_cast<std::size_t>(im.pos_of_var[var_b]) * 6u;
    for (int r = 0; r < 6; ++r) {
      for (int c = 0; c < 6; ++c) {
        const std::size_t gr = ra + static_cast<std::size_t>(r);
        const std::size_t gc = rb + static_cast<std::size_t>(c);
        if (gr < gc) continue;
        im.H.add(gr, gc, M[r * 6 + c]);
      }
    }
  };

  while (sum.iterations < opts.max_iterations && trials < max_trials) {
    if (cancelled(cancel)) {
      return set_last_error(ScanError::kCancelled, "pose_graph: cancelled");
    }

    if (need_assembly) {
      im.H.zero();
      std::fill(im.g.begin(), im.g.end(), 0.0);
      for (const BetweenFactor& f : im.between) {
        const std::uint32_t va = im.var_of_node[f.i];
        const std::uint32_t vb = im.var_of_node[f.j];
        if (va == kInvalidNode && vb == kInvalidNode) continue;
        BetweenLinear lin;
        between_linearize(im.nodes[f.i].q, im.nodes[f.i].p, im.nodes[f.j].q, im.nodes[f.j].p, f.q,
                          f.p, &lin, true);
        double we[6];
        mat6_apply(f.information, lin.e, we);
        const double s = vec6_dot(lin.e, we);
        const double w = opts.robust_reweight ? huber(s, f.huber_delta).weight : 1.0;
        if (va != kInvalidNode) {
          scatter(va, lin.Ji, va, lin.Ji, f.information, w);
          double gi[6];
          for (int r = 0; r < 6; ++r) {
            double acc = 0.0;
            for (int k = 0; k < 6; ++k) acc += lin.Ji[k * 6 + r] * we[k];
            gi[r] = w * acc;
          }
          const std::size_t base = static_cast<std::size_t>(im.pos_of_var[va]) * 6u;
          for (int r = 0; r < 6; ++r) im.g[base + static_cast<std::size_t>(r)] -= gi[r];
        }
        if (vb != kInvalidNode) {
          scatter(vb, lin.Jj, vb, lin.Jj, f.information, w);
          double gj[6];
          for (int r = 0; r < 6; ++r) {
            double acc = 0.0;
            for (int k = 0; k < 6; ++k) acc += lin.Jj[k * 6 + r] * we[k];
            gj[r] = w * acc;
          }
          const std::size_t base = static_cast<std::size_t>(im.pos_of_var[vb]) * 6u;
          for (int r = 0; r < 6; ++r) im.g[base + static_cast<std::size_t>(r)] -= gj[r];
        }
        if (va != kInvalidNode && vb != kInvalidNode) {
          // Both off-diagonal halves; scatter() drops whichever lands in the
          // strict upper triangle, so calling it twice stores exactly one.
          scatter(va, lin.Ji, vb, lin.Jj, f.information, w);
          scatter(vb, lin.Jj, va, lin.Ji, f.information, w);
        }
      }
      for (const PriorFactor& f : im.priors) {
        const std::uint32_t va = im.var_of_node[f.i];
        if (va == kInvalidNode) continue;
        PriorLinear lin;
        prior_linearize(im.nodes[f.i].q, im.nodes[f.i].p, f.q, f.p, &lin, true);
        double we[6];
        mat6_apply(f.information, lin.e, we);
        const double s = vec6_dot(lin.e, we);
        const double w = opts.robust_reweight ? huber(s, f.huber_delta).weight : 1.0;
        scatter(va, lin.Ji, va, lin.Ji, f.information, w);
        double gi[6];
        for (int r = 0; r < 6; ++r) {
          double acc = 0.0;
          for (int k = 0; k < 6; ++k) acc += lin.Ji[k * 6 + r] * we[k];
          gi[r] = w * acc;
        }
        const std::size_t base = static_cast<std::size_t>(im.pos_of_var[va]) * 6u;
        for (int r = 0; r < 6; ++r) im.g[base + static_cast<std::size_t>(r)] -= gi[r];
      }
      need_assembly = false;
    }

    // Damped copy. Marquardt scaling (diag *= 1+lambda) plus a floor, so a
    // variable that no factor touches still gets a finite pivot instead of
    // making the whole factorization fail.
    damped = im.H;
    for (std::size_t r = 0; r < n; ++r) {
      double& dg = damped.at(r, r);
      dg = dg * (1.0 + lambda) + lambda * 1e-9;
    }
    ++trials;
    if (!skyline_ldlt(&damped, &im.diag_d, cancel)) {
      if (cancelled(cancel)) {
        return set_last_error(ScanError::kCancelled, "pose_graph: cancelled");
      }
      lambda *= opts.lambda_up;
      if (lambda > opts.max_lambda) break;
      ++sum.rejected_steps;
      continue;
    }
    skyline_solve(damped, im.diag_d, im.g, &im.delta);

    // Apply.
    trial = im.nodes;
    double max_rot = 0.0, max_trans = 0.0;
    bool finite = true;
    for (std::size_t i = 0; i < nn; ++i) {
      const std::uint32_t v = im.var_of_node[i];
      if (v == kInvalidNode) continue;
      const std::size_t base = static_cast<std::size_t>(im.pos_of_var[v]) * 6u;
      double d6[6];
      for (int r = 0; r < 6; ++r) {
        d6[r] = im.delta[base + static_cast<std::size_t>(r)];
        if (!std::isfinite(d6[r])) finite = false;
      }
      if (!finite) break;
      for (int r = 0; r < 3; ++r) max_rot = std::max(max_rot, std::fabs(d6[r]));
      for (int r = 3; r < 6; ++r) max_trans = std::max(max_trans, std::fabs(d6[r]));
      retract(trial[i].q, trial[i].p, d6);
    }
    if (!finite) {
      lambda *= opts.lambda_up;
      ++sum.rejected_steps;
      if (lambda > opts.max_lambda) break;
      continue;
    }

    const double chi2_trial = im.robust_chi2(trial, true);
    if (chi2_trial < chi2_cur) {
      im.nodes.swap(trial);
      ++sum.iterations;
      sum.max_rot_step_rad = max_rot;
      sum.max_trans_step_m = max_trans;
      const double rel = chi2_cur > 0.0 ? (chi2_cur - chi2_trial) / chi2_cur : 0.0;
      chi2_cur = chi2_trial;
      sum.final_chi2 = chi2_cur;
      lambda = std::max(lambda * opts.lambda_down, opts.min_lambda);
      need_assembly = true;
      if (rel < opts.relative_chi2_tol ||
          (max_rot < opts.converge_rot_rad && max_trans < opts.converge_trans_m)) {
        sum.converged = true;
        break;
      }
    } else {
      ++sum.rejected_steps;
      lambda *= opts.lambda_up;
      if (lambda > opts.max_lambda) {
        // No further progress is available; that IS convergence for LM.
        sum.converged = true;
        break;
      }
    }
  }

  sum.final_lambda = lambda;
  sum.final_chi2 = im.robust_chi2(im.nodes, true);
  return sum;
}

}  // namespace post
}  // namespace scanengine
