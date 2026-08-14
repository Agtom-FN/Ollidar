# A6 — live lidar-inertial odometry (Mid-360)

**Scope:** `engine/include/scanengine/slam/{lio,eskf,ivox}.h`, `engine/src/slam/`,
`engine/tests/test_lio.cpp`.
**Spec:** §3.3 "Mid-360 live". **Contract:** `engine/DESIGN.md` §2, §5, §6.
**Consumes:** A3's Mid-360 point/IMU path (`docs/A3-mid360-driver.md` §4) and
A4's clock mapping (`docs/A4-timesync.md` §7).

A1 shipped `slam/slam.h` as a seam. A6 fills it in with a Point-LIO /
FAST-LIO2-family iterated ESKF: IMU propagation at 200 Hz, scan-to-map at
10 Hz against an incremental voxel map, input decimated to ~40k pts/s, poses
out through `poses/PoseSource`, and the registered world-frame cloud into a
`PageStore` — which is the thing the app draws when it says "the map".

Verified on 2026-08-15 against a **real Mid-360 capture**, not only against
simulation. Every number below was measured; §7 says which host.

---

## 1. What ships

| Piece | File | Lines |
| --- | --- | --- |
| `IVox` — incremental voxel map, knn, trim | `slam/ivox.h`, `src/slam/ivox.cpp` | 109 / 172 |
| `Eskf` — 18-state error-state filter, IMU propagation | `slam/eskf.h`, `src/slam/eskf.cpp` | 150 / 262 |
| `LioOdometry` — scan assembly, undistortion, iterated update, map/pose publication | `slam/lio.h`, `src/slam/lio.cpp` | 324 / 829 |
| `LioPoseSource` — the trajectory, as a `PoseSource` | `slam/lio.h`, `src/slam/lio_pose_source.cpp` | — / 171 |
| SO(3), 3x3 eigen, 18x18 LDL^T | `src/slam/lio_math.{h,cpp}` | 365 / 158 |
| Tests | `tests/test_lio.cpp` | 1,297 |

**No new dependencies.** Not even Eigen — see §3.5. The module builds and all
its tests pass under `-DENGINE_WITH_EIGEN=OFF` and `-DENGINE_WITH_LIVOX_SDK2=OFF`.
No `CMakeLists.txt` change was needed: `src/*.cpp` and `tests/test_*.cpp` are
globbed.

---

## 2. Wiring

```
Mid360Driver ──points (lidar frame, m)──▶ PageStore(kLidarMid360)
             │                            └────────────────────▶ LioOdometry::push_points()
             └──IMU (g, device clock)───▶ ImuIngest (A4: → m/s², engine ns)
                                          └────────────────────▶ LioOdometry::push_imu()

LioOdometry ├─ poses()      ▶ LioPoseSource  (PoseSource: pose_at(), callbacks)
            └─ map_store()  ▶ PageStore      ← the app renders this
```

```cpp
LioConfig cfg;
cfg.map_store = &engine_page_store;      // or leave null: the LIO owns one
cfg.internal_thread = true;              // live capture; false = inline, for tests
LioOdometry lio(cfg);
if (!lio.start().ok()) { /* ... */ }

// on the driver's IMU sink (A4 has already mapped the clock and the units):
lio.push_imu(s.t_engine_ns, s.gyro_rad_s, s.accel_m_s2);

// on the driver's point path, per datagram:
lio.push_points(points, model.apply(header.timestamp));

// anywhere:
Pose p; lio.current_pose(&p);
lio.poses().pose_at(t_ns, &p);           // interpolated; A8/A11 use this
```

The driver does no geometry (A3 §4). A6 applies, in order: the lidar→IMU
extrinsic, the motion undistortion, and the world transform.

### `push_imu` takes primitives, not a struct — on purpose

`slam/slam.h` declares `scanengine::ImuSample` and `timesync/imu_ingest.h`
declares a **different** `scanengine::ImuSample`. Including both in one
translation unit does not compile; having both in one program is an ODR
violation, and `scanengine_tests` has had exactly that since A4 landed
(`test_timesync.cpp` sees one, `test_headers.cpp` the other). A6 cannot fix it
— the fix touches `tests/test_headers.cpp`, which A6 does not own — so A6
refuses to take a side: `push_imu(t_ns, gyro[3], accel[3])` has both `float`
and `double` overloads and `lio.h` includes neither header. §9 has the
one-line request.

---

## 3. The algorithm, and the choices inside it

### 3.1 State

18-dimensional error state, in this order (`eskf.h` is the contract):

```
[ 0: 3) dp    position     (world)      [ 9:12) dbg   gyro bias   (body)
[ 3: 6) dth   orientation  (R·Exp(dth)) [12:15) dba   accel bias  (body)
[ 6: 9) dv    velocity     (world)      [15:18) dg    gravity     (world)
```

**Gravity is a full 3-vector, not FAST-LIO2's 2-DOF S² quantity.** FAST-LIO2
fixes |g| because it is known. We do not, for two reasons: the third degree of
freedom absorbs accelerometer scale error, which on the Mid-360's MEMS IMU is
real and otherwise leaks straight into vertical velocity; and an unconstrained
R^18 error state makes the whole update one dense symmetric solve with no chart
bookkeeping. The price is that |g| is *observed* rather than *imposed* — which
makes it a free health check, and `tests/test_lio.cpp` asserts on it in both
the synthetic and the real case.

**The world frame is z-up by construction.** `init_from_static()` builds the
initial attitude as the rotation taking the measured specific force onto +Z, so
the session's local metric frame is gravity-aligned from the first pose and
§3.6's floor-plan slice band means what it says. Yaw is unobservable from an
IMU and is left at identity. Measured on a scanner tilted 20°/10° at rest, the
recovered up-axis is exact to 1e-9 (`eskf/static_init_aligns_gravity_to_z`).

### 3.2 Propagation

Standard right-perturbation ESKF at every IMU sample:

```
F[dp , dv ] = I·dt        F[dth, dth] = Exp(-w·dt)     F[dv , dth] = -R·[a]x·dt
F[dv , dba] = -R·dt       F[dth, dbg] = -Jr(w·dt)·dt   F[dv , dg ] = I·dt
P <- F P F^T + Q
```

Gaps longer than `max_step_s` (20 ms) are split into sub-steps so the
first-order covariance propagation stays valid; gaps longer than `max_gap_s`
(0.5 s) return `kTimeout` and **re-anchor the clock without integrating a
fiction** (`eskf/long_gap_is_reported_not_integrated`). `P` is symmetrized and
`R` re-orthonormalized every step — both cost nothing and both are expensive to
debug later.

### 3.3 Undistortion

Each scan keeps a pose snapshot at every IMU step inside it. Every point is
mapped into the **scan-end body frame** using the snapshot pair bracketing its
own timestamp (SO(3) interpolation, linear in position), then the update works
in one rigid frame.

Per-point times come from `push_points`: the batch's stamp and the *previous*
batch's stamp bracket the batch, and times are interpolated linearly across it.
For a 96-point Mid-360 datagram at 2,083 packets/s that is ~5 µs of granularity
— per-point resolution in everything but name. This is why `PointVertex` never
had to grow a timestamp field and the S3-proven 16-byte GPU layout is untouched.

Not optional: a scanner turning at 30 °/s smears a 100 ms scan by 3°, which at
10 m is half a metre.

### 3.4 The iterated update

Per iteration, for each undistorted point `pb`:

1. `w = R·pb + p`, then `knn(w, 5, max_correspondence_m)` in the voxel map.
2. Fit a plane to the 5 neighbours: centroid, covariance, smallest
   eigenvector. Reject if `λ0 > 0.1·λ1` (collinear, not planar) or if any
   neighbour is more than `plane_thickness_m` off the plane.
3. `r = n·w + d`; `H = [ n^T | -((R^T n) x pb)^T ]`, zero in the other 12 columns.

**On that Jacobian.** `∂r/∂θ` is `-n^T R [pb]x`, which is `-((R^T n) × pb)^T`
and *not* `-(n × R·pb)^T`. The two differ by a rotation and the wrong one still
converges on a slow synthetic trajectory — it only shows up as drift on real
data. It is called out in the source because it is exactly the kind of error a
green test suite hides.

Because only 6 of 18 columns are non-zero, the update accumulates a 6x6
information block directly (36 multiply-adds per residual instead of 324) and
then solves the full MAP problem:

```
A     = H^T R^-1 H + P^-1            (18x18, symmetric positive definite)
rhs   = (H^T R^-1 H)·delta_j - H^T R^-1 r
delta = A^-1 · rhs                   (LDL^T)
x     = x_prior [+] delta            (re-applied from the prior each iteration)
P     = A^-1                         (on the final iteration)
```

Iterate until the increment is below `converge_rot_rad` / `converge_trans_m`
(1e-4 rad / 0.1 mm) or `max_iterations` (4). Correspondences are re-searched
every iteration. The `J` Jacobian of the ⊟ operator is approximated by `I`,
which is the usual simplification and is valid while the increments stay small
— they do: 1.97 iterations per scan on the synthetic room.

A scan with fewer than `min_correspondences` (30) usable residuals is
**propagated but not updated**, published as `PoseQuality::kFair`, and counted
in `scans_skipped`. An honest dead-reckoned pose beats a fit to noise.

### 3.5 The map: a hash grid, not a kd-tree

FAST-LIO2 ships an incremental kd-tree; Faster-LIO replaced it with exactly
this structure and measured a 1.5–2x end-to-end speedup, because a live map's
cost is not the search — it is the rebalancing on every scan. A hash grid has
none. Three properties the odometry depends on:

* **Determinism.** `knn()` walks a fixed 3x3x3 neighbour order and, inside a
  voxel, insertion order; distance ties keep the earlier candidate. No
  `nth_element` (not stable), no iteration over hash-bucket order.
  `ivox/knn_is_deterministic` asserts bit-identical results across two
  independently built maps.
* **Bounded memory.** A voxel stops accepting at `max_points_per_voxel` (20);
  the map stops creating voxels at `max_voxels` (1 M). So the map is a
  voxel-downsampled cloud whose size follows the *volume scanned*, not the
  session length — which is what makes "insert every scan forever" safe.
* **No FIFO eviction.** A full voxel drops new points rather than replacing old
  ones, because `insert()` reports what was stored and the LIO publishes
  **exactly the stored points** to the PageStore. The rendered cloud and the
  map are the same set, and they stay that way — the PageStore only appends.

`plane fit`: covariance eigen-decomposition about the centroid, not FAST-LIO2's
`n·p + 1 = 0` least squares. The latter is cheaper but singular for any plane
through the world origin — and the world origin is the scanner's own starting
pose, a place a hand-carried scanner walks back through.

### 3.6 Why no Eigen

Eigen is linked and available; A7 will want it. A6 needs exactly four things:
3-vectors, 3x3 rotations, a 3x3 symmetric eigen-decomposition, and an 18x18
SPD solve. ~520 lines of `lio_math.{h,cpp}` buys three properties worth more
than the convenience:

1. The module builds and runs with `-DENGINE_WITH_EIGEN=OFF`. No `#ifdef`
   forest, no second code path nobody tests.
2. **Determinism is under our control.** Eigen's expression templates choose
   different vectorized reduction orders per `-march` and per platform, so
   "identical input ⇒ identical output" would silently become an
   identical-toolchain promise across the five CI legs. Every loop here reduces
   in a fixed index order.
3. The only non-trivial routine is an unpivoted LDL^T of a well-conditioned SPD
   information matrix — the right algorithm, 40 lines, and pivoting would cost
   determinism for nothing. Verified against closed forms:
   `|A·A⁻¹ − I| ≤ 2.8e-15`, worst solve component error 5.3e-15.

### 3.7 Decimation

`live_points_per_sec` (40,000, §3.3) sets a per-scan stride computed from the
**previous** scan's arrival count — deterministic, unlike a wall-clock rate
estimate, and a no-op when the driver has already decimated to the same budget.

The stride **rounds**, it does not truncate. Truncation always errs on the
"keep more" side and the error reaches 2x: on the real capture,
`basis/target = 2.9` truncates to 2 and runs the update at 58k pts/s — 45% over
the budget the whole setting exists to enforce. That was a live bug, found by
comparing measured against configured rate.

---

## 4. Determinism

**Identical input ⇒ bit-identical trajectory and map on a given build.**
`lio/is_deterministic` asserts exact equality of the final pose (all 7
components), the map point and voxel counts, and the published point count.

"Identical input" includes **batch boundaries**: `push_points()` stamps a batch
and per-point times are interpolated across it, so splitting one 8192-point
call into two 4096-point calls is a different input. A3 batches
deterministically, so a replay of the same bytes reproduces the same batches.

`lio/internal_thread_matches_inline` runs the same input with and without the
odometry thread and asserts the final positions are **exactly** equal. It found
two genuine scheduling dependencies, both now fixed:

1. **The static-init window.** `try_init()` averaged "however many IMU samples
   happened to be buffered". With a worker thread that is 100 samples or 400
   depending on when it last woke, and a different average is a different
   initial attitude — i.e. the whole session depended on scheduling. It now
   averages exactly the first `init_imu_samples`, and hands the remainder to
   the filter instead of dropping them.
2. **Points straddling initialization.** Points arriving in the same drain as
   the init sample survived; points drained earlier did not. There is now an
   explicit floor: points older than the filter's birth are dropped and counted
   (`LioStats::points_late`), because they predate every pose snapshot and
   cannot be undistorted anyway.

A scan is closed only when **both** streams have passed its end (IMU so the
propagation is complete, points so none of that scan is still in flight), which
is what makes the scan contents independent of when the worker runs. A third
condition — IMU more than 3 scan periods past the boundary — keeps dead
reckoning alive when the lidar goes silent instead of stalling forever
(`lio/survives_a_lidar_dropout`).

---

## 5. Threads

`DESIGN.md` §2's table should gain one row (that file is not A6's to edit):

| Thread | Owner | Runs |
| --- | --- | --- |
| LIO odometry | `LioOdometry` (when `internal_thread`) | drains the IMU/point queues, propagates, undistorts, runs the iterated update, inserts into the voxel map, appends to the `PageStore`, publishes the pose. One per instance. |

Lock order is `proc_m` → `in_m`; nothing takes them the other way.
`LioPoseSource` callbacks fire **outside** its lock, so a subscriber cannot
deadlock the odometry by calling `pose_at()` from one — the engine-wide "quick,
no re-entry" rule (DESIGN §2) still applies. With `internal_thread = false`
(the default, and what every test uses) the pipeline runs inline on the
caller's thread, which is the engine's default posture.

---

## 6. Configuration

Every field has its reasoning in `lio.h`. The ones worth knowing:

| field | default | note |
| --- | --- | --- |
| `scan_period_s` | 0.1 | §3.3's 10 Hz |
| `live_points_per_sec` | 40000 | §3.3's live budget; 0 disables |
| `max_points_per_scan` | 12000 | burst ceiling, 3x nominal |
| `min/max_range_m` | 0.5 / 80 | the odometry's own reach, after A3's filter |
| `lidar_to_imu_{t,q}` | identity | the Mid-360's datasheet value is (0.011, 0.02329, −0.04412) m, no rotation; a real mount adds its own and A11's wizard is where a measured one comes from |
| `map.voxel_size_m` | 0.5 | FAST-LIO2/Faster-LIO's value |
| `plane_points` / `plane_thickness_m` | 5 / 0.1 | FAST-LIO2's values |
| `max_correspondence_m` | 1.0 | see the sweep in §7.3 — tightening it makes things **worse** |
| `max_iterations` | 4 | 1.97 used on synthetic, 3.93 on real |
| `point_sigma_m` | 0.02 | how hard the lidar pulls against the IMU prior |
| `min_correspondences` | 30 | below this: propagate, publish `kFair`, do not update |
| `map_radius_m` | 0 (off) | live trim radius; A7 re-runs at full density from the .lscan, so forgetting is safe |
| `internal_thread` | false | true for live capture |

---

## 7. Verification (2026-08-15)

Host: Apple M4, macOS 26.5.1 (Darwin 25.5.0). Apple clang, CMake 4.4.2, Ninja,
`RelWithDebInfo`, clean build directory, **universal binary (arm64 + x86_64)**,
`lipo` confirmed, running the arm64 slice.

```
$ ctest -LE sim
1/4 scanengine_tests ......... Passed  7.36 sec
2/4 scanengine_capi_smoke .... Passed
3/4 engine_cli_selftest ...... Passed
4/4 engine_cli_version ....... Passed
100% tests passed out of 4

$ ./scanengine_tests
[doctest] test cases:    272 |    272 passed | 0 failed | 5 skipped
[doctest] assertions: 775418 | 775418 passed | 0 failed |
```

A6 adds **24 cases** (6 math, 4 iVox, 2 pose source, 5 ESKF, 7 pipeline
including the real capture). Every pre-existing case
still passes unchanged. Also clean under `-DENGINE_WARNINGS_AS_ERRORS=ON`,
`-DENGINE_WITH_EIGEN=OFF` and `-DENGINE_WITH_LIVOX_SDK2=OFF`. Whole suite
runs in 7.36 s, and the seven `lio/*` pipeline cases (which include parsing and
replaying the whole 17.5 MB real capture) account for ~6 s of that — well
inside the 30 s ceiling, so nothing needed a "slow" ctest label and no
`CMakeLists.txt` change was required to register one.

### 7.1 Synthetic room — the only place accuracy can be checked against truth

A 10 x 8 x 3 m room with two pillars and a bench; an analytic figure-eight
trajectory with a ±0.5 rad yaw sweep, stationary for the first 0.6 s; IMU
synthesised from the trajectory by central differences and corrupted with bias
(4 mrad/s, 0.05 m/s²) and noise (2 mrad/s, 0.02 m/s²); 96-point datagrams every
480 µs (200,000 pts/s, exactly A3's measured rate) ray-cast through the room
with a Mid-360 FOV (−7°..+52°) and 1 cm of range noise. 8 seconds.

| metric | measured |
| --- | --- |
| **ATE (RMS)** | **5.21 mm** |
| ATE (max) | 10.1 mm |
| final drift | 10.0 mm |
| rotation error (RMS) | **0.282°** |
| **estimated \|g\|** | **9.807 m/s²** (truth 9.80665) |
| **gravity tilt from −Z** | **0.150°** |
| point-to-plane residual (RMS) | 1.26 cm (range noise is 1 cm) |
| scans / skipped | 76 / 1 (the first, empty map) |
| decimated input rate | 37,524 pts/s (configured 40,000) |
| correspondences per scan | 3,589 |
| iterations per scan | 1.97 of 4 |
| map | 1,340 voxels / 25,767 points |

**The control.** `lio/the_lidar_is_what_removes_the_drift` re-runs byte-identical
input through the same filter with `min_correspondences` set past anything a
scan can supply, so the update never fires and the pipeline becomes pure IMU
dead reckoning:

| | ATE (RMS) | final drift |
| --- | --- | --- |
| dead reckoning only | 0.649 m | 1.418 m |
| **full LIO** | **0.0052 m** | **0.0100 m** |

A 125x improvement, from the same IMU and the same code path. Without this
control an accuracy number proves nothing about the lidar.

### 7.2 Real Mid-360 capture — end to end

`spikes/s2-mid360-sim/fixtures/outdoor_imu_ccby_6s.livoxdump`, 17.5 MB,
6.0 s, parsed with the `.livoxdump` container documented in
`tools/remote-capture/capture_mid360.py` and decoded through the **engine's
own** `mid360::parse_packet` / `point_passes` / `LossTracker` — no
reimplementation, no SDK.

**Wire level** (re-measuring A3/S2's numbers as a side effect):

| metric | measured |
| --- | --- |
| datagrams | 13,700 (12,500 point + 1,200 IMU) |
| point packets accepted | 12,350 (+150 rejected, see below) |
| malformed packets | 0 |
| packets lost (`udp_cnt`) | 2 = 0.016% · 0 duplicates · 1 counter reset |
| filter kept | 685,847 / 1,185,600 = **57.85%** |
| — no-returns dropped | 496,856 = **41.9%** (S2 measured 67.8% on a different outdoor slice, 34.7% indoor) |
| — spatial-noise tags dropped | 2,897 = 0.24% |
| IMU rate | 1,200 samples over 5.994 s = **200.2 Hz** |

**The first 150 point datagrams carry `timestamp == 0`** while arrival advances
72 ms — the device clock had not started. They are fed to the `LossTracker`
(loss accounting is per datagram and must see all of them; skipping them would
be reported as 150 lost packets) but withheld from the offset estimator, which
is exactly what a driver's "device stamp went backwards" rule does.

**Odometry:**

| metric | measured |
| --- | --- |
| scans / skipped | 55 / 1 |
| points decoded → kept → mapped | 685,847 → 208,688 → 183,708 |
| decimated rate | 37,943 pts/s (configured 40,000) |
| correspondences per scan | 1,625 |
| iterations per scan | 3.93 of 4 |
| point-to-plane residual (RMS) | 10.7 cm — see §7.3 |
| **estimated \|g\|** | **9.816 m/s²** |
| path length / net displacement | 32.39 m / 32.27 m |
| final speed | 6.02 m/s (≈ 21.7 km/h) |
| **map** | **39,672 voxels / 183,708 points, 1 PageStore page** |
| NaNs / divergence | none; every pose and every published point finite |
| PageStore append failures | 0 |

There is no ground truth, so the assertions are the ones that can be made
without it: no NaN anywhere in the trajectory or the published cloud, poses
monotone in time, no divergence flag, |g| inside [9.4, 10.2], speed under
20 m/s, and path length under 60 m. The 32 m over 5.5 s at 6 m/s is consistent
with a vehicle-mounted capture, and §7.3's sweep is the strongest available
evidence that it is a measurement rather than drift.

**Honest limitation on the clock path.** The A4 pipeline is exercised
structurally — `TimeSync::add_pair`, `TimeModel::apply` per datagram,
`ImuIngest::add_g` for the IMU — but this fixture's arrival stamps are
`device_timestamp + 1.7e18` **exactly**, for all 12,339 packets. They were
synthesised by a paced replay, not captured off a NIC. So the reported
`jitter_ns = 0` / `SyncQuality::kGood` are true of the file and say nothing
about a real network. A real-hardware bench session is still owed that number
(A3 §8 item 3).

**One deliberate departure from A4's defaults:** the point stream and the IMU
are fed into the *same* (`kLidarMid360`) estimator rather than one each. The
Mid-360 stamps both from one device clock (A3 §4), so a second estimator would
inject a few milliseconds of independent estimation noise **between** the two
streams — precisely the quantity undistortion is sensitive to. Recommended for
A3's driver too; see §9.

### 7.3 The correspondence gate, and why 1.0 m

Sweeping `max_correspondence_m` on the real capture:

| gate | correspondences/scan | residual RMS | final speed | map voxels | per-scan |
| --- | --- | --- | --- | --- | --- |
| 0.3 m | 400 | **3.2 cm** | 12.60 m/s | 65,636 | 9.3 ms |
| 0.5 m | 1,308 | 8.2 cm | 6.03 m/s | 43,072 | 13.3 ms |
| **1.0 m** | **1,625** | 10.7 cm | **6.02 m/s** | **39,672** | 14.7 ms |

**The smallest residual is the worst setting.** At 0.3 m the update keeps only
the 400 matches that already agree with the prior, stops constraining the
filter, and the final speed doubles — while the map inflates by 65% because a
drifting trajectory smears the same surfaces across more voxels. Residual RMS
measures how well the accepted matches fit, not how well the pose is
determined; the voxel count and the speed agreement between 0.5 and 1.0 m are
the honest indicators. 10.7 cm is what "planes" mean outdoors at a 15 m mean
range: road, kerb, foliage, building faces.

---

## 8. Performance

Measured, arm64, `RelWithDebInfo`, one thread, per 100 ms scan:

| workload | mean | p50 | p95 | max | budget used (of 2 cores) |
| --- | --- | --- | --- | --- | --- |
| synthetic room, 40k pts/s, 1.97 iter | 12.70 ms | 13.09 | 13.80 | 16.24 | 0.060 |
| **real capture, 40k pts/s, 3.93 iter** | **14.74 ms** | 16.00 | 17.91 | 18.93 | **0.068** |

Scaling with the decimation budget, real capture:

| budget | points/scan | correspondences/scan | mean per-scan | path length |
| --- | --- | --- | --- | --- |
| 20k pts/s | ~1,900 | 740 | 6.45 ms | 32.52 m |
| **40k pts/s** | ~3,800 | 1,625 | **14.66 ms** | **32.39 m** |
| 80k pts/s (capped at 12k/scan) | ~11,400 | 5,046 | 55.30 ms | 32.32 m |
| no decimation | ~11,400 | 5,046 | 54.82 ms | 32.32 m |

Cost is very nearly linear in points × iterations (the knn dominates; the
18x18 solve is ~6k flops and runs 4 times a scan). **The trajectory changes by
0.6% across a 6x range of point count** — which says both that the 40k budget
is not what limits accuracy, and that the 32 m result is not a decimation
artefact.

### Extrapolation to mobile — stated as an extrapolation

The M4 P-core runs at ~4.4 GHz. This workload is knn over a hash grid: pointer
chasing and cache misses, not vector arithmetic, so it tracks memory latency
and issue width more than clock. A current Android big core (Cortex-X4 class,
~3.3 GHz, smaller L2, slower DRAM) should land at **2.5–3.5x** the wall time on
this shape of work. That gives:

* **37–52 ms per 100 ms scan on one big core** — i.e. **0.4–0.5 of one core**,
  comfortably inside §3.3's "≤ 2 big cores", with 4x headroom for thermal
  throttling before the odometry stops keeping up with real time.
* Headroom is real but not free: at 80k pts/s the same extrapolation gives
  138–194 ms per scan, which does *not* fit. 40k pts/s is the right budget and
  `max_points_per_scan` (12,000) is the guardrail that keeps a burst from
  turning into a missed scan.

**This is a projection, not a measurement.** The Android leg of
`engine-ci.yml` is build-only (no device), so nothing in this repo has run the
LIO on a phone. The first device session should re-run §7.2 verbatim and
compare against the 14.74 ms / 0.068 line — and should watch the p95 rather
than the mean, because a missed scan is what the user sees.

Memory: the voxel map is bounded by `max_voxels` x `max_points_per_voxel`
(1 M x 20 worst case, ~250 MB, never reached in practice — 39,672 voxels /
184k points = 2.2 MB for the 6 s outdoor capture). The pose ring is 36,000
poses = 2 MB, an hour at 10 Hz. Everything else is per-scan scratch that is
reused, not reallocated.

---

## 9. Seams A6 did NOT take (owner action required)

Each is a small change in a file A6 does not own.

1. **`StreamId` has no value for a SLAM map.** `core/types.h` (A1) should gain
   `kSlamMap = 8` and, arguably, `kPoseLio = 9`. Until then `LioConfig::map_stream`
   and `LioPoseSource`'s stream default to `kLidarMid360` — which is at least
   *true* (that is the stream the map and the poses were derived from) and is
   configurable, but it means a `.lscan` cannot distinguish the raw cloud from
   the registered one. The enum is append-only, so this is a two-line change
   plus the `to_string()` and C-ABI mirrors.
2. **`Engine` should own a `LioOdometry`** and pass its own `PageStore` as
   `LioConfig::map_store`, so the map raises `kPointsAvailable` through the
   Engine's existing page subscriber and the app renders it with no new code.
   Today a caller must construct the LIO itself; the seam is one config field.
3. **`slam/slam.h`'s `ImuSample` should be deleted** in favour of
   `timesync/imu_ingest.h`'s, and the one line in `tests/test_headers.cpp` that
   reads `ImuSample imu; CHECK(imu.t_mono_ns == 0);` updated. The two
   definitions are a live ODR violation in `scanengine_tests` today (A4 vs A1);
   A6 documented it in `slam.h` and routed around it rather than adding a third
   party to the clash.
4. **A3's driver should feed IMU and points into one estimator.** See §7.2. One
   argument to the `ImuIngest` constructor: `ImuIngest imu(ts, StreamId::kLidarMid360)`.
5. **A5's recorder** should record the LIO trajectory as a `.lscan` pose stream
   so a replay reproduces it without re-running the odometry, and A7 has a
   starting trajectory for its pose graph.
6. **A7** should reuse `IVox` and `Eskf` directly for the full-density re-run —
   they are public headers with no live-only assumptions. The only settings to
   change are `live_points_per_sec = 0`, `map_radius_m = 0`, and a larger
   `max_points_per_voxel`.

---

## 10. What is still hardware-only

1. **Real IMU error characteristics.** Bias instability, scale factor, axis
   misalignment and the true lidar↔IMU extrinsic (A3 §8 item 4). The fixture
   has real IMU data but no truth to compare a bias estimate against, and the
   extrinsic is left at identity because nobody has measured one.
2. **Real clock jitter.** §7.2: this fixture's arrival stamps are synthetic, so
   the entire A4 error budget is untested against the LIO. A capture with 15 ms
   of real jitter would show up as undistortion error, and that is the number
   A11's colorization budget cares about.
3. **Ground truth for a real trajectory.** Everything in §7.2 is
   self-consistency. A capture with survey control or RTK would turn "32.39 m,
   plausible" into an ATE.
4. **Phone CPU and thermals.** §8 is arithmetic, not a measurement.
5. **Loop closure and long-session drift.** Out of scope by design — that is
   A7's pose graph. The live map has a `map_radius_m` trim precisely because
   the live path is not expected to stay globally consistent for 30 minutes.
6. **Degenerate geometry.** A long featureless corridor or an open field
   under-constrains the update along one axis. `min_correspondences` catches
   *no* geometry; it does not catch *rank-deficient* geometry. The information
   matrix `A` is available and its smallest eigenvalue would be an honest
   degeneracy detector — worth adding when there is a capture that exhibits it.
