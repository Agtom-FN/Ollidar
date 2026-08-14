// scan_context.cpp — the Scan Context descriptor and its search (A7).
//
// Kim & Kim, IROS 2018. See the header for why it is implemented here and for
// the gravity-alignment coupling to A6.
#include "scanengine/slam/post/scan_context.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace scanengine {
namespace post {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;

}  // namespace

Status build_scan_context(Span<const PointVertex> body_points, const ScanContextConfig& cfg,
                          std::int64_t t_ns, ScanContextDescriptor* out) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "scan_context: null output");
  }
  const std::uint32_t nr = cfg.rings;
  const std::uint32_t ns = cfg.sectors;
  if (nr == 0 || ns == 0 || !(cfg.max_radius_m > 0.0)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "scan_context: rings/sectors/max_radius must be > 0");
  }
  out->t_ns = t_ns;
  out->rings = nr;
  out->sectors = ns;
  const std::size_t bins = static_cast<std::size_t>(nr) * ns;
  out->data.assign(bins, cfg.empty_value);
  out->points = 0;

  // Occupancy is tracked separately from the height, so a bin whose only
  // point is BELOW empty_value still records that height. Folding the two
  // into one array (the usual shortcut) makes an entirely-below-zero cloud —
  // a scan taken from a gantry, or any capture whose world origin is above
  // the floor — descriptor-identical to an empty one.
  std::vector<char> filled(bins, 0);
  const double ring_step = cfg.max_radius_m / static_cast<double>(nr);
  for (std::size_t i = 0; i < body_points.size(); ++i) {
    const PointVertex& p = body_points[i];
    const double x = p.x, y = p.y, z = p.z;
    const double rad = std::sqrt(x * x + y * y);
    if (rad >= cfg.max_radius_m) continue;
    std::uint32_t r = static_cast<std::uint32_t>(rad / ring_step);
    if (r >= nr) r = nr - 1;
    double az = std::atan2(y, x);
    if (az < 0.0) az += kTwoPi;
    std::uint32_t s = static_cast<std::uint32_t>(az * static_cast<double>(ns) / kTwoPi);
    if (s >= ns) s = ns - 1;
    const std::size_t bin = static_cast<std::size_t>(r) * ns + s;
    const float zf = static_cast<float>(z);
    if (filled[bin] == 0 || zf > out->data[bin]) out->data[bin] = zf;
    filled[bin] = 1;
    ++out->points;
  }

  out->ring_key.assign(nr, 0.f);
  for (std::uint32_t r = 0; r < nr; ++r) {
    double acc = 0.0;
    for (std::uint32_t s = 0; s < ns; ++s) acc += out->data[static_cast<std::size_t>(r) * ns + s];
    out->ring_key[r] = static_cast<float>(acc / static_cast<double>(ns));
  }
  out->sector_key.assign(ns, 0.f);
  for (std::uint32_t s = 0; s < ns; ++s) {
    double acc = 0.0;
    for (std::uint32_t r = 0; r < nr; ++r) acc += out->data[static_cast<std::size_t>(r) * ns + s];
    out->sector_key[s] = static_cast<float>(acc / static_cast<double>(nr));
  }
  return kOkStatus;
}

double scan_context_distance(const ScanContextDescriptor& a, const ScanContextDescriptor& b,
                             std::uint32_t* best_shift) {
  if (best_shift != nullptr) *best_shift = 0;
  if (a.rings != b.rings || a.sectors != b.sectors || a.rings == 0 || a.sectors == 0) return 1.0;
  const std::uint32_t nr = a.rings;
  const std::uint32_t ns = a.sectors;

  double best = 2.0;
  std::uint32_t best_s = 0;
  for (std::uint32_t shift = 0; shift < ns; ++shift) {
    double sum = 0.0;
    std::uint32_t used = 0;
    for (std::uint32_t s = 0; s < ns; ++s) {
      const std::uint32_t sb = (s + shift) % ns;
      double dot = 0.0, na = 0.0, nb = 0.0;
      for (std::uint32_t r = 0; r < nr; ++r) {
        const double va = a.data[static_cast<std::size_t>(r) * ns + s];
        const double vb = b.data[static_cast<std::size_t>(r) * ns + sb];
        dot += va * vb;
        na += va * va;
        nb += vb * vb;
      }
      // A column empty in BOTH descriptors carries no information; scoring it
      // as a perfect match is how two sparse scans become each other's twin.
      if (na <= 0.0 && nb <= 0.0) continue;
      ++used;
      if (na <= 0.0 || nb <= 0.0) {
        sum += 1.0;  // one has structure here and the other does not
        continue;
      }
      sum += 1.0 - dot / (std::sqrt(na) * std::sqrt(nb));
    }
    if (used == 0) continue;
    const double d = sum / static_cast<double>(used);
    // Strict `<` keeps the smallest shift on a tie — deterministic, and the
    // smallest shift is the smaller yaw correction.
    if (d < best) {
      best = d;
      best_s = shift;
    }
  }
  if (best > 1.0) return 1.0;
  if (best_shift != nullptr) *best_shift = best_s;
  return best;
}

// --- ScanContextDb ----------------------------------------------------------

struct ScanContextDb::Impl {
  ScanContextConfig cfg;
  std::vector<ScanContextDescriptor> entries;
};

ScanContextDb::ScanContextDb(const ScanContextConfig& cfg) : impl_(new Impl) { impl_->cfg = cfg; }
ScanContextDb::~ScanContextDb() = default;

std::uint32_t ScanContextDb::add(Span<const PointVertex> body_points, std::int64_t t_ns) {
  ScanContextDescriptor d;
  (void)build_scan_context(body_points, impl_->cfg, t_ns, &d);
  impl_->entries.push_back(std::move(d));
  return static_cast<std::uint32_t>(impl_->entries.size() - 1);
}

std::size_t ScanContextDb::size() const { return impl_->entries.size(); }
const ScanContextDescriptor& ScanContextDb::descriptor(std::uint32_t index) const {
  return impl_->entries[index];
}
const ScanContextConfig& ScanContextDb::config() const { return impl_->cfg; }
void ScanContextDb::clear() { impl_->entries.clear(); }

ScanContextMatch ScanContextDb::query(std::uint32_t index) const {
  ScanContextMatch m;
  const Impl& im = *impl_;
  if (index >= im.entries.size()) return m;
  const ScanContextDescriptor& q = im.entries[index];
  if (q.points == 0) return m;

  // Exclusion window: the last `min_index_gap` keyframes and anything inside
  // `min_time_gap_s` are the same place trivially.
  const std::int64_t min_gap_ns =
      static_cast<std::int64_t>(im.cfg.min_time_gap_s * 1e9);
  const std::uint32_t hi =
      index > im.cfg.min_index_gap ? index - im.cfg.min_index_gap : 0u;

  // Shortlist by ring key (L1 over the rotation-invariant key). Kept sorted
  // ascending by distance with the lower index winning ties.
  struct Cand {
    double d;
    std::uint32_t i;
  };
  const std::uint32_t want = im.cfg.ring_key_candidates == 0 ? 1u : im.cfg.ring_key_candidates;
  std::vector<Cand> shortlist;
  shortlist.reserve(want + 1);
  for (std::uint32_t i = 0; i < hi; ++i) {
    const ScanContextDescriptor& c = im.entries[i];
    if (c.points == 0 || c.rings != q.rings) continue;
    if (min_gap_ns > 0 && (q.t_ns - c.t_ns) < min_gap_ns) continue;
    double d = 0.0;
    for (std::uint32_t r = 0; r < q.rings; ++r) {
      d += std::fabs(static_cast<double>(q.ring_key[r]) - static_cast<double>(c.ring_key[r]));
    }
    if (shortlist.size() < want) {
      shortlist.push_back(Cand{d, i});
    } else if (d < shortlist.back().d) {
      shortlist.back() = Cand{d, i};
    } else {
      continue;
    }
    // Bubble the (possibly new) last element into place. Strict `<` keeps the
    // earlier-seen — i.e. lower-index — candidate ahead on a tie.
    std::size_t p = shortlist.size() - 1;
    while (p > 0 && shortlist[p].d < shortlist[p - 1].d) {
      const Cand tmp = shortlist[p - 1];
      shortlist[p - 1] = shortlist[p];
      shortlist[p] = tmp;
      --p;
    }
  }

  double best = im.cfg.distance_threshold;
  for (const Cand& c : shortlist) {
    std::uint32_t shift = 0;
    const double d = scan_context_distance(q, im.entries[c.i], &shift);
    ++m.candidates;
    if (d < best) {
      best = d;
      m.found = true;
      m.index = c.i;
      m.distance = d;
      m.shift = shift;
      // The winning shift aligns query column s with match column s + shift,
      // i.e. an azimuth in the query maps to azimuth + shift*(2pi/ns) in the
      // match, so R_match_from_query = Rz(+shift * 2pi/ns).
      m.yaw_rad = static_cast<double>(shift) * kTwoPi / static_cast<double>(q.sectors);
    }
  }
  return m;
}

}  // namespace post
}  // namespace scanengine
