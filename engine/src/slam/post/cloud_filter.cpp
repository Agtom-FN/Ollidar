// cloud_filter.cpp — voxel dedup, statistical outlier removal, and the
// streaming voxel accumulator (A7).
#include "scanengine/slam/post/cloud_filter.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "point_grid.h"

namespace scanengine {
namespace post {
namespace {

struct VoxelAcc {
  double x = 0.0, y = 0.0, z = 0.0;
  double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
  std::uint32_t n = 0;
  PointVertex first{};
};

}  // namespace

std::size_t voxel_downsample(Span<const PointVertex> in, const VoxelDedupConfig& cfg,
                             std::vector<PointVertex>* out, CancelToken* cancel) {
  out->clear();
  if (in.size() == 0) return 0;
  if (!(cfg.voxel_size_m > 0.0)) {
    out->assign(in.data(), in.data() + in.size());
    return out->size();
  }
  const double inv = 1.0 / cfg.voxel_size_m;
  std::unordered_map<std::uint64_t, std::uint32_t> slot;
  slot.reserve(in.size() / 4 + 16);
  std::vector<VoxelAcc> acc;
  acc.reserve(in.size() / 4 + 16);

  for (std::size_t i = 0; i < in.size(); ++i) {
    if ((i & 0xFFFFu) == 0 && cancelled(cancel)) return 0;
    const PointVertex& p = in[i];
    const std::uint64_t k = detail::cell_key(detail::quantize(p.x, inv), detail::quantize(p.y, inv),
                                             detail::quantize(p.z, inv));
    auto it = slot.find(k);
    std::uint32_t idx;
    if (it == slot.end()) {
      idx = static_cast<std::uint32_t>(acc.size());
      slot.emplace(k, idx);
      acc.emplace_back();
      acc.back().first = p;
    } else {
      idx = it->second;
    }
    VoxelAcc& v = acc[idx];
    v.x += p.x;
    v.y += p.y;
    v.z += p.z;
    v.r += p.r;
    v.g += p.g;
    v.b += p.b;
    v.a += p.a;
    ++v.n;
  }

  out->reserve(acc.size());
  for (const VoxelAcc& v : acc) {
    PointVertex p = v.first;
    const double inv_n = 1.0 / static_cast<double>(v.n);
    if (cfg.average_position) {
      p.x = static_cast<float>(v.x * inv_n);
      p.y = static_cast<float>(v.y * inv_n);
      p.z = static_cast<float>(v.z * inv_n);
    }
    if (cfg.average_color) {
      p.r = static_cast<std::uint8_t>(v.r * inv_n + 0.5);
      p.g = static_cast<std::uint8_t>(v.g * inv_n + 0.5);
      p.b = static_cast<std::uint8_t>(v.b * inv_n + 0.5);
      p.a = static_cast<std::uint8_t>(v.a * inv_n + 0.5);
    }
    out->push_back(p);
  }
  return out->size();
}

Status statistical_outlier_filter(Span<const PointVertex> in, const OutlierFilterConfig& cfg,
                                  double auto_radius_hint_m, std::vector<PointVertex>* out,
                                  OutlierFilterStats* stats, CancelToken* cancel) {
  out->clear();
  OutlierFilterStats st;
  st.in = in.size();
  if (!cfg.enabled || in.size() == 0) {
    out->assign(in.data(), in.data() + in.size());
    st.kept = out->size();
    if (stats != nullptr) *stats = st;
    return kOkStatus;
  }
  const std::uint32_t k = cfg.neighbors == 0 ? 1u : cfg.neighbors;
  double radius = cfg.search_radius_m;
  if (!(radius > 0.0)) {
    radius = auto_radius_hint_m > 0.0 ? 8.0 * auto_radius_hint_m : 0.5;
  }

  // Cell size == radius keeps the 3x3x3 probe exact for this radius.
  detail::PointIndex index;
  index.build(&in[0].x, sizeof(PointVertex) / sizeof(float), in.size(), radius);

  std::vector<double> mean_d(in.size(), -1.0);
  std::vector<std::uint32_t> idx(k + 1);
  std::vector<double> d2(k + 1);

  double sum = 0.0, sum2 = 0.0;
  std::size_t counted = 0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if ((i & 0xFFFFu) == 0 && cancelled(cancel)) {
      return set_last_error(ScanError::kCancelled, "post: outlier filter cancelled");
    }
    const double q[3] = {in[i].x, in[i].y, in[i].z};
    // k + 1 because the query point is its own nearest neighbour.
    const std::size_t n = index.knn(q, static_cast<std::size_t>(k) + 1, radius, idx.data(),
                                    d2.data());
    if (n < static_cast<std::size_t>(cfg.min_neighbors) + 1) {
      ++st.removed_isolated;
      continue;
    }
    double acc = 0.0;
    std::size_t used = 0;
    for (std::size_t j = 0; j < n; ++j) {
      if (idx[j] == static_cast<std::uint32_t>(i)) continue;  // self
      acc += std::sqrt(d2[j]);
      ++used;
    }
    if (used == 0) {
      ++st.removed_isolated;
      continue;
    }
    const double m = acc / static_cast<double>(used);
    mean_d[i] = m;
    sum += m;
    sum2 += m * m;
    ++counted;
  }

  if (counted == 0) {
    st.kept = 0;
    if (stats != nullptr) *stats = st;
    return kOkStatus;
  }
  const double mean = sum / static_cast<double>(counted);
  const double var = std::max(0.0, sum2 / static_cast<double>(counted) - mean * mean);
  const double sigma = std::sqrt(var);
  const double thresh = mean + cfg.std_dev_mul * sigma;
  st.mean_distance_m = mean;
  st.std_dev_m = sigma;
  st.threshold_m = thresh;

  out->reserve(counted);
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (mean_d[i] < 0.0) continue;  // already removed as isolated
    if (mean_d[i] > thresh) {
      ++st.removed_statistical;
      continue;
    }
    out->push_back(in[i]);
  }
  st.kept = out->size();
  if (stats != nullptr) *stats = st;
  return kOkStatus;
}

// --- VoxelAccumulator -------------------------------------------------------

struct VoxelAccumulator::Impl {
  double voxel = 0.05;
  double inv = 20.0;
  bool average = true;
  std::unordered_map<std::uint64_t, std::uint32_t> slot;
  std::vector<VoxelAcc> acc;
  std::uint64_t seen = 0;
};

VoxelAccumulator::VoxelAccumulator(double voxel_size_m, bool average) : impl_(new Impl) {
  impl_->voxel = voxel_size_m > 0.0 ? voxel_size_m : 0.05;
  impl_->inv = 1.0 / impl_->voxel;
  impl_->average = average;
}

VoxelAccumulator::~VoxelAccumulator() = default;

void VoxelAccumulator::add(double x, double y, double z, std::uint8_t r, std::uint8_t g,
                           std::uint8_t b, std::uint8_t a) {
  Impl& im = *impl_;
  ++im.seen;
  const std::uint64_t k = detail::cell_key(detail::quantize(x, im.inv), detail::quantize(y, im.inv),
                                           detail::quantize(z, im.inv));
  auto it = im.slot.find(k);
  std::uint32_t idx;
  if (it == im.slot.end()) {
    idx = static_cast<std::uint32_t>(im.acc.size());
    im.slot.emplace(k, idx);
    im.acc.emplace_back();
    PointVertex first;
    first.x = static_cast<float>(x);
    first.y = static_cast<float>(y);
    first.z = static_cast<float>(z);
    first.r = r;
    first.g = g;
    first.b = b;
    first.a = a;
    im.acc.back().first = first;
  } else {
    idx = it->second;
  }
  VoxelAcc& v = im.acc[idx];
  v.x += x;
  v.y += y;
  v.z += z;
  v.r += r;
  v.g += g;
  v.b += b;
  v.a += a;
  ++v.n;
}

void VoxelAccumulator::add(Span<const PointVertex> points) {
  for (std::size_t i = 0; i < points.size(); ++i) {
    const PointVertex& p = points[i];
    add(p.x, p.y, p.z, p.r, p.g, p.b, p.a);
  }
}

std::size_t VoxelAccumulator::voxel_count() const { return impl_->acc.size(); }
std::uint64_t VoxelAccumulator::points_seen() const { return impl_->seen; }

void VoxelAccumulator::extract(std::vector<PointVertex>* out) const {
  out->clear();
  out->reserve(impl_->acc.size());
  for (const VoxelAcc& v : impl_->acc) {
    PointVertex p = v.first;
    if (impl_->average) {
      const double inv_n = 1.0 / static_cast<double>(v.n);
      p.x = static_cast<float>(v.x * inv_n);
      p.y = static_cast<float>(v.y * inv_n);
      p.z = static_cast<float>(v.z * inv_n);
      p.r = static_cast<std::uint8_t>(v.r * inv_n + 0.5);
      p.g = static_cast<std::uint8_t>(v.g * inv_n + 0.5);
      p.b = static_cast<std::uint8_t>(v.b * inv_n + 0.5);
      p.a = static_cast<std::uint8_t>(v.a * inv_n + 0.5);
    }
    out->push_back(p);
  }
}

void VoxelAccumulator::clear() {
  impl_->slot.clear();
  impl_->acc.clear();
  impl_->seen = 0;
}

}  // namespace post
}  // namespace scanengine
