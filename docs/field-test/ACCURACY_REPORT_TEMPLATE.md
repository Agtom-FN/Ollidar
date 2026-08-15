# LidarScan — Field Accuracy Report

Fill in one copy of this file per field session (or duplicate the tables
per-run if a session covers multiple runs from `docs/field-test/PROTOCOL.md`).
Every table's **Synthetic baseline** column is a citation, not a guess —
copy the number and its source doc verbatim so a reader can go check it. The
**Field-measured** column is what this session actually produced; leave a row
blank with a reason (e.g. "no survey control available") rather than deleting
it, so the report shows what was skipped and why.

---

## Session metadata

| Field | Value |
| --- | --- |
| Date | |
| Operator(s) | |
| App build (git commit / release tag) | |
| Host(s) used (desktop OS / Android device model) | |
| Sensors (serials) | D6: · Mid-360: · RTK rover: |
| Site(s) | |
| Weather / conditions (outdoor runs) | |
| Runs performed (§ from PROTOCOL.md) | |
| `.lscan` capture(s) retained at | |

---

## 0. Mount calibration

| Metric | Synthetic baseline (source) | Field-measured |
| --- | --- | --- |
| Mid-360 extrinsic rotation error | 0.200° at N=8 poses (`A8-pushbroom.md` §5.1) | |
| Mid-360 extrinsic translation error | 3.2 mm at N=8 poses (`A8-pushbroom.md` §5.1) | |
| Mid-360 reprojection @ 3 m | 4.2 px at N=8, budget 20.2 px (`A8-pushbroom.md` §5.1, `s6-calibration/REPORT.md` §2.2) | |
| Mid-360 `split_half_px` gate | ≤ ~5 mm-at-3 m display bar (`s6-calibration/WIZARD.md`) | |
| D6 extrinsic rotation error (bench, N=45) | 0.360° at 30 mm noise (`A8-pushbroom.md` §5.2) | |
| D6 extrinsic translation error (bench, N=45) | 7.5 mm at 30 mm noise (`A8-pushbroom.md` §5.2) | |
| D6 reprojection @ 3 m (bench, N=45) | 9.0 px, budget 20.2 px (`A8-pushbroom.md` §5.2) | |
| **D6 range noise σ, 1–2 m, static target** (open question) | datasheet bound ±30 mm, NOT a measured σ (`A8-pushbroom.md` §5.3, `s6-calibration/REPORT.md` §4) | **first field measurement — record raw samples** |

---

## 1. Indoor loop (Mid-360) — loop closure

| Metric | Synthetic baseline (source) | Field-measured |
| --- | --- | --- |
| Loop length | 41 m synthetic hall walk (`A7-post.md` §6.1) | |
| ATE RMS before loop closure (odometry only) | 1.733 m (synthetic, `A7-post.md` §6.1) | |
| ATE RMS after loop closure | 0.185 m (synthetic, `A7-post.md` §6.1) | n/a — no ground truth without survey control; report qualitative closure instead |
| ATE improvement factor | 9.4x (synthetic, `A7-post.md` §6.1) | |
| Final drift after closure | 0.015 m (synthetic, `A7-post.md` §6.1) | |
| Loop candidates detected / accepted | 8 / 6 (synthetic, `A7-post.md` §6.1) — real capture on file: 0/0, correctly, no revisit (`A7-post.md` §6.3) | |
| `graph.final_chi2` (initial → final) | 2270 → 1675, 6 LM iterations (synthetic, `A7-post.md` §6.1) | |
| Estimated \|g\| | 9.807 m/s² synthetic (`A6-lio.md` §7.1); 9.816 m/s² on the one real capture (`A6-lio.md` §7.2) | |
| Path length / scans / keyframes | — (session-specific) | |
| Qualitative closure (screenshot) | — | attach |

---

## 2. Corridor pushbroom (D6 + ARCore)

No synthetic baseline exists for assembled-cloud flatness or a known-dimension
check — `A8-pushbroom.md`'s own accuracy numbers are all *extrinsic* solve
accuracy (the table above), not assembled-profile accuracy. This run
establishes the field baseline; future field sessions compare against **this**
row, not a synthetic one.

| Metric | Baseline | Field-measured |
| --- | --- | --- |
| Wall planarity (plane-fit RMS over a straight run) | — (first field measurement) | |
| Known dimension vs. exported-cloud measurement | — (first field measurement) | error: |
| Mount held from §0.2 calibration (yes/no) | — | |
| ARCore tracking loss events during the walk | — | |

---

## 3. Outdoor RTK walk vs. known points

| Fix mix in this walk | Reported CEP95 target (`A10-gnss.md` §5) | Reported CEP95 (field) | Worst-corner error target | Worst-corner error (field) |
| --- | --- | --- | --- | --- |
| All RTK Fixed | 0.049 m | | 2.0 mm | |
| Fixed 3 : Float 1 | 0.050 m | | 2.6 mm | |
| All RTK Float | 0.737 m | | 29 mm | |
| Float 3 : Single 1 | 0.769 m | | 38 mm | |
| All Single | 4.916 m | | 195 mm | |

| Metric | Synthetic/reference baseline (source) | Field-measured |
| --- | --- | --- |
| Yaw error (georef solve) | 0.0009°–0.086° across fix mixes (`A10-gnss.md` §5) | |
| End-to-end path CEP95 (all-Fixed, real NMEA path) | 4.9 cm (`A10-gnss.md` §5) | |
| LAS opens correctly in CloudCompare/QGIS | required (Tech Spec M3 exit criterion) | pass / fail |
| Correction age — engine (`Rtcm3Framer::age_s`) | — | |
| Correction age — rover (`GnssFix::correction_age_s`, GGA field 13) | — (normally larger than the engine's — `A10-gnss.md` §3) | |
| Cold-start / TTFF | hardware-only, never measured (`A10-gnss.md` §9.8) | **first field measurement** |
| Fix flapping observed | hardware-only, never measured (`A10-gnss.md` §9.8) | |
| Caster used | RTK2go first, then a production caster (`s5-rtk-sim/PUBLIC_CASTERS.md`) | |

---

## 4. Two-session merge

| Metric | Synthetic baseline (source) | Field-measured |
| --- | --- | --- |
| Georeferenced auto-align, worst-corner error | 6.2e-10 mm (composition only — accuracy is A10's, `A13-merge.md` §3, §8) | inherits Run 3's number: |
| Manual 3-point pick, realistic click noise | 83.7 mm / 0.80° (`A13-merge.md` §4) | |
| ICP refine RMS at 0.5 m gate (true alignment) | 9.6 mm vs. 5 mm injected noise (`A13-merge.md` §5) | |
| ICP refine: coarse-error convergence | 394 mm/2.00° → 0.88 mm/0.0061° (`A13-merge.md` §5) | |
| Overlap fraction (symmetric) | pair-dependent; < 0.15 is `low_overlap`, a valid outcome (`A13-merge.md` §5) | |
| Dedup: input → merged points | 34,949 → 27,329 synthetic (`A13-merge.md` §6) | |
| `AlignSource` used (georeferenced / manual / yaw-search / icp) | — | |

---

## Summary — open questions this session closed or advanced

- [ ] D6 real range-noise σ at 1–2 m (`A8-pushbroom.md` §5.3 blocking item)
- [ ] Real loop-closure data (`A7-post.md` §10 item 1)
- [ ] Real clock jitter measurement, Mid-360 (`A6-lio.md` §10 item 2, `A7-post.md` §10 item 5)
- [ ] `GnssSource → GeorefFusion` real-hardware coupling (`A10-gnss.md` §9.3 item 2, §9.8)
- [ ] Two real sessions of one place, for A13's georeferenced path (`A13-merge.md` §11 item 2)
- [ ] Candidate promoted to a committed golden fixture (which one, where): 

## Free-form notes

(surprises, near-misses, anything the tables above didn't have a place for)
