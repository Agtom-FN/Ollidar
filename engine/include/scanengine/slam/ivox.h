// ivox.h — the incremental voxel map the live LIO registers against.
//
// Tech Spec §3.3 asks for an "incremental voxel map (iVox-style)". This is
// that: a hash grid from quantized (x, y, z) to a small, capacity-bounded
// bucket of points, supporting O(1) insertion and a k-nearest-neighbour query
// over the 27 voxels around a point.
//
// WHY A HASH GRID AND NOT A KD-TREE. FAST-LIO2 ships an incremental kd-tree
// (ikd-tree) and Faster-LIO replaced it with exactly this structure, measuring
// a 1.5-2x end-to-end speedup, because the kd-tree's cost is not the search —
// it is the rebalancing that a *live* map does on every scan. A hash grid has
// no rebalancing at all, and the phone-side budget in §3.3 (≤ 2 big cores) is
// spent on the iterated update, not on maintaining a tree.
//
// THE THREE PROPERTIES THE ODOMETRY DEPENDS ON, in the order they matter:
//
//   1. DETERMINISM. knn() walks a fixed neighbour-offset order and, within a
//      voxel, insertion order; ties in distance break on insertion index.
//      No std::nth_element (not stable), no iteration over the hash map's
//      bucket order (unspecified). Identical input therefore gives an
//      identical map and an identical trajectory.
//   2. BOUNDED MEMORY. A voxel stops accepting points once it holds
//      `max_points_per_voxel`, and the map stops creating voxels once it
//      holds `max_voxels`. So the map is a voxel-downsampled cloud whose
//      size is a function of the volume scanned, not of the session length —
//      which is what makes "insert every scan forever" safe. insert() reports
//      whether the point was actually stored, and the LIO publishes exactly
//      the stored points to the PageStore, so the rendered cloud and the map
//      never diverge.
//   3. NO FIFO EVICTION. A full voxel drops new points rather than replacing
//      old ones. Replacement would silently invalidate points already handed
//      to the renderer (the PageStore only ever appends) and would make the
//      map depend on arrival order in a way plane fitting does not benefit
//      from — the first 20 returns in a 0.5 m cell already describe its
//      surface.
//
// Owner: A6.
#ifndef SCANENGINE_SLAM_IVOX_H
#define SCANENGINE_SLAM_IVOX_H

#include <cstddef>
#include <cstdint>
#include <memory>

namespace scanengine {

struct IVoxConfig {
  // Grid pitch. 0.5 m is the FAST-LIO2/Faster-LIO indoor default and the
  // resolution at which a 20-point bucket is a usable local plane sample.
  double voxel_size_m = 0.5;

  // Points kept per voxel. 20 is enough for a stable 5-point plane fit with
  // margin and keeps the per-voxel scan in one cache line group.
  std::uint32_t max_points_per_voxel = 20;

  // Hard ceiling on live voxels. 1 M voxels at 0.5 m is a 125,000 m^3
  // envelope and ~250 MB worst case at full buckets; past it the map stops
  // growing rather than the process dying. A7's post pipeline has no such
  // cap because it is not competing with a renderer for the phone's RAM.
  std::uint32_t max_voxels = 1000000;

  // Search the 27 voxels around the query (3x3x3) rather than 7 (the voxel
  // plus its 6 face neighbours). 27 costs ~2.5x the buckets scanned and is
  // what keeps the correspondence rate up on sparse outdoor data; set false
  // for the cheapest possible query on dense indoor scans.
  bool search_27 = true;
};

// Points are stored as float triples: the map is metric to ~1 mm at 100 m
// scene scale, which is well inside the Mid-360's own range noise, and the
// halved footprint matters at a million voxels.
class IVox {
 public:
  static constexpr std::size_t kMaxK = 8;  // knn() k ceiling

  explicit IVox(const IVoxConfig& cfg = {});
  ~IVox();

  IVox(const IVox&) = delete;
  IVox& operator=(const IVox&) = delete;

  // True if the point was stored (its voxel had room and the map had room
  // for a new voxel). False means "already represented at this resolution",
  // not an error.
  bool insert(double x, double y, double z);

  // Up to `k` (<= kMaxK) nearest stored points within `max_dist_m` of the
  // query, written as k*3 doubles into `out_xyz`, nearest first. Returns how
  // many were written.
  std::size_t knn(const double q[3], std::size_t k, double max_dist_m, double* out_xyz) const;

  void clear();
  std::size_t voxel_count() const;
  std::size_t point_count() const;
  const IVoxConfig& config() const;

  // Drop every voxel whose centre is farther than `radius_m` from `centre`.
  // The live map's forgetting mechanism (A7 re-runs at full density from the
  // .lscan, so nothing is lost). Returns the number of voxels removed.
  std::size_t trim(const double centre[3], double radius_m);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_SLAM_IVOX_H
