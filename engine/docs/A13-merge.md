# A13 — multi-session merge

**Scope:** `engine/include/scanengine/merge/**`, `engine/src/merge/**`,
`engine/tests/test_merge.cpp`.
**Spec:** §3.10 — *"Coarse: automatic when sessions are georeferenced (shared
CRS); otherwise manual 3-point / drag alignment. Refine: voxel-downsampled
point-to-plane ICP per pair; optional global relaxation for >2 sessions.
Merge: unified cloud with per-session provenance, voxel dedup, combined
export. Primary UX: desktop merge workbench (session list, pair alignment,
residual report); Android offers georeferenced auto-merge only."*
**Contract:** `engine/DESIGN.md` §2, §5, §6.
**Consumes:** A7's post pipeline (`docs/A7-post.md`) — its final cloud,
keyframes, `PoseGraph` and `icp_point_to_plane`; A10's `GeorefSolution` and
`crs::EnuFrame` (`docs/A10-gnss.md`); A8's `poses/se3.h`; A1's `PageStore`.

Nothing outside `merge/` was edited. `slam/post/` and `gnss/` are **included,
never modified**: the ICP kernel, the voxel dedup, the pose graph and the
whole geodesy layer are used as they ship.

---

## 1. What ships

| Piece | Header | Impl | Lines (h / cpp) |
| --- | --- | --- | --- |
| Session model: non-owning cloud, keyframes, georef + its ENU frame, provenance | `merge/session.h` | *(in merge_project.cpp)* | 158 / — |
| Coarse: ENU↔ENU composition, Horn correspondence solve, yaw+translation search | `merge/align.h` | `align.cpp` | 192 / 533 |
| Pairwise point-to-plane ICP with a residual trace, and the overlap gate | `merge/icp.h` | `icp.cpp` | 204 / 315 |
| `MergeProject`, `MergeReport`, `MergeResult`, dedup + priority, publish | `merge/merge.h` | `merge_project.cpp` | 353 / 872 |
| Deterministic exact-key voxel table | *(internal)* `src/merge/voxel_grid.h` | — | 131 |
| Tests | — | `tests/test_merge.cpp` | 1,074 |

**No new dependencies.** `vcpkg.json` and `CMakeLists.txt` are untouched
(`src/*.cpp` and `tests/test_*.cpp` are globbed with `CONFIGURE_DEPENDS`).
19 test cases, 990 assertions, **0.88 s**.

`MergePair` and `MergeReport` keep the names, field order and defaults A1
declared, so `tests/test_headers.cpp` still compiles unchanged; every A13
field is appended and defaulted.

---

## 2. The input is memory, not a `.lscan` — and why

A1's seam was `SessionMerger::add_session(const std::string& lscan_dir)`.
**That entry point cannot be implemented today and A13 did not fake it.**
Nothing writes a processed cloud into a `.lscan`: `ChunkType::kPointsXyzRgba`
exists but `stream_of()` maps it to `StreamId::kUnknown`, so A7's final cloud
has nowhere to be written (`docs/A7-post.md` §8 item 2, `docs/A5-lscan.md`).
Reading a file format that no writer produces would be dead code with a test
that only proves it can read what it just wrote.

So the supported path is in-memory, and it is shaped by what actually exists:

```cpp
SessionInput in;
in.provenance_id = "north-wing-2026-08-14";
in.cloud.add(pipeline.final_cloud());          // A7's vector, or…
collect_pages(store, StreamId::kSlamMap, &in.cloud);  // …a PageStore's pages
in.keyframes = /* A7 keyframes */;
in.georef.solution = fusion.solution();        // A10
in.georef.enu      = gnss.enu_frame();         // and the frame it lives in
project.add_session(in);
```

**The cloud is a list of spans and A13 copies nothing.** Three full-density
sessions are ~1 GB of `PointVertex`; a merger that copied them on the way in
would double that before doing any work. `PageView::data` is documented stable
for the store's lifetime (DESIGN.md §5: a page allocates once and never
reallocates), which is exactly what makes `collect_pages()` safe. `build()` is
the only method that copies points, and it copies only the survivors.

The `SessionMerger` interface is left declared in the header, marked
not-implemented, with the pointer to `MergeProject`. When somebody lands the
`processed/final.cloud` decision, `add_session(lscan_dir)` becomes a reader
plus one `add_session(SessionInput)` call.

---

## 3. Coarse path 1: georeferenced — and the trap in "shared CRS"

§3.10 says georeferenced sessions align automatically because they share a
CRS. They do, but **"shared CRS" is not "shared frame"**, and getting this
wrong is a silent, plausible-looking error:

`GeorefSolution::global_from_local` maps a session's local frame into **the
ENU tangent frame that session's own `GnssSource` anchored** — normally at its
first fix, i.e. wherever the operator switched the rover on that morning. Two
sessions captured on two days have two different ENU origins. Composing their
"global" transforms as if they were one frame puts the second session at the
offset between the two origins — hundreds of metres, with no warning.

So `SessionGeoref` carries the `crs::EnuFrame` next to the solution, and the
composition goes through ECEF:

```
X_ecef = R_from · X_from + o_from          (crs::EnuFrame)
X_to   = R_to^T (X_ecef − o_to)
⇒  to_from = [ R_to^T R_from | R_to^T (o_from − o_to) ]      (align.h::enu_from_enu)
```

and then

```
world_from_session_k = (world_from_ENU_ref) · (ENU_ref_from_ENU_k) · (ENU_k_from_local_k)
```

where the merged frame is the **anchor session's local frame** by default
(`set_project_enu_frame()` overrides it with a project CRS frame). Keeping the
merged frame local matters for a reason `gnss/georef.h` already states about
doubles: `PointVertex` positions are float32, which holds ~1 mm at 8 km from
the origin, so a merged cloud pinned to a UTM easting would quantize the
deliverable. Coordinates stay small; the CRS is metadata.

Two details that are not cosmetic:

* **`enu_from_enu` is not the identity for two nearby frames.** The ENU basis
  follows the ellipsoid normal — ~0.009°/km — so skipping the rotation costs
  ~16 cm of vertical at 1 km. `merge/enu_composition_matches_the_geodetic_route`
  checks the composition against an independent route (through geodetic
  coordinates rather than through the ECEF rotation product) and asserts the
  two agree to **under a micrometre**, and separately that the transform is
  *not* the identity.
* **A non-unit scale is refused, not absorbed.** `WeightedSimilarityEstimator`
  locks scale to 1 by default, but it can be run unlocked; a session whose
  scale is more than 1e-3 off is skipped with a blocker rather than baked into
  a "rigid" matrix that `mat4_inverse_rigid` would then quietly mis-invert.
  Rotation blocks are re-orthonormalized through a quaternion on the way in.

**Measured** (`merge/geo_aligns_three_sessions_through_the_shared_crs`): three
sessions, three ENU origins 200–400 m apart, one EPSG:32632 project, aligned to
a worst error of **6.2 × 10⁻¹⁰ mm and 0°** against ground truth. The
composition is exact; the accuracy of this path in the field is **A10's
transform accuracy and nothing of A13's** — `GeorefSolution` already reports it
honestly (`horizontal_sigma_m`, which is dominated by yaw sigma × lever arm),
and `SessionSummary::georef_horizontal_sigma_m` carries it into the report.

`align_georeferenced()` requires the **anchor** to be georeferenced when no
project ENU frame is set — the merged frame is the anchor's local frame, so
there is nothing to place the others against otherwise. That returns
`kInvalidState` with a blocker naming the fix, and is tested.

---

## 4. Coarse path 2: manual correspondences, and path 3: the yaw search

### 3-point (or n-point) picks

`solve_correspondences()` is weighted **Horn (1987) absolute orientation**: the
rotation is the eigenvector of the largest eigenvalue of a 4×4 symmetric matrix
built from the cross-covariance, extracted by cyclic Jacobi with a fixed sweep
order (deterministic on all five CI legs, same construction as A7's 3×3 in
`loop_closure.cpp`).

Why the quaternion form and not a 3×3 SVD Kabsch: **a unit quaternion cannot be
a reflection.** The `det < 0` correction a Kabsch needs is exactly the branch
that is easy to get subtly wrong for a degenerate — i.e. exactly-three-point —
pick set, and three points is the case §3.10 names. Why not Ceres/Eigen:
`docs/A8-pushbroom.md` §2, unchanged.

What it returns beyond the matrix, because a UI that only shows a matrix cannot
tell an operator they mis-clicked:

* per-pick residual, RMS and max;
* `implied_scale` — what a free scale *would* have been. A pick set that
  disagrees with a rigid model by 2% says so, and the transform is still
  rigid. If `allow_scale` is on and the scale comes out non-unit, the solve is
  reported but **not applied** (`kNotSupported`): everything downstream — the
  pose graph, `mat4_inverse_rigid`, the ICP init — is rigid;
* `spread_m[3]`, the square roots of the eigenvalues of the pick scatter,
  ascending. For three picks `spread_m[0]` is always ~0 (three points are
  coplanar) — that is fine and expected. **`spread_m[1]` is the collinearity
  test**, and a pick set that fails it is *refused*, because three points on
  one line define the transform only up to a rotation about that line, and an
  operator clicking three points along one wall must be told rather than
  handed a plausible matrix.

**Measured** (`merge/picks_recover_a_known_transform`): exact picks recover the
transform to **< 10⁻³ mm / 10⁻⁶ °**. Picks with 2 cm of click noise on both
ends, over an ~8 m pick baseline, land at **83.7 mm / 0.80°** with a reported
RMS of 40 mm. That number is the argument for the next section: a manual
placement is an ICP initialization, never a deliverable.

### The yaw + translation search — and where it is not to be trusted

For each candidate yaw (2° sweep over 360°, then a ±¼-step refinement around
the winner) the rotated source's occupancy is projected onto x, y and z as
three 1-D histograms and each is cross-correlated against the target's, with a
parabolic sub-bin peak fit. Walls, floor and ceiling put sharp peaks in those
marginals, so three separable 1-D searches replace one 3-D one, and a full
sweep costs milliseconds. The winner is then re-scored by real 3-D occupancy
overlap — the marginals only propose. Only yaw is searched because both local
frames are gravity-aligned (A6's ESKF static init, ARCore, A10's
`local_is_gravity_aligned`), so roll and pitch are already common.

It works, and it is the last thing you should reach for. Measured, in the tests:

| case | result |
| --- | --- |
| Two sessions of the **same** 13 m volume, different local frames (`yaw_search_places_a_co_located_session`) | overlap 1.00, margin 0.15 → **0.116 m / 2°**, and ICP then finishes at **0.76 mm / 0.006°** |
| **Square room**, source rotated 37° (`yaw_search_says_ambiguous_in_a_symmetric_room`) | overlap 1.00 at four yaws 90° apart, **margin 0.00 → `ambiguous`, `ok = false`** |
| Sessions 0 and 1 of the corridor: 30% overlap, extruded building (`yaw_search_slides_along_a_corridor_and_says_it_is_confident`) | **overlap 0.956, margin 0.088, and 9.9 m wrong** |

The third row is the one to read twice, and the test asserts it deliberately.
When two sessions overlap only partially in a building that is extruded along
one axis, occupancy overlap is maximized by **hiding the source inside the
target** rather than by putting it where it belongs: the wrong answer scores
0.96, the correct one about 0.3. And nothing downstream catches it — ICP
converges onto the wrong 10 m with a **28 mm** residual and a 0.93 overlap.
`margin` catches the symmetric-room failure; it does not catch this one.

That is why §3.10 makes the 3-point pick the non-georeferenced path, and why
`MergeProject::align_yaw_search()` is documented as operator-confirmed: it is a
proposal to show in a viewport, not an alignment to trust. §9 lists what would
actually fix it.

---

## 5. Refinement: A7's ICP with the outer loop unrolled

`post::icp_point_to_plane()` already is the engine's point-to-plane ICP —
FAST-LIO2 plane fits, Huber kernel, A6's Jacobian, deterministic 6×6 LDL^T, a
Levenberg floor for the single-wall rank-deficient case, measured by A7 at
0.00043° / 0.048 mm against a known transform. A second implementation here
would be a second set of those bugs.

What it does not do is hand back the iteration history, and §3.10's residual
report needs it. So A13 calls it with `max_iterations = 1` in its own loop:

* **Cost:** the target index is rebuilt once per iteration. After the voxel
  downsample a target is ~10⁴ points and a merge runs a handful of pairs, not
  thousands of loop candidates — the whole 8-iteration refinement of a 10 k ×
  10 k pair measures **58 ms**. This is the one place A13 pays for reuse, and
  it is the right side of the trade.
* **Gain:** the outer loop owns convergence, the coarse-to-fine gate, and the
  rollback.

**The trace is monotone non-increasing by construction.** A step whose residual
rose is undone, is not recorded, and stops the loop; `rejected_steps` counts
them. The caveat that makes this non-trivial is that ICP's residual is measured
over a correspondence set that *changes* between iterations, so a raw
comparison is not apples to apples — which is exactly why the rollback returns
the best iterate rather than trusting the last one. A rollback after at least
one improving step is reported as **converged**: a stationary point is
convergence. A rollback on the very first step is not (it means the
initialization was already the local optimum).

### The correspondence gate: 0.5 m, and why A13 differs from A7

A7 keeps `max_correspondence_m = 1.0` and A6 §7.3 measured that *tightening*
it makes things worse. Both remain true for a loop closure — where the
residual is an accept/reject gate and the initialization can be metres out.
A13's residual is **a number shown to an operator next to a merge button**, and
its initialization is decimetres, so the same gate lies:

| gate, at the **true** alignment | RMS | mean \|r\| | correspondences |
| --- | --- | --- | --- |
| 1.0 m (A7's) | **52.1 mm** | 10.5 mm | 3,498 |
| 0.5 m (A13's) | **9.6 mm** | 5.3 mm | 3,146 |

Same alignment, same clouds, 5× the reported residual — and the extra is
entirely **surface-edge correspondences**: a point in the corridor past the end
of a partition, matched to that partition's extended plane. The range noise in
the fixture is 5 mm, so 9.6 mm is roughly the floor and 52 mm is an artefact.
`merge/icp_gate_choice_is_measured_not_preferred` is that table as a test.

Because "decimetres" is not true of the yaw-search fallback, the first
`coarse_iterations` (6) run at `coarse_gate_scale` × the gate (1.5 m) and then
tighten. Coarse-to-fine: pull it in from a metre out, then measure the residual
on correspondences that mean something. Each `IcpIteration` records the gate
that was in force, because the step down at the stage boundary is visible in
the residual and must not look like a bug. Convergence *within* the coarse
stage tightens the gate and continues rather than returning an estimate fitted
to correspondences the fine stage is about to discard.

**Measured** (`merge/icp_improves_a_perturbed_alignment`): from a 2° / 0.15 m
coarse error,

```
  it 0  gate 1.5 m   rms 280.2 mm   mean|r| 174.0 mm   3948 inliers   step 2.050° / 600.5 mm
  it 1  gate 1.5 m   rms 108.2 mm   mean|r|  35.3 mm   3799 inliers   step 0.250° /  79.7 mm
  it 2  gate 1.5 m   rms 105.7 mm   mean|r|  22.2 mm   3806 inliers   step 0.009° /   1.8 mm
  it 3  gate 1.5 m   rms 105.6 mm   …
  it 4  gate 1.5 m   rms 105.6 mm   …                                 step 0.0001° / 0.003 mm
  it 5  gate 0.5 m   rms  11.1 mm   mean|r|   7.0 mm   3148 inliers   step 0.187° /  15.3 mm
  it 6  gate 0.5 m   rms   9.6 mm   mean|r|   5.3 mm   3147 inliers   step 0.012° /   0.8 mm
  it 7  gate 0.5 m   rms   9.6 mm   (final evaluation at the returned estimate)
```

and the alignment error goes **393.98 mm / 2.000° → 0.88 mm / 0.0061°**.

### Overlap is a gate, not a statistic

Point-to-plane ICP does not know that two sessions are of two different wings:
every source point still finds *some* nearest plane and the solve is still
well-posed. So overlap is measured independently of the fit — occupancy of one
cloud's voxels in the other's, at a one-voxel tolerance (the 2×2×2 block around
each sampled position, so no 27-fold dilation of the stored set), sampled by
stride and **symmetric**: `min(a_in_b, b_in_a)`, because a small session fully
inside a big one scores 1.0 one way and 0.1 the other, and only the second
number says the big session is barely constrained by this pair.

A pair below `min_overlap` (0.15 — deliberately low, a corridor joining two
wings is a legitimate 20%) is **not refined and not merged**, and reports
`low_overlap` with the reason. That is a different answer from "aligned
badly", and the report keeps them different.

**Measured** (`merge/non_overlapping_pair_reports_overlap_not_a_merge`):
sessions covering x ∈ [0,13] and [18,30] give overlap **0.000 both ways**,
`refine()` returns `kNotFound`, nothing moves, and
`merge/overlap_survey_matches_the_geometry` confirms the 4 m-overlap pairs
score 0.20–0.50 while the disjoint one stays under 0.05.

### Global relaxation for > 2 sessions

Nodes are sessions (`world_from_session`), the anchor is the fixed node, and
each converged pair becomes a `BetweenFactor` with `i = b, j = a` — which is
literally `b_from_a`, because `pose_graph.h`'s convention is `z = T_i⁻¹ T_j`.
Translation sigma is `max(icp_sigma_trans_m, pair RMS)`, the rule A7 uses for
loop edges, with a Huber kernel at 2σ.

Georeferenced sessions additionally get **`add_position_prior()` — the seam
`pose_graph.h` calls "THE A10 SEAM"** — at the CRS-composed position, weighted
by that session's own `horizontal_sigma_m`. That is what stops a chain of
pairwise ICPs from sliding a long merge off its survey control, and it
constrains position only: a GNSS fix says nothing about heading, and the
rotation block of that factor's information is zero.

With two sessions the graph is skipped entirely (a two-node, one-edge graph's
optimum *is* the ICP result); alignments are then propagated by a spanning
tree out of the anchor, pairs visited in index order.

**Measured** (three sessions, two edges, two priors): χ² **0.00375 → 0.000164
in 3 iterations**, 2 variables, worst pair RMS 9.9 mm.

---

## 6. Provenance, dedup and the merged cloud

### Per-point provenance without a per-point field

§3.10 requires per-session provenance in the merged cloud. `PointVertex` is 16
bytes of position + RGBA8 and DESIGN.md §5 forbids changing it (the S3-proven
GPU layout), so there is no channel to put a session id in. A parallel
`uint32` array would be +25% memory on the deliverable.

**A13 stores a run table instead.** The merged cloud is emitted as one
contiguous run per session, in priority order, and `MergeResult::ranges` is the
table; `session_at(i)` is a binary search over at most N entries. That *is*
per-point provenance — exact, not a summary — in O(N) memory instead of
O(points). It is lossless because dedup is defined so that **every surviving
point belongs to exactly one session** (below).

Published into a `PageStore`, each run becomes a run of pages. DESIGN.md §5
already says pages are single-*stream* so that "provenance survives into merged
exports (A13)"; single-*session* pages are the natural extension and A13 gets
them for every page except the ones a session boundary falls inside — because
`PageStore` has no page-break API. So `publish()` reports the mapping it
actually produced, captured from the store's own subscriber callback rather
than predicted: one `PageProvenance` per (page, session), which is one entry
per page for every unshared page, and `MergeReport::pages_shared` counts the
rest. Measured: 27,329 points into **7 pages, 2 shared** (three sessions ⇒ at
most two boundaries). §9 item 1 is the one-line `PageStore` change that takes
it to zero.

### Voxel dedup with a priority rule

Two stages, both on the **world-frame lattice** so they agree with each other:

1. **Within a session** — A7's `VoxelAccumulator` (insertion-ordered,
   optionally averaging, `docs/A7-post.md` §6.4), fed with each point already
   transformed into the merged frame.
2. **Across sessions** — sessions are laid down in priority order and each
   claims voxels; a voxel already claimed is a duplicate and the later
   session's point is dropped. One pass, deterministic, and it is what keeps
   provenance single-valued.

Priority is `MergePriority::kGeoreferencedFirst` (default: a session tied to
the CRS owns the overlap, because its coordinates are the ones a survey
deliverable quotes) or `kSessionPriority` (the caller's per-session integer,
lower wins, ties on session id). `require_alignment` defaults to true so an
unaligned session is skipped rather than dumped at the origin — the worst
failure mode available, because it looks like data.

**Measured** (`merge/build_keeps_per_session_provenance_and_dedups`, 15 cm
dedup): 34,949 → **27,329** points = 4,482 intra-session + 3,138 cross-session
duplicates removed, with the identity
`input == output + intra + cross` asserted, the run table asserted contiguous
and gap-free, every sampled point's colour tint checked against its run's
session, and the merged bounds checked to lie inside the building.
`merge/priority_decides_who_owns_the_overlap` flips the priority and asserts
the loser is the only session that loses points.

### Determinism

`merge/two_runs_are_bit_identical` runs the whole pipeline twice and compares
the merged cloud with `memcmp`, plus every range, every pair's RMS / overlap /
iteration count / 16 transform doubles by exact `==`, the graph's final χ², and
every session's `world_from_session`. Nothing iterates a hash container on a
path that affects a result: `src/merge/voxel_grid.h` uses **exact** (i,j,k)
keys in an `unordered_map` (so two far-apart voxels can never collide — a real
hazard at 3 cm over a 400 m site, where a 21-bit-per-axis packing wraps) and
every output order comes from an insertion-ordered vector.

---

## 7. The residual report — C6's surface

`MergeProject::report()` returns a `MergeReport` that is the merge workbench's
model. It is rebuilt by `survey_overlap()`, `refine()` and `build()`, and it
never needs a second call to explain itself:

```
MergeReport
  pairs[]                 MergePair — one per session pair, ALWAYS present
    session_a, session_b
    b_from_a[16]          the refined relative transform
    rms_residual_m        after refinement          ─┐ the four A1 seam fields,
    overlap_fraction      symmetric (conservative)   │ unchanged in name and
    converged                                        │ meaning
    rms_before_m          before refinement         ─┘
    overlap_a_in_b, overlap_b_in_a
    iterations, rejected_steps, inliers, inlier_ratio
    refined               ICP actually ran
    low_overlap           the gate fired: reported, NOT merged
    in_graph              became an edge of the relaxation
    blocker               stable string, empty when nothing went wrong
  worst_rms_m, worst_overlap
  pairs_refined / pairs_converged / pairs_low_overlap
  relaxed, graph          post::PoseGraphSummary (χ², iterations, variables…)
  sessions[]              SessionSummary — provenance id, AlignSource + label,
                          anchor, georeferenced + its sigma, world_from_session,
                          input/kept/dropped point counts, keyframe count, and
                          the session's [first_point, count) run in the cloud
  input_points, merged_points, dedup_dropped_points, priority_dropped_points
  pages_appended, pages_shared
```

Plus, per pair, the full `PairIcpResult::trace` from `refine_pair()` when a
workbench wants to plot the convergence rather than summarize it — one entry
per iteration with RMS, mean |r|, inliers, step size and the gate in force.

Three deliberate properties for a UI:

* **Every blocker is a stable English string**, never a localized or formatted
  one, so a log and a test can both match on it (A7's `loop_is_acceptable()`
  convention).
* **A low-overlap pair is a first-class outcome**, not an error and not a bad
  fit. The workbench should draw it as "these two do not see the same place".
* **`AlignSource` + `to_string()`** says how each session got where it is —
  `georeferenced` / `manual` / `yaw-search` / `icp` / `relaxed` — which is the
  provenance an operator needs before pressing export.

Android's "georeferenced auto-merge only" (§3.10) is
`align_georeferenced()` → `refine()` → `build()` with no other call.

---

## 8. Verification (2026-08-15)

Host: Apple M4, macOS (Darwin 25.5.0), Apple clang, CMake 4.4.2, Ninja,
`RelWithDebInfo`, clean build directory outside the repo (deleted afterwards).
`src/merge/*.cpp` also compile clean under `-Wall -Wextra -Wshadow -Werror`.

```
$ ctest -LE 'sim|sim-rtk'
1/5 scanengine_tests ......... Passed  17.68 sec
2/5 scanengine_capi_smoke .... Passed
3/5 engine_cli_selftest ...... Passed
4/5 engine_cli_version ....... Passed
5/5 engine_cli_post .......... Passed
100% tests passed out of 5

$ ./scanengine_tests
[doctest] test cases:     480 |     480 passed | 0 failed | 7 skipped
[doctest] assertions: 2279641 | 2279641 passed | 0 failed |

$ ./scanengine_tests --test-case="merge/*"
[doctest] test cases:  19 |  19 passed | 0 failed |   (0.88 s)
```

The 19 cases: 2 ENU/georef composition, 2 georeferenced alignment, 5 pick
subcases, 3 ICP (improvement, trace, gate), 2 overlap, 3 yaw search, 4 build /
priority / publish / determinism, 3 plumbing (pages, keyframes, labels).

Headline numbers, all from `test_merge.cpp` against ground truth:

| claim | measured |
| --- | --- |
| georeferenced auto-align, 3 sessions, 3 ENU origins | **6.2e-10 mm** worst (spec asks < 5 mm) |
| ENU↔ENU composition vs. an independent geodetic route | < 1 µm |
| exact 3-point picks | < 1e-3 mm / 1e-6 ° |
| 3-point picks with 2 cm click noise | 83.7 mm / 0.80° (reported RMS 40 mm) |
| ICP from a 2° / 0.15 m coarse error | 394 mm / 2.00° → **0.88 mm / 0.0061°** |
| ICP residual, same pair | 180 mm → **9.9 mm** (5 mm range noise) |
| ICP trace | monotone non-increasing, 8 entries, 1 rolled-back step, 58 ms |
| disjoint sessions | overlap 0.000 / 0.000, `low_overlap`, nothing moved |
| global relaxation, 3 sessions | χ² 3.75e-3 → 1.64e-4, 3 iterations |
| dedup at 15 cm | 34,949 → 27,329 (4,482 intra + 3,138 cross) |
| publish | 7 pages, 2 shared between two sessions |
| determinism | merged cloud byte-identical across two runs |

---

## 9. Seams A13 did not take (owner action required)

1. **`PageStore` has no page-break API**, so a session boundary can fall inside
   a page and 2 of 7 pages carry two sessions. `Status append(StreamId, Span,
   int64, uint32_t* appended, bool start_new_page)` — or a `seal_page()` — makes
   merged pages single-session and `MergeReport::pages_shared` permanently 0.
   A1/A14 own `cloud/`.
2. **A processed cloud still cannot live in a `.lscan`** (A7 §8 item 2). Until
   it can, `SessionMerger::add_session(lscan_dir)` cannot be implemented and a
   merge project cannot be reopened from disk. §3.11's `merged/` directory
   ("merge graphs + results") has no writer either — `MergeReport` +
   `MergeResult::ranges` is exactly what belongs in it, and serializing it is
   `record/`'s decision, not `merge/`'s.
3. **No C ABI.** §3.10 gives Android georeferenced auto-merge only, which is
   three calls; `capi/scanengine_c.h` is not A13's file and the ABI version
   would have to be bumped in lockstep. The three-call sequence is in §7.
4. **No `engine_cli --merge`.** `tools/engine_cli.cpp` is outside A13's
   ownership; the cloud worker (§3.8) would want `--merge <dir>...` the same way
   A7 wanted `--post`.
5. **`MergeReport` is not a `kJobProgress` payload** and `build()` takes a
   `post::CancelToken` but reports no progress fraction. A15's job runner is
   where the republishing lambda belongs; A7 made the same call for the same
   reason (`docs/A7-post.md` §8 item 3).
6. **Colorized merges (A11) are unhandled in dedup.** `average_within_session`
   averages colour with position inside a voxel; across sessions the priority
   rule picks one session's colour outright, which is right for geometry and
   possibly wrong for appearance if two sessions were colorized under different
   lighting. A11 owns what "correct" means there.

---

## 10. Known limitations

* **The yaw-search fallback can be confidently wrong** in an extruded or
  repetitive building at partial overlap — measured at 9.9 m with a 0.96
  overlap score (§4). What would fix it: scoring on distinctive structure
  rather than raw occupancy (the marginal-correlation peak *sharpness* is
  already computed and thrown away), requiring the score to be consistent with
  the sessions' extents, or a proper global registration (4PCS / FPFH+RANSAC),
  which is a task, not a tweak.
* **The overlap measure is occupancy, not surface agreement.** Two parallel
  walls 20 cm apart score as overlapping at a 30 cm voxel. It is a gate against
  *nothing in common*, and the residual is what says the geometry agrees.
* **Pair RMS is reported from the pairwise solve, not recomputed after the
  global relaxation.** The graph moves sessions by less than the residual it is
  reconciling, and the χ² says how much the pairs disagreed; a post-relaxation
  re-evaluation would be one more ICP evaluation pass per pair and is not done.
* **All pairs are refined: O(N²) ICPs.** Fine for a workbench with a handful of
  sessions, wrong for fifty. The overlap survey is the natural pre-filter and is
  already O(N²) but ~100× cheaper.
* **A session is rigid.** §3.10 says so, but a long session with residual drift
  cannot be bent to fit another one here; that is what A7's pose graph is for,
  inside the session. The honest version — one node per *keyframe* across all
  sessions, with inter-session loop edges — is a merge of pose graphs, not of
  clouds, and `SessionInput::keyframes` is carried so that it has somewhere to
  attach.
* **The merged frame is a local ENU/anchor frame, and coordinates are float32.**
  Good to ~1 mm within 8 km of the anchor; a project spanning more than that
  needs tiling, which is A14/A9 territory.
* **No intensity/colour-aware dedup**, no LOD generation on the merged cloud,
  and no incremental merge: adding a fourth session re-runs everything.

---

## 11. What is still hardware-only

1. **Every number in §8 is synthetic.** The fixture is a ray-cast-free
   sampling of flat surfaces with 5 mm Gaussian range noise; a real Mid-360
   cloud has incidence-dependent noise, mixed pixels at edges, and dynamic
   objects (people walking between two sessions of the same room) that this
   fixture has none of. Mixed pixels in particular are what the surface-edge
   correspondences of §5 look like in the wild, and they will be worse.
2. **No two real sessions of the same place exist in the repo.** The only real
   capture is 6 s of a vehicle driving in a straight line (A7 §6.3). Two
   captures of one building, ideally one of them RTK-fixed, is the single most
   valuable fixture that could be added — it would turn the georeferenced path
   from "the composition is exact" into an end-to-end accuracy claim.
3. **The A10 coupling is untested against a real `GeorefFusion`.** The tests
   construct `GeorefSolution` + `EnuFrame` directly. Nothing has yet run
   `GnssSource → GeorefFusion → SessionGeoref` on one machine, and the ENU-origin
   trap in §3 is exactly the kind of thing that only bites in that integration.
4. **Memory at real scale.** Three 30-minute sessions is ~1 GB of resident
   `PointVertex` plus the merged output; the non-owning span design is what
   makes that possible, but it has never been run.
