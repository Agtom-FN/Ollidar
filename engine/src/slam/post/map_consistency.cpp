#include "scanengine/slam/post/map_consistency.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace scanengine {
namespace post {
namespace {

struct CellKey {
  std::int64_t x = 0, y = 0, z = 0;
  std::uint32_t window = 0;
  bool operator<(const CellKey& o) const {
    if (window != o.window) return window < o.window;
    if (x != o.x) return x < o.x;
    if (y != o.y) return y < o.y;
    return z < o.z;
  }
  bool same_cell(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct Entry {
  CellKey key;
  float px = 0, py = 0, pz = 0;
};

// Symmetric 3x3 eigen-decomposition by cyclic Jacobi. Fixed iteration count —
// determinism beats the last bit of convergence, and 12 sweeps takes a 3x3
// symmetric matrix to machine precision for any input this sees.
//
// Hand-rolled on purpose (no Eigen; DESIGN.md doctrine), and small enough that
// the whole thing is auditable at a glance.
void jacobi3(const double a_in[6], double eval[3], double evec[9]) {
  // a_in = (xx, xy, xz, yy, yz, zz)
  double m[3][3] = {{a_in[0], a_in[1], a_in[2]},
                    {a_in[1], a_in[3], a_in[4]},
                    {a_in[2], a_in[4], a_in[5]}};
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 12; ++sweep) {
    for (int p = 0; p < 2; ++p) {
      for (int q = p + 1; q < 3; ++q) {
        if (std::fabs(m[p][q]) < 1e-18) continue;
        const double theta = (m[q][q] - m[p][p]) / (2.0 * m[p][q]);
        const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                         (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (int k = 0; k < 3; ++k) {
          const double mkp = m[k][p], mkq = m[k][q];
          m[k][p] = c * mkp - s * mkq;
          m[k][q] = s * mkp + c * mkq;
        }
        for (int k = 0; k < 3; ++k) {
          const double mpk = m[p][k], mqk = m[q][k];
          m[p][k] = c * mpk - s * mqk;
          m[q][k] = s * mpk + c * mqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }
  for (int i = 0; i < 3; ++i) {
    eval[i] = m[i][i];
    for (int k = 0; k < 3; ++k) evec[3 * i + k] = v[k][i];
  }
}

// The mean |distance| of `b` from the plane fitted to `a`, or < 0 when the
// cell has no usable surface. `a`'s plane is the reference in both directions
// the caller uses it, so the answer is not symmetric by construction — which
// is correct: it asks "where does the later pass land relative to the surface
// the earlier pass described".
double offset_along_normal(const Entry* a, std::size_t na, const Entry* b, std::size_t nb,
                           double max_planarity_ratio) {
  double cx = 0, cy = 0, cz = 0;
  for (std::size_t i = 0; i < na; ++i) {
    cx += a[i].px;
    cy += a[i].py;
    cz += a[i].pz;
  }
  const double inv = 1.0 / static_cast<double>(na);
  cx *= inv;
  cy *= inv;
  cz *= inv;

  double s[6] = {0, 0, 0, 0, 0, 0};
  for (std::size_t i = 0; i < na; ++i) {
    const double dx = a[i].px - cx, dy = a[i].py - cy, dz = a[i].pz - cz;
    s[0] += dx * dx;
    s[1] += dx * dy;
    s[2] += dx * dz;
    s[3] += dy * dy;
    s[4] += dy * dz;
    s[5] += dz * dz;
  }
  for (int i = 0; i < 6; ++i) s[i] *= inv;

  double eval[3], evec[9];
  jacobi3(s, eval, evec);
  int lo = 0, hi = 0;
  for (int i = 1; i < 3; ++i) {
    if (eval[i] < eval[lo]) lo = i;
    if (eval[i] > eval[hi]) hi = i;
  }
  if (!(eval[hi] > 0.0)) return -1.0;
  if (eval[lo] / eval[hi] > max_planarity_ratio) return -1.0;

  const double nx = evec[3 * lo + 0], ny = evec[3 * lo + 1], nz = evec[3 * lo + 2];
  double acc = 0.0;
  for (std::size_t i = 0; i < nb; ++i) {
    acc += std::fabs((b[i].px - cx) * nx + (b[i].py - cy) * ny + (b[i].pz - cz) * nz);
  }
  return acc / static_cast<double>(nb);
}

double median_of(std::vector<double>& v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

double percentile_of(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  std::size_t i = static_cast<std::size_t>(p * static_cast<double>(sorted.size()));
  if (i >= sorted.size()) i = sorted.size() - 1;
  return sorted[i];
}

}  // namespace

MapConsistencyReport measure_map_consistency(const std::vector<PointVertex>& points,
                                             const std::vector<std::int64_t>& point_times_ns,
                                             const MapConsistencyConfig& cfg) {
  MapConsistencyReport rep;
  rep.window_seconds = cfg.window_seconds;
  const std::size_t n = std::min(points.size(), point_times_ns.size());
  rep.points = n;
  if (n < 2 * cfg.min_points_per_window) {
    rep.blocker = "too few points";
    return rep;
  }
  if (!(cfg.cell_m > 0.0) || !(cfg.window_seconds > 0.0)) {
    rep.blocker = "bad configuration";
    return rep;
  }

  std::int64_t t0 = point_times_ns[0], t1 = point_times_ns[0];
  for (std::size_t i = 1; i < n; ++i) {
    t0 = std::min(t0, point_times_ns[i]);
    t1 = std::max(t1, point_times_ns[i]);
  }
  const double span_s = static_cast<double>(t1 - t0) * 1e-9;
  if (span_s < cfg.window_seconds) {
    rep.blocker = "capture shorter than one window";
    return rep;
  }
  const std::size_t nwin =
      static_cast<std::size_t>(span_s / cfg.window_seconds) + 1;
  rep.windows = nwin;

  const double inv_cell = 1.0 / cfg.cell_m;
  const double win_ns = cfg.window_seconds * 1e9;

  std::vector<Entry> all;
  all.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const PointVertex& p = points[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
    Entry e;
    e.px = p.x;
    e.py = p.y;
    e.pz = p.z;
    e.key.x = static_cast<std::int64_t>(std::floor(static_cast<double>(p.x) * inv_cell));
    e.key.y = static_cast<std::int64_t>(std::floor(static_cast<double>(p.y) * inv_cell));
    e.key.z = static_cast<std::int64_t>(std::floor(static_cast<double>(p.z) * inv_cell));
    const double dt = static_cast<double>(point_times_ns[i] - t0);
    std::size_t w = static_cast<std::size_t>(dt / win_ns);
    if (w >= nwin) w = nwin - 1;
    e.key.window = static_cast<std::uint32_t>(w);
    all.push_back(e);
  }
  // One sort, and every traversal below is over this order — which is what
  // makes the answer independent of the order points arrived in.
  //
  // The comparator is TOTAL, tie-breaking on the coordinates themselves, and
  // that is load-bearing rather than tidy. `std::sort` is not stable, so
  // ordering only by (window, cell) would leave the order WITHIN a cell
  // unspecified — and the plane fit sums floats over exactly that order, so
  // two runs over the same points arriving differently would differ in the
  // last bits. The first version of this file did that and the determinism
  // test caught it at 1 ULP. A number that gets quoted in a field report has
  // to be bit-identical, not approximately identical.
  std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
    if (a.key < b.key) return true;
    if (b.key < a.key) return false;
    if (a.px != b.px) return a.px < b.px;
    if (a.py != b.py) return a.py < b.py;
    return a.pz < b.pz;
  });

  // Runs of equal (window, cell).
  struct Run {
    std::size_t begin = 0, end = 0;
  };
  std::vector<Run> runs;
  for (std::size_t i = 0; i < all.size();) {
    std::size_t j = i + 1;
    while (j < all.size() && all[j].key.window == all[i].key.window &&
           all[j].key.same_cell(all[i].key)) {
      ++j;
    }
    if (j - i >= cfg.min_points_per_window) runs.push_back({i, j});
    i = j;
  }
  if (runs.size() < 2) {
    rep.blocker = "no cell holds enough points";
    return rep;
  }

  // --- the floor: one window against itself, split in half -----------------
  {
    std::vector<double> self;
    for (const Run& r : runs) {
      const std::size_t cnt = r.end - r.begin;
      if (cnt < 2 * cfg.min_points_per_window) continue;
      const std::size_t half = cnt / 2;
      const double d = offset_along_normal(&all[r.begin], half, &all[r.begin + half], cnt - half,
                                           cfg.max_planarity_ratio);
      if (d >= 0.0) self.push_back(d);
    }
    rep.self_cells = self.size();
    rep.self_floor_m = median_of(self);
  }

  // --- shared cells across windows -----------------------------------------
  //
  // `runs` is sorted by (window, cell), so the same cell in two windows is not
  // adjacent. Re-index by cell: collect (cell, window, run) and sort by cell.
  struct Ref {
    std::int64_t x, y, z;
    std::uint32_t window;
    std::size_t run;
    bool operator<(const Ref& o) const {
      if (x != o.x) return x < o.x;
      if (y != o.y) return y < o.y;
      if (z != o.z) return z < o.z;
      return window < o.window;
    }
  };
  std::vector<Ref> refs;
  refs.reserve(runs.size());
  for (std::size_t k = 0; k < runs.size(); ++k) {
    const CellKey& c = all[runs[k].begin].key;
    refs.push_back({c.x, c.y, c.z, c.window, k});
  }
  std::sort(refs.begin(), refs.end());

  std::vector<std::vector<double>> buckets(cfg.max_separation + 1);
  for (std::size_t i = 0; i < refs.size();) {
    std::size_t j = i + 1;
    while (j < refs.size() && refs[j].x == refs[i].x && refs[j].y == refs[i].y &&
           refs[j].z == refs[i].z) {
      ++j;
    }
    for (std::size_t a = i; a < j; ++a) {
      for (std::size_t b = a + 1; b < j; ++b) {
        const std::size_t sep = refs[b].window - refs[a].window;
        if (sep == 0 || sep > cfg.max_separation) continue;
        const Run& ra = runs[refs[a].run];
        const Run& rb = runs[refs[b].run];
        const double d = offset_along_normal(&all[ra.begin], ra.end - ra.begin, &all[rb.begin],
                                             rb.end - rb.begin, cfg.max_planarity_ratio);
        if (d >= 0.0) buckets[sep].push_back(d);
      }
    }
    i = j;
  }

  for (std::size_t sep = 1; sep <= cfg.max_separation; ++sep) {
    if (buckets[sep].empty()) continue;
    std::sort(buckets[sep].begin(), buckets[sep].end());
    MapConsistencySeparation s;
    s.separation = sep;
    s.seconds = static_cast<double>(sep) * cfg.window_seconds;
    s.median_offset_m = percentile_of(buckets[sep], 0.5);
    s.p90_offset_m = percentile_of(buckets[sep], 0.9);
    s.cells = buckets[sep].size();
    rep.by_separation.push_back(s);
  }

  if (rep.by_separation.empty()) {
    rep.blocker = "no surface was painted twice";
    return rep;
  }
  rep.measurable = true;
  rep.nearest_offset_m = rep.by_separation.front().median_offset_m;
  rep.nearest_separation = rep.by_separation.front().separation;
  return rep;
}

}  // namespace post
}  // namespace scanengine
