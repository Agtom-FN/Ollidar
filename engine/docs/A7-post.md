# A7 — Mid-360 post-processing pipeline ("Finish scan")

**Scope:** `engine/include/scanengine/slam/post/**`, `engine/src/slam/post/**`,
`engine/tests/test_post.cpp`.
**Spec:** §3.3 "Mid-360 post" — *full-density LIO re-run → Scan Context loop
candidates → GTSAM pose-graph optimization → re-integration → voxel
dedup/outlier filter. Foreground service (Android) / background task
(desktop), cancellable, progress-reported.*
**Contract:** `engine/DESIGN.md` §2, §5, §6.
**Consumes:** A5's `.lscan` reader (`docs/A5-lscan.md`), A6's `LioOdometry` /
`IVox` (`docs/A6-lio.md`), A3's Mid-360 wire layer, A4's clock mapping, A8's
`poses/se3.h`.

One line of the spec is not implemented as written: **GTSAM is not used.**
§4 is why, and where it becomes right.

---

## 1. What ships

| Piece | Header | Impl | Lines (h / cpp) |
| --- | --- | --- | --- |
| Cancel token, stage enum, progress record | `slam/post/progress.h` | `src/slam/post/progress.cpp` | 114 / 26 |
| `PoseGraph` — SE(3) Gauss-Newton/LM, RCM + skyline LDL^T | `slam/post/pose_graph.h` | `pose_graph.cpp` | 273 / 846 |
| `ScanContextDb` — the descriptor and its search | `slam/post/scan_context.h` | `scan_context.cpp` | 158 / 217 |
| `icp_point_to_plane` + the loop acceptance gate | `slam/post/loop_closure.h` | `loop_closure.cpp` | 114 / 312 |
| Voxel dedup, statistical outlier filter, streaming accumulator | `slam/post/cloud_filter.h` | `cloud_filter.cpp` | 123 / 260 |
| `PostSlamPipeline` — the whole thing | `slam/post/post_pipeline.h` | `post_pipeline.cpp` | 296 / 928 |
| Unbounded insertion-ordered hash grid | *(internal)* `src/slam/post/point_grid.h` | — | 156 |
| Tests | — | `tests/test_post.cpp` | 1,635 |

**No new dependencies.** Not GTSAM, not Ceres, not Eigen — `vcpkg.json` is
unchanged, and so is `CMakeLists.txt` (`src/*.cpp` and `tests/test_*.cpp` are
globbed with `CONFIGURE_DEPENDS`). The module builds and all its tests pass
under `-DENGINE_WITH_EIGEN=OFF` and `-DENGINE_WITH_LIVOX_SDK2=OFF`, and clean
under `-DENGINE_WARNINGS_AS_ERRORS=ON`.

`PostSlamPipeline` implements the `scanengine::PostPipeline` seam A1 declared
in `slam/slam.h` (`run` / `progress` / `cancel`), so nothing in that file
needed to change either.

---

## 2. The pipeline

```
.lscan ──FileRecordReader──┬─ kMid360Points ─▶ parse_packet ─▶ point_passes ─┐
                           └─ kMid360Imu ────▶ ImuIngest (A4) ───────────────┤
                                                                             │
  PASS 1  kOdometry     LioOdometry (full density) ◀──────────────────────────┘
                          ├─ poses  ─▶ odometry track
                          └─ keyframes: pose + body-frame cloud every 0.5 m / 10 deg / 2 s
                                                │
          kLoopDetection  ScanContextDb ─▶ candidate ─▶ local submap ─▶ point-to-plane ICP
                                                │            └─ accept/reject gate
          kOptimization   PoseGraph: (N-1) odometry edges + accepted loop edges, node 0 fixed
                                                │
                          corrected trajectory = per-keyframe correction, blended
                                                │
  PASS 2  kReintegration  every point ─▶ corrected pose_at(t) ─▶ world ─▶ VoxelAccumulator
          kFiltering      statistical outlier filter
          kPublishing     PageStore(kSlamMap)
```

### It runs from the recording, and it reads it twice

Input is a `.lscan` directory, not a live session. That is what record-always
is for (§3 key rule 2): the live pass decimated to 40k pts/s and let the map
forget outside `map_radius_m`, and neither loss matters because the raw
datagrams are on disk. The post run is not a refinement of the live result —
it is a second, better run from the same bytes, and it is the same code the
cloud worker executes (§3.8).

**Two passes, not one.** The corrections are not known until the graph has
solved, so re-integration cannot ride along with the odometry. Buffering every
raw point instead costs ~5.7 GB on a 30-minute session. Re-reading a
sequential file costs a second decode; A5's reader is built for exactly this
("a `.lscan` is read start-to-finish far more often than seeked into",
`docs/A5-lscan.md` §3).

### It decodes Mid-360 chunks itself rather than using `ReplaySource`

`lscan::ReplaySource` pushes bytes into a live `Engine` through
`push_serial_bytes()`, and **no such entry point exists for Mid-360** —
`docs/A5-lscan.md` §4 says so explicitly and A3 has not added one. Rather than
invent an `Engine` API from inside `slam/`, `PostSlamPipeline` reads chunks and
drives `mid360::parse_packet` / `mid360::point_passes` / `LioOdometry`
directly. That is the engine's own production decode path — the same one
`tests/test_lio.cpp` runs against the real capture — not a reimplementation.
§8 has the one-paragraph change that would let `ReplaySource` take over.

### Full density: what it buys and what it does not

`PostConfig`'s constructor sets `live_points_per_sec = 0`,
`max_points_per_scan = 0`, `map_radius_m = 0`, and raises the iVox caps —
exactly the four settings `docs/A6-lio.md` §9 item 6 asks A7 to change.

But be clear about what that is for. A6 §8 measured undecimated odometry at
54.8 ms/scan against 14.7 ms at 40k pts/s, for **a trajectory that differs by
0.6%**. Full density does not buy trajectory accuracy — the loop closure does.
Full density buys the *density of the final cloud*, which is the deliverable,
and the re-integration pass uses every point regardless of what the odometry
was decimated to. A caller optimizing for time can put the live budget back on
the odometry and lose almost nothing; the real-capture smoke test in §6.3 does
exactly that and says so.

---

## 3. Keyframing

A keyframe is emitted when the odometry pose has moved `keyframe_translation_m`
(0.5), turned `keyframe_rotation_deg` (10), **or** `keyframe_max_interval_s`
(2.0) have passed. The third one is not a nicety: a stationary scanner emits no
keyframes, and without it the pending-point buffer is unbounded. There is a
hard `keyframe_buffer_cap` behind it as well, and `PostStats::buffer_overflow_points`
reports if it ever fires.

The keyframe's cloud is everything swept up since the previous keyframe,
**each point transformed by the pose at its own time** (per-point times
interpolated across the datagram exactly as A6 does, `docs/A6-lio.md` §3.3)
and then expressed in the keyframe's own body frame. Deskewing here rather
than stamping the whole sweep with one pose matters at speed: the real fixture
moves at 6 m/s, so a 0.1 s scan smears 0.6 m.

It is then voxel-downsampled to `keyframe_voxel_m` (0.3) and capped at
`max_points_per_keyframe` (4,000) **by a stride, not by truncation** —
truncating keeps "the first N voxels the scan happened to hit", which for a
sweeping lidar is one side of the room.

**Memory.** Keyframe clouds are the pipeline's resident cost:
`(session length / 0.5 m) x 4,000 x 16 B` = 64 KB per keyframe, so a 1.8 km
30-minute walk is 3,600 keyframes = 230 MB. The final cloud is bounded by the
*volume* scanned at `dedup_voxel_m`, not by session length — the same argument
that makes A6's iVox safe. Everything else streams. §9 has the fix if a
session ever exceeds that.

---

## 4. The GTSAM decision

**Decision: hand-rolled. `vcpkg.json` is unchanged and no Boost enters the
build.**

### Why

1. **The problem is one factor type.** A Mid-360 post graph is N SE(3)
   variables, N-1 odometry between-factors in a chain, and a handful of loop
   between-factors. That is ~250 lines of analytic Jacobian and one sparse
   solve. GTSAM earns its keep on *heterogeneous* graphs — landmarks, IMU
   preintegration, incremental relinearization — none of which appear here.
2. **The dependency is not local.** `vcpkg.json`'s onboarding note is explicit:
   every new port must build on all five CI legs, and `gtsam` pulls Boost
   (serialization, thread, date-time, regex, timer, chrono, system) through the
   **macOS universal overlay triplet**, which compiles two architectures in one
   pass and therefore breaks any port whose portfile runs configure checks
   (`spikes/s7-windows-toolchain/TOOLCHAIN_NOTES.md`). Five-legged risk, one
   factor type.
3. **Determinism is a requirement, not a preference.** A6 promised
   bit-identical output for identical input and bought it by controlling every
   reduction order (`docs/A6-lio.md` §3.6, §4). A third-party solver with its
   own ordering heuristics, its own BLAS dispatch and its own `-march`-dependent
   vectorization would turn that into an identical-toolchain promise. Every
   loop in `pose_graph.cpp` reduces in a fixed index order, the RCM ordering
   breaks ties on node index, and nothing iterates a hash container on a path
   that affects a result. `post/two_runs_are_bit_identical` asserts the whole
   pipeline end to end.
4. **A8 set the precedent.** The mount-extrinsics solver declined Ceres for the
   same shape of argument and shipped a 120-line LM with a hand-written 6x6
   LDL^T (`docs/A8-pushbroom.md` §2, `vcpkg.json`'s A8 note).

### Where GTSAM becomes the right answer — the crossover

Stated up front so the next task does not re-litigate it:

1. **Landmarks or a second modality in the same graph** — visual features from
   A11's colorization keyframes, plane/line landmarks. Schur-complement
   machinery is real work and GTSAM has it.
2. **Incremental optimization** — re-solving every few keyframes during a long
   capture instead of once at the end. That is iSAM2, and iSAM2 is not a
   weekend.
3. **Switchable-constraint or max-mixture loop formulations**, i.e. anything
   robust beyond the Huber IRLS implemented here.
4. **Marginalization / fixed-lag smoothing** for a bounded-memory session.

**Not on that list: A10.** GNSS georeferencing needs *unary factors on
position*, weighted by fix quality (§3.4), added to exactly this graph.
`PoseGraph::add_position_prior(i, xyz, sigma_m, huber_delta)` is that seam —
implemented, and tested by `pgraph/position_priors_are_the_A10_seam`, which
also checks the thing that would silently break a georeferenced product: the
rotation block of a position factor's information is zero, so a fix constrains
where a node is and says nothing about which way it points. A10 adds a call,
not a solver. A10's remaining piece — the local↔global *similarity* transform —
is 7 parameters shared by every node and belongs in a wrapper around this
class, not inside it.

### What the solver actually is

```
residual   e = ( Log(Rz^T Ri^T Rj) ,  Rz^T (Ri^T (pj - pi) - pz) )     in R^6
retraction R <- R Exp(dtheta),  p <- p + R dp                          right, decoupled
normal eq  (J^T W Omega J + lambda diag) delta = -J^T W Omega e        Levenberg
solve      reverse Cuthill-McKee ordering + skyline (profile) LDL^T
robust     Huber IRLS on loop edges; none on odometry edges
gauge      node 0 removed from the system, not pinned with a large prior
```

Two choices in there are load-bearing:

**The error is decoupled, not the full SE(3) log.** With the full log the
translation residual is mixed through the left Jacobian and a "5 cm" sigma
stops meaning 5 cm. Decoupled, the translation block of the information matrix
means metres — which is exactly what a GNSS sigma or an ICP RMS is quoted in.

**RCM + skyline is what makes a hand-rolled sparse solve viable.** A pose
graph's Hessian is a chain plus a few long-range loop edges. In the natural
(time) ordering *one* loop from keyframe 0 to keyframe N-1 gives the matrix
full bandwidth and a profile factorization degrades to dense O(N³). RCM
reorders a cycle into 0, 1, N-1, 2, N-2, … — the textbook answer to precisely
this graph, ~50 deterministic lines, and no symbolic elimination-tree analysis
is needed because Cholesky of a symmetric matrix never creates a nonzero
outside the envelope. Measured (`pgraph/rcm_ordering_keeps_a_loop_narrow`): a
400-node chain plus one 0↔399 edge gives **bandwidth 1 block and a 22,707-scalar
envelope**, against 2.9 M scalars for the dense triangle — a 128x reduction,
and the reason a 30-minute session's graph is a sub-second solve rather than a
coffee break.

The Jacobians are checked, not assumed: `pgraph/one_edge_solves_exactly` drives
chi² from 7.5e4 to **1.3e-29 in 3 iterations** and then asserts the solved node
*is* the measurement to 1e-9 m and 1e-7 deg. A wrong rotation Jacobian still
converges slowly on an easy problem — that is exactly the class of bug A6 §3.4
warns about for the point-to-plane Jacobian — so the test asserts the iteration
count too.

---

## 5. Loop closure

### Scan Context (implemented, not depended on)

Kim & Kim, IROS 2018: a 20-ring x 60-sector matrix of maximum heights, a
20-vector rotation-invariant ring key, and a column distance minimized over all
cyclic sector shifts. Three properties make it right here:

* **Yaw-invariant by construction, and it hands the yaw back.** The winning
  shift *is* the relative yaw, so the step that finds the candidate also solves
  the hardest part of verifying it. Measured exactly: rotating a scan by
  7/14/…/42 sectors recovers the yaw to **1e-9 rad** with a descriptor distance
  of −6.7e-17 (`sctx/descriptor_is_yaw_covariant_and_recovers_the_yaw`).
* **It needs a gravity-aligned z, and A6 guarantees one** — the ESKF's static
  init puts +Z on measured gravity to 1e-9 on a tilted scanner
  (`docs/A6-lio.md` §3.1). This is the coupling to remember if A7's input ever
  stops coming from the LIO.
* **It is a bin maximum, not a count**, so it is insensitive to point density —
  which changes 6x across A6's decimation settings.

The paper's KD-tree over ring keys is replaced by a linear scan. With a few
thousand keyframes, scanning a 20-float key costs microseconds, is trivially
deterministic, and removes a data structure from the module. Revisit past
~50k keyframes.

**A measured finding worth carrying forward: indoors the distance range is
compressed by about an order of magnitude.** Five places in a 24 x 18 x 3.5 m
hall, each described twice from independent noise seeds
(`sctx/tells_two_places_apart`):

| | place 0 | 1 | 2 | 3 | 4 |
| --- | --- | --- | --- | --- | --- |
| **0** | **0.0033** | 0.137 | 0.164 | 0.239 | 0.326 |
| **1** | 0.137 | **0.0044** | 0.096 | 0.175 | 0.333 |
| **2** | 0.164 | 0.097 | **0.00003** | 0.158 | 0.341 |
| **3** | 0.237 | 0.174 | 0.156 | **0.00099** | 0.403 |
| **4** | 0.325 | 0.332 | 0.341 | 0.403 | **0.00000** |

The diagonal is the row minimum every time and the separation is **22x**, so
the search works — but the absolute numbers sit an order of magnitude below the
paper's outdoor KITTI figures, because a flat ceiling puts nearly every
occupied bin at the same height and the cosine term saturates. The
discrimination that remains comes from the *occupancy* pattern (which rings are
filled per sector), not from height contrast. Consequences:

* `distance_threshold` is documented as **scene-dependent** and left at the
  paper's 0.13. Indoors that is permissive.
* Permissive is the right side to err on, because **Scan Context is not the
  gate** — see below. The cost of a loose threshold is CPU, not a folded map.
* An earlier version of that test used two point-symmetric positions in a
  point-symmetric room and measured 0.020 — i.e. "the same place". That is a
  true property of the descriptor (a 180° rotation is one of the 60 shifts it
  searches), not a bug, and it is exactly why the verification step exists.

### ICP verification is the gate

A false loop edge is the single most destructive thing that can happen to a
pose graph: unlike odometry drift, which is smooth and bounded, a wrong loop
folds the map and every downstream product — floor plan, export, merge —
inherits the fold. So a candidate becomes an edge only if a **point-to-plane
ICP of the query keyframe against a local submap around the match** converges
and clears every gate in `LoopAcceptConfig`.

* **A submap, not the single matched keyframe.** One Mid-360 keyframe is a
  fraction of a second of a non-repetitive scan pattern, and two such clouds of
  the same place overlap much less than either overlaps the accumulated local
  map. Default ±5 keyframes, assembled with the (locally consistent) odometry.
* **Point-to-plane, not point-to-point**, because the residual is then
  invariant to sliding along a surface — which is precisely the ambiguity a
  lidar revisit has. Plane fits are FAST-LIO2's parameters (5 neighbours,
  0.1 m thickness, 0.1 planarity ratio) via a covariance eigen-decomposition
  about the centroid, not the `n·p + 1 = 0` least squares, which is singular
  for any plane through the world origin — and the world origin is the
  scanner's own starting pose (A6 §3.5).
* **`max_correspondence_m` stays wide (1.0 m).** A6 §7.3 measured that
  *tightening* it makes things worse: a small gate keeps only the matches that
  already agree with the prior and stops constraining the fit.
* **The acceptance gate measures how far ICP MOVED, not how far the loop is
  from identity.** `IcpResult` echoes its own initialization back for exactly
  this reason: a corridor revisited in the opposite direction is a legitimate
  180° loop, and a gate on absolute rotation would throw away precisely the
  loops that matter most.

Measured against a known transform (`picp/recovers_a_known_transform`): from a
yaw-only initialization, 5 iterations, 96.8% inliers, **0.00043° rotation error
and 0.048 mm translation error** on a 30k-point cloud with 5 mm range noise.

### Edge weights

Odometry edges: constant per-edge sigmas (0.5°, 2 cm). The LIO's own ESKF
covariance is available but is a covariance over a different state and is not
calibrated as an inter-keyframe uncertainty; a constant is the honest
placeholder, and it makes the loop edge's *relative* weight the only knob that
matters. Loop edges: `max(loop_sigma_trans_m, icp.rms_m)` — a loop that fit
badly pulls proportionally less — plus a Huber kernel at 2σ. **Odometry edges
get no robust kernel**, deliberately: a kernel on the chain hides real odometry
failures.

`pgraph/huber_contains_one_wrong_loop` is the control. A straight 40-node chain
plus one utterly wrong loop edge claiming node 39 sits on node 0: without a
kernel the chain collapses to **15.2 m**; with Huber it stays at **38.8 m** of
its true 39 m.

---

## 6. Verification (2026-08-15)

Host: Apple M4, macOS 26.5.1 (Darwin 25.5.0), Apple clang, CMake 4.4.2, Ninja,
`RelWithDebInfo`, clean build directory.

Clean build directory outside the repo (deleted afterwards), also clean under
`-DENGINE_WARNINGS_AS_ERRORS=ON`, `-DENGINE_WITH_EIGEN=OFF` and
`-DENGINE_WITH_LIVOX_SDK2=OFF` — zero warnings from any `slam/post/` or
`tests/test_post.cpp` translation unit in any of them.

```
$ ctest -LE sim
1/4 scanengine_tests ......... Passed  18.17 sec
2/4 scanengine_capi_smoke .... Passed
3/4 engine_cli_selftest ...... Passed
4/4 engine_cli_version ....... Passed
100% tests passed out of 4

$ ./scanengine_tests
[doctest] test cases:     336 |     336 passed | 0 failed | 7 skipped
[doctest] assertions: 2254382 | 2254382 passed | 0 failed |
```

A7 adds **20 test cases**: 6 pose graph, 3 Scan Context, 2 ICP, 3 filter,
1 composed loop-closure, 5 pipeline. Every pre-existing case still passes
unchanged. The whole suite runs in 18.2 s, of which A7 is ~10 s (ploop 3.6,
post 5.3, picp 0.9, pfilter 0.3, sctx 0.1, pgraph <0.01) — inside the budget,
so nothing needed a `slow` ctest label and `CMakeLists.txt` needed no change.

### 6.1 The ATE claim — `ploop/loop_closure_cuts_the_ate`

A circular 41 m walk through the synthetic hall returning to its start,
sampled as 44 keyframes. Odometry = truth corrupted by a drift that accumulates
per keyframe (a ~0.57°/keyframe yaw bias plus a 4 mm/keyframe translation
bias) — the shape of real LIO drift. Then the three real stages run on it:
Scan Context over the ray-cast keyframe clouds, point-to-plane ICP against the
±4-keyframe submap, and the pose graph.

| | ATE RMS | ATE max | final drift |
| --- | --- | --- | --- |
| odometry (drifted) | 1.733 m | 2.616 m | 2.616 m |
| **after loop closure** | **0.185 m** | **0.286 m** | **0.015 m** |
| improvement | **9.4x** | 9.1x | **175x** |
| *control: same graph, loop edges removed* | 1.733 m | 2.616 m | 2.616 m |

8 Scan Context candidates, **6 accepted** by the ICP gate (2 rejected). Graph:
43 variables, chi² 2270 → 1675 in 6 LM iterations, 0 rejected steps.

The control matters more than the headline. The same graph with the loop edges
deleted reproduces the input ATE **to 1e-6 m** — because an odometry chain is
already its own optimum — so the improvement is attributable to the loop
closures and to nothing else. Without that line, "the optimizer improved the
trajectory" would be an unfalsifiable claim about an optimizer that was handed
the answer.

### 6.2 End to end from a synthetic `.lscan`

`post/runs_end_to_end_from_a_recording` writes a **real `.lscan`** with A5's
`FileRecordWriter` — 1,667 Mid-360 point datagrams and 1,601 IMU datagrams,
byte-identical in layout to what A3's driver records — from a ray-cast circular
walk through the hall, then runs the pipeline on the directory.

| metric | measured |
| --- | --- |
| chunks read | 3,268 (1,667 point / 1,601 IMU), 0 malformed |
| points decoded | 127,425 |
| keyframes | 37 (68,950 points), 76 odometry poses, 37.7 m path |
| loop candidates / accepted | 2 / 2 |
| graph | 36 variables, bandwidth 2, keyframe shift 0.135 m RMS / 0.205 m max |
| re-integrated → dedup → final | 119,402 → 92,977 → 87,020 |
| **final cloud bounds** | **[−12.27, −9.31, −1.48] .. [12.29, 9.35, 2.20] m** |
| wall clock | **654 ms** (odom 420, loops 48, opt 0.09, reint 27, filter 158) |

The bounds are the assertion that matters: the hall is 24 x 18 x 3.5 m and the
world origin is the sensor at 1.4 m above the floor, so a correct
reconstruction lands in exactly that box with the floor at −1.4 and the ceiling
at +2.1. It does, to a centimetre.

Also asserted: every reported stage in `PostStage` order, progress monotone
non-decreasing and finishing at exactly 1.0, `PageStore` point count equal to
`final_points`, and every published coordinate finite.

### 6.3 Real Mid-360 capture, end to end

`post/real_mid360_capture` transcodes
`spikes/s2-mid360-sim/fixtures/outdoor_imu_ccby_6s.livoxdump` (17.5 MB, 6.0 s,
13,700 datagrams) into a `.lscan` with `FileRecordWriter` and runs the pipeline
on it — so the fixture goes through the pipeline's *actual* input path, not a
back door.

| metric | measured |
| --- | --- |
| chunks | 13,700 (12,500 point / 1,200 IMU), 0 malformed, 150 pre-clock |
| wire filter kept | 685,847 / 1,185,600 = 57.85% (matches A6 §7.2 exactly) |
| scans / skipped | 55 / 1 · \|g\| **9.816 m/s²** · residual RMS 10.0 cm |
| keyframes | 55 · path length **32.40 m** (A6 §7.2 measured 32.39 m) |
| loop candidates / accepted | **0 / 0** |
| graph | 54 variables, bandwidth 1, envelope 3,042 scalars, chi² 4.2e-25 |
| re-integrated → dedup (10 cm) → final | 637,727 → 417,702 → **391,069** |
| outliers removed | 3,315 isolated + 23,318 statistical (6.4%) |
| **wall clock** | **2.33 s total** — odom 941 ms, loops 4 ms, opt 0.04 ms, reint 172 ms, filter 1,209 ms |

Three of those numbers deserve a sentence each.

**Zero loops is the correct answer, and the test asserts it.** This is 6 s of a
vehicle driving 32 m down a road; it revisits nothing. `min_time_gap_s` (15 s)
excludes the whole capture from being its own revisit. A pipeline that invented
a loop here would be the bug, so `CHECK(s.loops_accepted == 0)` is a real
assertion rather than a placeholder. It also means **the ICP verification path
is exercised only on synthetic data** — see §10.

**chi² = 4.2e-25 is not a bug.** With no loop edges, the odometry chain's
measurements are by construction the between-transforms of the odometry poses,
so the initial estimate is already the exact optimum. The optimizer correctly
does nothing.

**The outlier filter is the expensive stage**, at 1.2 s of the 2.3 s — an 8-NN
search over 418k points. `OutlierFilterConfig::enabled = false` is one field
away for a caller that does not want it, and §9 lists the obvious speedups.

Extrapolating to a 30-minute session at these rates: ~470 s odometry (at the
live budget; ~4x that at full density), ~85 s re-integration, and an outlier
filter that scales with the deduped cloud rather than the session. Loop
detection grows as O(N) ICP runs at ~5 ms each. That is a "leave it running"
job on a desktop, which is what §3.8 already says it is.

### 6.4 Determinism

`post/two_runs_are_bit_identical` runs the whole pipeline twice on the same
`.lscan` and compares:

* every keyframe timestamp, odometry pose (7 doubles) and optimized pose
  (7 doubles), by exact `==`;
* the entire final cloud by `memcmp` — 68,123 points, 1.09 MB, byte-identical;
* `graph.final_chi2` by exact `==`, plus loops accepted, dedup points and
  re-integrated points.

The two places that would have broken it, and what was done:

1. **Hash iteration order.** `voxel_downsample`, `VoxelAccumulator` and
   `PointIndex` all keep an insertion-ordered side vector and never iterate the
   `unordered_map`. Output order is the order each voxel's *first* point
   arrived.
2. **Ordering heuristics.** RCM seeds each component at the lowest-index node
   of minimum degree and expands each frontier in (degree, index) order.

`pfilter/voxel_dedup_reduces_and_is_deterministic` and
`pfilter/streaming_accumulator_matches_the_batch_dedup` pin the first one
independently, the latter by asserting the streaming accumulator produces a
byte-identical result to the batch path (19,191 voxels from 50,000 points).

### 6.5 Cancellation

`post/cancellation_stops_every_stage` cancels from the progress callback once
inside each of `kOdometry`, `kLoopDetection`, `kReintegration` and
`kFiltering`, plus once before `run()` starts, plus once through an
**external** `CancelToken` (the A15 path). Every one returns
`ScanError::kCancelled`, leaves `stage()` at `PostStage::kCancelled`, and
leaves `progress()` below 1.0. Observed stop points: 0.131, 0.583, 0.786, 0.980.

Granularity is per datagram in the streaming passes (an atomic relaxed load
against ~2,000 datagrams/s), per keyframe in loop detection, per LM iteration
and per 4,096 factorization rows in the solver, and per 65,536 points in
`voxel_downsample` and the outlier filter. `pgraph/optimize_honours_the_cancel_token`
covers the solver on its own.

---

## 7. Output, and the stream-id question

The final cloud goes into a `PageStore` on **`StreamId::kSlamMap`** (= 8,
"registered world-frame map points"), which integration landed while A7 was in
flight and which is exactly what this is. There is no dedicated "final cloud"
stream id, and A7 did not add one — `core/types.h` is not A7's file, and the
enum is a C-ABI mirror.

`PostConfig::out_stream` is a config field precisely so that changing this
decision is not a code change. If integration later adds e.g. `kFinalCloud`,
set the field. Note that the live LIO map (A6) also publishes to `kSlamMap` by
default, so a caller that wants both resident at once should hand A7 its own
`PageStore` (`PostConfig::store`) rather than the engine's — which is the
default behaviour when `store` is null.

The final cloud is **not** written back into the `.lscan` as a
`kPointsXyzRgba` chunk in `processed/`. `ChunkType::kPointsXyzRgba` exists for
it and `stream_of()` currently maps it to `StreamId::kUnknown`, so landing it
means touching `src/record/lscan.cpp` — A5's file. §8 item 2.

---

## 8. Seams A7 did NOT take (owner action required)

Each is a small change in a file A7 does not own.

1. **`Engine` has no Mid-360 byte entry point**, so `lscan::ReplaySource` still
   cannot replay a Mid-360 capture (A5 §4 flagged this for A3/A10). A7 routes
   around it by decoding chunks itself. If A3 adds
   `Engine::push_mid360_datagram(DeviceId, ByteSpan, TimePoint)`, `ReplaySource`
   becomes able to drive a live `Engine` from a Mid-360 `.lscan`, and A7's
   decode loop could then be re-expressed on top of it — which would make
   "replay == capture" literally true for the second sensor too.
2. ~~**`stream_of(ChunkType::kPointsXyzRgba)` returns `kUnknown`**, so a processed
   cloud has nowhere to be written in a `.lscan`.~~ **CLOSED, ROUND 8 (0.5.0).**
   It now maps to `StreamId::kSlamMap`, which lands in its own file,
   `streams/map.bin` — its own rather than `lidar.bin` because replaying
   `kD6Raw` walks `lidar.bin`, and interleaving a 16-byte-per-point vertex
   stream into it would make every replay read and CRC megabytes it discards.
   The engine writes it live for a D6 session only (`Engine::Impl::on_page_update`
   gates on the pushbroom being on): at ~3,600 pts/s that is 57 KB/s and it
   makes opening a saved scan instant, whereas a Mid-360's 40k pts/s map would
   cost 640 KB/s for something A7 rebuilds better anyway. It is a **cache**,
   not the source of truth — Process re-resolves and overwrites it, and
   deleting `map.bin` costs speed, never data. A7 itself still does not write
   its own output there; the bytes are ready and the stream now exists.
3. **`PostStats` should become a `kJobProgress` payload.** `jobs/job.h` says
   progress is reported through `EventType::kJobProgress` on the event bus. A7
   deliberately exposes a plain C++ callback instead (see `progress.h`) so the
   pipeline runs in a unit test and in the cloud worker's CLI with no `Engine`.
   A15's job runner is where the two-line republishing lambda belongs.
4. **`engine_cli` has no `--post <lscan-dir>` flag.** `tools/engine_cli.cpp` is
   outside A7's ownership, but the cloud worker (§3.8, workstream D) needs
   exactly that one flag, and it is ~15 lines on top of `PostSlamPipeline`.
5. **A6's `LioOdometry` cannot report per-scan covariance**, so odometry edge
   weights are constants (§5). If A6 ever exposes the ESKF's `P` at scan
   boundaries in a form calibrated as an inter-keyframe uncertainty,
   `PostConfig::odom_sigma_*` should become that instead.
6. **A9's exporters should consume `PostSlamPipeline::final_cloud()` /
   `trajectory()`.** The trajectory is a `LioPoseSource`, i.e. a plain
   `PoseSource`, so nothing downstream can tell an optimized trajectory from a
   live one — which is Tech Spec §3 key rule 3 working as intended.

---

## 9. Known limitations

* **Keyframe clouds are all resident** (§3): ~230 MB for a 30-minute walk. The
  fix is to spill them to `processed/` after the descriptor is built and page
  them back for the ICP — the access pattern is already sequential-then-random
  over a handful of indices. Not implemented.
* **The outlier filter is the slowest stage** (§6.3: 1.2 s of 2.3 s). Three
  obvious speedups, none taken: reuse one `PointIndex` across both of its
  passes (it already does), sample the distance distribution rather than
  computing it for every point, and parallelise — which would need a
  deterministic reduction order to keep §6.4.
* **Loop search is O(N) ICP runs.** Each query keyframe yields at most one
  candidate, so the cost is bounded, but at ~5 ms per verification a 3,600-
  keyframe session spends ~18 s there. A ring-key KD-tree would not help (the
  shortlist is not the cost); a cheaper pre-verification (e.g. a coarse
  point-to-point fitness on 200 points) would.
* **`min_time_gap_s` is wall-clock, not path-length.** A scanner that stands
  still for a minute and then moves 2 m will consider its own recent past a
  revisit. A path-length exclusion would be strictly better and is a two-line
  change once `Keyframe` carries cumulative distance.
* **Scan Context's indoor dynamic range** (§5). Documented, measured, and left
  as the paper defines it.
* **No degeneracy detection on the graph.** A session whose only loop is along
  a featureless corridor is under-constrained in one direction, and the solver
  will happily produce the minimum-norm answer. The information matrix is
  available and its smallest eigenvalue would be an honest detector — the same
  gap A6 §10 item 6 records for the odometry update.
* **The corrected trajectory blends corrections between bracketing keyframes**
  (slerp on rotation, lerp on position). It is smooth and it is exact at the
  keyframes, but it is not a re-optimization of the intra-keyframe odometry.
  For 0.5 m keyframe spacing the difference is well under the sensor noise; for
  a session with sparse keyframes it would not be.

---

## 10. What is still hardware-only

1. **A real loop on real data.** Every loop-closure number in §6.1 is
   synthetic. The only real capture available is 6 s of a vehicle driving in a
   straight line, which correctly produces zero loops — so the ICP verification
   path, the acceptance gate and the graph's loop edges have never seen real
   lidar. A capture that walks a building and returns to its start is the
   single most valuable thing that could be added to `spikes/*/fixtures/`.
2. **Ground truth for a real trajectory.** §6.3 is self-consistency, exactly as
   A6 §7.2 was. Survey control or RTK would turn "32.40 m, plausible" into an
   ATE — and would let §6.1's claim be re-made on real data.
3. **A long session.** The longest thing this has run on is 8 s synthetic and
   6 s real. The memory model in §3 and the extrapolation in §6.3 are
   arithmetic, not measurements; a 30-minute capture would test the keyframe
   memory ceiling, the O(N) loop search and the buffer caps at once.
4. **Phone CPU and thermals.** A post run on Android is a foreground service
   (§3.8) and nothing in this repo has run it on a device.
5. **Real clock jitter.** A6 §7.2's caveat applies unchanged: the fixture's
   arrival stamps are synthetic, so A4's error budget is untested against
   either the live or the post pipeline.
