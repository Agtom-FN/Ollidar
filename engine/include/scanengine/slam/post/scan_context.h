// scan_context.h — Scan Context place recognition, for loop-closure
// candidates (Tech Spec §3.3: "Scan Context loop candidates").
//
// Implements Kim & Kim, "Scan Context: Egocentric Spatial Descriptor for Place
// Recognition within 3D Point Cloud Map" (IROS 2018). It is implemented here
// rather than pulled in because it is genuinely small — a 20x60 matrix of
// maximum heights, a 20-vector rotation-invariant key, and a shifted column
// distance — and because A7's dependency budget is already spent (see
// pose_graph.h on GTSAM).
//
// THE THREE PROPERTIES THAT MAKE IT THE RIGHT DESCRIPTOR HERE:
//
//   1. It is YAW-INVARIANT BY CONSTRUCTION, and it hands back the yaw as a
//      side effect. A handheld scanner revisits a corridor facing the other
//      way; a descriptor that has to be searched over rotations is a descriptor
//      that is searched 60 times. The column shift that minimizes the distance
//      IS the relative yaw, which is then the ICP initialization — so the step
//      that finds the candidate also solves the hardest part of verifying it.
//   2. It needs a GRAVITY-ALIGNED z, and A6 guarantees one: the ESKF's static
//      initialization builds the world frame with +Z along measured gravity
//      (docs/A6-lio.md §3.1, exact to 1e-9 on a tilted scanner). Without that
//      the max-height bin is meaningless. This is the coupling to be aware of
//      if A7's input ever stops coming from the LIO.
//   3. It is a bin maximum, not an occupancy count, so it is insensitive to
//      point density — which changes by 6x across the decimation settings in
//      docs/A6-lio.md §8 and by more than that between indoor and outdoor.
//
// WHAT IT IS NOT: a verifier. A Scan Context match is a *candidate*. Two
// parallel corridors produce the same descriptor and the descriptor cannot
// tell them apart; that is what loop_closure.h's point-to-plane ICP against
// the local map is for, and no candidate reaches the pose graph without it.
//
// Owner: A7.
#ifndef SCANENGINE_SLAM_POST_SCAN_CONTEXT_H
#define SCANENGINE_SLAM_POST_SCAN_CONTEXT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"

namespace scanengine {
namespace post {

struct ScanContextConfig {
  // 20 rings x 60 sectors is the paper's configuration and what its published
  // KITTI numbers are measured with. 60 sectors is also a 6-degree yaw
  // resolution, which is a good ICP initialization.
  std::uint32_t rings = 20;
  std::uint32_t sectors = 60;
  // Points beyond this are ignored: the outer rings would otherwise be filled
  // by whatever happened to be visible at 60 m, which is not a property of the
  // *place*. 40 m matches the paper's 80 m KITTI setting scaled to a Mid-360
  // indoor/urban range.
  double max_radius_m = 40.0;
  // Bins with no points get this height. The paper uses 0; using the floor of
  // the scan instead would make "no data" indistinguishable from "flat ground".
  float empty_value = 0.0f;

  // --- search ------------------------------------------------------------
  // Ring-key (rotation-invariant) shortlist size. The paper uses a KD-tree
  // over ring keys; with a few thousand keyframes a linear scan of a 20-float
  // key costs microseconds, is trivially deterministic, and removes a data
  // structure from the module. Revisit past ~50k keyframes.
  std::uint32_t ring_key_candidates = 10;
  // Accept a candidate below this Scan Context distance. The paper's
  // high-precision operating point, kept deliberately: a false candidate here
  // is not a false loop — it still has to survive the ICP gate — so the cost
  // of being generous is CPU, not a folded map.
  //
  // THE THRESHOLD IS SCENE-DEPENDENT and this default is a starting point, not
  // a constant of nature. Measured in tests/test_post.cpp on a 24 x 18 x 3.5 m
  // indoor hall: revisiting a place scores 0.000-0.004, five different places
  // score 0.096-0.403, a 22x separation. Indoors the whole range is compressed
  // by roughly an order of magnitude against the paper's outdoor figures,
  // because a flat ceiling puts nearly every occupied bin at the same height
  // and the cosine term saturates — see engine/docs/A7-post.md §5.
  double distance_threshold = 0.13;
  // Exclusion window: a keyframe always matches its own neighbours. Both are
  // applied; either may be 0.
  std::uint32_t min_index_gap = 30;
  double min_time_gap_s = 15.0;
};

struct ScanContextDescriptor {
  std::int64_t t_ns = 0;
  std::uint32_t rings = 0;
  std::uint32_t sectors = 0;
  // rings * sectors, row-major: bin(r, s) = data[r * sectors + s].
  std::vector<float> data;
  // Row means: rotation-invariant, `rings` long.
  std::vector<float> ring_key;
  // Column means: `sectors` long. Kept for diagnostics and for a future
  // coarse yaw pre-alignment.
  std::vector<float> sector_key;
  std::uint32_t points = 0;  // points that landed inside max_radius_m
};

struct ScanContextMatch {
  bool found = false;
  std::uint32_t index = 0;      // the earlier keyframe this one matches
  double distance = 1.0;        // 0 = identical, 1 = nothing in common
  double yaw_rad = 0.0;         // rotation of the MATCH from the QUERY:
                                //   R_match_from_query = Rz(yaw_rad)
  std::uint32_t shift = 0;      // the winning sector shift
  std::uint32_t candidates = 0; // how many shortlisted entries were scored
};

// Append-only database of keyframe descriptors. Not thread-safe; the pipeline
// owns one and drives it from one thread.
class ScanContextDb {
 public:
  explicit ScanContextDb(const ScanContextConfig& cfg = {});
  ~ScanContextDb();
  ScanContextDb(const ScanContextDb&) = delete;
  ScanContextDb& operator=(const ScanContextDb&) = delete;

  // `body_points` are in the keyframe's own body frame, z up. Returns the new
  // descriptor's index (== keyframe index, since the pipeline adds one per
  // keyframe in order).
  std::uint32_t add(Span<const PointVertex> body_points, std::int64_t t_ns);

  // Search entries [0, index) for a match to entry `index`, honouring the
  // exclusion window. `found == false` when nothing clears the threshold.
  ScanContextMatch query(std::uint32_t index) const;

  std::size_t size() const;
  const ScanContextDescriptor& descriptor(std::uint32_t index) const;
  const ScanContextConfig& config() const;
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The paper's distance: mean over columns of (1 - cosine similarity of the
// two column vectors), minimized over all cyclic sector shifts. Columns where
// both descriptors are entirely empty are skipped rather than scored as a
// perfect match — otherwise two mostly-empty scans are "identical".
//
// Exposed so a test can check the descriptor and the search independently.
double scan_context_distance(const ScanContextDescriptor& a, const ScanContextDescriptor& b,
                             std::uint32_t* best_shift);

// Build one descriptor without a database (tests, and A13's merge if it ever
// wants coarse place matching across sessions).
Status build_scan_context(Span<const PointVertex> body_points, const ScanContextConfig& cfg,
                          std::int64_t t_ns, ScanContextDescriptor* out);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_SCAN_CONTEXT_H
