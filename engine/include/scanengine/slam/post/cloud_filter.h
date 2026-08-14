// cloud_filter.h — the two cleanup steps at the end of the post pipeline
// (Tech Spec §3.3: "re-integration → voxel dedup/outlier filter"), plus the
// streaming accumulator that makes the re-integration pass memory-bounded.
//
// Both are exposed as free functions over Span<const PointVertex> rather than
// hidden inside the pipeline, because A13 (multi-session merge, §3.10: "voxel
// dedup") and A9 (export) want exactly these and should not each grow their
// own.
//
// DETERMINISM. Output order is INSERTION ORDER OF THE FIRST POINT IN EACH
// VOXEL, never hash order. That costs one extra `std::vector<uint64_t>` and
// buys "two runs produce a bit-identical cloud", which is the property A6
// established and A7 has to keep (docs/A6-lio.md §4). Averaging inside a voxel
// also sums in insertion order for the same reason.
//
// Owner: A7.
#ifndef SCANENGINE_SLAM_POST_CLOUD_FILTER_H
#define SCANENGINE_SLAM_POST_CLOUD_FILTER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/slam/post/progress.h"

namespace scanengine {
namespace post {

struct VoxelDedupConfig {
  // 5 cm: below the Mid-360's own range noise plus registration error, so it
  // removes duplicate returns of the same surface without removing detail the
  // sensor actually resolved. A floor-plan run (§3.6) wants this coarser; an
  // as-built survey wants it finer. 0 disables the step entirely.
  double voxel_size_m = 0.05;
  // true: the surviving point is the centroid of its voxel's members, which
  // averages away ~1/sqrt(n) of the range noise. false: the first point wins,
  // which keeps every output coordinate a value the sensor actually measured
  // (what a survey deliverable may prefer).
  bool average_position = true;
  // Same choice for colour/reflectivity.
  bool average_color = true;
};

// Returns the number of output points. `out` is cleared first.
std::size_t voxel_downsample(Span<const PointVertex> in, const VoxelDedupConfig& cfg,
                             std::vector<PointVertex>* out, CancelToken* cancel = nullptr);

struct OutlierFilterConfig {
  bool enabled = true;
  // Neighbours per point. 8 is enough for the mean-distance statistic to be
  // stable and cheap; more mainly costs time.
  std::uint32_t neighbors = 8;
  // Keep a point when its mean neighbour distance is within
  // `std_dev_mul` standard deviations of the cloud-wide mean. 1.0 is
  // aggressive, 2.0 conservative; 1.5 is the usual starting point and is what
  // A7 defaults to because the input has already been voxel-deduped, which
  // narrows the distribution.
  double std_dev_mul = 1.5;
  // Neighbour search radius. 0 = auto: 8 * the dedup voxel size, or 0.5 m if
  // dedup was disabled. A point with fewer than `min_neighbors` inside the
  // radius is dropped outright — that IS the isolated-speckle case, and
  // giving it an undefined mean distance instead would let it survive.
  double search_radius_m = 0.0;
  std::uint32_t min_neighbors = 3;
};

struct OutlierFilterStats {
  std::size_t in = 0;
  std::size_t kept = 0;
  std::size_t removed_isolated = 0;   // fewer than min_neighbors in radius
  std::size_t removed_statistical = 0;  // past the mean + k*sigma gate
  double mean_distance_m = 0.0;
  double std_dev_m = 0.0;
  double threshold_m = 0.0;
};

// Two passes: gather each point's mean distance to its `neighbors` nearest,
// then keep the points inside mean + std_dev_mul * sigma. `out` is cleared
// first; `stats` may be null. Returns kCancelled if the token fires.
Status statistical_outlier_filter(Span<const PointVertex> in, const OutlierFilterConfig& cfg,
                                  double auto_radius_hint_m, std::vector<PointVertex>* out,
                                  OutlierFilterStats* stats = nullptr,
                                  CancelToken* cancel = nullptr);

// --- streaming accumulator --------------------------------------------------
//
// The re-integration pass pushes every point of a full-density capture through
// the optimized trajectory. A 30-minute Mid-360 session is ~360 M raw points
// (5.7 GB as PointVertex), which must not be resident — but the voxel-deduped
// result is bounded by the VOLUME scanned, not the session length, exactly as
// A6's IVox is. So dedup happens on the way in, not afterwards.
//
// Insertion order is preserved: extract() emits voxels in the order their
// first point arrived.
class VoxelAccumulator {
 public:
  explicit VoxelAccumulator(double voxel_size_m = 0.05, bool average = true);
  ~VoxelAccumulator();
  VoxelAccumulator(const VoxelAccumulator&) = delete;
  VoxelAccumulator& operator=(const VoxelAccumulator&) = delete;

  void add(double x, double y, double z, std::uint8_t r, std::uint8_t g, std::uint8_t b,
           std::uint8_t a = 255);
  void add(Span<const PointVertex> points);

  std::size_t voxel_count() const;
  std::uint64_t points_seen() const;
  void extract(std::vector<PointVertex>* out) const;
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_CLOUD_FILTER_H
