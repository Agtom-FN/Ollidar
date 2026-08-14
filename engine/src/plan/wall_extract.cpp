// wall_extract.cpp — steps 2-4 of §3.6: RANSAC line extraction over the
// occupancy grid, orthogonality snapping, face pairing, collinear merge with
// gap analysis (openings), and corner intersection trimming/joining.
//
// WHY SEQUENTIAL RANSAC AND NOT A HOUGH TRANSFORM. A Hough accumulator over
// (rho, theta) needs a bin size, and the bin size is exactly the thing that
// decides whether the two faces of a 100 mm partition are one wall or two —
// the single most consequential judgement in this whole file. RANSAC puts
// that decision on a distance in metres (`WallOptions::inlier_m`) that a user
// can reason about, gives an explicit inlier set (so residuals, coverage and
// a confidence are free), and degrades gracefully on a non-Manhattan plan,
// where a Hough peak-picker starts inventing walls out of accumulator ridges.
//
// DETERMINISM. The cell list arrives in row-major order (occupancy.cpp), the
// PRNG is this file's own splitmix64 with a seed from WallOptions, ties are
// broken toward the first candidate found, and every sort is a total order.
// Nothing here iterates a hash container.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "plan_internal.h"

namespace scanengine {
namespace plan {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kHalfPi = kPi / 2.0;

double deg2rad(double d) { return d * kPi / 180.0; }

// Fold an angle difference into (-period/2, period/2].
double fold(double a, double period) {
  const double k = std::floor(a / period + 0.5);
  return a - k * period;
}

double line_heading(Vec2 d) {
  double t = std::atan2(d.y, d.x);
  while (t < 0.0) t += kPi;
  while (t >= kPi) t -= kPi;
  return t;
}

// splitmix64 — 8 lines, no <random>, identical on every platform. std::mt19937
// would also be portable but its *distributions* are not specified, and
// `% n` on a 64-bit stream is both specified and good enough for choosing a
// cell index.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint32_t below(std::uint32_t n) {
    return n == 0 ? 0u : static_cast<std::uint32_t>(next() % n);
  }
};

struct Interval {
  double lo = 0.0;
  double hi = 0.0;
  double length() const { return hi - lo; }
};

// A line fitted to occupied cells, plus where along it the cells actually are.
struct FitLine {
  Vec2 p;  // a point on the line (the inlier centroid)
  Vec2 d;  // unit direction
  std::vector<std::uint32_t> inliers;
  std::vector<Interval> runs;  // parameter along d, measured from p
  double rms = 0.0;
  double total_run_m = 0.0;
  bool snapped = false;
  bool paired = false;
};

// A wall: one line (single face) or two paired faces reduced to a centerline.
struct WallLine {
  Vec2 p;
  Vec2 d;
  double thickness = 0.10;
  WallEvidence evidence = WallEvidence::kSingleFace;
  std::vector<Interval> intervals;
  double rms = 0.0;
  std::uint32_t support = 0;
  bool snapped = false;
};

double point_line_distance(Vec2 q, Vec2 p, Vec2 d) { return std::fabs(cross(d, q - p)); }

std::vector<Interval> merge_intervals(std::vector<Interval> in, double join_gap) {
  if (in.empty()) return in;
  std::sort(in.begin(), in.end(), [](const Interval& a, const Interval& b) {
    if (a.lo != b.lo) return a.lo < b.lo;
    return a.hi < b.hi;
  });
  std::vector<Interval> out;
  out.push_back(in[0]);
  for (std::size_t k = 1; k < in.size(); ++k) {
    if (in[k].lo - out.back().hi <= join_gap) {
      out.back().hi = std::max(out.back().hi, in[k].hi);
    } else {
      out.push_back(in[k]);
    }
  }
  return out;
}

double intervals_total(const std::vector<Interval>& v) {
  double t = 0.0;
  for (const auto& i : v) t += i.length();
  return t;
}

double intervals_overlap(const std::vector<Interval>& a, const std::vector<Interval>& b) {
  double total = 0.0;
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const double lo = std::max(a[i].lo, b[j].lo);
    const double hi = std::min(a[i].hi, b[j].hi);
    if (hi > lo) total += hi - lo;
    if (a[i].hi < b[j].hi) {
      ++i;
    } else {
      ++j;
    }
  }
  return total;
}

// Project the inliers onto (p, d), split them into contiguous runs, pad each
// run by half a cell (a cell centre sits half a cell short of the surface it
// represents, and that half cell is exactly the systematic error a door
// width would otherwise carry).
void recompute_runs(const std::vector<Vec2>& cells, FitLine* line, double res, double run_gap,
                    double min_run) {
  line->runs.clear();
  line->total_run_m = 0.0;
  if (line->inliers.empty()) return;
  std::vector<double> t;
  t.reserve(line->inliers.size());
  for (std::uint32_t idx : line->inliers) t.push_back(dot(cells[idx] - line->p, line->d));
  std::sort(t.begin(), t.end());

  const double pad = res * 0.5;
  double start = t[0];
  double prev = t[0];
  std::vector<Interval> runs;
  for (std::size_t k = 1; k < t.size(); ++k) {
    if (t[k] - prev > run_gap) {
      runs.push_back(Interval{start - pad, prev + pad});
      start = t[k];
    }
    prev = t[k];
  }
  runs.push_back(Interval{start - pad, prev + pad});

  for (const auto& r : runs) {
    if (r.length() >= min_run) line->runs.push_back(r);
  }
  line->total_run_m = intervals_total(line->runs);
}

// Total-least-squares refit: the principal axis of the inliers' scatter.
void refit(const std::vector<Vec2>& cells, FitLine* line) {
  const std::size_t n = line->inliers.size();
  if (n < 2) return;
  double cx = 0.0, cy = 0.0;
  for (std::uint32_t idx : line->inliers) {
    cx += cells[idx].x;
    cy += cells[idx].y;
  }
  const double inv = 1.0 / static_cast<double>(n);
  cx *= inv;
  cy *= inv;
  double sxx = 0.0, sxy = 0.0, syy = 0.0;
  for (std::uint32_t idx : line->inliers) {
    const double dx = cells[idx].x - cx;
    const double dy = cells[idx].y - cy;
    sxx += dx * dx;
    sxy += dx * dy;
    syy += dy * dy;
  }
  const double theta = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
  line->p = Vec2{cx, cy};
  line->d = Vec2{std::cos(theta), std::sin(theta)};
}

void recompute_rms(const std::vector<Vec2>& cells, FitLine* line) {
  if (line->inliers.empty()) {
    line->rms = 0.0;
    return;
  }
  double acc = 0.0;
  for (std::uint32_t idx : line->inliers) {
    const double e = cross(line->d, cells[idx] - line->p);
    acc += e * e;
  }
  line->rms = std::sqrt(acc / static_cast<double>(line->inliers.size()));
}

// --- step 2: sequential RANSAC ---------------------------------------------

std::vector<FitLine> ransac_lines(const std::vector<Vec2>& cells, const WallOptions& w,
                                  double res, PlanCancelToken* cancel) {
  std::vector<FitLine> out;
  if (cells.size() < 4) return out;

  const std::uint32_t min_inliers =
      std::max<std::uint32_t>(12u, static_cast<std::uint32_t>(w.min_wall_length_m / res));

  std::vector<std::uint32_t> remaining(cells.size());
  for (std::uint32_t k = 0; k < cells.size(); ++k) remaining[k] = k;

  Rng rng(w.seed);
  std::vector<std::uint32_t> best_inliers;
  std::vector<std::uint32_t> trial;

  while (out.size() < w.max_lines && remaining.size() >= min_inliers) {
    if (cancelled(cancel)) break;
    const std::uint32_t m = static_cast<std::uint32_t>(remaining.size());
    Vec2 best_p{}, best_d{1.0, 0.0};
    std::size_t best_count = 0;
    std::uint32_t budget = w.iterations;

    for (std::uint32_t it = 0; it < budget; ++it) {
      const std::uint32_t ia = rng.below(m);
      const std::uint32_t ib = rng.below(m);
      if (ia == ib) continue;
      const Vec2 a = cells[remaining[ia]];
      const Vec2 b = cells[remaining[ib]];
      const Vec2 ab = b - a;
      if (length(ab) < w.min_sample_separation_m) continue;
      const Vec2 d = normalized(ab);

      std::size_t count = 0;
      for (std::uint32_t idx : remaining) {
        if (point_line_distance(cells[idx], a, d) <= w.inlier_m) ++count;
      }
      if (count > best_count) {
        best_count = count;
        best_p = a;
        best_d = d;
        // Adaptive early-out: once a hypothesis explains a fraction `frac` of
        // what is left, the chance that 300 more samples beat it is tiny.
        const double frac = static_cast<double>(count) / static_cast<double>(m);
        if (frac > 0.02 && frac < 0.999) {
          const double need = std::log(1e-4) / std::log(1.0 - frac * frac);
          const std::uint32_t n_need =
              static_cast<std::uint32_t>(std::min<double>(w.iterations, std::ceil(need)));
          budget = std::min(budget, std::max<std::uint32_t>(n_need, it + 8u));
        }
      }
    }

    if (best_count < min_inliers) break;

    FitLine line;
    line.p = best_p;
    line.d = best_d;
    // Two refit rounds: the first pulls the line off the two seed cells and
    // onto the whole face, the second re-collects the inliers that move
    // brought in. A third changes nothing measurable.
    for (int round = 0; round < 3; ++round) {
      trial.clear();
      for (std::uint32_t idx : remaining) {
        if (point_line_distance(cells[idx], line.p, line.d) <= w.inlier_m) trial.push_back(idx);
      }
      if (trial.size() < 2) break;
      line.inliers = trial;
      if (round < 2) refit(cells, &line);
    }
    if (line.inliers.size() < min_inliers) break;

    recompute_rms(cells, &line);
    recompute_runs(cells, &line, res, w.run_gap_m, w.min_run_m);

    // Whatever this line explained is consumed, accepted or not: leaving it
    // in would make the next iteration re-find the identical hypothesis.
    best_inliers = line.inliers;
    std::sort(best_inliers.begin(), best_inliers.end());
    std::vector<std::uint32_t> keep;
    keep.reserve(remaining.size() - best_inliers.size());
    std::set_difference(remaining.begin(), remaining.end(), best_inliers.begin(),
                        best_inliers.end(), std::back_inserter(keep));
    remaining.swap(keep);

    if (line.total_run_m >= w.min_wall_length_m) out.push_back(std::move(line));
  }
  return out;
}

// --- step 2b: merge collinear lines -----------------------------------------
//
// Sequential RANSAC finds one line per pass, so a single wall face whose
// occupied cells are two or three deep can come back as two or three nearly
// coincident lines: the first pass takes the core of the band, and a later
// pass fits the tail that the first one's inlier distance did not reach. It
// also splits ONE straight wall into two lines when a long occlusion falls in
// the middle. Both cases are the same fix — lines that agree in heading and
// in perpendicular offset are the same wall, whatever their extents — and it
// has to happen before pairing, or a tail line pairs with its own core and
// invents a 30 mm-thick wall.
//
// The offset threshold is therefore bounded from above by
// WallOptions::thickness_min_m: merge further than that and the two faces of
// the thinnest partition you want to resolve become one line.
void merge_collinear(const std::vector<Vec2>& cells, std::vector<FitLine>* lines,
                     const WallOptions& w, double res) {
  const std::size_t n = lines->size();
  if (n < 2) return;
  const double ang_tol = deg2rad(w.collinear_angle_deg);

  std::vector<std::size_t> parent(n);
  for (std::size_t k = 0; k < n; ++k) parent[k] = k;
  std::function<std::size_t(std::size_t)> find = [&](std::size_t k) {
    while (parent[k] != k) {
      parent[k] = parent[parent[k]];
      k = parent[k];
    }
    return k;
  };

  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      const FitLine& A = (*lines)[i];
      const FitLine& B = (*lines)[j];
      if (std::fabs(fold(line_heading(A.d) - line_heading(B.d), kPi)) > ang_tol) continue;
      // Symmetric offset test: measuring B's centroid against A's line and
      // A's centroid against B's line, and requiring both, keeps a pair of
      // long lines meeting at a shallow angle from merging on the strength of
      // one lucky centroid.
      if (std::fabs(cross(A.d, B.p - A.p)) > w.collinear_offset_m) continue;
      if (std::fabs(cross(B.d, A.p - B.p)) > w.collinear_offset_m) continue;
      const std::size_t ra = find(i);
      const std::size_t rb = find(j);
      if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
    }
  }

  std::vector<FitLine> merged;
  std::vector<std::size_t> slot_of(n, static_cast<std::size_t>(-1));
  for (std::size_t k = 0; k < n; ++k) {
    const std::size_t r = find(k);
    if (slot_of[r] == static_cast<std::size_t>(-1)) {
      slot_of[r] = merged.size();
      merged.push_back((*lines)[r]);
      merged.back().inliers.clear();
    }
    FitLine& dst = merged[slot_of[r]];
    const FitLine& src = (*lines)[k];
    dst.inliers.insert(dst.inliers.end(), src.inliers.begin(), src.inliers.end());
    dst.snapped = dst.snapped || src.snapped;
  }
  for (auto& l : merged) {
    std::sort(l.inliers.begin(), l.inliers.end());
    refit(cells, &l);
    recompute_rms(cells, &l);
    recompute_runs(cells, &l, res, w.run_gap_m, w.min_run_m);
  }
  // A merge can only ever lengthen a line, so nothing that survived RANSAC's
  // length gate can fail it here; the filter is kept for the degenerate case
  // where a refit over two shallow-angle groups collapses the runs.
  std::vector<FitLine> kept;
  for (auto& l : merged) {
    if (l.total_run_m >= w.min_wall_length_m) kept.push_back(std::move(l));
  }
  *lines = std::move(kept);
}

// --- step 3: dominant directions + orthogonality snapping -------------------

double estimate_dominant_angle(const std::vector<FitLine>& lines, double min_len) {
  double c = 0.0, s = 0.0;
  bool any = false;
  for (const auto& l : lines) {
    if (l.total_run_m < min_len) continue;
    any = true;
    const double th = line_heading(l.d);
    c += l.total_run_m * std::cos(4.0 * th);
    s += l.total_run_m * std::sin(4.0 * th);
  }
  if (!any) {
    for (const auto& l : lines) {
      const double th = line_heading(l.d);
      c += l.total_run_m * std::cos(4.0 * th);
      s += l.total_run_m * std::sin(4.0 * th);
      any = true;
    }
  }
  if (!any || (c == 0.0 && s == 0.0)) return 0.0;
  // The x4 folds the pi/2 symmetry of a rectangular building into one full
  // turn, so a circular mean over it is well defined.
  double th0 = std::atan2(s, c) / 4.0;
  while (th0 < 0.0) th0 += kHalfPi;
  while (th0 >= kHalfPi) th0 -= kHalfPi;
  return th0;
}

void snap_orthogonal(const std::vector<Vec2>& cells, std::vector<FitLine>* lines, double th0,
                     double tol_rad, const WallOptions& w, double res, std::uint32_t* snapped) {
  for (auto& l : *lines) {
    const double th = line_heading(l.d);
    const double delta = fold(th - th0, kHalfPi);
    if (std::fabs(delta) > tol_rad) continue;
    const double th_new = th - delta;
    l.d = Vec2{std::cos(th_new), std::sin(th_new)};
    l.snapped = true;
    ++*snapped;
    // Rotating about the inlier centroid keeps the line where the evidence
    // is; rotating about an endpoint would translate the far end by up to
    // length * sin(7 deg) = 1.2 m on a 10 m wall.
    recompute_rms(cells, &l);
    recompute_runs(cells, &l, res, w.run_gap_m, w.min_run_m);
  }
}

// --- step 4a: face pairing --------------------------------------------------

struct PairCandidate {
  std::size_t i = 0;
  std::size_t j = 0;
  double overlap = 0.0;
  double offset = 0.0;
};

std::vector<WallLine> pair_faces(std::vector<FitLine>& lines, const WallOptions& w) {
  const double ang_tol = deg2rad(w.pair_angle_deg);
  std::vector<PairCandidate> cands;

  if (w.pair_faces) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
      for (std::size_t j = i + 1; j < lines.size(); ++j) {
        const FitLine& A = lines[i];
        const FitLine& B = lines[j];
        // Headings live in [0, pi), so their difference folds by PI, not by
        // PI/2: folding by PI/2 would make two PERPENDICULAR walls look
        // parallel and let them pair into a nonexistent thick wall.
        const double da = std::fabs(fold(line_heading(A.d) - line_heading(B.d), kPi));
        if (da > ang_tol) continue;
        const double off = cross(A.d, B.p - A.p);
        const double abs_off = std::fabs(off);
        if (abs_off < w.thickness_min_m || abs_off > w.thickness_max_m) continue;

        // B's runs, expressed in A's parameter.
        const bool flip = dot(A.d, B.d) < 0.0;
        const double shift = dot(B.p - A.p, A.d);
        std::vector<Interval> bruns;
        bruns.reserve(B.runs.size());
        for (const auto& r : B.runs) {
          if (flip) {
            bruns.push_back(Interval{-r.hi + shift, -r.lo + shift});
          } else {
            bruns.push_back(Interval{r.lo + shift, r.hi + shift});
          }
        }
        std::sort(bruns.begin(), bruns.end(),
                  [](const Interval& x, const Interval& y) { return x.lo < y.lo; });
        const double ov = intervals_overlap(A.runs, bruns);
        const double need =
            w.pair_min_overlap_frac * std::min(A.total_run_m, B.total_run_m);
        if (ov < need || ov <= 0.0) continue;
        cands.push_back(PairCandidate{i, j, ov, off});
      }
    }
    // Greedy, best overlap first; the (i, j) tie-break keeps it deterministic.
    std::sort(cands.begin(), cands.end(), [](const PairCandidate& a, const PairCandidate& b) {
      if (a.overlap != b.overlap) return a.overlap > b.overlap;
      if (a.i != b.i) return a.i < b.i;
      return a.j < b.j;
    });
  }

  std::vector<WallLine> walls;
  for (const auto& c : cands) {
    FitLine& A = lines[c.i];
    FitLine& B = lines[c.j];
    if (A.paired || B.paired) continue;
    A.paired = true;
    B.paired = true;

    WallLine wl;
    wl.d = A.d;
    wl.thickness = std::fabs(c.offset);
    wl.evidence = WallEvidence::kPairedFaces;
    // The centerline sits half the measured thickness off face A, toward B.
    wl.p = A.p + left_normal(A.d) * (c.offset * 0.5);
    wl.rms = 0.5 * (A.rms + B.rms);
    wl.snapped = A.snapped && B.snapped;
    wl.support = static_cast<std::uint32_t>(A.inliers.size() + B.inliers.size());

    // Union of both faces' runs. This is the payoff of pairing: a stretch of
    // wall that one face lost to occlusion is recovered from the other, and
    // a gap that BOTH faces show (a doorway) survives as a gap.
    const bool flip = dot(A.d, B.d) < 0.0;
    const double shift = dot(B.p - A.p, A.d);
    std::vector<Interval> all = A.runs;
    for (const auto& r : B.runs) {
      if (flip) {
        all.push_back(Interval{-r.hi + shift, -r.lo + shift});
      } else {
        all.push_back(Interval{r.lo + shift, r.hi + shift});
      }
    }
    wl.intervals = merge_intervals(std::move(all), 0.0);
    walls.push_back(std::move(wl));
  }

  // A single-face line that lies INSIDE an already-paired wall is not a wall:
  // it is the noise band between that wall's two faces, which sequential
  // RANSAC will happily fit once the faces themselves have been consumed. It
  // survives the collinear merge because it is further than
  // `collinear_offset_m` from either face (that threshold has to stay below
  // half the thinnest partition, or the faces would merge into it). The test
  // that catches it is the one thing that is always true: no wall lives
  // inside another wall.
  auto inside_paired_wall = [&](const FitLine& l) {
    for (const auto& wl : walls) {
      if (wl.evidence != WallEvidence::kPairedFaces) continue;
      if (std::fabs(fold(line_heading(wl.d) - line_heading(l.d), kPi)) > ang_tol) continue;
      if (std::fabs(cross(wl.d, l.p - wl.p)) > wl.thickness * 0.5) continue;
      const double shift = dot(l.p - wl.p, wl.d);
      std::vector<Interval> lruns;
      lruns.reserve(l.runs.size());
      const bool flip = dot(wl.d, l.d) < 0.0;
      for (const auto& r : l.runs) {
        lruns.push_back(flip ? Interval{-r.hi + shift, -r.lo + shift}
                             : Interval{r.lo + shift, r.hi + shift});
      }
      std::sort(lruns.begin(), lruns.end(),
                [](const Interval& x, const Interval& y) { return x.lo < y.lo; });
      if (intervals_overlap(wl.intervals, lruns) > 0.5 * intervals_total(lruns)) return true;
    }
    return false;
  };

  for (auto& l : lines) {
    if (l.paired) continue;
    if (inside_paired_wall(l)) continue;
    WallLine wl;
    wl.p = l.p;
    wl.d = l.d;
    wl.thickness = w.default_thickness_m;
    wl.evidence = WallEvidence::kSingleFace;
    wl.intervals = merge_intervals(l.runs, 0.0);
    wl.rms = l.rms;
    wl.snapped = l.snapped;
    wl.support = static_cast<std::uint32_t>(l.inliers.size());
    walls.push_back(std::move(wl));
  }
  return walls;
}

// --- step 4b: gaps -> segments + openings ----------------------------------

float wall_confidence(const WallSegment& s, double inlier_m, double res) {
  const double residual_term =
      inlier_m > 0.0 ? std::max(0.0, 1.0 - s.rms_residual_m / inlier_m) : 0.0;
  const double coverage_term = std::min(1.0, std::max(0.0, s.coverage));
  const double expect = std::max(1.0, s.length() / res * 2.0);
  const double support_term =
      std::min(1.0, static_cast<double>(s.support_cells) / expect);
  double c = 0.40 * coverage_term + 0.35 * residual_term + 0.25 * support_term;
  if (s.evidence == WallEvidence::kPairedFaces) c += 0.05;
  return static_cast<float>(std::min(1.0, std::max(0.0, c)));
}

void segments_from_wall_line(const WallLine& wl, const PlanOptions& o, double res,
                             std::uint32_t* next_wall_id, std::uint32_t* next_open_id,
                             std::vector<WallSegment>* walls_out,
                             std::vector<Opening>* openings_out) {
  const OpeningOptions& oo = o.openings;
  std::vector<Interval> iv = merge_intervals(wl.intervals, oo.min_gap_m);
  if (iv.empty()) return;

  struct Piece {
    double lo = 0.0;
    double hi = 0.0;
    double covered = 0.0;
    std::vector<Interval> gaps;
  };
  std::vector<Piece> pieces;
  Piece cur;
  cur.lo = iv[0].lo;
  cur.hi = iv[0].hi;
  cur.covered = iv[0].length();
  for (std::size_t k = 1; k < iv.size(); ++k) {
    const double gap = iv[k].lo - cur.hi;
    if (oo.enabled && gap <= oo.max_bridge_m) {
      cur.gaps.push_back(Interval{cur.hi, iv[k].lo});
      cur.hi = iv[k].hi;
      cur.covered += iv[k].length();
    } else {
      pieces.push_back(cur);
      cur = Piece{};
      cur.lo = iv[k].lo;
      cur.hi = iv[k].hi;
      cur.covered = iv[k].length();
    }
  }
  pieces.push_back(cur);

  for (const auto& pc : pieces) {
    const double len = pc.hi - pc.lo;
    if (len < o.walls.min_wall_length_m) continue;

    WallSegment s;
    s.id = (*next_wall_id)++;
    s.a = wl.p + wl.d * pc.lo;
    s.b = wl.p + wl.d * pc.hi;
    s.thickness_m = wl.thickness;
    s.evidence = wl.evidence;
    s.rms_residual_m = wl.rms;
    s.coverage = len > 0.0 ? pc.covered / len : 0.0;
    s.support_cells = wl.support;
    s.snapped = wl.snapped;
    s.confidence = wall_confidence(s, o.walls.inlier_m, res);
    walls_out->push_back(s);

    for (const auto& g : pc.gaps) {
      Opening op;
      op.id = (*next_open_id)++;
      op.wall_id = s.id;
      op.a = wl.p + wl.d * g.lo;
      op.b = wl.p + wl.d * g.hi;
      op.width_m = g.length();
      if (op.width_m < oo.door_min_m) {
        op.kind = OpeningKind::kNarrowGap;
        op.confidence = 0.25f;
      } else if (op.width_m <= oo.door_max_m) {
        op.kind = OpeningKind::kDoorCandidate;
        op.confidence = 0.60f;
      } else {
        op.kind = OpeningKind::kWideOpening;
        op.confidence = 0.45f;
      }
      openings_out->push_back(op);
    }
  }
}

// --- step 4c: corner intersection trimming / joining ------------------------

bool line_intersection(Vec2 p1, Vec2 d1, Vec2 p2, Vec2 d2, Vec2* out) {
  const double den = cross(d1, d2);
  if (std::fabs(den) < 1e-9) return false;
  const double t = cross(p2 - p1, d2) / den;
  *out = p1 + d1 * t;
  return true;
}

void join_corners(std::vector<WallSegment>* walls, const WallOptions& w) {
  const std::size_t n = walls->size();
  if (n < 2) return;
  const double min_ang = deg2rad(w.corner_min_angle_deg);

  // Every move is computed from the ORIGINAL geometry and applied at the end,
  // so the result does not depend on the order walls happen to be in.
  std::vector<Vec2> move_a(n), move_b(n);
  std::vector<double> cost_a(n, 1e30), cost_b(n, 1e30);

  for (std::size_t i = 0; i < n; ++i) {
    const WallSegment& A = (*walls)[i];
    const Vec2 da = A.direction();
    for (std::size_t j = 0; j < n; ++j) {
      if (i == j) continue;
      const WallSegment& B = (*walls)[j];
      const Vec2 db = B.direction();
      // Fold by PI (see pair_faces): under a PI/2 fold every right-angle
      // corner in the building reads as 0 degrees and no corner is ever
      // joined, which is exactly the case this trims.
      const double ang = std::fabs(fold(line_heading(da) - line_heading(db), kPi));
      if (ang < min_ang) continue;
      Vec2 x{};
      if (!line_intersection(A.a, da, B.a, db, &x)) continue;
      // The intersection must be plausibly ON B (its ends may be short by up
      // to corner_join_m), otherwise two far-apart walls would drag each
      // other's endpoints to a meeting point neither of them reaches.
      const double sb = dot(x - B.a, db);
      if (sb < -w.corner_join_m || sb > B.length() + w.corner_join_m) continue;

      const double ca = distance(A.a, x);
      if (ca <= w.corner_join_m && ca < cost_a[i]) {
        cost_a[i] = ca;
        move_a[i] = x;
      }
      const double cb = distance(A.b, x);
      if (cb <= w.corner_join_m && cb < cost_b[i]) {
        cost_b[i] = cb;
        move_b[i] = x;
      }
    }
  }

  for (std::size_t i = 0; i < n; ++i) {
    if (cost_a[i] < 1e29) (*walls)[i].a = move_a[i];
    if (cost_b[i] < 1e29) (*walls)[i].b = move_b[i];
  }

  // Weld: endpoints that ended up within weld_m become literally the same
  // coordinate, which is what lets the room stage build a planar graph with
  // shared vertices instead of near-misses.
  struct EndRef {
    std::size_t wall;
    bool is_a;
  };
  std::vector<EndRef> ends;
  ends.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    ends.push_back(EndRef{i, true});
    ends.push_back(EndRef{i, false});
  }
  const std::size_t e = ends.size();
  std::vector<std::size_t> parent(e);
  for (std::size_t k = 0; k < e; ++k) parent[k] = k;
  std::function<std::size_t(std::size_t)> find = [&](std::size_t k) {
    while (parent[k] != k) {
      parent[k] = parent[parent[k]];
      k = parent[k];
    }
    return k;
  };
  auto pos = [&](std::size_t k) {
    const EndRef& r = ends[k];
    return r.is_a ? (*walls)[r.wall].a : (*walls)[r.wall].b;
  };
  for (std::size_t k = 0; k < e; ++k) {
    for (std::size_t l = k + 1; l < e; ++l) {
      if (distance(pos(k), pos(l)) <= w.weld_m) {
        const std::size_t rk = find(k);
        const std::size_t rl = find(l);
        if (rk != rl) parent[std::max(rk, rl)] = std::min(rk, rl);
      }
    }
  }
  std::vector<Vec2> sum(e, Vec2{});
  std::vector<std::uint32_t> cnt(e, 0);
  for (std::size_t k = 0; k < e; ++k) {
    const std::size_t r = find(k);
    sum[r] = sum[r] + pos(k);
    ++cnt[r];
  }
  for (std::size_t k = 0; k < e; ++k) {
    const std::size_t r = find(k);
    if (cnt[r] < 2) continue;
    const Vec2 avg = sum[r] * (1.0 / static_cast<double>(cnt[r]));
    const EndRef& ref = ends[k];
    if (ref.is_a) {
      (*walls)[ref.wall].a = avg;
    } else {
      (*walls)[ref.wall].b = avg;
    }
  }
}

// --- step 4d: the window sill re-slice --------------------------------------

// Occupied fraction of a stretch of wall centerline in `g`, sampling the full
// wall thickness plus one cell either side.
double band_occupancy(const OccupancyGrid& g, Vec2 p, Vec2 d, double lo, double hi,
                      double half_width) {
  if (!g.valid() || hi <= lo) return 0.0;
  const double step = g.res_m * 0.5;
  const Vec2 nrm = left_normal(d);
  const int lat = std::max(1, static_cast<int>(std::ceil(half_width / g.res_m)));
  std::uint32_t total = 0, hit = 0;
  for (double t = lo; t <= hi; t += step) {
    ++total;
    bool any = false;
    for (int k = -lat; k <= lat && !any; ++k) {
      const Vec2 q = p + d * t + nrm * (static_cast<double>(k) * g.res_m);
      std::uint32_t i = 0, j = 0;
      if (g.cell_of(q.x, q.y, &i, &j) && g.occupied(i, j)) any = true;
    }
    if (any) ++hit;
  }
  return total == 0 ? 0.0 : static_cast<double>(hit) / static_cast<double>(total);
}

void classify_openings(const OccupancyGrid& sill, const PlanOptions& o, PlanModel* model) {
  const OpeningOptions& oo = o.openings;
  for (auto& op : model->openings) {
    if (op.kind != OpeningKind::kDoorCandidate && op.kind != OpeningKind::kWideOpening) continue;
    const WallSegment* wall = model->wall_by_id(op.wall_id);
    if (wall == nullptr) continue;
    const Vec2 d = wall->direction();
    const double half = wall->thickness_m * 0.5 + sill.res_m;

    // Does the lower band see this wall at all? Measured on the wall OUTSIDE
    // the opening, so a wall that is simply not scanned low down reports
    // kNoData instead of masquerading as a door.
    const double t_a = dot(wall->a - wall->a, d);  // 0
    const double t_b = dot(wall->b - wall->a, d);
    const double g_lo = dot(op.a - wall->a, d);
    const double g_hi = dot(op.b - wall->a, d);
    double support = 0.0;
    double support_len = 0.0;
    if (g_lo - t_a > 0.2) {
      support += band_occupancy(sill, wall->a, d, t_a, g_lo, half) * (g_lo - t_a);
      support_len += g_lo - t_a;
    }
    if (t_b - g_hi > 0.2) {
      support += band_occupancy(sill, wall->a, d, g_hi, t_b, half) * (t_b - g_hi);
      support_len += t_b - g_hi;
    }
    const double support_frac = support_len > 0.0 ? support / support_len : 0.0;
    if (support_len <= 0.0 || support_frac < oo.sill_wall_support_frac) {
      op.sill = SillCheck::kNoData;
      continue;
    }

    const double occ = band_occupancy(sill, wall->a, d, g_lo, g_hi, half);
    op.sill_occupancy = occ;
    if (occ >= oo.sill_solid_frac) {
      op.sill = SillCheck::kSolidBelow;
      op.kind = OpeningKind::kWindowCandidate;
      op.confidence = 0.70f;
    } else if (occ <= oo.sill_open_frac) {
      op.sill = SillCheck::kOpenBelow;
      op.confidence = std::min(0.85f, op.confidence + 0.20f);
    } else {
      // Half-open below: a door standing ajar, a low cill, a radiator, a
      // pile of boxes. Refusing to decide is the honest answer.
      op.sill = SillCheck::kNoData;
      op.confidence = std::max(0.20f, op.confidence - 0.15f);
    }
  }
}

}  // namespace

// --- the public entry point (extract_walls() in floor_plan.cpp forwards) ----

Status extract_walls_impl(const OccupancyGrid& grid, const OccupancyGrid* sill,
                          const PlanOptions& opts, PlanModel* out, PlanCancelToken* cancel) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "plan: extract_walls(out == null)");
  }
  *out = PlanModel{};
  out->slice_z_min_m = opts.slice.z_min_m;
  out->slice_z_max_m = opts.slice.z_max_m;
  out->grid_res_m = grid.valid() ? grid.res_m : opts.slice.grid_res_m;
  out->up = opts.slice.up;
  if (!grid.valid()) return kOkStatus;  // empty band is a legitimate answer

  if (sill != nullptr && sill->valid()) {
    if (sill->w != grid.w || sill->h != grid.h || sill->res_m != grid.res_m ||
        sill->origin_x != grid.origin_x || sill->origin_y != grid.origin_y) {
      return set_last_error(ScanError::kInvalidArgument,
                            "plan: the sill grid is not on the same lattice as the main grid");
    }
  }

  const std::vector<Vec2> cells = grid.occupied_centers();
  out->stats.grid_w = grid.w;
  out->stats.grid_h = grid.h;
  out->stats.occupied_cells = static_cast<std::uint32_t>(cells.size());
  if (cells.empty()) return kOkStatus;

  std::vector<FitLine> lines = ransac_lines(cells, opts.walls, grid.res_m, cancel);
  out->stats.ransac_lines = static_cast<std::uint32_t>(lines.size());
  if (cancelled(cancel)) return ScanError::kCancelled;
  merge_collinear(cells, &lines, opts.walls, grid.res_m);

  const double th0 = estimate_dominant_angle(lines, opts.walls.dominant_min_length_m);
  out->stats.dominant_angle_rad = th0;
  std::uint32_t snapped = 0;
  if (opts.slice.snap_orthogonal && opts.slice.snap_tolerance_deg > 0.f) {
    snap_orthogonal(cells, &lines, th0, deg2rad(opts.slice.snap_tolerance_deg), opts.walls,
                    grid.res_m, &snapped);
  }
  out->stats.snapped_walls = snapped;

  std::vector<WallLine> wall_lines = pair_faces(lines, opts.walls);
  for (const auto& wl : wall_lines) {
    if (wl.evidence == WallEvidence::kPairedFaces) ++out->stats.paired_walls;
  }

  std::uint32_t next_wall = 1, next_open = 1;
  for (const auto& wl : wall_lines) {
    segments_from_wall_line(wl, opts, grid.res_m, &next_wall, &next_open, &out->walls,
                            &out->openings);
  }

  // Openings must move with their walls, so corner joining happens first and
  // the openings are re-projected onto the trimmed centerline afterwards.
  std::vector<std::pair<Vec2, Vec2>> before;
  before.reserve(out->walls.size());
  for (const auto& s : out->walls) before.push_back({s.a, s.b});
  join_corners(&out->walls, opts.walls);
  for (auto& op : out->openings) {
    for (std::size_t i = 0; i < out->walls.size(); ++i) {
      if (out->walls[i].id != op.wall_id) continue;
      const Vec2 d0 = normalized(before[i].second - before[i].first);
      const Vec2 d1 = out->walls[i].direction();
      const double ta = dot(op.a - before[i].first, d0);
      const double tb = dot(op.b - before[i].first, d0);
      const double shift = dot(out->walls[i].a - before[i].first, d1);
      op.a = out->walls[i].a + d1 * (ta - shift);
      op.b = out->walls[i].a + d1 * (tb - shift);
      break;
    }
  }

  if (sill != nullptr && sill->valid() && opts.slice.window_sill_check) {
    classify_openings(*sill, opts, out);
  }

  for (const auto& s : out->walls) {
    out->stats.total_wall_length_m += s.length();
    out->bounds.expand(s.a);
    out->bounds.expand(s.b);
  }

  if (opts.rooms.enabled) {
    SCAN_TRY(detect_rooms(out->walls, opts.rooms, &out->rooms));
    for (const auto& r : out->rooms) out->stats.total_room_area_m2 += r.area_m2;
  }
  for (const auto& r : out->rooms) {
    for (const auto& p : r.polygon) out->bounds.expand(p);
  }
  return kOkStatus;
}

}  // namespace plan
}  // namespace scanengine
