#pragma once

// ROUND 9, owner item 34 — the COIN-D6 fan frame, defined once.
//
// > "The output is left right reversed."
//
// This header exists because that bug was possible. Before ROUND 9 the polar →
// Cartesian conversion was written out twice — once in `D6Driver::on_point()`
// for the live preview, once in `D6PushbroomAssembler::resolve_()` for the
// assembled cloud — and *neither* said which physical end of the sensor its
// `+z` pointed out of. A frame convention that is never written down cannot be
// checked, and this one was wrong.
//
// ---------------------------------------------------------------------------
// 1. The frame
// ---------------------------------------------------------------------------
//
// The D6 fan frame is RIGHT-HANDED and pinned to the physical unit as:
//
//     +y  the 0-degree beam direction (the zero mark on the housing)
//     +z  the spin axis, pointing out of the BASE of the unit
//         (i.e. AWAY from the cap / optical-window end)
//     +x  = y cross z
//
// and a return at vendor angle `theta`, range `d`, lands at
//
//     p_lidar = ( -d*sin(theta),  d*cos(theta),  0 )                      (1)
//
// `theta` is the angle the vendor protocol reports, in degrees, increasing in
// the direction the mirror actually turns.
//
// ---------------------------------------------------------------------------
// 2. Why `+z` is the BASE, and why the sign in (1) is negative
// ---------------------------------------------------------------------------
//
// The datasheet's own words, as transcribed in `docs/bench/BENCH_SETUP.md` §3.1
// from the vendor manual's "Scan data coordinate system definition":
//
//     "left-hand coordinate system ... rotation angle increases clockwise ...
//      zero-degree direction marked in the figure"
//
// Two things are being said there, and only one of them was ever acted on.
//
//   (a) The angle increases CLOCKWISE **as the figure is drawn**, and that
//       figure is a top view — looking down the spin axis from the cap end.
//       So: stand at the cap, look back down the axis at the unit, and the
//       beam sweeps clockwise away from the zero mark.
//
//   (b) The coordinate system the datasheet quotes its `(x, y)` in is
//       LEFT-handed. A left-handed triple transcribed verbatim into a
//       right-handed frame keeps `x` and `y` and silently REVERSES the sense of
//       rotation about the third axis. That is the entire bug: nothing in the
//       pipeline ever compensated for (b), so the fan came out reflected.
//
// Deriving (1) from (a). Put the frame's `+y` on the zero mark and `+z` out of
// the base, so an observer at the CAP is looking along `+z` toward the origin —
// which is the datasheet figure's viewpoint. In a right-handed frame viewed
// from the `-z` side, `+x` appears to the LEFT and `+y` up, so "clockwise from
// +y" runs from `+y` toward `-x`. Hence `x = -d*sin(theta)`, `y = d*cos(theta)`.
//
// The pre-ROUND-9 code used `x = +d*sin(theta)` with the same `+z`. That is
// clockwise seen from the BASE — the figure read as if it were a bottom view.
// It is a pure reflection of (1) across the y-z plane.
//
// ---------------------------------------------------------------------------
// 3. Why nothing caught it
// ---------------------------------------------------------------------------
//
// Every return in the fan has `z == 0` exactly. Restricted to that plane, the
// reflection `x -> -x` is ALSO what the proper rotation `diag(-1, +1, -1)`
// (180 degrees about the fan's own 0-degree axis) does. So a reflected fan is
// indistinguishable from a mount extrinsic yawed by 180 degrees, and:
//
//   * `se3::mat4_is_rigid()` never fires — the stored extrinsic really is a
//     proper rotation, `det = +1`. The owner's `scan-017` manifest is exactly
//     that, and it is not the thing that was wrong.
//   * every existing geometry test stayed green, because all of them measure
//     SIGN-BLIND quantities: axis extents, best-fit-plane RMS, point counts.
//     A mirrored room has identical extents and identical planarity.
//
// `tests/test_round9_chirality.cpp` is the first test in the suite that can
// tell a room from its mirror image. See §5 for the shape of it.
//
// ---------------------------------------------------------------------------
// 4. What this means for the mount extrinsic
// ---------------------------------------------------------------------------
//
// With `+z` out of the BASE, the owner's stated rig — D6 on the back of the
// phone, 0-degree beam UP, cap/top FORWARD along the walk — makes the fan frame
// and the ARCore camera frame agree axis for axis:
//
//     lidar +y (0 deg)   = camera +Y  (up)
//     lidar +z (base)    = camera +Z  (backward; ARCore looks along -Z)
//     lidar +x           = camera +X  (operator's right)
//
// i.e. `phone_from_lidar` has an IDENTITY rotation, which is precisely what
// `BracketNominals.cadNominal(COIN_D6)` has always carried. The CAD nominal was
// never the wrong part; formula (1) was.
//
// ---------------------------------------------------------------------------
// 5. Reading a pre-0.6.0 recording
// ---------------------------------------------------------------------------
//
// A `.lscan` captured before this fix stores raw D6 bytes plus a
// `phone_from_lidar` that was solved/trimmed against the OLD formula. Because
//
//     old_fan(theta) == diag(-1, +1, -1) * new_fan(theta)
//
// exactly, re-resolving such a container with today's code and the STORED
// extrinsic is what un-mirrors it — the fix lands on every archived capture for
// free, with no manifest migration. `d6_legacy_fan_extrinsic()` goes the other
// way, reproducing the old pipeline's output bit for bit from the new one; it
// exists so `test_round9_chirality.cpp` can run the pre-fix convention through
// the production code path and watch it fail.

#include <cmath>

// Global `d6`, matching `d6_parser.h` / `commands.h` — the S1 spike's namespace,
// which is deliberately not nested inside `scanengine`.
namespace d6 {

// Formula (1). `angle_deg` is the vendor angle; `range_m` the range. `out` is
// the return in the fan frame, metres. Always exactly `out[2] == 0`.
inline void fan_point(double angle_deg, double range_m, double out[3]) {
  constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
  const double a = angle_deg * kDegToRad;
  out[0] = -range_m * std::sin(a);
  out[1] = range_m * std::cos(a);
  out[2] = 0.0;
}

// The inverse of `fan_point`, for the coarse `Span<const PointVertex>` seam
// that hands the assembler Cartesian points instead of polar ones. Returns the
// angle in [0, 360).
inline double fan_angle_deg(double x, double y) {
  constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
  double a = std::atan2(-x, y) * kRadToDeg;
  if (a < 0.0) a += 360.0;
  return a;
}

// Post-multiply a `phone_from_lidar` by `diag(-1, +1, -1)` (row-major 4x4, in
// place safe). Applying this to an extrinsic and then resolving with
// `fan_point()` reproduces the pre-ROUND-9 pipeline exactly — see §5. It is
// its own inverse.
//
// This is a TEST AND MIGRATION helper. Nothing in the live path calls it: a
// legacy container is un-mirrored by re-resolving it, not by rewriting its
// manifest.
inline void d6_legacy_fan_extrinsic(const double in[16], double out[16]) {
  for (int r = 0; r < 4; ++r) {
    out[r * 4 + 0] = -in[r * 4 + 0];
    out[r * 4 + 1] = in[r * 4 + 1];
    out[r * 4 + 2] = -in[r * 4 + 2];
    out[r * 4 + 3] = in[r * 4 + 3];
  }
}

}  // namespace d6
