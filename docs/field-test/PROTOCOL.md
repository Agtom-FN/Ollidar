# LidarScan — Field Validation Protocol

**Workstream E3** (Tech Spec §4). Companion to `docs/bench/TEST_CHECKLIST.md`
(spike S4): that checklist proves wiring/enumeration/protocol-liveness
("packets flow at expected rates") on a bench, with no LidarScan app code
involved. This protocol is the next step up — real captures through the real
app, on real hardware, in real spaces — and it exists for the day the
hardware kit (`tools/remote-capture/lidarscan-capture-kit.zip`) comes back
from the field, or a developer is at the bench with both sensors attached.

Every accuracy **target** below is cited from an existing engine doc's
*synthetic* (or, for the one real Mid-360 capture the repo has, real-but-no-
ground-truth) measurement — `engine/docs/A6-lio.md`, `A7-post.md`,
`A8-pushbroom.md`, `A10-gnss.md`, `A13-merge.md` — plus the S6 calibration
feasibility spike (`spikes/s6-calibration/REPORT.md`). None of those numbers
are field measurements; that is exactly the gap this protocol closes. Every
run below produces the field-measured column of
`docs/field-test/ACCURACY_REPORT_TEMPLATE.md`.

**Prerequisites for every run below:**

- `docs/bench/TEST_CHECKLIST.md` passed on today's hardware/host combination
  (a wiring fault dressed up as an accuracy problem wastes a field session).
- The app build under test recorded (git commit or release build number) —
  the report template has a field for it.
- A charged phone/tripod rig per `docs/bench/BENCH_SETUP.md` §3, and the D6
  zero-angle reference mark re-photographed if the mount changed since the
  last run (`TEST_CHECKLIST.md` §C, last bullet).
- Record-always is not optional (Tech Spec §3 key rule 2) — every run below
  produces a `.lscan` capable of being re-processed later with a better
  pipeline. Losing the raw capture loses the ability to ever re-measure a run.

---

## 0. Mount calibration (run before Runs 1–2, and after any mount change)

Per S6's verdict (`spikes/s6-calibration/REPORT.md` §1, §5) and A8's own
measured accuracy (`engine/docs/A8-pushbroom.md` §5): **a planar checkerboard
for both sensors**, not the spec's original "corner/doorframe" wording — S6
found a bare wall or corner capture 3–6x over the colorization budget for
either sensor, and unusable in principle for the D6 (§5).

### 0.1 Mid-360 — handheld wizard

- A1-size checkerboard (per `spikes/s6-calibration/WIZARD.md`), **8 poses
  minimum**, varied azimuth **and elevation and roll** — S6 §3.1 measured
  roll-varied poses cut reprojection error roughly in half at the same pose
  count (T2: 4.5 px azimuth/elevation-only vs 8.7 px... the wizard UI must
  visibly prompt for roll, not just position).
- **Target:** ≤ 0.20° / ≤ 3.2 mm extrinsic (A8 §5.1's N=8 synthetic number,
  which itself already meets WIZARD.md §0's ~0.2°/3 mm requirement), and the
  wizard's own `split_half_px` gate should read at or under the ~5 mm-at-3 m
  bar it displays.
- **Record:** the wizard's reported rotation/translation, `split_half_px`,
  and `condition_number`; keep the raw checkerboard-pose capture as a future
  regression fixture (§4 below).

### 0.2 COIN-D6 — bench procedure, not handheld

S6 §5 / A8 §5.2 found the D6's wizard **NO-GO for a handheld capture at the
device's full ±30 mm datasheet noise** (12-pose handheld: 0.811° / 17.0 mm,
254 px reprojection at 3 m — outside the colorization budget before anything
else is added). A bench procedure is required instead:

- **45 poses**, varied azimuth/elevation/roll, on a bench rig (not handheld) —
  A8 §5.2 measured 0.360° / 7.5 mm / 9.0 px at N = 45, inside budget. An XL
  checkerboard at the same pose count does better still (5.8 px).
- **This step ALSO closes the S1/S6 open question** (`A8-pushbroom.md`
  §5.3, `S6-calibration/REPORT.md` §4's "blocking dependency on S1"): measure
  the D6's **real 1σ range noise** at 1–2 m against a static, flat target,
  ≥ 1000 returns, not the ±30 mm datasheet bound. Log the raw range samples.
  The D6's calibration accuracy is highly sensitive to this number — A8 §5.3's
  table shows 10 mm noise behaves like a Mid-360 (6.5 px) while 30 mm needs
  the bench (23.8 px handheld-equivalent) — so this single measurement decides
  whether a future handheld D6 wizard is worth attempting at all.
- **Target:** ≤ 0.36° / ≤ 7.5 mm extrinsic at whatever noise σ is actually
  measured (interpolate against A8 §5.3's table using the measured σ, not the
  datasheet one).

---

## 1. Indoor loop (Mid-360) — loop-closure ATE

**What this closes:** `engine/docs/A7-post.md` §10 item 1 — *"A real loop on
real data... the only real capture available is 6 s of a vehicle driving in a
straight line, which correctly produces zero loops... A capture that walks a
building and returns to its start is the single most valuable thing that
could be added."* Every loop-closure number in A7 §6.1 (9.4x ATE
improvement, 0.185 m RMS / 0.015 m final drift on a synthetic 41 m loop) is
synthetic. This run is the first time the pipeline's Scan Context detector,
ICP verification gate and pose graph see a real revisit.

### Procedure

1. Walk a closed loop through an indoor space, returning to within arm's
   reach of the start point. Target loop length **30–60 m** (comparable to
   A7 §6.1's 41 m synthetic loop) through at least two distinguishable rooms
   or a room-plus-corridor — a single feature-poor corridor risks the
   degeneracy A6 §10 item 6 and A7 §9 both flag as untested.
2. Walk at a natural pace (~1 m/s); avoid the Mid-360 on a rigid pole with no
   sway if possible — some real IMU excitation is needed for the static-init
   window (A6 §3.1) but the walk itself can be as steady as normal.
3. Run **Live** capture through to a completed `.lscan`, then run **Post**
   (Finish scan) — this is what exercises the loop-closure stages
   (`PostStage::kLoopDetection` / `kOptimization`), not the live map.
4. Record the post-run's reported stats (Review screen / `engine_cli
   --post`'s stdout — `PostStats`): `scans`, `keyframes`,
   `loop_candidates`/`loops_accepted`, `graph.final_chi2`,
   `trajectory_length_m`, `final_points`.

### Targets (report both; there is no ground truth for either walk shape)

- **Loop detected at all**: `loops_accepted >= 1` for a loop this size is the
  first pass/fail bar — A7 §6.3's only real capture correctly reports zero
  loops because it never revisits anything; this run is designed to revisit.
- **Qualitative closure**: view the post-processed cloud in the desktop
  review workspace and confirm walls/features that should coincide (the start
  and end of the loop) visibly do, not offset by a visible drift smear. A7's
  own synthetic claim (§6.1) is a **9.4x ATE improvement**; without survey
  control there is no field ATE number, so this run's target is "the fold is
  gone," recorded as a screenshot for the report.
- If a total station, tape-measured control points, or a second (RTK-tagged)
  session of the same building is available, re-run §5.1's georeferenced
  merge path against it and get a real ATE — see Run 4.

---

## 2. Corridor pushbroom (D6 + ARCore)

**What this closes:** A8's pushbroom assembler and mount-calibration solver
have never run against a real D6 + real ARCore trajectory (`A8-pushbroom.md`
§7's hardware-only list). Do this run **after** §0.2's bench calibration.

### Procedure

1. Walk a straight-to-gently-curved corridor, ARCore tracking active
   throughout (watch the confidence UX per Tech Spec §Risks
   "ARCore tracking loss"; re-run if tracking drops mid-corridor).
2. Capture at a steady walking pace; the D6 sweeps a vertical profile at
   10 Hz per `docs/bench/BENCH_SETUP.md`/spec §2.1, so a steady pace is what
   turns that into an even along-corridor sample spacing.
3. Finish scan; export a PLY/LAS and view the extruded profile.

### Targets

- **Wall flatness / verticality**: pick a straight run of corridor wall in
  the exported cloud and measure its planarity (e.g. in CloudCompare — fit a
  plane, report RMS). No synthetic target exists for this in the engine docs
  (A8's own accuracy numbers are all *extrinsic* accuracy, not assembled-cloud
  flatness), so this run establishes the field baseline the accuracy report
  should carry forward as its own row.
- **Known-dimension check**: measure one real, tape-measured dimension in the
  corridor (a doorway width, a marked distance along the floor) and compare
  against the same measurement taken in the exported cloud with the desktop
  measure tool (C3, `MeasureDock`). Report absolute error in mm.
- **Extrinsic held from §0.2**: confirm the mount was not disturbed between
  calibration and this run (the zero-angle reference photo from
  `TEST_CHECKLIST.md`'s pre-run check is the same evidence).

---

## 3. Outdoor RTK walk vs. known points

**What this closes:** every number in `engine/docs/A10-gnss.md` §5 is
synthetic (a simulated 40x25 m loop with per-fix-quality noise injected).
`GeorefFusion`'s coupling to a real `GnssSource` end to end has also never
run on one machine (A10 §9.3 item 2 is the wiring seam; the tests construct
`GeorefSolution` directly). This run is the first real-hardware pass A10 §9.8
asks for, against RTK2go first (protocol plumbing) then a production caster
(HK SatRef, `spikes/s5-rtk-sim/PUBLIC_CASTERS.md` — re-verify hostnames before
use).

### Procedure

1. Survey (tape measure, or a second independent GNSS fix, or published
   coordinates) **4 known points** bounding a walkable outdoor area —
   mirrors A10 §5's "worst of the four loop corners" metric so the field
   number is comparable to the synthetic one.
2. Pair the RTK rover over Bluetooth, configure NTRIP per B9's UI, and
   capture a walking loop touching or passing near all 4 known points, with
   the fix-quality strip visible throughout (log which segments were Fixed
   vs. Float vs. Single/no-fix — Tech Spec's capture-gating logic may refuse
   Single-fix segments depending on session settings; note if capture was
   gated at all).
3. Georeference and export (LAS 1.4, real EPSG/CRS — confirm it **opens
   correctly in CloudCompare or QGIS**, the explicit M3 exit criterion).

### Targets — from A10 §5's fix-quality table, using whatever fix mix the
### walk actually achieved

| Fix mix achieved | Reported CEP95 target | Worst-corner error target |
| --- | --- | --- |
| All RTK Fixed | ≤ 0.05 m | ≤ 5 mm (synthetic: 2.0 mm) |
| Mostly Fixed, some Float | ≤ 0.05 m | ≤ 5 cm |
| All RTK Float | ≤ 0.74 m | ≤ 30 mm |
| Float/Single mix | ≤ 0.77 m | ≤ 4 cm |

Measure actual corner error by comparing the exported cloud's coordinates at
each known point (desktop measure tool, in the export CRS) against the
surveyed coordinate. **Both columns should be monotone in fix quality** (A10
§5's own assertion, restated as a field pass/fail bar) — if a Float-heavy
walk reports a *better* CEP95 than a Fixed-heavy one, something upstream
(antenna lever arm, epoch assembly, GST sigma parsing) is wrong, not just
noisy.

Also record: cold-start/TTFF (A10 §9.8 — hardware-only, never measured),
correction age at both the engine (`Rtcm3Framer::age_s`) and the rover
(`GnssFix::correction_age_s`, GGA field 13 — A10 §3 notes these are *not* the
same number and a report should say which), and any fix flapping.

---

## 4. Two-session merge validation

**What this closes:** `engine/docs/A13-merge.md` §11 — every merge number is
synthetic, and *"no two real sessions of the same place exist in the
repo... the single most valuable fixture"* is exactly what this run
produces.

### Procedure

1. Capture the **same space twice**, as two separate sessions — ideally the
   Run 1 indoor loop location, walked again on a different day/heading, OR a
   georeferenced pair from Run 3's outdoor area (RTK-Fixed on at least one
   session, so the georeferenced auto-align path — §3 of A13 — is exercised,
   not just the ICP/yaw-search fallback).
2. Open the desktop merge workbench (C6): if both sessions are
   georeferenced, use auto-align; otherwise use the 3-point manual pick
   (never the yaw-search fallback as a final answer — A13 §4 measured it can
   be confidently wrong at partial overlap, 9.9 m off with a 0.96 overlap
   score, in an extruded/repetitive building).
3. Run ICP refine, inspect the residual report and the overlap survey, and
   build the merged cloud.

### Targets — from A13 §8's synthetic headline numbers

- **Georeferenced auto-align** (if both sessions have RTK fixes):
  spec's own bar is **< 5 mm** worst-corner error for the composition itself
  (A13 §3 measured 6.2e-10 mm synthetically — the composition is exact; field
  accuracy is **A10's transform accuracy**, i.e. inherits Run 3's numbers,
  not a new number of A13's own).
- **Manual 3-point pick**: expect **decimetres**, not millimetres — A13 §4
  measured 83.7 mm / 0.80° from realistic 2 cm click noise. This
  initializes the ICP refine; it is not the deliverable residual.
- **ICP refine, at the 0.5 m gate** (A13's own, not A7's 1.0 m loop-closure
  gate — §5's table explains why the two differ): target **RMS residual in
  the 1–3 cm range** for real overlapping surfaces (A13's synthetic number at
  the *true* alignment was 9.6 mm against 5 mm injected range noise; real
  Mid-360 noise, mixed pixels at edges, and any people/dynamic objects
  between the two sessions will likely push this higher — report the number,
  don't gate the run on hitting the synthetic figure exactly).
- **Overlap**: report `overlap_fraction` for the pair; anything below 0.15
  is `low_overlap` (a first-class outcome, not a failure — see the merge
  report's own convention, A13 §7).

---

## 5. Per-run data-capture checklist

Keep everything below for **every** run in this protocol, whether or not the
run "passed" — a run that missed its target is often more valuable as a
regression fixture than one that hit it exactly, and a bad run tells the next
person what to avoid.

- [ ] The complete `.lscan` directory (raw streams, not just the exported
      cloud — record-always exists precisely so a future, better pipeline
      can re-run this exact capture, Tech Spec §3 key rule 2).
- [ ] Every export produced during the run (PLY/LAS/PCD; DXF/PDF for a
      floor-plan run) — cheap to keep, useful for comparing tool behavior
      later.
- [ ] The app's own log output for the session (engine_cli stdout for a
      desktop run driven via CLI, or the app's on-screen job log /
      screenshot for one driven through the UI).
- [ ] `PostStats` / `MergeReport` / `GeorefSolution` printed values — the
      MESSAGE-style numeric dump this protocol's targets are measured
      against — captured as text, not just eyeballed once.
- [ ] Device/host metadata: sensor serial numbers, app build/commit, OS,
      date, operator, and (per `TEST_CHECKLIST.md`'s own convention) whether
      this is a first-time or repeat run of this exact procedure.
- [ ] Any known-dimension or survey-control measurements taken independently
      of the app (tape measure, second GPS unit, published coordinates) —
      without these, "field-measured" in the report template degenerates
      back into "self-consistent," which A6/A7/A10/A13's synthetic sections
      already are.
- [ ] Photos: the mount/rig as set up, the zero-angle reference mark, and
      (for Runs 3–4) the surveyed/known points.

**Promoting a run to a committed golden fixture.** A run whose `.lscan` is
small enough to commit (see `spikes/s2-mid360-sim/FIXTURES.md`'s licence and
size discipline — CC-BY or owner-captured only, target well under 25 MB) and
whose numbers are stable is a strong candidate for
`engine/tests/integration/data/` (see that directory's `README.md`) or a new
`spikes/*/fixtures/` entry, the same way the CC-BY outdoor capture became
A6/A7's real-capture regression test. Flag such a run to the engine
workstream owner rather than committing it directly from here — E3 collects
the evidence, A-workstream owns what becomes a permanent fixture.

---

## 6. Reporting

Fill in `docs/field-test/ACCURACY_REPORT_TEMPLATE.md` after each run (or
batch of runs from one field session). Every row there cites the exact
synthetic-baseline number this protocol cited it from, so the field number
goes in next to a stated point of comparison, not in a vacuum.
