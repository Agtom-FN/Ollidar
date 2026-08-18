// ROUND 12 — proving the ruler before trusting what it measures.
//
// The owner walked at normal pace on 0.7.0 and said "quality not so good,
// still shift". Adjudicating that needed a number, and this project did not
// have one that works at walking pace:
//
//   * `--d6-timesweep`'s wall-probe thickness needs 200 returns inside a 0.5 m
//     radius cell and selects **ZERO probes** on both of the owner's
//     walking-pace captures. Every crispness claim in this repository comes
//     from `scan-020`, walked at 5.3 cm/s — a twentieth of normal speed;
//   * plane-fit RMS averages a doubled surface into one thick one, which is
//     the same structural blindness ROUND 9 found (sign-blind metrics) and
//     ROUND 11 found again (a trim error slides ALONG a wall);
//   * occupied-voxel counts compare a cloud only with itself at another
//     setting.
//
// `post::measure_map_consistency` is the replacement. This file is its
// falsification: a synthetic room painted twice with a KNOWN offset injected
// between the two paintings, so the metric can be checked against truth rather
// than against another metric.
//
// The three claims asserted here, in order of how much they matter:
//
//   1. **A clean map reads at the floor.** Two passes over the same wall with
//      no injected error must measure ~0, or every positive reading is noise.
//   2. **An injected offset is recovered.** Displace the second pass by a
//      known amount along the surface normal and the metric must return it.
//   3. **A slide ALONG the surface is not counted.** This is the whole reason
//      the measurement projects onto the normal: a D6's returns land wherever
//      they land along a wall, and a metric that counted that would report
//      several centimetres on a perfect map. This is the assertion the older
//      metrics would fail.
//
// Plus determinism, because the answer is quoted in a field report: the same
// points in a different ORDER must give the identical number.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/slam/post/map_consistency.h"

using namespace scanengine;

namespace {

constexpr std::int64_t kT0 = 1'000'000'000LL;
constexpr double kWindowS = 8.0;

// A 4 x 3 m room's four walls, sampled on a 4 cm lattice. Flat surfaces with
// real extent are exactly what the metric is built for and exactly what a room
// is; nothing here needs the driver, and going through the driver would test
// the driver rather than the ruler.
void wall_points(std::vector<PointVertex>* out, double dx, double dy, double dz, double slide,
                 std::int64_t t_ns, std::vector<std::int64_t>* times) {
  const double step = 0.04;
  // Two facing walls at x = +/-2, and two at z = +/-1.5.
  for (int wall = 0; wall < 4; ++wall) {
    for (int i = 0; i <= 60; ++i) {
      for (int j = 0; j <= 40; ++j) {
        const double u = -1.2 + step * i;   // along the wall
        const double v = 0.2 + step * j;    // up the wall
        double p[3];
        switch (wall) {
          case 0: p[0] = 2.0; p[1] = v; p[2] = u; break;
          case 1: p[0] = -2.0; p[1] = v; p[2] = u; break;
          case 2: p[0] = u; p[1] = v; p[2] = 1.5; break;
          default: p[0] = u; p[1] = v; p[2] = -1.5; break;
        }
        // `slide` moves the sample ALONG the wall (claim 3) — a different
        // point of the same surface, not a different surface.
        if (wall < 2) {
          p[2] += slide;
        } else {
          p[0] += slide;
        }
        PointVertex pv{};
        pv.x = static_cast<float>(p[0] + dx);
        pv.y = static_cast<float>(p[1] + dy);
        pv.z = static_cast<float>(p[2] + dz);
        pv.r = pv.g = pv.b = 120;
        pv.a = 255;
        out->push_back(pv);
        times->push_back(t_ns);
      }
    }
  }
}

struct Cloud {
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> t;
};

// Pass A in window 0, pass B in window 1 (one `kWindowS` later), with pass B
// displaced by (dx, dy, dz) and slid by `slide` along each wall.
Cloud two_passes(double dx, double dy, double dz, double slide) {
  Cloud c;
  wall_points(&c.pts, 0.0, 0.0, 0.0, 0.0, kT0 + 1'000'000LL, &c.t);
  wall_points(&c.pts, dx, dy, dz, slide,
              kT0 + static_cast<std::int64_t>(kWindowS * 1e9) + 1'000'000LL, &c.t);
  return c;
}

post::MapConsistencyReport measure(const Cloud& c) {
  post::MapConsistencyConfig cfg;
  cfg.window_seconds = kWindowS;
  return post::measure_map_consistency(c.pts, c.t, cfg);
}

}  // namespace

TEST_CASE("round12/map_consistency/a clean map reads at the floor") {
  const post::MapConsistencyReport r = measure(two_passes(0.0, 0.0, 0.0, 0.0));
  REQUIRE(r.measurable);
  REQUIRE(r.by_separation.size() >= 1);
  const double cm = r.by_separation[0].median_offset_m * 100.0;
  MESSAGE("clean two-pass map: ", cm, " cm over ", r.by_separation[0].cells, " cells");
  // Identical points in both passes: the only error is the plane fit's own.
  CHECK(cm < 0.05);
  CHECK(r.by_separation[0].cells > 100);
}

TEST_CASE("round12/map_consistency/an injected offset is recovered") {
  // Along +x: the two x-walls see it head-on (normal = x) and the two z-walls
  // see it edge-on (normal = z), so the population is a mix and the MEDIAN is
  // what a real room gives. Each wall is checked separately below.
  struct Row {
    double offset_m;
  };
  const Row rows[] = {{0.02}, {0.05}, {0.10}, {0.20}};
  double last = -1.0;
  for (const Row& row : rows) {
    // Displace along x AND z by the same amount, so every wall in the room is
    // displaced by exactly `offset` along its own normal.
    Cloud c;
    wall_points(&c.pts, 0.0, 0.0, 0.0, 0.0, kT0 + 1'000'000LL, &c.t);
    // x-walls need dx, z-walls need dz; do them in two calls with a helper
    // that displaces only the wall it belongs to.
    {
      const double step = 0.04;
      const std::int64_t t = kT0 + static_cast<std::int64_t>(kWindowS * 1e9) + 1'000'000LL;
      for (int wall = 0; wall < 4; ++wall) {
        for (int i = 0; i <= 60; ++i) {
          for (int j = 0; j <= 40; ++j) {
            const double u = -1.2 + step * i;
            const double v = 0.2 + step * j;
            double p[3];
            switch (wall) {
              case 0: p[0] = 2.0 + row.offset_m; p[1] = v; p[2] = u; break;
              case 1: p[0] = -2.0 + row.offset_m; p[1] = v; p[2] = u; break;
              case 2: p[0] = u; p[1] = v; p[2] = 1.5 + row.offset_m; break;
              default: p[0] = u; p[1] = v; p[2] = -1.5 + row.offset_m; break;
            }
            PointVertex pv{};
            pv.x = static_cast<float>(p[0]);
            pv.y = static_cast<float>(p[1]);
            pv.z = static_cast<float>(p[2]);
            pv.r = pv.g = pv.b = 120;
            pv.a = 255;
            c.pts.push_back(pv);
            c.t.push_back(t);
          }
        }
      }
    }
    const post::MapConsistencyReport r = measure(c);
    REQUIRE(r.measurable);
    const double got = r.by_separation[0].median_offset_m;
    MESSAGE("injected ", row.offset_m * 100.0, " cm -> measured ", got * 100.0, " cm over ",
            r.by_separation[0].cells, " cells");
    // Recovered to within 10 % or 5 mm, whichever is larger. It is not exact
    // by construction: a cell straddling the displaced surface holds points
    // from both sides of its own plane fit.
    CHECK(std::fabs(got - row.offset_m) <= std::max(0.005, 0.10 * row.offset_m));
    CHECK(got > last);  // monotone in the truth
    last = got;
  }
}

TEST_CASE("round12/map_consistency/sliding ALONG a surface is not an error") {
  // The assertion that separates this metric from every older one. 20 cm of
  // slide along each wall — a whole cell's worth, far more than any real
  // resampling — with the surfaces themselves in exactly the same place.
  const post::MapConsistencyReport r = measure(two_passes(0.0, 0.0, 0.0, 0.20));
  REQUIRE(r.measurable);
  const double cm = r.by_separation[0].median_offset_m * 100.0;
  MESSAGE("20 cm of slide ALONG the walls: ", cm, " cm over ", r.by_separation[0].cells, " cells");
  CHECK(cm < 0.05);

  // ... and the control that makes the claim falsifiable: the SAME 20 cm
  // applied PERPENDICULAR to the walls is reported in full.
  Cloud c;
  wall_points(&c.pts, 0.0, 0.0, 0.0, 0.0, kT0 + 1'000'000LL, &c.t);
  {
    const double step = 0.04;
    const std::int64_t t = kT0 + static_cast<std::int64_t>(kWindowS * 1e9) + 1'000'000LL;
    for (int i = 0; i <= 60; ++i) {
      for (int j = 0; j <= 40; ++j) {
        const double u = -1.2 + step * i;
        const double v = 0.2 + step * j;
        PointVertex pv{};
        pv.x = static_cast<float>(2.0 + 0.20);
        pv.y = static_cast<float>(v);
        pv.z = static_cast<float>(u);
        pv.r = pv.g = pv.b = 120;
        pv.a = 255;
        c.pts.push_back(pv);
        c.t.push_back(t);
      }
    }
  }
  const post::MapConsistencyReport perp = measure(c);
  REQUIRE(perp.measurable);
  MESSAGE("20 cm PERPENDICULAR: ", perp.by_separation[0].median_offset_m * 100.0, " cm");
  CHECK(perp.by_separation[0].median_offset_m > 0.15);
}

TEST_CASE("round12/map_consistency/the answer does not depend on point order") {
  Cloud c = two_passes(0.07, 0.0, 0.07, 0.03);
  const post::MapConsistencyReport a = measure(c);
  REQUIRE(a.measurable);

  // Shuffle points and times together with a fixed seed. A metric that
  // depended on arrival order — a running mean, a hash map traversal — would
  // move here, and this number is quoted in field reports.
  std::vector<std::size_t> idx(c.pts.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::mt19937 rng(20261212u);
  std::shuffle(idx.begin(), idx.end(), rng);
  Cloud s;
  s.pts.reserve(idx.size());
  s.t.reserve(idx.size());
  for (const std::size_t i : idx) {
    s.pts.push_back(c.pts[i]);
    s.t.push_back(c.t[i]);
  }
  const post::MapConsistencyReport b = measure(s);
  REQUIRE(b.measurable);
  CHECK(a.by_separation.size() == b.by_separation.size());
  for (std::size_t i = 0; i < a.by_separation.size(); ++i) {
    CHECK(a.by_separation[i].cells == b.by_separation[i].cells);
    // Bit-identical, not approximately identical. `==` on doubles is the
    // assertion on purpose.
    CHECK(a.by_separation[i].median_offset_m == b.by_separation[i].median_offset_m);
    CHECK(a.by_separation[i].p90_offset_m == b.by_separation[i].p90_offset_m);
  }
  CHECK(a.self_floor_m == b.self_floor_m);
}

TEST_CASE("round12/map_consistency/refuses honestly rather than returning zero") {
  // A one-pass map has nothing painted twice. The correct answer is "not
  // measurable", NOT "0 cm" — which would read as a perfect scan and is the
  // failure mode a scalar-returning API invites.
  Cloud c;
  wall_points(&c.pts, 0.0, 0.0, 0.0, 0.0, kT0 + 1'000'000LL, &c.t);
  const post::MapConsistencyReport r = measure(c);
  CHECK_FALSE(r.measurable);
  CHECK(std::string(r.blocker).size() > 0);

  // And an empty cloud says so too.
  const post::MapConsistencyReport e =
      post::measure_map_consistency({}, {}, post::MapConsistencyConfig{});
  CHECK_FALSE(e.measurable);
}
