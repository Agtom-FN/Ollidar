// point_grid.h — the unbounded, insertion-ordered hash grid the post
// pipeline searches and dedups with.
//
// INTERNAL to src/slam/post/. Nothing outside this directory may include it
// (DESIGN.md §1).
//
// WHY NOT A6's IVox. docs/A6-lio.md §9 item 6 tells A7 to reuse IVox, and A7
// does — for the full-density LIO re-run, which is IVox's job and where its
// two hard bounds (max_points_per_voxel, max_voxels) are exactly the right
// safety property for a live-derived map. They are the wrong property here:
//
//   * The statistical outlier filter asks "how far is this point from its 8
//     nearest neighbours". A structure that silently stops storing the 21st
//     point of a cell answers that question wrongly and *systematically* —
//     points in dense regions look isolated, which is the opposite of the
//     truth, and the filter then deletes the best-sampled surfaces in the
//     scan. There is no threshold that fixes this; the structure has to hold
//     every point it is asked to hold.
//   * IVox stores float triples and cannot hand back a payload, so it cannot
//     be the voxel-dedup accumulator (which has to average colour) and cannot
//     be the ICP target (which needs stable indices).
//   * IVox::knn caps k at 8 (kMaxK) — fine for a plane fit, but a hard ceiling
//     A7 should not inherit for a filter parameter a user can turn up.
//
// So: ~150 lines, the same three determinism rules IVox lives by (fixed
// neighbour-offset order, insertion order within a cell, ties keep the earlier
// candidate), and no caps.
#ifndef SCANENGINE_SRC_SLAM_POST_POINT_GRID_H
#define SCANENGINE_SRC_SLAM_POST_POINT_GRID_H

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace scanengine {
namespace post {
namespace detail {

// 64-bit key from three quantized coordinates. 21 bits each covers
// +-1,048,575 cells, i.e. +-52 km at a 5 cm pitch — past any plausible
// session, and a collision would only ever merge two cells 100 km apart
// (which the per-point distance test in every consumer then rejects).
inline std::uint64_t cell_key(std::int64_t x, std::int64_t y, std::int64_t z) {
  const std::uint64_t ux = static_cast<std::uint64_t>(x + 1048576) & 0x1FFFFFull;
  const std::uint64_t uy = static_cast<std::uint64_t>(y + 1048576) & 0x1FFFFFull;
  const std::uint64_t uz = static_cast<std::uint64_t>(z + 1048576) & 0x1FFFFFull;
  return (ux << 42) | (uy << 21) | uz;
}

inline std::int64_t quantize(double v, double inv_size) {
  return static_cast<std::int64_t>(std::floor(v * inv_size));
}

// A spatial index over an EXTERNAL point array: the grid stores indices, the
// caller owns the points. That keeps one copy of a 20 M-point cloud in memory
// instead of two.
//
// Determinism: cells are visited in a fixed 3x3x3 offset order and, inside a
// cell, in insertion order; `knn` breaks distance ties in favour of the
// candidate seen first. The unordered_map is only ever probed, never iterated.
class PointIndex {
 public:
  PointIndex() = default;

  void build(const float* xyz_stride, std::size_t stride_floats, std::size_t count,
             double cell_size_m) {
    cell_size_ = cell_size_m > 0.0 ? cell_size_m : 1.0;
    inv_ = 1.0 / cell_size_;
    cells_.clear();
    cells_.reserve(count / 4 + 16);
    for (std::size_t i = 0; i < count; ++i) {
      const float* p = xyz_stride + i * stride_floats;
      const std::uint64_t k = cell_key(quantize(p[0], inv_), quantize(p[1], inv_),
                                       quantize(p[2], inv_));
      cells_[k].push_back(static_cast<std::uint32_t>(i));
    }
    base_ = xyz_stride;
    stride_ = stride_floats;
    count_ = count;
  }

  std::size_t size() const { return count_; }
  std::size_t cell_count() const { return cells_.size(); }
  double cell_size() const { return cell_size_; }

  // Up to `k` nearest points within `max_dist`, nearest first. Writes indices
  // and squared distances; returns how many were written. `k` must be <=
  // out_idx capacity.
  std::size_t knn(const double q[3], std::size_t k, double max_dist,
                  std::uint32_t* out_idx, double* out_d2) const {
    if (k == 0 || count_ == 0) return 0;
    const double max_d2 = max_dist * max_dist;
    const std::int64_t cx = quantize(q[0], inv_);
    const std::int64_t cy = quantize(q[1], inv_);
    const std::int64_t cz = quantize(q[2], inv_);
    // How many cell rings the radius spans; 1 for the usual "radius <= cell".
    const int ring = static_cast<int>(std::ceil(max_dist / cell_size_ - 1e-9));
    const int r = ring < 1 ? 1 : ring;

    std::size_t n = 0;
    for (int dz = -r; dz <= r; ++dz) {
      for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
          auto it = cells_.find(cell_key(cx + dx, cy + dy, cz + dz));
          if (it == cells_.end()) continue;
          for (std::uint32_t idx : it->second) {
            const float* p = base_ + static_cast<std::size_t>(idx) * stride_;
            const double ddx = static_cast<double>(p[0]) - q[0];
            const double ddy = static_cast<double>(p[1]) - q[1];
            const double ddz = static_cast<double>(p[2]) - q[2];
            const double d2 = ddx * ddx + ddy * ddy + ddz * ddz;
            if (d2 > max_d2) continue;
            if (n < k) {
              // Insertion sort into the (short) result list. Strict `<` keeps
              // the earlier-seen candidate on a tie.
              std::size_t pos = n;
              while (pos > 0 && d2 < out_d2[pos - 1]) {
                out_d2[pos] = out_d2[pos - 1];
                out_idx[pos] = out_idx[pos - 1];
                --pos;
              }
              out_d2[pos] = d2;
              out_idx[pos] = idx;
              ++n;
            } else if (d2 < out_d2[k - 1]) {
              std::size_t pos = k - 1;
              while (pos > 0 && d2 < out_d2[pos - 1]) {
                out_d2[pos] = out_d2[pos - 1];
                out_idx[pos] = out_idx[pos - 1];
                --pos;
              }
              out_d2[pos] = d2;
              out_idx[pos] = idx;
            }
          }
        }
      }
    }
    return n;
  }

 private:
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
  const float* base_ = nullptr;
  std::size_t stride_ = 4;
  std::size_t count_ = 0;
  double cell_size_ = 1.0;
  double inv_ = 1.0;
};

}  // namespace detail
}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SRC_SLAM_POST_POINT_GRID_H
