// lscan_plan.cpp — ROUND 15 item 56. See lscan_plan.h.
#include "scanengine/slam/post/lscan_plan.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/log.h"
#include "scanengine/plan/occupancy.h"
#include "scanengine/slam/post/d6_resolve.h"
#include "scanengine/slam/post/reprocess.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "lscanplan";

std::string join(const std::string& dir, const std::string& leaf) {
  if (dir.empty()) return leaf;
  const char last = dir[dir.size() - 1];
  return (last == '/' || last == '\\') ? dir + leaf : dir + "/" + leaf;
}

// The ladder, most-evidence-first. Each rung is (min_cell_points, grid
// multiplier). The multiplier only ever coarsens: a 4 cm cell holds four
// times the returns of a 2 cm one, which is the same trade as lowering
// min_cell_points but keeps the "three points agree" evidence rule intact.
struct Rung {
  std::uint32_t min_cell_points;
  double res_mul;
};
constexpr Rung kLadder[] = {{3, 1.0}, {2, 1.0}, {1, 1.0}, {2, 2.0}, {1, 2.0}, {1, 4.0}};

// --- the floor MAP -----------------------------------------------------------
//
// Projecting every return between 0.2 m and 2.4 m straight down produces a
// solid blob: a hand-carried D6 walked through a flat paints the floor, the
// furniture and the ceiling coving from a hundred viewpoints, and the union of
// all of that fills every cell the operator walked past. Measured on the
// owner's scan-033 it is 9 x 9 m of continuous grey with no room in it.
//
// What separates a WALL from everything else in a downward projection is not
// how many returns land in the cell — it is how TALL the column of returns is.
// A wall is hit from the skirting to the coving as the fan sweeps past, so its
// column spans well over a metre. A floor tile, a tabletop, a sofa back and a
// ceiling patch each span a few centimetres. So a cell counts as structure
// when its returns span at least `min_span_m` vertically, and that single test
// turns the blob into an outline.
//
// Deterministic: one pass over the points in cloud order, min/max per cell,
// no sorting and no floating-point accumulation that depends on visit order.
plan::OccupancyGrid wall_likeness_grid(const std::vector<PointVertex>& pts, plan::UpAxis up,
                                       double res_m, double z_lo, double z_hi, double min_span_m,
                                       std::uint64_t* out_band_points) {
  plan::OccupancyGrid g;
  if (pts.empty() || !(res_m > 0.0)) return g;

  double minx = 0, miny = 0, maxx = 0, maxy = 0;
  bool any = false;
  for (const PointVertex& p : pts) {
    const double u = plan::up_coord(p.x, p.y, p.z, up);
    if (u < z_lo || u > z_hi) continue;
    const plan::Vec2 q = plan::project(p.x, p.y, p.z, up);
    if (!any) {
      minx = maxx = q.x;
      miny = maxy = q.y;
      any = true;
    } else {
      minx = std::min(minx, q.x);
      maxx = std::max(maxx, q.x);
      miny = std::min(miny, q.y);
      maxy = std::max(maxy, q.y);
    }
  }
  if (!any) return g;

  const double w = (maxx - minx) / res_m + 2.0;
  const double h = (maxy - miny) / res_m + 2.0;
  if (!(w > 0.0) || !(h > 0.0) || w * h > static_cast<double>(plan::kMaxGridCells)) return g;
  g.origin_x = minx - res_m;
  g.origin_y = miny - res_m;
  g.res_m = res_m;
  g.w = static_cast<std::uint32_t>(w);
  g.h = static_cast<std::uint32_t>(h);
  g.min_points = 1;
  g.counts.assign(static_cast<std::size_t>(g.w) * g.h, 0u);

  std::vector<float> lo(g.counts.size(), 0.f), hi(g.counts.size(), 0.f);
  std::vector<std::uint8_t> seen(g.counts.size(), 0u);
  std::uint64_t in_band = 0;
  for (const PointVertex& p : pts) {
    const double u = plan::up_coord(p.x, p.y, p.z, up);
    if (u < z_lo || u > z_hi) continue;
    ++in_band;
    const plan::Vec2 q = plan::project(p.x, p.y, p.z, up);
    std::uint32_t i = 0, j = 0;
    if (!g.cell_of(q.x, q.y, &i, &j)) continue;
    const std::size_t k = g.index(i, j);
    const float uf = static_cast<float>(u);
    if (seen[k] == 0u) {
      seen[k] = 1u;
      lo[k] = uf;
      hi[k] = uf;
    } else {
      lo[k] = std::min(lo[k], uf);
      hi[k] = std::max(hi[k], uf);
    }
  }
  for (std::size_t k = 0; k < g.counts.size(); ++k) {
    if (seen[k] != 0u && static_cast<double>(hi[k] - lo[k]) >= min_span_m) g.counts[k] = 1u;
  }
  if (out_band_points != nullptr) *out_band_points = in_band;
  return g;
}

}  // namespace

Status floor_plan_from_lscan(const std::string& lscan_dir, const LscanPlanOptions& opts,
                             LscanPlanReport* out) {
  LscanPlanReport rep;
  rep.slice_min_m = opts.slice_min_m;
  rep.slice_max_m = opts.slice_max_m;

  // --- 1. the best cloud this container has ---------------------------------
  //
  // load_recorded_cloud() already prefers processed/map_stitched.bin over the
  // live cache (d6_resolve.cpp), which is exactly the preference item 56
  // wants: a plan should be drawn from the PROCESSED map when there is one.
  PageStoreConfig psc;
  psc.page_capacity = 1u << 20;
  psc.max_pages = 4096;
  PageStore store(psc);

  std::uint64_t n = 0;
  const bool stitched = has_stitched_cloud(lscan_dir);
  Status ld = load_recorded_cloud(lscan_dir, &store, StreamId::kSlamMap, &n);
  if (ld.ok() && n > 0) {
    rep.cloud_source = stitched ? "processed/map_stitched.bin" : "streams/map.bin";
  } else {
    // No cache at all — a container sealed before ROUND 8, or one whose map
    // was deleted. Re-resolve, which is the same arithmetic the capture did.
    store.recycle_all();
    D6ResolveConfig cfg;
    cfg.store = &store;
    D6ResolvePipeline pipe(cfg);
    const Status st = pipe.run(lscan_dir);
    if (!st.ok()) {
      if (out != nullptr) *out = rep;
      return st;
    }
    n = store.total_points();
    rep.cloud_source = "re-resolved from raw";
  }
  if (n == 0) {
    if (out != nullptr) *out = rep;
    return set_last_error(ScanError::kNotFound,
                          "floor plan: '%s' holds no points", lscan_dir.c_str());
  }

  std::vector<PointVertex> pts;
  pts.reserve(static_cast<std::size_t>(store.total_points()));
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) pts.push_back(v.data[k]);
  }
  rep.cloud_points = pts.size();

  // --- ROUND 16 item 59: the walked path, for the sheet ---------------------
  //
  // Read from `processed/trajectory.bin` — the corrected walk that
  // `reprocess_d6_container` writes beside the corrected cloud. Deliberately
  // NOT re-derived: the cloud loaded above prefers `processed/map_stitched.bin`
  // when it exists, so the path has to be the one that belongs to THAT cloud,
  // and the only way to be sure of that is to read the file the same pass
  // wrote. A container that has never been processed simply has no path, and
  // the plan is drawn exactly as it was before this round.
  //
  // Projected into plan coordinates here rather than in `plan/`, because that
  // directory deliberately knows nothing about world frames or up-axes.
  std::vector<plan::Vec2> walk;
  std::vector<std::uint32_t> walk_breaks;
  {
    std::FILE* tf = std::fopen(join(lscan_dir, "processed/trajectory.bin").c_str(), "rb");
    if (tf != nullptr) {
      std::uint8_t head[16];
      bool v1 = false, v2 = false;
      if (std::fread(head, 1, sizeof(head), tf) == sizeof(head) && head[0] == 'L' &&
          head[1] == 'S' && head[2] == 'T' && head[3] == 'R' && head[4] == 'A' &&
          head[5] == 'J' && head[6] == '0') {
        v1 = head[7] == '1';
        v2 = head[7] == '2';
      }
      if (v1 || v2) {
        const std::uint32_t count = static_cast<std::uint32_t>(head[8]) |
                                    (static_cast<std::uint32_t>(head[9]) << 8) |
                                    (static_cast<std::uint32_t>(head[10]) << 16) |
                                    (static_cast<std::uint32_t>(head[11]) << 24);
        // Thinned to about 12 cm, the same rule the phone draws by, so the
        // sheet and the screen show one walk.
        constexpr double kStrideM = 0.12;
        const std::size_t rec_bytes = v2 ? 16 : 12;
        double lx = 0.0, ly = 0.0, lz = 0.0;
        bool have = false;
        // ROUND 18 item 70: a jump flag anywhere between two KEPT points
        // breaks the polyline at the next kept point — a segment the tracker
        // was blind across must not be inked as a walked line on the sheet.
        bool pending_break = false;
        for (std::uint32_t i = 0; i < count && i < 4000000u; ++i) {
          std::uint8_t rec[16];
          if (std::fread(rec, 1, rec_bytes, tf) != rec_bytes) break;
          float xyz[3];
          std::memcpy(xyz, rec, 12);
          std::uint32_t flags = 0;
          if (v2) {
            flags = static_cast<std::uint32_t>(rec[12]) |
                    (static_cast<std::uint32_t>(rec[13]) << 8) |
                    (static_cast<std::uint32_t>(rec[14]) << 16) |
                    (static_cast<std::uint32_t>(rec[15]) << 24);
          }
          if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) {
            continue;
          }
          if ((flags & 2u) != 0) pending_break = true;
          // An UNTRACKED pose's position is the tracker's held guess, not a
          // measurement — it draws nothing.
          if ((flags & 1u) != 0) continue;
          const double dx = xyz[0] - lx, dy = xyz[1] - ly, dz = xyz[2] - lz;
          if (have && dx * dx + dy * dy + dz * dz < kStrideM * kStrideM) continue;
          lx = xyz[0];
          ly = xyz[1];
          lz = xyz[2];
          have = true;
          if (pending_break && !walk.empty()) {
            walk_breaks.push_back(static_cast<std::uint32_t>(walk.size()));
          }
          pending_break = false;
          walk.push_back(plan::project(xyz[0], xyz[1], xyz[2], opts.up));
        }
      }
      std::fclose(tf);
    }
  }
  rep.trajectory_points = walk.size();

  plan::PlanInput in;
  in.points = Span<const PointVertex>(pts.data(), pts.size());
  in.up = opts.up;

  // --- 2. the ladder ---------------------------------------------------------
  plan::PlanModel model;
  plan::OccupancyGrid grid;
  plan::PlanStats gstats;
  bool have_walls = false;
  std::uint32_t rung_min = opts.min_cell_points;
  double rung_res = opts.grid_res_m;

  const std::size_t rungs = opts.adapt_density ? (sizeof(kLadder) / sizeof(kLadder[0])) : 1u;
  for (std::size_t r = 0; r < rungs; ++r) {
    const std::uint32_t mcp = opts.adapt_density ? kLadder[r].min_cell_points : opts.min_cell_points;
    const double res = opts.grid_res_m * (opts.adapt_density ? kLadder[r].res_mul : 1.0);

    plan::PlanOptions po;
    po.slice.z_min_m = static_cast<float>(opts.slice_min_m);
    po.slice.z_max_m = static_cast<float>(opts.slice_max_m);
    po.slice.grid_res_m = static_cast<float>(res);
    po.slice.up = opts.up;
    po.slice.min_cell_points = mcp;
    // The sill re-slice needs a band BELOW the main one; on a 1.0-1.5 m band
    // A12's 0.35-0.80 m default is right, and on a band the operator moved it
    // is not. Shift it to sit just under whatever band was asked for.
    const double sill_top = std::max(0.10, opts.slice_min_m - 0.15);
    po.slice.sill_z_min_m = static_cast<float>(std::max(0.05, sill_top - 0.45));
    po.slice.sill_z_max_m = static_cast<float>(sill_top);

    plan::PlanModel m;
    const Status st = plan::extract_floor_plan(in, po, &m);
    if (!st.ok()) continue;

    // The grid is rebuilt here rather than plumbed out of the extractor
    // because the extractor does not return it and A12's seam (build_occupancy)
    // is public and cheap. Same band, same lattice rule, same answer.
    plan::OccupancyGrid g;
    plan::PlanStats gs;
    (void)plan::build_occupancy(in, plan::main_band(po.slice), &g, &gs);

    // Keep the FIRST rung that fits any wall; otherwise keep the rung with
    // the most occupied cells, which is the best picture available.
    if (!m.walls.empty()) {
      model = std::move(m);
      grid = std::move(g);
      gstats = gs;
      rung_min = mcp;
      rung_res = res;
      have_walls = true;
      break;
    }
    if (!grid.valid() || g.occupied_count() > grid.occupied_count()) {
      model = std::move(m);
      grid = std::move(g);
      gstats = gs;
      rung_min = mcp;
      rung_res = res;
    }
  }

  if (!grid.valid()) {
    if (out != nullptr) *out = rep;
    return set_last_error(ScanError::kNotFound,
                          "floor plan: the %.2f-%.2f m band of '%s' is empty", opts.slice_min_m,
                          opts.slice_max_m, lscan_dir.c_str());
  }

  rep.ran = true;
  rep.mode = have_walls ? plan::PlanRenderMode::kWalls : plan::PlanRenderMode::kDensity;
  rep.band_points = gstats.points_in_band;
  rep.occupied_cells = grid.occupied_count();
  rep.grid_w = grid.w;
  rep.grid_h = grid.h;
  rep.min_cell_points_used = rung_min;
  rep.grid_res_used_m = rung_res;
  rep.walls = static_cast<std::uint32_t>(model.walls.size());
  for (const plan::WallSegment& w : model.walls) {
    if (w.evidence == plan::WallEvidence::kPairedFaces) ++rep.walls_paired;
  }
  rep.openings = static_cast<std::uint32_t>(model.openings.size());
  for (const plan::Opening& o : model.openings) {
    if (o.kind == plan::OpeningKind::kDoorCandidate) ++rep.doors;
    if (o.kind == plan::OpeningKind::kWindowCandidate) ++rep.windows;
  }
  rep.rooms = static_cast<std::uint32_t>(model.rooms.size());
  rep.total_wall_length_m = model.stats.total_wall_length_m;
  rep.total_room_area_m2 = model.stats.total_room_area_m2;
  for (const plan::Room& rm : model.rooms) {
    rep.largest_room_area_m2 = std::max(rep.largest_room_area_m2, rm.area_m2);
  }
  rep.no_room_closed = have_walls && model.rooms.empty();

  // --- 2b. the backdrop, from its own band ------------------------------------
  //
  // See LscanPlanOptions::map_band_min_m. This grid is what the operator
  // actually recognises as their flat; the plan slice is what RANSAC was
  // allowed to fit to.
  plan::OccupancyGrid map_grid;
  if (opts.draw_map_backdrop) {
    map_grid = wall_likeness_grid(pts, opts.up, opts.map_res_m, opts.map_band_min_m,
                                  opts.map_band_max_m, opts.map_min_span_m, &rep.map_band_points);
    rep.map_cells = map_grid.valid() ? map_grid.occupied_count() : 0u;
  }

  // --- 2c. walls from the FLOOR MAP, when the thin slice could not close one --
  //
  // A12's `extract_walls()` takes a grid the caller already built — the seam
  // the desktop editor's slice slider uses. That is exactly what is needed
  // here: the wall-likeness grid is a far better-conditioned input for RANSAC
  // than a 50 cm band of a 10 Hz single-line scanner, because every one of its
  // cells is already evidence of something a metre tall. The thin slice is
  // still tried FIRST, because when it works its geometry is the more
  // literal answer (a plan is conventionally cut at 1.2 m, and A12's face
  // pairing measures a real thickness there). This runs only when the slice
  // failed to close a room.
  if (map_grid.valid() && (!have_walls || model.rooms.empty())) {
    plan::PlanOptions mo;
    mo.slice.grid_res_m = static_cast<float>(opts.map_res_m);
    mo.slice.up = opts.up;
    // The inlier band must clear the cell, or RANSAC cannot gather a line
    // that is one cell wide. A12's 3.5 cm is set for a 2 cm grid.
    mo.walls.inlier_m = std::max(0.035, opts.map_res_m * 1.2);
    mo.walls.collinear_offset_m = std::max(0.05, opts.map_res_m * 1.5);
    mo.walls.run_gap_m = std::max(0.10, opts.map_res_m * 3.0);
    mo.walls.min_run_m = std::max(0.12, opts.map_res_m * 3.0);
    // Nothing below this is wall in a projection of a whole room.
    mo.walls.min_wall_length_m = 0.80;
    mo.openings.enabled = true;
    mo.rooms.enabled = true;
    plan::PlanModel mm;
    if (plan::extract_walls(map_grid, nullptr, mo, &mm).ok() && !mm.walls.empty()) {
      const bool better = model.walls.empty() ||
                          (model.rooms.empty() && !mm.rooms.empty()) ||
                          (model.rooms.empty() && mm.walls.size() > model.walls.size());
      if (better) {
        mm.slice_z_min_m = opts.map_band_min_m;
        mm.slice_z_max_m = opts.map_band_max_m;
        mm.grid_res_m = opts.map_res_m;
        mm.up = opts.up;
        model = std::move(mm);
        have_walls = true;
        rep.walls_from_floor_map = true;
        rung_res = opts.map_res_m;
        rung_min = 1;
      }
    }
  }

  rep.mode = have_walls ? plan::PlanRenderMode::kWalls : plan::PlanRenderMode::kDensity;
  rep.walls = static_cast<std::uint32_t>(model.walls.size());
  rep.walls_paired = 0;
  for (const plan::WallSegment& w : model.walls) {
    if (w.evidence == plan::WallEvidence::kPairedFaces) ++rep.walls_paired;
  }
  rep.openings = static_cast<std::uint32_t>(model.openings.size());
  rep.doors = 0;
  rep.windows = 0;
  for (const plan::Opening& o : model.openings) {
    if (o.kind == plan::OpeningKind::kDoorCandidate) ++rep.doors;
    if (o.kind == plan::OpeningKind::kWindowCandidate) ++rep.windows;
  }
  rep.rooms = static_cast<std::uint32_t>(model.rooms.size());
  rep.total_wall_length_m = model.stats.total_wall_length_m;
  rep.total_room_area_m2 = model.stats.total_room_area_m2;
  rep.largest_room_area_m2 = 0.0;
  for (const plan::Room& rm : model.rooms) {
    rep.largest_room_area_m2 = std::max(rep.largest_room_area_m2, rm.area_m2);
  }
  rep.no_room_closed = have_walls && model.rooms.empty();
  rep.min_cell_points_used = rung_min;
  rep.grid_res_used_m = rung_res;

  {
    plan::PlanBounds b = model.bounds;
    b.expand(grid.extent());
    if (map_grid.valid()) b.expand(map_grid.extent());
    rep.extent_x_m = b.width();
    rep.extent_y_m = b.height();
  }

  // --- 3. the three files ----------------------------------------------------
  const std::string dir = opts.out_dir.empty() ? join(lscan_dir, "processed") : opts.out_dir;
  const std::string base = opts.base_name.empty() ? std::string("floorplan") : opts.base_name;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(dir), ec);

  if (opts.write_png) {
    plan::PlanRasterOptions ro;
    ro.max_dimension_px = opts.png_max_px;
    ro.title = opts.title;
    // ROUND 16 item 59.
    ro.trajectory = walk;
    ro.trajectory_breaks = walk_breaks;  // ROUND 18 item 70
    plan::PlanRasterInfo pi;
    const std::string path = join(dir, base + ".png");
    const plan::OccupancyGrid* backdrop = map_grid.valid() ? &map_grid : &grid;
    const Status s = plan::write_plan_png(model, backdrop, ro, path, &pi);
    if (s.ok()) {
      rep.png_path = path;
      rep.png_px_per_m = pi.px_per_m;
      rep.png_scale_bar_m = pi.scale_bar_m;
      rep.png_w = pi.width_px;
      rep.png_h = pi.height_px;
      // The renderer decides its own mode from the model it was handed; the
      // report quotes the renderer rather than re-deriving, so the picture
      // and the numbers can never disagree.
      rep.mode = pi.mode;
    }
  }
  // DXF and PDF carry the MODEL, so in density mode they would be empty
  // sheets. They are written only when something was fitted — an empty DXF
  // that opens to nothing is worse than an absent one, and the caller can
  // tell the difference from an empty path.
  if (opts.write_dxf && have_walls) {
    plan::DxfOptions dx;
    const std::string path = join(dir, base + ".dxf");
    if (plan::write_dxf(model, dx, path).ok()) rep.dxf_path = path;
  }
  if (opts.write_pdf && have_walls) {
    plan::PdfOptions pd;
    pd.title = opts.title.empty() ? std::string("Floor plan") : opts.title;
    pd.project = opts.project;
    pd.date = opts.date;
    pd.reference = lscan_dir;
    const std::string path = join(dir, base + ".pdf");
    if (plan::write_pdf(model, pd, path).ok()) rep.pdf_path = path;
  }

  rep.summary = !have_walls ? "no wall could be fitted; the returns themselves are the map"
              : rep.no_room_closed
                  ? "walls fitted, but no outline closed into a room"
                  : "walls fitted and at least one room closed";
  SCAN_LOG_INFO(kMod,
                "'%s': %llu points (%s), band %llu, %u cells @ %.3f m / min %u -> %u walls "
                "(%u paired), %u rooms, %.2f m2 [%s]",
                lscan_dir.c_str(), static_cast<unsigned long long>(rep.cloud_points),
                rep.cloud_source, static_cast<unsigned long long>(rep.band_points),
                rep.occupied_cells, rep.grid_res_used_m, rep.min_cell_points_used, rep.walls,
                rep.walls_paired, rep.rooms, rep.total_room_area_m2, to_string(rep.mode));

  if (out != nullptr) *out = rep;
  return kOkStatus;
}

}  // namespace post
}  // namespace scanengine
