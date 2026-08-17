# A8 — D6 pushbroom assembler + mount-calibration solver

**Scope:** `engine/include/scanengine/poses/`, `engine/src/poses/`,
`engine/include/scanengine/slam/pushbroom/`, `engine/src/slam/pushbroom/`,
`engine/tests/test_pushbroom.cpp`, `engine/tests/test_mount_calib.cpp`.
**Spec:** §3.3 (D6 pushbroom), §3.5 (calibration), §3.4 (pose fusion).
**Contracts:** `engine/DESIGN.md` §2 (threading), §3 (errors), §5 (PageStore),
§6 (how to add a module).
**Reads:** `spikes/s6-calibration/REPORT.md` + `WIZARD.md` — every number in
§4–§6 below is a re-measurement of one of theirs. `engine/docs/A4-timesync.md`
for the clock mapping the pose stream goes through.

Nothing outside those directories was touched. §7 lists the C-ABI and wiring
additions the orchestrator has to place in files A8 does not own.

---

## 1. What landed

| File | What |
| --- | --- |
| `poses/se3.h` | Rigid-transform vocabulary over plain arrays: quaternion slerp, exp/log SO(3), row-major 4x4 compose/invert/apply, a rigidity check. Header-only, no dependencies. |
| `poses/pose_interpolator.h` | `PoseSample` + `PoseGate` + the `PoseInterpolator` seam. The reason it exists is §2.3. |
| `poses/external_pose_source.h/.cpp` | `ExternalPoseSource` — the ARCore/RTK/replay ingestion path. Ring, SE(3) interpolation, A4 time mapping, staleness/confidence/tracking-loss gating. |
| `slam/pushbroom/pushbroom_assembler.h/.cpp` | `D6PushbroomAssembler` — profiles × trajectory × mount extrinsic → world points into the `PageStore`. |
| `slam/pushbroom/mount_calibration.h/.cpp` | `MountCalibrationSolver` — the S6 planar-checkerboard extrinsic solve, two-stage, with the split-half quality gate. |
| `tests/test_pushbroom.cpp` | 16 cases: SE(3), interpolation edge cases, analytic ground truth, wraparound, tracking-loss flagging, live-vs-offline determinism, bounds. |
| `tests/test_mount_calib.cpp` | 12 cases, containing a C++ port of the S6 simulation, which regenerates §5's tables on every run. |

28 test cases, 26,839 assertions, **0.65 s** for the whole A8 suite.

Both new test files include their headers **first and alone**, so they also
serve as the self-containment check `tests/test_headers.cpp` performs for the
A1 seams (that file is not A8's to edit — see §7).

---

## 2. Solver choice: neither Ceres nor Eigen

**Decision: a hand-written Levenberg–Marquardt on a 6x6 dense normal matrix,
with no external dependency at all.** ~120 lines in
`src/slam/pushbroom/mount_calibration.cpp`.

### Why not Ceres

`slam/slam.h` and `vcpkg.json` both anticipated Ceres arriving at A7/A8. It is
not warranted here:

* **The problem is six parameters.** One rigid transform. The normal equations
  are 6x6 whether the capture has 8 poses or 45 and whether each pose carries
  20 returns or 600. Ceres exists to make *large sparse* problems tractable —
  its schur complement, its sparse linear solvers, its automatic
  differentiation — and none of that applies to a dense 6-vector.
* **The Jacobian is analytic and two lines long.** With a left perturbation
  `R ← exp(δθ)R, t ← t + δt`, the point-on-plane residual differentiates to
  `∂r/∂δθ = (Rp) × n` and `∂r/∂δt = n`. There is nothing for autodiff to earn.
* **The cost of the port is real.** `vcpkg.json`'s `$dependency-onboarding-order`
  is explicit: every port must build on all five CI legs, and autotools-based
  ports cannot compile-and-run their configure checks for two architectures at
  once, so they **fail under `cmake/triplets/universal-osx.cmake`** (S7's
  `TOOLCHAIN_NOTES.md`). Ceres pulls glog and — depending on the feature set —
  SuiteSparse/METIS behind it. That is a meaningful risk to the macOS
  universal and Android NDK legs, taken on behalf of a 6x6 solve.
* **It runs on the phone.** The wizard solves on-device between two screens.
  A dependency-free solve is a few milliseconds and adds nothing to the APK.

### Why not Eigen either

Eigen is already available (`ENGINE_WITH_EIGEN`, default ON), so using it would
have been free. It is not used because after writing the geometry the *entire*
remaining linear-algebra need is **one damped 6x6 symmetric solve** — plus a
6x6 inverse and a 6x6 symmetric eigenvalue pass, both of which serve
diagnostics only. `solve_sym6()` is a 28-line LDLᵀ; it is exercised by every
one of the ~600 solves the test suite runs.

Two things are gained by that, beyond line count:

1. **`poses/se3.h` stays includable from public headers.** No Eigen type
   appears in any A8 API — poses are quaternion+position arrays and transforms
   are `double[16]`, which is what `slam/slam.h`, `color/colorize.h` and
   `merge/merge.h` already declared and what the C ABI can mirror as a memcpy.
2. **`ENGINE_WITH_EIGEN=OFF` remains a working configuration**, which matters
   because nothing in the engine required Eigen before this task and A8 had no
   business being the module that made it mandatory.

Accuracy is not the trade: §5's tables reproduce S6's scipy results, and the
zero-noise case recovers the extrinsic to **0.000° / 6e-13 mm**.

If a future task needs a *joint* solve — extrinsics and time offset and
intrinsics together across many poses, or the A7 pose graph — that is the point
at which Ceres/GTSAM earns its place. The residual formulation here transfers
unchanged.

---

## 3. The assembler

### 3.1 The join

```
p_world(t) = world_from_phone(t) · phone_from_lidar · p_lidar(angle, range)
```

`phone_from_lidar` is what §4 recovers. It is the *same transform* the
calibration solver calls `camera_from_lidar` and `color/colorize.h` calls
`camera_from_lidar`: ARCore's pose is the camera pose, so the phone frame and
the camera frame are one frame. One transform, two names, no conversion.

The sensor-frame convention lives in **`drivers/d6/d6_fan.h`** and nowhere else:

```
D6 fan frame, RIGHT-handed:
  +y = the 0-degree beam direction (the vendor's zero mark)
  +z = the spin axis, pointing out of the BASE of the unit (away from the cap)
  +x = y × z

p_lidar = ( −d·sin θ,  d·cos θ,  0 )
```

`D6Driver::on_point()` calls the same function for the live preview, so the
extrinsic means the same thing in both places.

> **ROUND 9 (0.6.0) — the x term used to be `+d·sin θ`, and that was the
> mirror.** The owner's report was *"the output is left right reversed"*, and
> it was right. Two things had to be true at once for this to survive as long
> as it did:
>
> * **The vendor states its angle convention in a LEFT-HANDED coordinate
>   system.** `docs/bench/BENCH_SETUP.md` §3.1 quotes the manual directly —
>   *"left-hand coordinate system … rotation angle increases clockwise …
>   zero-degree direction marked in the figure"*. S1 transcribed the
>   datasheet's `(x, y)` pair verbatim into a right-handed frame. Keeping `x`
>   and `y` while the handedness flips **reverses the sense of rotation about
>   the third axis**, and nothing downstream ever compensated. The datasheet's
>   figure is a top view — the sweep is clockwise seen from the CAP — and the
>   old formula implemented clockwise seen from the BASE.
>
> * **A planar fan's mirror is achievable by a proper rotation.** Every return
>   has `z == 0` exactly, and restricted to that plane the reflection `x → −x`
>   is identical to `diag(−1, +1, −1)`, a det = +1 rotation of 180° about the
>   fan's own 0° axis. So §4.4's rigidity guard was never going to fire, and
>   the owner's `scan-017` manifest storing a det = +1 extrinsic proved
>   nothing. That is worth stating plainly next to §4.4, which offers "mirrored
>   cloud" as a symptom of a **non-rigid** matrix: this round's mirror came
>   from a perfectly rigid one.
>
> It also survived because **every geometry test in the suite measured a
> sign-blind quantity** — axis extents (`D6PushbroomGeometryTest`), best-fit
> plane RMS (§3.7, ROUND 7's gait test, ROUND 8's reopen test), point counts,
> live-vs-offline equality. A mirrored room has exactly the same extents and
> exactly the same planarity as the real one. `tests/test_round9_chirality.cpp`
> is the first test in the tree that can tell a room from its mirror image: a
> corridor walk with a doorway cut into the wall on the operator's LEFT, with
> "left" computed as `up × forward` from the resolved trajectory rather than
> hard-coded. Fixed convention: 0 returns left of the walk in the doorway band,
> 1,440 right. Pre-fix convention, same returns, same assembler: exactly
> reversed.
>
> **The CAD nominal did not change and did not need to.** Under the corrected
> frame the owner's rig — 0° beam UP, cap FORWARD, D6 on the back of a portrait
> phone — maps lidar `+y → camera +Y`, lidar `+z` (the base) `→ camera +Z`
> (backward, so the cap faces forward), lidar `+x → camera +X`. That is the
> identity rotation `BracketNominals.cadNominal(COIN_D6)` has always carried.
> Changing BOTH the formula and the nominal would have been a no-op; exactly
> one of them was wrong, and it was the formula.
>
> **Legacy captures are fixed by re-resolving, with no manifest migration**,
> because `old_fan(θ) ≡ diag(−1, +1, −1) · new_fan(θ)`. Re-running a pre-0.6.0
> `.lscan` through today's `D6ResolvePipeline` with its own stored extrinsic
> un-mirrors it. `d6::d6_legacy_fan_extrinsic()` goes the other way and is what
> the chirality test's control arm uses to run the old convention through the
> production code path.

### 3.2 Three properties worth the code

**Per-point time, not per-packet time.** The D6 spins at 10 Hz, so a
revolution spans 100 ms. Walking at 1 m/s that is **10 cm of rig travel inside
one revolution**, and ~3° of yaw at a gentle turn. `ProfilePoint` therefore
carries its own `t_mono_ns`. The `Span<const PointVertex>` overload inherited
from the A1 `PushbroomAssembler` seam shares one stamp across a batch and is
documented as the coarse path.

> **ROUND 9 — where that per-point stamp now comes from.** See A2 §9: it is no
> longer a wire-rate byte position but the instant the sample was *taken*,
> reconstructed from the device's own 4 kHz sampling rate. The two differ
> because the D6 buffers a packet and transmits it ~1.7x faster than it samples.

**The pose side is densified too.** ARCore delivers ~30 Hz — 33.3 ms median on
the owner's real `scan-017` — while the lidar now stamps every return
individually at 4 kHz, so one pose bracket covers ~133 returns whose trajectory
is entirely whatever a lerp/slerp says. `poses/imu_densified_pose.h`
(`ImuDensifiedPoseSource`) replaces the *orientation* half of that with a
gyro-integrated path between the same two ARCore poses, pinned to both
endpoints by a linearly distributed closing error, so it can never drag the
trajectory away from VIO or accumulate drift. Orientation, not position, is
where this pays: 1° of orientation error puts a 3 m return 5 cm out of place;
1 mm of position error puts it 1 mm out. Measured in
`tests/test_round9_imu_densify.cpp` against 1.5° of 12 Hz rotational jitter —
below the 30 Hz pose Nyquist, so this is an *attenuation* argument, not an
aliasing one: **wall plane-fit RMS 0.739 cm (plain slerp) → 0.021 cm
(IMU-densified), against an analytic-truth floor of 0.0007 cm — 97.3 % of the
recoverable error closed.** With the jitter removed the win collapses to 1.8x,
which is the control.

**Does it survive imperfect lidar timestamps?** This is the question ROUND 9's
two halves ask of each other, and it is not rhetorical: densification makes the
trajectory *follow* 12 Hz motion instead of smoothing it away, and a path with
more high-frequency content is in principle more sensitive to being sampled at
the wrong instant — a plain slerp is accidentally robust to timing error
precisely because it has already thrown the fast motion away. Measured rather
than argued, with a random per-packet stamp error (constant within a 24-sample
packet, independent between packets — the shape a byte-position reconstruction
actually produces):

| per-packet stamp error | plain slerp | IMU-densified | ratio |
| ---: | ---: | ---: | ---: |
| ±0 ms | 0.739 cm | 0.021 cm | 0.03x |
| ±1 ms | 0.743 cm | 0.059 cm | 0.08x |
| ±4 ms | 0.772 cm | 0.234 cm | 0.30x |
| ±8 ms | 0.840 cm | 0.473 cm | 0.56x |

Densification **never becomes counter-productive**, even with stamps an order
of magnitude worse than A2 §9's sample clock should produce — so it is safe to
leave on by default rather than gated on timestamp quality. It does degrade
monotonically, which is the honest half, and that degradation is the argument
for A2 §9 in one table: the better the stamps, the more the gyro is worth.

**Buffer, never guess.** ARCore delivers ~30 Hz *behind* a 4 kpts/s point
stream, so most points arrive before the pose that brackets them. A point with
no pose yet is PENDING — not dropped, not extrapolated. `drain()` resolves what
the poses allow (stopping at the first still-future point, since the queue is
time-ordered); `flush()` gives up on the remainder at end of stream. The queue
is bounded (`max_pending_points`, default 200k ≈ 50 s of D6) and sheds its
oldest entries with a counter and a once-per-episode warning.

**Tracking loss is flagged and excluded by default** (§3.3). `exclude_flagged`
is that default. Setting it false keeps the points and stamps `flagged_alpha`
(default 96) into the alpha channel, so a renderer greys them and an exporter
can filter them — rather than mixing unmarked garbage into the cloud. The three
flag reasons are counted separately, because they mean different things to a
user: tracking lost ("walk back and rescan"), stale pose ("your pose stream
stuttered"), low confidence ("ARCore was struggling").

### 3.3 Why `PoseInterpolator` exists

`PoseSource::pose_at()` returns a `Status` and a `Pose`. That is the right
minimal A1 contract, but §3.3's "flagged and excluded by default" needs five
outcomes distinguished, and a single `ScanError` collapses exactly the
distinction being asked for:

| gate | meaning | assembler's response |
| --- | --- | --- |
| `kOk` | usable | emit |
| `kNoData` / `kFuture` | pose has not arrived yet | keep pending, retry |
| `kBeforeFirst` | predates the pose stream, or rolled off the ring | drop, count |
| `kStale` | bracketing poses further apart than `max_gap_ns` | flag |
| `kTrackingLost` | either bracketing pose carries `tracking_lost` | flag |
| `kLowConfidence` | below the confidence/quality floor | flag |

`PoseSample::has_pose` is true for the last three: the geometry is available,
it just must not be trusted silently. A10's fusion layer implements the same
interface, which is what lets the assembler consume "ARCore indoors, RTK
outdoors, blended when both" without a code path per source (§3 key rule 3).

### 3.4 Interpolation

Position lerps; orientation SLERPs on the **shortest arc**. The sign fix in
`quat_slerp` is not cosmetic — a quaternion and its negation are the same
rotation and ARCore/Eigen/GTSAM all hand out either sign freely; without it a
nearly-opposed pair interpolates the long way and a pushbroom sweep folds a
wall inside out for one sample interval. There is a dedicated test.

Forward extrapolation past the newest pose is **off by default** and, when
enabled, **holds** the last pose rather than projecting it — projecting a VIO
pose forward is how a stationary rig becomes a rocket.

Every interpolated scalar takes the *pessimistic* of the two bracketing
samples: an interval is only as trustworthy as its worse end.

### 3.5 Time mapping (A4)

`ExternalPoseConfig::timesync` routes incoming stamps through
`TimeSync::to_engine_time(stream, t)`. For `StreamId::kPoseAr` A4 installs a
passthrough estimator — ARCore is already `CLOCK_BOOTTIME`, the engine's own
domain — so this is an identity today. It is wired anyway because it is the
seam that makes a pose stream with its own clock work without touching any
consumer, and because the D6 has no device clock at all, so its point stamps
are arrival stamps in the same domain by construction.

### 3.6 Replay == capture (§3 key rule 2)

The assembler reads no clock and samples no wall time; it is a pure function of
(profile points, pose stream, extrinsic). A capture in which poses and points
interleave and profiles are torn into ragged 13-point chunks produces a cloud
**bit for bit identical** to an offline pass over a replayed `.lscan`, where
every pose is available before the first point.
`pushbroom/assembles_identically_live_and_offline` asserts that on 3,880 points
with `==` on the floats, not a tolerance.

### 3.7 Measured

`pushbroom/world_points_match_the_analytic_ground_truth`: 4,000 returns over
10 revolutions against a constant-velocity, constant-yaw-rate trajectory with
the S6 case-(a) D6 mount, with ground truth computed by an independent
quaternion-sandwich path (never by a 4x4 product).

**Worst world-point error: 0.24 µm.** That is float32 storage of a ~4 m
coordinate and SLERP round-off; the tolerance is 10 µm, so a wrong frame, a
wrong composition order or a wrong timestamp cannot hide.

The trajectory is deliberately exactly representable by the interpolator (a
straight line for lerp, a constant angular velocity for SLERP). Adding a
1.3 Hz vertical bob makes the lerp error alone **185 µm across a 33 ms ARCore
interval** — a real and useful number for A11's budget, and exactly the kind of
term that would mask the bugs this test is for, which is why it is stated here
rather than left in the tolerance.

### 3.8 Threading

The assembler is **not** internally synchronized and owns no thread — same
contract as `D6Driver` (DESIGN §2: one `Driver` instance is pushed from one
thread at a time). It runs on whichever thread decodes D6 packets: the app's
serial reader live, the replay thread offline. `ExternalPoseSource` **is**
thread-safe, so the app pushes ARCore poses from its own thread concurrently.
No row is added to DESIGN §2's thread table: A8 introduces no thread.

---

## 4. The mount-calibration solver

### 4.1 Residual

One scalar per lidar return (Zhang & Pless 2004 / Unnikrishnan & Hebert 2005):

```
r_ik = ( n_i · (R·p_ik + t) − d_i ) / σ_i
```

`(n_i, d_i)` is the target plane **as the camera measured it**, in the camera
frame; `p_ik` are the lidar returns segmented onto the board. The camera side
is a plane rather than a pose deliberately: it is all the residual needs, and
it keeps checkerboard detection and PnP on the app side where the image already
is.

`σ_i` whitens the residual, so a capture mixing sensors or ranges is weighted
correctly — and it is the number S6's blocking action A is about: at 10 mm the
D6 behaves like a Mid-360, at 30 mm it does not.

**There is only one residual family.** S6 §5: point-to-point corner
correspondence is unavailable to a 2-D scanner *in principle*, because its scan
plane samples a single 1-D slice, so the "corner" it finds is the bend in that
slice and that bend slides along the corner line as the rig moves. §3.3's
"guided corner/doorframe capture" is not implementable for the D6, and the spec
has since been amended to planar-checkerboard-for-both-sensors.

### 4.2 Two stages

Stage 1 is plain L2 from the bracket's CAD nominal; stage 2 is a soft-L1 IRLS
refinement (`w = 1/√(1+(r/f)²)`, `f = 2.5` whitened, matching S6's `f_scale`)
from the now-close L2 answer.

S6 §2.3 recorded that a robust kernel in stage 1 **stalled** its solve — from
4° / 25 mm out every residual looks like an outlier, so soft-L1 down-weights
the whole problem.

**Measured here, this solver does not reproduce that stall, and the reason is
structural rather than lucky.** Its damping is Marquardt's `λ·diag(H)`, not
`λ·I`, which makes the step invariant to a uniform rescaling of `H` and `g` —
and a robust kernel applied when every residual is large is, to first order,
exactly such a rescale. S6's prototype used scipy's trust-region-reflective,
which bounds the step in a scaled variable instead, so the same down-weighting
does shrink the step. Measured over 21 sessions from a 4° / 25 mm start, and
also from a 15° / 100 mm start and on the weakly-conditioned D6:

| stage-1 kernel | clean Mid-360 N=8 |
| --- | ---: |
| plain L2 → robust (shipped) | 5.110 px @ 3 m |
| robust → robust | 5.110 px @ 3 m |

**The order is kept regardless.** It costs nothing, it is what the study
mandates, and it is the only order that stays correct if the damping strategy
is ever changed. `MountCalibConfig::robust_first_stage` exists so the test can
keep *measuring* that claim rather than citing it — if it ever starts failing,
the damping changed and the S6 stall is back.

What stage 2 is actually for, measured on 12 poses of which every 6th is
mis-segmented onto the wall behind the board:

| | clean | with outliers |
| --- | ---: | ---: |
| L2 only | 4.89 px | 389.5 px |
| L2 → robust | 5.11 px | **130.8 px** |

A robust kernel costs a little statistical efficiency on purely Gaussian data —
that is the premium it pays for the insurance — and returns 3x on contaminated
data.

### 4.3 The quality gate

**Split-half agreement, never the covariance** (S6 action G). Solve twice on
two disjoint halves of the captured poses (even/odd indices, both seeded from
the same CAD nominal, because in the field that is the only initial guess there
is) and report how far apart the two answers place a point at 3 m, in pixels.
Bands are WIZARD.md screen 4: **≤ 12 px Good, ≤ 30 px Usable, > 30 px Reject.**

`sigma_rot_deg` / `sigma_trans_mm` / `condition_number` are computed and
reported as diagnostics, and the header says in as many words that they must
never gate. The reason, re-measured over 21 D6 sessions that differ only in
their noise realisation:

| | median | coefficient of variation |
| --- | ---: | ---: |
| true rotation error | 0.811° | **1.83** |
| reported σ_rot | 0.476° | **0.13** |

The covariance is essentially constant while the thing it claims to describe
varies by a factor of nearly two either way. It cannot rank sessions, which is
the entire job of a gate. (S6 measured a rank correlation of ≈ 0.1 and reached
the same conclusion.)

The split-half number does rank them, and — as WIZARD.md predicted — it does
not merely scale on a bad capture, it explodes:

| capture | split-half gate | true error @ 3 m | verdict |
| --- | ---: | ---: | --- |
| clean, Mid-360 A1, 12 poses | **7.1 px** | 3.7 px | Good **20 / 21** |
| board not clear of the wall (1 pose in 3 mis-segmented) | **909 px** | 497.7 px | Reject **21 / 21** |

On a good capture it reads ~1.9x the true error (S6 measured ~2.5x); on a bad
one it is 2.5 orders of magnitude out. That asymmetry is what a safety gate
wants: a reliable *detector* of a bad capture, not a precision estimator of a
good one.

Degeneracy is handled separately from the gate: fewer than 3 observations is
**refused** (`kInvalidArgument` — six unknowns against 2–3 constraints per pose
is undetermined, not merely ill-conditioned); fewer than
`min_observations` (5, the Zhang–Pless floor) or a normal matrix with condition
number > 1e12 is solved but marked `degenerate` and forced to `kReject`.

### 4.4 Input validation

A rigid-transform check (`se3::mat4_is_rigid`) guards every `double[16]`
crossing an API boundary, on both the assembler and the solver. The two ways
this fails in the field are a **column-major matrix handed across JNI** and an
uninitialised buffer; both otherwise produce a plausible-looking but mirrored
or sheared cloud that nobody notices until export. Non-unit plane normals,
non-positive σ, non-finite points and point arrays whose length is not a
multiple of 3 are rejected the same way.

---

## 5. Measured accuracy

`tests/test_mount_calib.cpp` contains a C++ port of the S6 simulation — the
same camera (4032x3024, fx = 2912), the same two ground-truth mounts including
their bracket misalignment, the same R3 low-discrepancy azimuth/elevation/roll
schedule, the same board sizes, the same "2-D sees a line / 3-D sees a patch"
observation models, the same 4° / 25 mm CAD start. Medians over 21 sessions,
regenerated on every test run.

Two departures from S6, both deliberate: randomness is a fixed-seed xorshift64
+ Box-Muller (as `test_timesync.cpp` does — `<random>`'s distributions are not
specified bit-for-bit and the five CI legs would disagree), and the camera side
is modelled as "checkerboard-PnP plane + noise" (0.02° normal, 0.1 mm offset)
rather than by re-running a PnP solve. Those camera numbers have to be that
small: S6's own noise sweep is linear down to 5 mm with no floor, so the camera
term must sit well below the lidar term — at 0.5 mm of offset error the sweep
visibly floors out at the low end and stops matching.

### 5.1 Mid-360 — pose count (S6 T1)

A1 checkerboard, 20 mm range noise.

| N | rot | trans | px @ 3 m | p90 | S6 px @ 3 m |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 3 | 0.405° | 5.8 mm | 10.3 | 16.7 | 8.2 |
| 5 | 0.257° | 4.7 mm | 7.0 | 10.3 | 6.2 |
| **8** | **0.200°** | **3.2 mm** | **5.1** | 8.8 | **4.2** |
| 12 | 0.172° | 2.4 mm | 3.7 | 6.7 | 4.3 |

**The task's target — 8+ poses with roll variation and Mid-360-class noise
recovering the extrinsic to ~0.2° / 3 mm — is met exactly: 0.200° / 3.2 mm at
N = 8**, which is WIZARD.md §0's requirement (better than 0.16° / 3 mm → ~4 px)
to within the difference between this camera model and S6's. The curve
flattens past 8, as S6 found; asking a user for 12 buys little.

### 5.2 COIN-D6 — what is achievable vs pose count (S6 T4)

A1 checkerboard at the D6's **full specified 30 mm** range noise. This is the
table the task asks to be documented, and it is the evidence for the bench
procedure.

| N | rot | trans | px @ 3 m | p90 | split-half gate | S6 px @ 3 m | in budget (20.2 px)? |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 12 (handheld ceiling) | 0.811° | 17.0 mm | **23.8** | 40.9 | 254 px → Reject | 37.5 | ✗ |
| 20 | 0.413° | 9.7 mm | **10.5** | 21.6 | 35.5 px | 13.6 | ✓ |
| 30 | 0.444° | 10.6 mm | **11.8** | 23.1 | 19.0 px | 13.9 | ✓ |
| **45** (bench) | **0.360°** | **7.5 mm** | **9.5** | 12.9 | 21.0 px | 9.0 | ✓ |
| 45, XL board | 0.261° | 5.4 mm | **6.6** | 16.7 | — | 5.8 | ✓ |

Read this as the study does. **A handheld 12-pose wizard cannot calibrate a
30 mm-noise D6** — it lands outside the colorization budget before any other
error term is added, and its p90 is twice that again. **A tripod bench
procedure can**: 45 poses against an A1 board reach 9.5 px, and against an XL
board 6.6 px, which is better than the Mid-360's handheld result. The board
size only pays off once the pose count is high, exactly as S6 reported.

Note the split-half gate column: it is doing its job. At N = 12 it reads 254 px
and refuses the capture; from N = 20 it drops into the Usable band and at N = 30
approaches Good. A user cannot ship a D6 calibration this solver does not
believe in.

**The D6 answer is also only valid near its capture distance.** Poses here are
captured at 1.1–1.9 m, and S6 §6.2 shows a weakly-conditioned solve trades
rotation against translation so the two partially cancel near that distance and
stop cancelling away from it. Capture at a distance representative of use; do
not extrapolate. `MountCalibResult::gate_range_m` records what the gate was
measured at.

### 5.3 COIN-D6 — range noise (S6 T3 / blocking action A)

A1 board, 12 poses.

| σ_range | rot | trans | px @ 3 m | S6 px @ 3 m |
| ---: | ---: | ---: | ---: | ---: |
| 5 mm | 0.136° | 2.6 mm | **3.6** | 2.9 |
| 10 mm | 0.252° | 4.3 mm | **6.5** | 5.5 |
| 20 mm | 0.499° | 8.8 mm | **13.2** | 14.0 |
| 30 mm (datasheet) | 0.811° | 17.0 mm | **23.8** | 25.9 |

Super-linear: **6x the noise costs 6.6x the error**, because the 2-D solve is
weakly conditioned to begin with so noise amplification compounds. This is
S6's blocking dependency restated with the shipped solver: **S1 must report the
D6's measured 1σ range noise at 1–2 m** (static target, ≥1000 returns, σ not
the datasheet bound). At 10 mm the D6 is inside budget with a 12-pose handheld
wizard; at 30 mm it needs the bench.

### 5.4 Roll is not optional (S6 T2)

D6, A1 board, 12 poses, 30 mm noise.

| pose style | rot | trans | px @ 3 m | p90 |
| --- | ---: | ---: | ---: | ---: |
| varied azimuth + elevation + **roll** | 0.811° | 17.0 mm | **23.8** | 40.9 |
| sideways steps only, phone upright | 3.587° | 104.1 mm | **103.5** | 357.9 |

**4.3x worse** without roll (S6 measured 2.3x; the direction and the magnitude
of the effect are the same, and the p90 blowup is dramatic in both). For the
2-D sensor, rolling the phone is the *only* thing that changes the direction of
its scan line across the target. WIZARD.md screen 2's roll chip and pose
diversity wheel are load-bearing UI, not decoration.

### 5.5 Sanity

* Zero noise, 8 poses: recovers the extrinsic to **0.000° / 6e-13 mm**, RMS
  residual < 1e-9 m, split-half < 0.01 px (S6 §2.4's first check).
* The reprojection metric: a pure 0.1° rotation error costs 5.69 / 5.52 / 5.47
  px at 1 / 3 / 8 m — **range-independent**, `fx·θ` = 5.08 px plus the
  off-axis RMS; a pure 10 mm translation error costs 31.6 px at 1 m and 10.5 px
  at 3 m, **decaying as `fx·Δ/r`**. Both are S6 §6.1's structural claims,
  reproduced as assertions.

---

## 6. What A11 and B7 should do with this

* **Wrap, do not reimplement.** `color/colorize.h`'s `ExtrinsicsSolver` seam
  takes `(Keyframe, Span<const PointVertex>)`. A11 should turn the keyframe's
  checkerboard detection into `(n, d)` and delegate to
  `MountCalibrationSolver`; `split_half_agreement_px()` is
  `MountCalibResult::split_half_px`.
* **Gate on `split_half_px`, show physical units.** WIZARD.md: "±5 mm at 3 m —
  Good", never pixels. `px · range / fx` is the conversion.
* **The constant clock offset is not this module's.** S6 §7.1 item 1 (the
  8-second sweep) removes the *systematic* half of the sync error; A4's
  `jitter_ns` bounds what is left. Both must be applied — see A4 §3.
* **Persist per bracket, not per project.** WIZARD.md §3: key the stored
  extrinsic on (phone model, bracket ID, lidar serial), and store the gate
  value, the pose count, the target size and the *capture distance* beside it
  (§5.2's last paragraph is why the distance matters).
* **For the D6, the in-app wizard is verify-and-correct only.** Seed from the
  stored bench calibration; do not let an end user re-solve from 12 handheld
  poses.

---

## 7. Seams this task did NOT take — orchestrator action required

Each of these is a small change in a file A8 does not own.

### 7.1 C ABI (`capi/scanengine_c.h` / `.cpp`) — B7 needs these for ARCore

The C++ API is complete and callable today; only the flat C mirror is missing.
Following DESIGN §4 (field-by-field conversion, out-params last, every fallible
call returns `scan_error_t`) and DESIGN §6 item 9 (**bump `SCAN_ABI_VERSION`
and `kEngineAbiVersion` together**):

```c
/* --- poses -------------------------------------------------------------- */
typedef struct {
  int64_t  t_mono_ns;
  double   position[3];
  double   orientation[4];   /* x, y, z, w */
  float    position_sigma_m;
  float    orientation_sigma_deg;
  uint8_t  source;           /* scan_stream_id */
  uint8_t  quality;          /* SCAN_POSE_QUALITY_* */
  uint8_t  tracking_lost;
} scan_pose;

/* confidence < 0 -> derive it from quality/tracking_lost (pose_confidence()) */
scan_error_t scan_engine_push_pose(scan_engine*, const scan_pose*, float confidence);
scan_error_t scan_engine_pose_at(scan_engine*, int64_t t_mono_ns,
                                 scan_pose* out, uint8_t* out_gate);

/* --- pushbroom ---------------------------------------------------------- */
scan_error_t scan_engine_set_mount_extrinsics(scan_engine*, const double phone_from_lidar[16]);
scan_error_t scan_engine_pushbroom_enable(scan_engine*, int on);
scan_error_t scan_engine_pushbroom_flush(scan_engine*);
scan_error_t scan_engine_pushbroom_stats(scan_engine*, scan_pushbroom_stats* out);

/* --- mount calibration -------------------------------------------------- */
typedef struct scan_mount_calib scan_mount_calib;
scan_error_t scan_mount_calib_create(scan_mount_calib**);
void         scan_mount_calib_destroy(scan_mount_calib*);
scan_error_t scan_mount_calib_add_observation(scan_mount_calib*, const double normal[3],
                                              double d, const scan_point_vertex* pts,
                                              uint32_t n, double sigma_m);
scan_error_t scan_mount_calib_solve(scan_mount_calib*, const double cad[16],
                                    scan_mount_calib_result* out);
```

New enum mirrors, all with the values already fixed in the C++ headers:
`SCAN_POSE_QUALITY_*` (0..3), `SCAN_POSE_GATE_*` (0..6, `pose_interpolator.h`),
`SCAN_CALIB_GATE_*` (0..3, `mount_calibration.h`).
`scan_point_vertex` already exists and is 16-byte-identical to `PointVertex`.
**No new `ScanError` value is needed** — everything A8 returns is
`kInvalidArgument`, `kNotFound`, `kAgain` or `kCapacityExceeded`, which all
already exist and already have C mirrors.

### 7.2 `Engine` wiring (`src/core/engine.cpp`, `include/scanengine/core/engine.h`)

The assembler is standalone by design (DESIGN §6 item 3: reach the engine only
through `DriverContext`/`EventBus`/`PageStore`/`TimeSync`, no back-pointer).
To make the C ABI above possible the Engine needs to own one
`ExternalPoseSource` and one `D6PushbroomAssembler`, and:

1. construct the pose source with `timesync = &impl_->timesync` and
   `stream = StreamId::kPoseAr`;
2. call `assembler.set_pose_source(&pose_source)`;
3. have `D6Driver` route decoded points to the assembler when it is enabled —
   `d6_driver.h`'s own header already reserves this ("A8 (pushbroom assembly
   replaces the point transform)"). The clean shape is a
   `D6Config::profile_sink` callback receiving `(angle_deg, range_m,
   intensity, high_refl, t_engine_ns)`, which is a strictly additive change to
   an A2-owned file;
4. call `assembler.flush()` from `stop_session()`;
5. publish `EventType::kPoseUpdate` on each accepted pose — the payload
   (`PoseUpdatePayload`) already exists and is currently never published by
   anyone.

Until (3) lands, an app can drive the assembler directly from C++; the desktop
Qt path can do that today.

### 7.3 Two one-liners in A1-owned files

* `tests/test_headers.cpp` should add `poses/se3.h`,
  `poses/pose_interpolator.h`, `poses/external_pose_source.h`,
  `slam/pushbroom/pushbroom_assembler.h` and
  `slam/pushbroom/mount_calibration.h` to its include list. A8's own two test
  files each include their headers first and alone, so the property is
  currently checked — just not in the file that documents it.
* `DESIGN.md` §1's module table should mark `poses/` as implemented (A8) and
  add the `slam/pushbroom/` row. §2's thread table needs **no** change: A8
  introduces no thread.

### 7.4 `.lscan` (A5) and the manifest

WIZARD.md §3 asks for the mount calibration to live in `manifest.json` (§3.11
already reserves "mount calib"): the extrinsic, its split-half gate value, the
estimated time offset, target size, pose count, **capture distance**, sensor
serial, bracket ID, timestamp and app version. A5/A15 own that file. A pose
stream chunk type for `StreamId::kPoseAr` would also let a replayed `.lscan`
re-assemble without the app — the assembler is already replay-clean (§3.6), it
just needs the poses to come back off disk.

> **ROUND 8 (0.5.0) — done, and it was load-bearing.** The paragraph above
> reads like a convenience; it was not. A COIN-D6 is a 2D lidar and the third
> dimension of its scan is *entirely* the trajectory, so a `.lscan` without
> poses is a container from which the 3D result can never be rebuilt by
> anyone. The owner's report — *"when i check the recording, it still show a
> 2D scan"* — was exactly this. Landed:
>
> * `Engine::push_pose()` now writes `ChunkType::kPoseAr` chunks (68-byte
>   `lscan::PoseChunkRecord`, ~2 KB/s at ARCore's 30 Hz);
> * `lscan::ReplaySource` replays them through `push_pose()`
>   (`ReplayConfig::replay_poses`, default on);
> * the resolved cloud is cached as `kPointsXyzRgba` in `streams/map.bin`
>   while the pushbroom is running, so Review opens without a re-resolve;
> * `FileRecordWriter::set_mount_calibration()` puts `phone_from_lidar` in the
>   manifest, which is what makes the container self-contained;
> * `post::D6ResolvePipeline` (`slam/post/d6_resolve.h`) is the offline
>   re-assembly this paragraph describes, driving the real `D6Driver`,
>   `ExternalPoseSource` and `D6PushbroomAssembler` rather than a
>   reimplementation.
>
> Proof: `tests/test_round8_d6_reopen.cpp` — a recorded walk, reopened cold
> off disk, comes back at 0.052 cm plane-fit RMS and **bit-identical** to the
> live pass, with a control showing the same bytes minus the poses produce
> zero points.

---

## 8. Tests

`tests/test_pushbroom.cpp` — 16 cases

| case | asserts |
| --- | --- |
| `se3/quaternion_matrix_roundtrip` | quat↔matrix over 20 axis/angle pairs incl. near-π, agreeing with an independent quaternion-sandwich rotation to 1e-12 |
| `se3/exp_log_roundtrip_including_near_pi` | exp/log stable at 0, 1e-10 and π−1e-7 |
| `se3/slerp_takes_the_short_way_round` | 170° apart with a negated quaternion still interpolates through 85°, not 275° |
| `se3/rigid_inverse_and_rigidity_check` | inverse round-trips to 1e-12; a scaled matrix and a reflection (det < 0) are both rejected |
| `poses/interpolates_a_constant_rate_trajectory_exactly` | 200 off-knot samples, position to 1e-9 m, orientation to 1e-7 |
| `poses/edge_cases_before_first_at_knot_and_in_the_future` | all five gates + the narrow `pose_at()` kNotFound/kAgain contract |
| `poses/gates_stale_gaps_low_confidence_and_tracking_loss` | a 400 ms hole, a tracking-loss interval, a kPoor recovery, and an explicit confidence overriding the derived one |
| `poses/rejects_bad_input_and_rolls_the_ring` | NaN, zero-norm quaternion, out-of-order stamp; ring overwrite makes rolled-off times `kBeforeFirst`, not `kFuture` |
| `poses/extrapolation_holds_the_last_pose_and_is_off_by_default` | held, never projected; window enforced |
| `pushbroom/world_points_match_the_analytic_ground_truth` | 4,000 points, worst error **0.24 µm** |
| `pushbroom/angle_wraparound_is_continuous` | 358°…1° across the seam, and 359.9° vs 0.1° are 0.2° apart at 3 m with nothing added by the wrap |
| `pushbroom/cartesian_seam_overload_agrees_with_the_polar_path` | the A1 `PointVertex` seam round-trips through polar to within 0.2 mm, tint preserved |
| `pushbroom/tracking_loss_points_are_excluded_by_default_and_flagged_when_kept` | ~150 of 900 points flagged over a 400 ms loss; excluded by default, alpha-marked when kept |
| `pushbroom/points_wait_for_their_pose_instead_of_being_dropped` | 200 points pend, then resolve when poses arrive; a pre-session point is dropped, not held |
| `pushbroom/assembles_identically_live_and_offline` | 3,880 points, `==` on the floats |
| `pushbroom/rejects_a_non_rigid_extrinsic_and_bounds_the_pending_queue` | the column-major trap, the range window, the queue bound |

`tests/test_mount_calib.cpp` — 12 cases, covering §4's mechanics
(zero-noise recovery, malformed input, undetermined captures, stage order) and
regenerating every table in §5 on each run. The whole A8 suite is 0.65 s, so
none of it is behind a `skip()` label.
