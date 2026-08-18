// mount_watch.cpp — ROUND 13 (owner item 48). See mount_watch.h for why the
// observable is where the returns LAND and not the fan plane's attitude.

#include "scanengine/slam/post/mount_watch.h"

#include <algorithm>
#include <cmath>

#include "scanengine/core/log.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "mountwatch";

// Median of a vector, by a full sort so the answer cannot depend on the
// partial-ordering an nth_element happens to leave behind.
double median_of(std::vector<double>& v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Sensor position at `t`, by linear interpolation between the bracketing
// poses. Rotation is not needed: this measures WHERE a return landed relative
// to the sensor along gravity, and gravity is a world direction.
bool sensor_at(const std::vector<TrajPose>& poses, std::int64_t t, double out[3]) {
  if (poses.size() < 2) return false;
  if (t <= poses.front().t_ns) {
    for (int i = 0; i < 3; ++i) out[i] = poses.front().p[i];
    return true;
  }
  if (t >= poses.back().t_ns) {
    for (int i = 0; i < 3; ++i) out[i] = poses.back().p[i];
    return true;
  }
  std::size_t lo = 0, hi = poses.size() - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    if (poses[mid].t_ns <= t) lo = mid; else hi = mid;
  }
  const double span = static_cast<double>(poses[hi].t_ns - poses[lo].t_ns);
  const double u = span > 0.0 ? static_cast<double>(t - poses[lo].t_ns) / span : 0.0;
  for (int i = 0; i < 3; ++i) out[i] = poses[lo].p[i] + u * (poses[hi].p[i] - poses[lo].p[i]);
  return true;
}

}  // namespace

const char* to_string(MountWatchVerdict v) {
  switch (v) {
    case MountWatchVerdict::kOk: return "ok";
    case MountWatchVerdict::kNotMeasurable: return "not-measurable";
    case MountWatchVerdict::kSuspect: return "suspect";
    case MountWatchVerdict::kMismatch: return "mismatch";
  }
  return "unknown";
}

MountWatchReport check_mount_consistency(const std::vector<TrajPose>& poses,
                                         Span<const PointVertex> cloud,
                                         Span<const std::int64_t> point_times,
                                         const MountWatchConfig& cfg) {
  MountWatchReport rep;
  rep.window_seconds = cfg.window_seconds;
  rep.operator_message = "";
  if (poses.size() < 2 || cloud.size() == 0 || cloud.size() != point_times.size()) {
    rep.reason = "no cloud or no trajectory";
    rep.operator_message = "";
    return rep;
  }
  const int ax = (cfg.up_axis >= 0 && cfg.up_axis <= 2) ? cfg.up_axis : 1;

  const std::int64_t t0 = poses.front().t_ns;
  const std::int64_t t_end =
      cfg.window_seconds > 0.0
          ? t0 + static_cast<std::int64_t>(cfg.window_seconds * 1e9)
          : poses.back().t_ns;
  const std::int64_t rev_ns = static_cast<std::int64_t>(cfg.revolution_seconds * 1e9);
  if (rev_ns <= 0) {
    rep.reason = "revolution_seconds must be positive";
    return rep;
  }

  // Bucket by revolution index. A plain vector indexed by (t - t0) / rev_ns
  // rather than a map, so traversal order is the index order and nothing
  // depends on hashing.
  const std::size_t nbuckets =
      static_cast<std::size_t>((t_end - t0) / rev_ns) + 1;
  std::vector<std::vector<double>> bucket(nbuckets);
  std::size_t used = 0;
  std::size_t impossible = 0;
  std::vector<double> ranges;
  ranges.reserve(cloud.size() / 4 + 1);

  for (std::size_t i = 0; i < cloud.size(); ++i) {
    const std::int64_t t = point_times[i];
    if (t < t0 || t >= t_end) continue;
    double s[3];
    if (!sensor_at(poses, t, s)) continue;
    const double d[3] = {cloud[i].x - s[0], cloud[i].y - s[1], cloud[i].z - s[2]};
    const double r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(r > cfg.min_range_m)) continue;
    const double dy = d[ax];
    const std::size_t b = static_cast<std::size_t>((t - t0) / rev_ns);
    if (b < bucket.size()) bucket[b].push_back(dy);
    ranges.push_back(r);
    ++used;
    if (std::fabs(dy) > cfg.impossible_elevation_m) ++impossible;
  }

  rep.points = used;
  if (used == 0) {
    rep.reason = "no returns in the window";
    return rep;
  }
  rep.median_range_m = median_of(ranges);
  rep.impossible_fraction = static_cast<double>(impossible) / static_cast<double>(used);

  // Per-revolution vertical extent, p5..p95 so one stray return cannot set it.
  std::vector<double> extents;
  extents.reserve(bucket.size());
  for (std::vector<double>& b : bucket) {
    if (b.size() < cfg.min_points_per_revolution) continue;
    std::sort(b.begin(), b.end());
    const std::size_t lo = static_cast<std::size_t>(0.05 * static_cast<double>(b.size() - 1));
    const std::size_t hi = static_cast<std::size_t>(0.95 * static_cast<double>(b.size() - 1));
    extents.push_back(b[hi] - b[lo]);
  }
  rep.revolutions = extents.size();
  if (rep.revolutions < cfg.min_revolutions) {
    rep.verdict = MountWatchVerdict::kNotMeasurable;
    rep.reason = "too few complete revolutions to judge the mount";
    return rep;
  }
  rep.median_revolution_extent_m = median_of(extents);

  if (rep.impossible_fraction > cfg.max_impossible_fraction) {
    rep.verdict = MountWatchVerdict::kMismatch;
    rep.reason = "returns are landing at elevations no room has, at ordinary ranges";
    rep.operator_message =
        "The lidar is not where the mount reference says it is. Stop, seat the puck the way "
        "it was when you set the reference, and re-zero.";
  } else if (rep.median_revolution_extent_m > cfg.warn_vertical_extent_m) {
    rep.verdict = MountWatchVerdict::kSuspect;
    rep.reason = "each sweep spans more height than a room usually has, but nothing is impossible";
    rep.operator_message =
        "This sweep covers an unusually tall space. If you are indoors, check the puck is seated "
        "and re-zero.";
  } else {
    rep.verdict = MountWatchVerdict::kOk;
    rep.reason = "sweeps land inside a room-shaped envelope";
  }

  SCAN_LOG_INFO(kMod,
                "%s: %zu revolutions / %zu returns in %.1f s, median sweep height %.2f m, "
                "impossible-elevation fraction %.2f%%, median range %.2f m",
                to_string(rep.verdict), rep.revolutions, rep.points, rep.window_seconds,
                rep.median_revolution_extent_m, 100.0 * rep.impossible_fraction,
                rep.median_range_m);
  return rep;
}

}  // namespace post
}  // namespace scanengine
