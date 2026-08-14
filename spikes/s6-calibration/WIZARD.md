# Mount-calibration wizard — design

**Spike:** S6 · **Feeds:** A8 (D6 pushbroom + mount solver), A11 (colorization), B7 (ARCore mount-calibration wizard + AR overlay)
**Status:** design proposal, derived from the S6 simulation results in [REPORT.md](REPORT.md)

Every number in this document is traceable to a table in `results/tables.md`.

---

## 0. What the wizard is for

The bracket holds the phone and the lidar rigidly, but the *as-built* transform between the camera and the lidar is unknown to a few degrees and a couple of centimetres. Colorization and the AR overlay both project lidar points through the camera, so that transform is on the critical path for two Phase 1 features.

The wizard's job is to recover **camera ← lidar** (6-DoF) to better than **0.16° and 3 mm**, which is what keeps the extrinsic term at 3.7 px — roughly a fifth of the 20.2 px colorization budget, and the *fourth*-largest term in it. It must do this in under two minutes, with one person, holding a phone.

### The three findings that shape the design

1. **A printed planar target is the only viable target.** Simulated against a bare wall or doorframe plane recovered by ARCore's plane detector, the extrinsic lands at 60–115 px of reprojection error at 3 m — 3–6× over the whole colorization budget on its own. The camera side has to be a checkerboard, because checkerboard PnP is roughly an order of magnitude more accurate in plane orientation than ARCore plane detection.
2. **Target size matters more than pose count.** Going from A2 to A1 halves the error for both sensors at every pose count (Mid-360 at 8 poses: 9.2 px → 4.2 px); going from 5 to 12 poses buys much less. The board's angular extent is what constrains the rotation.
3. **The user must be told to *roll* the phone.** Poses that vary only in position (stepping sideways, phone held upright) roughly double the error for both sensors — Mid-360 4.5 px → 8.7 px, D6 34.6 px → 80.0 px. The D6's scan-line direction across the target changes *only* when the phone rolls.

---

## 1. Sensor-specific verdicts

| | Livox Mid-360 | COIN-D6 |
|---|---|---|
| Target | A1 checkerboard, rigid backing | A1 checkerboard (a larger board does **not** rescue it) |
| Poses | **8** (5 acceptable, 12 no better) | 12 is the practical handheld ceiling — and not enough |
| Expected result | 0.16° / 2.6 mm → **4.2 px at 3 m** | 0.98° / 21 mm → **25.9 px at 3 m** |
| Wizard alone sufficient? | **Yes** | **No — needs a bench step** |

The D6 is limited by physics, not by the solver. A 2D scanner returns a *line* of points on the target rather than a patch (see `plots/p0_scene.png`), which supplies only 2 independent constraints per pose instead of 3, over a chord no longer than the target's diagonal. At its specified range noise the solve is noise-limited well above the colorization budget, and the trial-to-trial spread is wide (p90 = 62 px).

**Consequence for the product:** the D6's extrinsic must not be left to an end-user wizard. Ship a **per-bracket bench calibration** and expose the in-app wizard for the D6 only as a *verification and small-correction* step seeded from the stored value. This matches how the D6 is actually used: a fixed pushbroom bracket, not something re-mounted per session.

**The bench procedure** (REPORT.md §5, table T4) — tripod-mounted rig, XL board (≈1.16 × 0.90 m), **45 poses**, run once per bracket design:

| Poses | A1 board | XL board |
|---:|---:|---:|
| 12 (what a handheld wizard can ask for) | 37.5 px | 26.0 px |
| 20 | 13.6 px | 15.8 px |
| 30 | 13.9 px | 12.5 px |
| **45** | 9.0 px | **5.8 px** |

45 tripod poses bring the D6 to 5.8 px — better than the Mid-360's wizard result — at its *full* specified 30 mm range noise, with no change to the sensor. Note the board size only pays off once the pose count is high; at 12–20 poses A1 and XL are indistinguishable.

> **Contradiction with the spec to resolve.** Spec §3.3 proposes "guided corner/doorframe capture" for the D6 mount wizard. Point-to-point corner correspondence is **not available to a 2D scanner at all**: its scan plane samples a single 1D slice, so the "corner" it finds is the bend in that slice, and that bend slides along the corner line as the rig moves — it is not a repeatable world point. Corner capture is viable for the Mid-360 only (and even there a checkerboard beats it). Recommend amending §3.3 and §3.5 to specify a planar checkerboard for both sensors.

---

## 2. User flow

The flow is identical for both sensors; only the prescribed pose count and the pass thresholds differ. It is a five-screen wizard inside **Device setup → Mount calibration** (Android, ARCore session required).

### Screen 1 — Prepare

- Names the target and offers a **printable PDF** (A1, 8×6 inner corners at 100 mm, with the square size printed on it so the user can confirm the printer did not scale it) plus a "measure your square size" field as an escape hatch.
- One illustration and three rules:
  1. Mount the board on a wall or stand, **at least 0.5 m clear of anything behind it** (the lidar has to segment the board from the background).
  2. Stand back **1.2–2 m**. Matte, evenly lit, no glare.
  3. Do not move the board once you start.
- Blocking preconditions, each with its own fix-it copy: ARCore tracking is `TRACKING`, the lidar is streaming, the bracket is seated, storage is available.

### Screen 2 — Capture, pose by pose

A camera preview with a **live target ghost**: a translucent outline showing where to put the board in frame for the *current* prescribed pose, including the required roll. The prescribed poses are a low-discrepancy sweep of azimuth (−38°…+38°), elevation (−24°…+26°) and **roll (−60°…+60°)** — the same sequence the simulation uses, which is why its numbers apply.

Per-pose live checks, each a chip that goes green:

| Check | Rule | Copy when failing |
|---|---|---|
| Board fully visible | all inner corners detected, ≥40 px from the edge | "Move back a little — the whole board must be in frame" |
| Viewing angle | mean incidence < 62° | "Too side-on — face the board more" |
| Roll matched | within ±15° of the prescribed roll | "Tilt the phone to match the outline" |
| Lidar sees it | ≥ 20 (D6) / ≥ 150 (Mid-360) segmented returns on the board | "The lidar can't find the board — check it stands clear of the wall" |
| Hold still | ARCore linear + angular speed under threshold for the dwell | "Hold still…" (ring progress: 1.5 s D6, 1.0 s Mid-360) |

The shutter is automatic: when all chips are green the pose is captured, with haptic + tick. **The user never presses a button**, which is what keeps the rig still.

Progress is a **pose-diversity wheel**, not a counter: a 3-axis dial showing which regions of the azimuth/elevation/roll space have been covered, with the remaining prescribed poses as ghosts. This makes "you have 8 samples but they are all the same view" visible, which a bare "6 / 8" cannot.

### Screen 3 — Time-offset capture *(new step — see §4)*

A separate 8-second step: **"Sweep the phone smoothly left and right across the board, about one sweep per second."** Both sensors observe the same target through the motion; the solver cross-correlates the target's apparent bearing in the camera against its bearing in the lidar and recovers the **constant** camera↔lidar clock offset.

This is the highest-value 8 seconds in the wizard. Time-sync error is the dominant term in the colorization budget, and a constant offset is exactly the part of it that is cheap to remove — leaving only the random jitter that the error budget is written against.

### Screen 4 — Solve + quality gate

Solve runs on-device (< 1 s). Then the gate.

**The gate metric: split-half agreement.** Solve the extrinsic twice, on two disjoint halves of the captured poses, and report how far apart the two answers place a point at 3 m, in pixels. This is the metric to use because it *observes the actual capture* — it sees the real noise realisation and the real pose geometry. The solver's own linearised covariance does **not** work as a gate: with a fixed prescribed pose set it is nearly constant from session to session and cannot tell a good capture from a bad one (measured rank correlation with true error ≈ 0.1).

Shown to the user in physical terms, never in pixels:

> **Mount alignment: ±5 mm at 3 m — Good**
> Colours will land within about a fingernail's width at room distance.

| Band | Split-half gate (px @ 3 m) | Implied true error | Shown as | Action |
|---|---:|---:|---|---|
| Good | ≤ 12 | ≲ 5 px ≈ 5 mm @ 3 m | "Good — ready to scan" | Save, continue |
| Usable | 12 – 30 | 5–12 px ≈ 5–12 mm | "Usable, but colours may smear on edges" | Save; offer "improve it" |
| Reject | > 30 | > 12 px | "Not accurate enough" | Must redo; name the likely cause |

The gate reads about **2.5× the true error** on a well-conditioned capture (measured: 11.0 px gate against 4.3 px true error for the Mid-360 A1/8-pose configuration — each half has half the data, and the two half-solutions err independently). On a badly conditioned capture it does not merely scale up, it explodes: D6 sessions read 310–450 px against 28–46 px of true error. That is exactly the behaviour a safety gate wants — a reliable *detector* of a bad capture, not a precision estimator of a good one.

On reject, diagnose rather than just fail — the wizard knows which it was:
- diversity wheel sparse in roll → "Try again, and tilt the phone more between shots"
- few lidar returns per pose → "Move the board further from the wall"
- high residuals on 1–2 poses → drop those poses, re-solve, and ask for two replacements

### Screen 5 — Verify (the step that earns trust)

Show the **AR overlay live** with the new calibration: the current lidar scan projected onto the camera image, edges highlighted. The user points at a doorframe or table edge and sees for themselves whether the points sit on it. A single "Looks aligned / Looks off" confirmation.

This catches whole classes of failure that no residual can — wrong bracket slot, phone in the mount backwards, wrong lidar selected — and it costs five seconds.

---

## 3. Persistence and re-calibration

- Store in `manifest.json` per project (spec §3.11 already reserves "mount calib"): the extrinsic, its split-half gate value, the estimated time offset, target size, pose count, sensor serial, bracket ID, timestamp, and app version.
- **Calibration belongs to the bracket, not the project.** Keep a device-level store keyed by (phone model, bracket ID, lidar serial) and offer it as the default for new projects — the user should calibrate once, not once per scan.
- Re-prompt when: the bracket ID changes, the phone is re-seated (detect via a large disagreement between the stored extrinsic and a quick single-pose check), the stored calibration is older than a configurable interval, or the user reports colour misalignment.
- The **AR overlay is a free continuous monitor.** If the live overlay is systematically offset during capture, that is a stale calibration; surface a non-blocking "Re-check mount alignment?" hint.

---

## 4. What the wizard must achieve — and what it cannot fix

The wizard is responsible for **one** of the terms in the colorization budget, and it is not the biggest one.

| Term | Owner | Contribution at 3 m (15 ms jitter) | Target |
|---|---|---:|---|
| Extrinsic | **this wizard** | 3.7 px | ≤ 0.16° / 3 mm → ~4 px |
| Constant time offset | **this wizard, screen 3** | (folded into sync) | driven to ~0 |
| Time sync × turn rate | engine time-sync (A4) + capture UX | **16.7 px** | ≤ 5 ms, **or** gate turning at ≤ 15 °/s |
| Time sync × walk speed | engine time-sync (A4) | 4.3 px | — |
| Rolling shutter | engine projection model (A11) | 6.8 px | corrected per-row, not left in |
| ARCore relative pose | ARCore + keyframe selection (A11) | 11.4 px | prefer near-in-time keyframes |
| Lidar range noise | sensor | 0.5 px | — |
| **Total** | | **22.2 px** vs a 20.2 px budget | |

Two of these are the wizard's; the rest are the engine's, and they dominate. See REPORT.md §6 for the full budget and the resulting recommendations — in particular that **colorization must gate on rig motion** (prefer keyframes taken while turning slowly), which is a colorization-pipeline requirement, not a wizard one.

---

## 5. Open questions for the bench

1. **What is the D6's real 1σ range noise at 1–2 m?** The whole D6 verdict pivots on it: at 30 mm the wizard cannot close the budget, at 10 mm it can. **S1 must measure this** (static target, 1000 returns, report σ not the datasheet bound).
2. Does the D6 return usable intensity on a printed checkerboard, or does the wizard need a **retro-reflective / high-contrast** target for lidar-side segmentation? Spec §2.1 mentions a high-reflectivity flag.
3. How accurate is the achievable time-offset estimate from the screen-3 sweep on real hardware? Simulation says the constant part is removable; the bench has to confirm the residual.
4. Board flatness: a printed sheet on foam board bows by several millimetres. Budget for it, or specify a rigid backing (aluminium composite panel).
