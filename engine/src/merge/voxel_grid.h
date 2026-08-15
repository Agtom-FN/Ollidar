// voxel_grid.h — the small deterministic voxel table src/merge/ shares.
// INTERNAL to src/merge/ (DESIGN.md §1: nothing in src/ is included across
// module boundaries).
//
// Two properties, both load-bearing:
//
//   * EXACT KEYS. The key is the (i, j, k) triple, not a packed 64-bit hash
//     of it, so two far-apart voxels can never collide into one. The packed
//     form is a real hazard at merge scale: a 3 cm pitch over a 400 m site is
//     ~13,000 cells per axis, and a 21-bit-per-axis packing silently wraps
//     the moment a coordinate leaves its window. std::unordered_map compares
//     keys, so the hash only has to be fast.
//   * INSERTION ORDER. Nothing iterates the hash container on a path that
//     affects a result — the same rule A6 established and A7 kept
//     (docs/A7-post.md §6.4). Every table here keeps a parallel vector in
//     first-touch order and that vector is what the output is built from.
#ifndef SCANENGINE_SRC_MERGE_VOXEL_GRID_H
#define SCANENGINE_SRC_MERGE_VOXEL_GRID_H

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace scanengine {
namespace merge {
namespace detail {

struct VoxelKey {
  std::int32_t x = 0, y = 0, z = 0;
  bool operator==(const VoxelKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct VoxelKeyHash {
  std::size_t operator()(const VoxelKey& k) const noexcept {
    // Three odd primes and a final mix. Cheap, and spreads the sign bit —
    // merge coordinates straddle the origin far more often than a single
    // session's do.
    std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.x)) * 0x9E3779B185EBCA87ull;
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.y)) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.z)) * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
    return static_cast<std::size_t>(h);
  }
};

// False when the coordinate does not fit an int32 cell index at this pitch
// (|v| > ~64 km at 3 cm), which is a nonsense coordinate rather than a merge
// this code should attempt.
inline bool voxel_of(double x, double y, double z, double inv_pitch, VoxelKey* out) {
  const double fx = std::floor(x * inv_pitch);
  const double fy = std::floor(y * inv_pitch);
  const double fz = std::floor(z * inv_pitch);
  const double kLimit = 2147483000.0;
  if (!(std::fabs(fx) < kLimit) || !(std::fabs(fy) < kLimit) || !(std::fabs(fz) < kLimit)) {
    return false;
  }
  out->x = static_cast<std::int32_t>(fx);
  out->y = static_cast<std::int32_t>(fy);
  out->z = static_cast<std::int32_t>(fz);
  return true;
}

// An occupancy set with a "is anything within one voxel of this position"
// query (the 2x2x2 block around the position offset by half a pitch), which
// is how overlap gets a tolerance without dilating the stored set 27-fold.
class VoxelSet {
 public:
  explicit VoxelSet(double pitch_m) : inv_(pitch_m > 0.0 ? 1.0 / pitch_m : 0.0) {}

  bool valid() const { return inv_ > 0.0; }

  void insert(double x, double y, double z) {
    VoxelKey k;
    if (!voxel_of(x, y, z, inv_, &k)) return;
    cells_.emplace(k, 0u);
  }

  std::size_t size() const { return cells_.size(); }

  bool near(double x, double y, double z) const {
    VoxelKey k;
    if (!voxel_of(x - 0.5 / inv_, y - 0.5 / inv_, z - 0.5 / inv_, inv_, &k)) return false;
    for (int dz = 0; dz < 2; ++dz) {
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const VoxelKey q{k.x + dx, k.y + dy, k.z + dz};
          if (cells_.find(q) != cells_.end()) return true;
        }
      }
    }
    return false;
  }

 private:
  double inv_ = 0.0;
  std::unordered_map<VoxelKey, std::uint32_t, VoxelKeyHash> cells_;
};

// voxel -> the index of whoever claimed it first, plus the claim order.
class VoxelClaimMap {
 public:
  explicit VoxelClaimMap(double pitch_m) : inv_(pitch_m > 0.0 ? 1.0 / pitch_m : 0.0) {}

  bool valid() const { return inv_ > 0.0; }

  // True when this call claimed the voxel; false when somebody already had
  // it (or the coordinate is unrepresentable, which is also "not claimed").
  bool claim(double x, double y, double z, std::uint32_t owner) {
    VoxelKey k;
    if (!voxel_of(x, y, z, inv_, &k)) return false;
    const auto it = cells_.find(k);
    if (it != cells_.end()) return false;
    cells_.emplace(k, owner);
    return true;
  }

  std::size_t size() const { return cells_.size(); }

 private:
  double inv_ = 0.0;
  std::unordered_map<VoxelKey, std::uint32_t, VoxelKeyHash> cells_;
};

}  // namespace detail
}  // namespace merge
}  // namespace scanengine

#endif  // SCANENGINE_SRC_MERGE_VOXEL_GRID_H
