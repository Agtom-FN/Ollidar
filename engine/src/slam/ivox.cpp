// ivox.cpp — implementation of include/scanengine/slam/ivox.h.
#include "scanengine/slam/ivox.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace scanengine {
namespace {

struct Key {
  std::int32_t x, y, z;
  bool operator==(const Key& o) const noexcept { return x == o.x && y == o.y && z == o.z; }
};

// Teschner et al.'s spatial hash — three large primes, xor-combined. Cheap,
// and it does not collapse axis-aligned structure the way a shift-and-add
// hash does (a scan of a flat wall is exactly axis-aligned structure).
struct KeyHash {
  std::size_t operator()(const Key& k) const noexcept {
    const std::uint64_t h = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.x)) * 73856093ull) ^
                            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.y)) * 19349669ull) ^
                            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.z)) * 83492791ull);
    return static_cast<std::size_t>(h);
  }
};

struct Voxel {
  // Fixed inline storage: max_points_per_voxel is a config value but the
  // ceiling is compile-time, so a voxel never heap-allocates and a million
  // of them never fragment.
  static constexpr std::uint32_t kCap = 32;
  float p[kCap][3];
  std::uint32_t n = 0;
};

inline std::int32_t quantize(double v, double inv_size) {
  return static_cast<std::int32_t>(std::floor(v * inv_size));
}

}  // namespace

struct IVox::Impl {
  IVoxConfig cfg;
  double inv_size = 2.0;
  std::uint32_t cap = 20;
  std::unordered_map<Key, Voxel, KeyHash> grid;
  std::size_t points = 0;
};

IVox::IVox(const IVoxConfig& cfg) : impl_(new Impl()) {
  impl_->cfg = cfg;
  if (!(impl_->cfg.voxel_size_m > 0.0)) impl_->cfg.voxel_size_m = 0.5;
  impl_->inv_size = 1.0 / impl_->cfg.voxel_size_m;
  impl_->cap = impl_->cfg.max_points_per_voxel;
  if (impl_->cap == 0) impl_->cap = 1;
  if (impl_->cap > Voxel::kCap) impl_->cap = Voxel::kCap;
  impl_->cfg.max_points_per_voxel = impl_->cap;
}

IVox::~IVox() = default;

const IVoxConfig& IVox::config() const { return impl_->cfg; }
std::size_t IVox::voxel_count() const { return impl_->grid.size(); }
std::size_t IVox::point_count() const { return impl_->points; }

void IVox::clear() {
  impl_->grid.clear();
  impl_->points = 0;
}

bool IVox::insert(double x, double y, double z) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
  const Key k{quantize(x, impl_->inv_size), quantize(y, impl_->inv_size),
              quantize(z, impl_->inv_size)};
  auto it = impl_->grid.find(k);
  if (it == impl_->grid.end()) {
    if (impl_->grid.size() >= impl_->cfg.max_voxels) return false;
    it = impl_->grid.emplace(k, Voxel{}).first;
  }
  Voxel& v = it->second;
  if (v.n >= impl_->cap) return false;
  v.p[v.n][0] = static_cast<float>(x);
  v.p[v.n][1] = static_cast<float>(y);
  v.p[v.n][2] = static_cast<float>(z);
  ++v.n;
  ++impl_->points;
  return true;
}

std::size_t IVox::knn(const double q[3], std::size_t k, double max_dist_m, double* out_xyz) const {
  if (k == 0 || k > kMaxK) return 0;
  const double max_d2 = max_dist_m * max_dist_m;

  // Bounded insertion sort over a k-slot array. Deterministic (equal
  // distances keep the earlier candidate) and, at k <= 8, faster than any
  // heap. `seq` is the global visit index and is only used to make the tie
  // rule explicit rather than incidental.
  double best_d2[kMaxK];
  double best_p[kMaxK][3];
  std::size_t have = 0;

  const Key base{quantize(q[0], impl_->inv_size), quantize(q[1], impl_->inv_size),
                 quantize(q[2], impl_->inv_size)};
  const int lo = impl_->cfg.search_27 ? -1 : 0;
  const int hi = impl_->cfg.search_27 ? 1 : 0;

  auto scan_voxel = [&](const Key& key) {
    auto it = impl_->grid.find(key);
    if (it == impl_->grid.end()) return;
    const Voxel& v = it->second;
    for (std::uint32_t i = 0; i < v.n; ++i) {
      const double dx = static_cast<double>(v.p[i][0]) - q[0];
      const double dy = static_cast<double>(v.p[i][1]) - q[1];
      const double dz = static_cast<double>(v.p[i][2]) - q[2];
      const double d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > max_d2) continue;
      if (have == k && d2 >= best_d2[k - 1]) continue;
      std::size_t pos = have < k ? have : k - 1;
      while (pos > 0 && best_d2[pos - 1] > d2) {
        best_d2[pos] = best_d2[pos - 1];
        best_p[pos][0] = best_p[pos - 1][0];
        best_p[pos][1] = best_p[pos - 1][1];
        best_p[pos][2] = best_p[pos - 1][2];
        --pos;
      }
      best_d2[pos] = d2;
      best_p[pos][0] = static_cast<double>(v.p[i][0]);
      best_p[pos][1] = static_cast<double>(v.p[i][1]);
      best_p[pos][2] = static_cast<double>(v.p[i][2]);
      if (have < k) ++have;
    }
  };

  for (int dx = lo; dx <= hi; ++dx) {
    for (int dy = lo; dy <= hi; ++dy) {
      for (int dz = lo; dz <= hi; ++dz) {
        scan_voxel(Key{base.x + dx, base.y + dy, base.z + dz});
      }
    }
  }

  for (std::size_t i = 0; i < have; ++i) {
    out_xyz[i * 3 + 0] = best_p[i][0];
    out_xyz[i * 3 + 1] = best_p[i][1];
    out_xyz[i * 3 + 2] = best_p[i][2];
  }
  return have;
}

std::size_t IVox::trim(const double centre[3], double radius_m) {
  if (!(radius_m > 0.0)) return 0;
  const double r2 = radius_m * radius_m;
  const double s = impl_->cfg.voxel_size_m;
  std::size_t removed = 0;
  for (auto it = impl_->grid.begin(); it != impl_->grid.end();) {
    const double cx = (static_cast<double>(it->first.x) + 0.5) * s - centre[0];
    const double cy = (static_cast<double>(it->first.y) + 0.5) * s - centre[1];
    const double cz = (static_cast<double>(it->first.z) + 0.5) * s - centre[2];
    if (cx * cx + cy * cy + cz * cz > r2) {
      impl_->points -= it->second.n;
      it = impl_->grid.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

}  // namespace scanengine
