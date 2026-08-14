// occupancy.cpp — the horizontal slice and the 2D occupancy grid.
//
// Two streaming passes over the source, never a copy of the cloud (the same
// posture A9's exporters take, docs/A9-export.md §"Two-pass streaming"):
// pass 1 finds the plan-frame extents of the in-band, region-accepted
// points, pass 2 fills the counts. The one case that materializes points is
// `BandOptions::outlier_filter`, which cannot be done streaming — and which
// is off by default precisely because of that.
#include "scanengine/plan/occupancy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "scanengine/plan/plan_editor.h"
#include "scanengine/slam/post/cloud_filter.h"

namespace scanengine {
namespace plan {

Vec2 project(float x, float y, float z, UpAxis up) {
  switch (up) {
    case UpAxis::kZ: return Vec2{static_cast<double>(x), static_cast<double>(y)};
    case UpAxis::kY: return Vec2{static_cast<double>(z), static_cast<double>(x)};
    case UpAxis::kX: return Vec2{static_cast<double>(y), static_cast<double>(z)};
  }
  return Vec2{static_cast<double>(x), static_cast<double>(y)};
}

double up_coord(float x, float y, float z, UpAxis up) {
  switch (up) {
    case UpAxis::kZ: return static_cast<double>(z);
    case UpAxis::kY: return static_cast<double>(y);
    case UpAxis::kX: return static_cast<double>(x);
  }
  return static_cast<double>(z);
}

bool OccupancyGrid::cell_of(double x, double y, std::uint32_t* i, std::uint32_t* j) const {
  if (!valid()) return false;
  const double fi = std::floor((x - origin_x) / res_m);
  const double fj = std::floor((y - origin_y) / res_m);
  if (fi < 0.0 || fj < 0.0) return false;
  if (fi >= static_cast<double>(w) || fj >= static_cast<double>(h)) return false;
  if (i) *i = static_cast<std::uint32_t>(fi);
  if (j) *j = static_cast<std::uint32_t>(fj);
  return true;
}

std::uint32_t OccupancyGrid::occupied_count() const {
  std::uint32_t n = 0;
  for (std::uint32_t c : counts) {
    if (c >= min_points) ++n;
  }
  return n;
}

PlanBounds OccupancyGrid::extent() const {
  PlanBounds b;
  if (!valid()) return b;
  b.expand(Vec2{origin_x, origin_y});
  b.expand(Vec2{origin_x + static_cast<double>(w) * res_m,
                origin_y + static_cast<double>(h) * res_m});
  return b;
}

std::vector<Vec2> OccupancyGrid::occupied_centers() const {
  std::vector<Vec2> out;
  if (!valid()) return out;
  out.reserve(occupied_count());
  // Row-major, ascending: the RANSAC's sampling order is derived from this
  // order, so it is part of the determinism contract.
  for (std::uint32_t j = 0; j < h; ++j) {
    for (std::uint32_t i = 0; i < w; ++i) {
      if (counts[index(i, j)] >= min_points) {
        out.push_back(Vec2{cell_center_x(i), cell_center_y(j)});
      }
    }
  }
  return out;
}

namespace {

bool stream_selected(Span<const StreamId> streams, StreamId s) {
  if (streams.empty()) return true;
  for (std::size_t k = 0; k < streams.size(); ++k) {
    if (streams[k] == s) return true;
  }
  return false;
}

// Calls fn(const PointVertex&) for every point of the input, in a
// deterministic order (page-id order for a store, index order for a span).
template <typename Fn>
void for_each_point(const PlanInput& in, Fn&& fn) {
  if (in.store != nullptr) {
    const std::vector<PageId> ids = in.store->page_ids();
    for (PageId id : ids) {
      const PageView v = in.store->page_view(id);
      if (!v.valid()) continue;
      if (!stream_selected(in.streams, v.stream)) continue;
      for (std::uint32_t k = 0; k < v.count; ++k) fn(v.data[k]);
    }
    return;
  }
  for (std::size_t k = 0; k < in.points.size(); ++k) fn(in.points[k]);
}

struct BandTest {
  double lo = 0.0;
  double hi = 0.0;
  UpAxis up = UpAxis::kZ;
  bool has_regions = false;
  Span<const PlanRegion> regions{};

  bool accept(const PointVertex& p, Vec2* xy) const {
    const double u = up_coord(p.x, p.y, p.z, up);
    if (u < lo || u > hi) return false;
    const Vec2 q = project(p.x, p.y, p.z, up);
    if (has_regions && !edit_accepts(regions, q.x, q.y)) return false;
    if (xy) *xy = q;
    return true;
  }
};

}  // namespace

Status build_occupancy(const PlanInput& in, const BandOptions& band, OccupancyGrid* out,
                       PlanStats* stats) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "plan: build_occupancy(out == null)");
  }
  *out = OccupancyGrid{};
  if (!in.has_source()) {
    return set_last_error(ScanError::kInvalidArgument,
                          "plan: build_occupancy has neither a PageStore nor a point span");
  }
  const double res = band.lattice != nullptr ? band.lattice->res_m : band.res_m;
  if (!(res > 0.0) || res > 100.0) {
    return set_last_error(ScanError::kInvalidArgument,
                          "plan: grid resolution %.6f m is not in (0, 100]", res);
  }
  if (!(band.z_max_m > band.z_min_m)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "plan: slice band [%.3f, %.3f] is empty or inverted", band.z_min_m,
                          band.z_max_m);
  }

  BandTest test;
  test.lo = band.z_min_m;
  test.hi = band.z_max_m;
  test.up = in.up;
  test.regions = band.regions;
  test.has_regions = !band.regions.empty();

  std::uint64_t considered = 0;
  std::uint64_t in_band = 0;

  // --- the outlier-filter path materializes; the default path does not ----
  std::vector<PointVertex> filtered;
  const bool materialize = band.outlier_filter;
  if (materialize) {
    std::vector<PointVertex> gathered;
    for_each_point(in, [&](const PointVertex& p) {
      ++considered;
      Vec2 q;
      if (!test.accept(p, &q)) return;
      ++in_band;
      gathered.push_back(p);
    });
    post::OutlierFilterConfig cfg;
    cfg.enabled = true;
    cfg.std_dev_mul = band.outlier_std_dev_mul;
    const Status st = post::statistical_outlier_filter(
        Span<const PointVertex>(gathered.data(), gathered.size()), cfg, 8.0 * res, &filtered);
    if (!st.ok()) return st;
  }

  // --- pass 1: extents ----------------------------------------------------
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = std::numeric_limits<double>::infinity();
  double max_x = -std::numeric_limits<double>::infinity();
  double max_y = -std::numeric_limits<double>::infinity();
  std::uint64_t kept = 0;

  auto observe = [&](Vec2 q) {
    ++kept;
    min_x = std::min(min_x, q.x);
    min_y = std::min(min_y, q.y);
    max_x = std::max(max_x, q.x);
    max_y = std::max(max_y, q.y);
  };

  if (materialize) {
    for (const auto& p : filtered) observe(project(p.x, p.y, p.z, in.up));
  } else if (band.lattice == nullptr) {
    for_each_point(in, [&](const PointVertex& p) {
      ++considered;
      Vec2 q;
      if (!test.accept(p, &q)) return;
      ++in_band;
      observe(q);
    });
  }

  // --- lattice ------------------------------------------------------------
  if (band.lattice != nullptr) {
    if (!band.lattice->valid()) {
      return set_last_error(ScanError::kInvalidArgument,
                            "plan: build_occupancy given an invalid lattice grid");
    }
    out->origin_x = band.lattice->origin_x;
    out->origin_y = band.lattice->origin_y;
    out->res_m = band.lattice->res_m;
    out->w = band.lattice->w;
    out->h = band.lattice->h;
  } else {
    if (kept == 0) {
      // Not an error: an empty band is a legitimate answer (the slider is
      // above the ceiling, or every point was excluded). The grid comes back
      // invalid() and the extractor produces an empty plan.
      if (stats != nullptr) {
        stats->points_considered = considered;
        stats->points_in_band = in_band;
        stats->points_after_filter = 0;
        stats->grid_w = 0;
        stats->grid_h = 0;
        stats->occupied_cells = 0;
      }
      return kOkStatus;
    }
    // Snap the origin to the global multiple-of-res lattice so that two
    // grids of the same cloud never disagree about where a cell boundary is.
    out->origin_x = std::floor(min_x / res) * res;
    out->origin_y = std::floor(min_y / res) * res;
    out->res_m = res;
    const double span_x = (max_x - out->origin_x) / res;
    const double span_y = (max_y - out->origin_y) / res;
    const double wd = std::floor(span_x) + 1.0;
    const double hd = std::floor(span_y) + 1.0;
    if (wd <= 0.0 || hd <= 0.0 ||
        wd * hd > static_cast<double>(kMaxGridCells)) {
      return set_last_error(
          ScanError::kCapacityExceeded,
          "plan: a %.0f x %.0f cell grid at %.4f m exceeds the %zu-cell cap; use a coarser "
          "resolution or an include region",
          wd, hd, res, static_cast<std::size_t>(kMaxGridCells));
    }
    out->w = static_cast<std::uint32_t>(wd);
    out->h = static_cast<std::uint32_t>(hd);
  }

  out->min_points = band.min_points == 0 ? 1u : band.min_points;
  out->counts.assign(static_cast<std::size_t>(out->w) * out->h, 0u);

  // --- pass 2: counts -----------------------------------------------------
  std::uint64_t gridded = 0;
  auto deposit = [&](Vec2 q) {
    std::uint32_t i = 0, j = 0;
    if (!out->cell_of(q.x, q.y, &i, &j)) return;
    ++out->counts[out->index(i, j)];
    ++gridded;
  };

  if (materialize) {
    for (const auto& p : filtered) deposit(project(p.x, p.y, p.z, in.up));
  } else {
    const bool count_again = band.lattice != nullptr;  // pass 1 was skipped
    if (count_again) {
      considered = 0;
      in_band = 0;
    }
    for_each_point(in, [&](const PointVertex& p) {
      if (count_again) ++considered;
      Vec2 q;
      if (!test.accept(p, &q)) return;
      if (count_again) ++in_band;
      deposit(q);
    });
  }

  if (stats != nullptr) {
    stats->points_considered = considered;
    stats->points_in_band = in_band;
    stats->points_after_filter = materialize ? filtered.size() : gridded;
    stats->grid_w = out->w;
    stats->grid_h = out->h;
    stats->occupied_cells = out->occupied_count();
  }
  return kOkStatus;
}

}  // namespace plan
}  // namespace scanengine
