# Owner design-review feedback (running log)

## 2026-08-16 — round 1 (verbal, against mockup v3)
1. **Android · Capture**: display settings are too narrow to operate — needs large-touch-target treatment (bottom sheet).
2. **Desktop (macOS + Windows) · Capture**: start-record / end-record buttons missing — must be prominent.

### Resolution — 2026-08-16 (mockup v4)
1. **Fixed.** Capture's display strip is now a Material bottom sheet: a 48 px Display button
   floats on the viewport; the sheet is 65 % tall with scrim, grabber, 20 px top radius,
   40 px colour chips and 44 px slider rows with 28 px knobs. Colour / point size / LOD bind
   live to `S.cap.*` and repaint the canvas without re-rendering, so the viewport stays
   visible and verifiable above the sheet. Closes via grabber, scrim, Esc or Done.
   Checklist: `a-cap-disp` reworded; `a-cap-sheet` and `a-cap-view` added.
2. **Fixed.** Desktop capture has a record cluster pinned under the viewport on both shells:
   Test device → **Start recording** (large ember pill) → Pause/Resume → **Stop recording**
   (red-tinted) with a ticking elapsed clock and a REC/PAUSED/ARMED badge. Start stays
   disabled until the self-test passes and states the threshold in words.
   Checklist: `d-cap-rec`, `d-cap-start`, `d-cap-gate`, `x-reccluster` added.

Verified headless via CDP: 60/60 assertions, zero console errors, no dead controls
(87 actions audited across every screen). Screenshots: `redesign-exports/fix-*.png`.

## 2026-08-16 — round 2
3. **Android · Capture**: move the AR setting into the Display bottom sheet — view mode (3D/AR) belongs with the display settings, not as a separate floating control.

### Resolution — 2026-08-16 (mockup v5)
3. **Fixed.** The floating 3D/AR segmented control is gone from the capture viewport. View mode
   is now the **top section of the Display sheet**: a full-width, 48 px-tall segmented row —
   two equal halves, **3D orbit / AR overlay** — above colour mode, point size and LOD. It is
   bound to the same `capView` action and `S.cap.view` state as before, so nothing was
   re-plumbed; `capView` just got the in-place treatment the colour chips already had: it
   patches `aria-pressed`, the row read-out and the *AR tracking* line behind the sheet, then
   `redraw()`s. The viewport swaps to the camera-anchored AR backdrop live, with the sheet
   still open and un-jumped. The Display button is now the single entry point for view +
   display settings; the **Orbit / Follow** camera control stays on the viewport, untouched,
   and desktop capture keeps its own `dcapView` control.
   The sheet grew 65 % → 73 % with tightened row rhythm (12 px) so all four rows still fit
   above the fold with no scrolling, while 77 px of live viewport stays on screen above it —
   enough that the AR swap is plainly visible (mean band brightness 14 → 38).
   Checklist: `a-cap-ar` rewritten to the new location (and to assert the overlay control is
   gone), `a-cap-sheet` reworded to "single entry point for view + display settings, ~70 %
   height", `a-cap-disp` now lists the 48 px View row first, `a-cap-view` extended to cover
   view mode. No items added or removed — still 110. No dead controls or orphan handlers:
   `capView` remains referenced (from the sheet), and no `data-act` was left behind.

Verified headless via CDP: 44/44 round-2 assertions plus the full round-1 suite re-run green
(60/60) — 104 total, zero console errors, zero uncaught exceptions, 87 actions audited across
every screen with the sheet both open and closed, and a record → AR-toggle → stop cycle driven
end to end. Screenshots: `redesign-exports/fix-r2-*.png`.

## 2026-08-16 — round 2 clarification
3b. **Clarified**: "AR setting" = the **ARCore + camera keyframes** controls (colorization capture), not the view-mode toggle. The Display sheet must include an "AR & Camera" section: camera-keyframes on/off (+ rate), AR tracking status. View row from 3a stays unless owner objects.

### Resolution — 2026-08-16 (mockup v6)
3b. **Fixed.** The sheet is renamed **"Capture settings"** (it now covers view + AR/camera +
   display; the viewport button, its `aria-label` and the grabber's label follow), and carries a
   new **AR & Camera** section between the View row and the display controls:
   * **Camera keyframes** — a 44 px switch (`capKf` → `S.cap.keyframes`, default **on**) labelled
     *Camera keyframes* over *for colorization · motion-gated*. With it on, a **KF counter chip**
     appears on the viewport during a recording and ticks at the selected rate; with it off the
     chip disappears and the count freezes where it stood rather than being rewritten. The counter
     is accumulated (`kfAcc`), not derived from elapsed time, so toggling mid-session is honest.
     Under `prefers-reduced-motion` the session clock already advances in ~2.9 s steps, so the
     counter steps with it (measured: 0 → 8 → 17 → 25) instead of animating.
     One deliberate departure from the brief: the chip rides the **top** band of the viewport, not
     the bottom health corner. The sheet covers the lower ~74 % of the screen, so a bottom-corner
     chip would be hidden by the very sheet whose switch controls it; in the top band it is
     verifiable while you flip the switch. The health chip itself is untouched.
   * **Keyframe rate** — a 2 / 3 / 5 fps segmented row (`capKfRate` → `S.cap.kfRate`, default 3),
     40 px chips. With keyframes off the row drops to 50 % opacity and its buttons take
     `aria-disabled`, but it keeps its label, its chip labels and its read-out — dimmed, not blank.
   * **AR tracking** — a read-only row, no `data-act`. The old `#cap-artrack` KV moved here; the
     line behind the sheet is kept in sync through one resolver + one painter over `[data-artrack]`,
     so the two can never disagree. It reads **TRACKING** in good-green whenever the ARCore session
     has a reason to run (AR view *or* keyframes on), **off** when neither does, and drops to
     **LIMITED** in amber for ~1.4 s after a view toggle — the re-anchoring beat real hardware pays.
   Sizing: 73 % → **74 %** (518 of the 700 px screen), which keeps **70 px of live viewport** on
   screen — above the 60 px floor. The extra section does not fit (542 px of content in a 286 px
   body), so the documented fallback applies: the **View row is pinned** in a `.sheet-pin` band
   between the head and the body, and AR & Camera + Display scroll under it. Every touch size is
   preserved: 48 px View halves, 44 px switch row, 40 px chips, 44 px slider rows with 28 px knobs,
   55 px Done, 30 px grabber. All four close paths (grabber, scrim, Esc, Done) still work.
   Checklist: **`a-cap-kf` added**; `a-cap-sheet` reworded (view + AR/camera + display, ~74 %,
   View pinned); `a-cap-disp` now lists AR & Camera between the View row and the display controls;
   `a-cap-ar` retitled to the sheet's new name. 110 → **111** items, no duplicates.

Verified headless via CDP: **161 assertions green** — 57 round-3, plus the full round-1 (60/60) and
round-2 (44/44) suites re-run after patching the assertions the resize made stale (chip selectors
that now matched the rate row, the "no scrolling" rule superseded by pinned-View + scroll, and the
`#cap-artrack` lookup that had to fall back to the body KV when the sheet is closed). Zero console
errors, zero uncaught exceptions, 89 distinct actions audited across every screen with the sheet
open *and* closed *and* with keyframes both on and off, and a record → keyframes-off → keyframes-on
→ rate-change → stop cycle driven end to end. Screenshots: `redesign-exports/fix-r3-*.png`.

## 2026-08-16 — round 3
4. **Android · Capture**: remove the "AR + camera keyframes" telemetry block (AR tracking / Keyframes written / Tracking-loss episodes / Skipped-turning / Rolling shutter) from the capture screen body. Relocate into a Diagnostics view opened from the device-health chip (one tap away, capture body stays clean).

### Resolution — 2026-08-16 (mockup v7)
4. **Fixed.** The five-row **"AR + camera keyframes"** KV block is gone from the Android capture
   body — with it, the whole 126 px `.body` that carried it. The capture screen is now viewport →
   stat strip → transport, and nothing else: the reclaimed height went to the live cloud
   (**225 px → 351 px**, 50 % of the screen), and the record cluster picked up the tab-bar
   clearance the removed body used to provide (12 px between Record and the tab bar).
   * **The health chip is the door.** The `Healthy` / `Degraded` chip in the bottom-right of the
     viewport is now a real `<button>` (`capDiag`, `aria-haspopup="dialog"`, `aria-controls`,
     `aria-expanded`). Its ink is chip-sized — 68.5 × 23.8 px — so a `.hit44` pseudo-element hangs
     an invisible **69 × 44 px** target off its centre rather than inflating the chip; measured by
     probing `elementFromPoint` around the target, not by trusting the CSS. (`overflow` has to be
     forced back to `visible` there: the `.vp .ovl .chip` ellipsis rule would otherwise crop the
     hit area away.)
   * **The Diagnostics sheet.** Same chrome as Capture settings — scrim, 30 px grabber, 20 px top
     radius, scrolling body, 55 px Done — but **60 %** tall (420 of 700 px) instead of 74 %,
     because nothing in it is a target: read-only rows need no thumb room, and the 168 px of live
     viewport it leaves on screen is the thing being diagnosed. Two sections:
     **Device** (state, points/sec, rotation, IMU, checksum pass rate, packet loss) and
     **AR + camera** (the five relocated rows verbatim, in their original order: AR tracking,
     keyframes written, tracking-loss episodes, skipped-turning, rolling shutter). Zero `data-act`
     and zero focusable elements inside the body — asserted, not assumed.
   * **Still live.** `paintDiag()` rides the same animation tick as the stat strip, off the same
     `S.cap.*`, so state / pts-per-second / skipped-frames / the keyframe count all keep moving
     while the sheet is open. The KF count in the sheet and the KF chip on the viewport are one
     number rendered twice — verified equal mid-recording (`KF 6` = sheet `6`) — and with camera
     keyframes off the row reads *off — no colorization* instead of freezing a stale integer.
   * **One sheet at a time.** Opening Diagnostics sets `S.cap.sheet=false`, opening Capture
     settings sets `S.cap.diag=false`; both dismiss through one `closeCapSheets()` path, so there
     is only ever one scrim, one `.sheet` and one dialog to own Esc. Esc reaches Diagnostics first
     when it is the sheet that is up. All four close paths work on both (scrim, Done, grabber,
     Esc), and navigating off capture closes either.
   * **One focus bug found and fixed on the way.** Opening a sheet re-renders the screen,
     which removes the button that was just pressed; Chrome answers that by resetting focus to
     `<body>`, and that fixup can land *after* the `.focus()` call — leaving an open dialog with
     focus nowhere. It surfaced as a ~1-in-8 flake on the round-1 assertion too, so it predates
     this round. Both sheets now go through one `focusSheet()` that claims focus and re-claims it
     on the next task. 12 consecutive full-suite runs green since.
   * **The sheet's own AR-tracking row stays.** It is a setting-context readout, not telemetry.
     The `[data-artrack]` painter now has exactly one live target at a time — the Capture-settings
     row, or the Diagnostics row — and still repaints on a view toggle (TRACKING → LIMITED).
   Checklist: **`a-cap-diag` added** (health chip → 44 px target → read-only Diagnostics, both
   sections, live values, four close paths, mutual exclusion); `a-cap-kf` extended to say the
   written-keyframe count and the rest of the AR telemetry now read out in the diagnostics sheet
   from the health chip rather than in the capture body; `a-cap-sheet` given the one-sheet-at-a-time
   rule. 111 → **112** items, no duplicates. The capture footer note now names the Healthy chip.

Verified headless via CDP: **207 assertions green** — 46 round-4, plus round-1 (60/60), round-2
(44/44) and round-3 (57/57) re-run after patching round-2's stale `#cap-artrack-body` fallback
(the AR-tracking line no longer has a home in the capture body, so it now falls back to the
Diagnostics row and then to the resolver itself). Zero console errors, zero uncaught exceptions,
**90 distinct actions audited** across every screen × Capture settings open / Diagnostics open /
both closed × keyframes on / off. Screenshots: `redesign-exports/fix-r4-*.png`.

## 2026-08-17 — round 4 (field-driven)
5. **Auto-detection required** (owner, after real-hardware GUI session): the apps must
   auto-detect device settings — Mid-360 via broadcast heartbeat (lidar IP/SN/persisted
   host revealed; proven manually in the field session), D6 via serial protocol probe,
   UM982 via port+baud sweep. Manual IP entry defeated the GUI on first contact.
6. **Single-instance guard** (desktop): a leftover instance holding UDP ports makes the
   next launch's SDK init fail with an opaque I/O error.

## 2026-08-17 — round 5 (owner, after first full hardware session with both sensors)
7. **No popup windows; fewer steps.** Replace dialogs with inline panels/sheets; collapse
   multi-step flows (self-test gates, wizards) — live preview showing points IS the proof
   a device works.
8. **Tab roles**: Projects = list of projects + preview of the selected scan, nothing else.
   Capture = creating new scan projects only.
9. **Auto-project creation**: Start always begins a NEW project; if the user typed no name,
   default = series number + date + time (e.g. Scan-014 2026-08-17 19:32).
10. **Pre-capture live preview**: before recording, stream live with all display parameters
    adjustable — live refresh rate, point size, gamma, brightness, etc. Capture itself also
    runs with live view.
11. **D6 is PHONE-ONLY and produces 3D, not 2D.** D6 has no built-in IMU (owner-verified).
    Mount: on the back of the phone; phone ARCore (camera+IMU VIO) supplies the 6-DoF
    trajectory; A8 pushbroom builds the 3D cloud. Remove D6 from desktop capture (desktop
    keeps D6 project post-processing/replay). Desktop capture = Mid-360 + RTK.

### round 5.1 (owner, on the round-5 mockup)
12. Auto-detect must never lock the user out: nothing found → inline manual-setup row
    opens by itself; "Manual setup" reachable anytime. Inline, not a dialog.
13. Point size: min 0.1 px, max 3.0 px, step 0.1.
14. Display settings chosen in preview carry unchanged into recording (same live panel)
    and are saved with the project as its default view.
15. Processing & Merge TABS removed; features fold into Projects: 1 selected → Process +
    Export beside preview; 2+ selected → Merge unlocks. Same engines, fewer tabs.

**Round 5 + 5.1 APPROVED by owner 2026-08-17 ("good to go") — items 7–15 are the build contract for the capture-flow redesign.**

### round 5.2 (owner)
16. **Phone-location georef fallback (Android)**: no RTK connected → automatically use the
    phone's fused location (~1 Hz) as the georeference source, recording Android's reported
    accuracy honestly as sigma into the A10 GNSS fusion. Inline chip shows "RTK Fixed ±2 cm"
    vs "Phone GPS ±N m". Location permission asked only when needed; denial → capture
    continues un-georeferenced with a quiet note (no blocking popup).

### round 5.3 (owner)
17. **Live refresh rate max is hardware-derived — never crash the renderer.** The slider's
    ceiling comes from the device (display refresh rate / measured render headroom), not a
    hardcoded 60. If frames start dropping or thermals climb, auto-downshift the live view
    (never the recording — recording is full-rate always) and say so quietly inline.
18. **Walkthrough-first: both apps assume the operator WALKS the space while scanning.**
    Defaults, UX and safeguards tuned for handheld motion: screen stays awake during
    capture, big touch targets usable one-handed mid-walk, live trajectory trail in the
    view so the operator sees where they have covered, gentle "moving too fast" hint tied
    to the existing motion gates. Stationary/tripod is the special case, not the default.

### round 6 (owner, second Pixel field session + kc-m4 GUI session)
19. AR overlay must never crash the app — enable path hardened; failure shows an inline
    error state instead of dying.
20. **Captures must never vanish.** Real-phone captures were not appearing in Projects —
    data-loss class bug: end-to-end trace + seal errors surfaced in UI + persistent
    on-device log file so field failures leave evidence.
21. Don't default to the phone's maximum settings; defaults are conservative per device
    class. D6 live 3D map stability + point alignment improved within phone budget.
22. Performance presets: quick-select Light / Optimal / Full on the capture screen;
    full parameter set stays available for advanced users (presets are starting points,
    not caps).
23. D6 mount re-zero: one-tap "set mount reference" while holding the rig still before
    a scan — records the current phone-IMU/AR orientation as the session's mount trim on
    top of the nominal bracket extrinsic (mount shifts between sessions).

### round 7 (owner — D6 scan quality is THE core purpose; macOS on hold)
24. D6 = 2D lidar acting as 3D via phone pose; phone held vertically, D6 on its back,
    operator walks the room. Scans must be stable with STRAIGHT walls (reference: "$8
    lidar → wireless 3D mapper" video). Origin per capture is fine; visible section
    misalignment is not.
25. AR function ARCHIVED for now (UI entry hidden; pose tracking stays — it drives the
    scan). Revive later.
26. From owner log: mount trim LOST on next capture (trim=none) — must persist until
    re-zeroed; and scan-009 recorded 0 points silently — no-data captures must scream
    during capture, never seal quietly empty.

### round 8 (owner, on 0.4.0)
27. **Recorded D6 scans must BE 3D**: opening a saved scan shows the 3D map, not raw 2D
    slices. Engine: record the trajectory (kPoseAr writer) + resolved map so the project
    is self-contained; Review re-resolves/loads the 3D result.
28. Capture tab layout: the scan view keeps >=60% of screen height; settings collapse
    into a few buttons.
29. Default display for lidar scan: colormap=intensity, point size 1 px, gamma 1.0,
    brightness 1.0, live refresh 30 fps.
30. "Set mount reference" must visibly work: persistent confirmation (trim angle + age)
    and clear refusal reasons; investigate the owner's "looks not working".
31. Post-capture flow: Stop => file kept, auto-return to Projects (new scan highlighted),
    Capture tab resets ready for the next scan.

### Resolution — 2026-08-18 (0.5.0)
27. **Fixed, end to end, and it was three bugs rather than one.**
    (a) `Engine::push_pose()` never touched the recorder, so `ChunkType::kPoseAr`
    had no writer — and a COIN-D6 is a 2D lidar whose third dimension IS the
    trajectory, so a saved D6 scan held nothing 3D at all. It now records poses
    (68 B, ~2 KB/s), caches the resolved cloud as `kPointsXyzRgba` in
    `streams/map.bin`, and writes `mountCalibration` + a real `sensors` list
    into the manifest (both were empty in every `.lscan` ever written).
    (b) New `post::D6ResolvePipeline` re-resolves a D6 project offline through
    the real driver/pose-source/assembler; `run_post_process` routes to it off
    the container's own chunk types. `ReplaySource` replays `kPoseAr` too.
    (c) Review probes the container and takes one of three paths — cached
    cloud, re-resolve, or the honest "recorded before trajectory storage" for a
    pre-0.5.0 capture — instead of showing an empty box.
    (d) Proved on the REOPENED project: 0.052 cm wall plane-fit RMS, 4.05 m
    walk extent, **0 points at exactly z = 0**, bit-identical to the live pass;
    controls show the same bytes without poses produce nothing. Emulator test
    proves the Android path against the real native engine.
    **Also found by measuring the owner's own export:** the Projects thumbnail
    was 50.2 % raw sensor-frame fan (2,027 of 4,040 points at exactly z = 0) —
    the writer sampled every stream where the renderer has filtered since B3.
    Fixed at the source plus a `PreviewSanity` verdict on write and on read.
28. **Fixed.** `CaptureLayout.MIN_VIEWPORT_FRACTION = 0.60`, enforced by
    arithmetic (258 dp of fixed chrome, viewport is the only weighted child).
    Measured 67.5 % on the emulator in the connected state. Settings collapsed
    into one mount row plus `[Capture · Optimal] [Display] [Diag]`.
29. **Fixed.** `DisplayParams.captureDefaults()` — INTENSITY, 1.0 px, gamma
    1.0, brightness 1.0, 30 fps. Also fixed the point-size mode being left on
    ADAPTIVE, which made the slider have no defined effect.
30. **Root cause found in the owner's log: the gate refused 7/7 on 0.4.0.** It
    judged the WORST of ~37 ARCore frames against 1.5°. Now p90 <= 2.5° with a
    6° outlier ceiling over a 1.0 s window (~1.5 s door to door), refusals
    report their measured numbers on screen and in the log, and a persistent
    `MOUNT SET · 132.8° · 2 min ago` chip is part of the capture chrome.
31. **Fixed.** Stop -> verified seal -> Projects with the new scan selected and
    previewing; the Capture tab re-arms with a fresh auto-name, still connected,
    live preview running, and keeps the mount trim.
    **Plus two owner messages taken mid-round:** the live pushbroom map is on by
    default in every preset but LIGHT and the D6 capture view defaults to
    Follow; and Follow is now a THIRD-PERSON camera (behind, above, pitched
    down, heading from the trajectory not phone yaw) because the D6's fan paints
    a ring around the operator and never anything ahead.

### round 9 (owner, on 0.5.0 — "much better")
32. Never declare data unrescuable: legacy (pre-0.5.0) scans get a rescue path on the
    roadmap (trajectory recovery from camera keyframes via SfM/VIO). First-principles.
33. Capture flow: entering Capture = a new-scan context; leaving WITHOUT recording
    leaves nothing behind (no empty projects, prune 0-point strays); record+stop =>
    keep + redirect to Projects (as shipped).
34. OUTPUT IS LEFT-RIGHT MIRRORED. Owner's exact mount: D6 on the BACK of the phone,
    0° direction facing UP, top of the lidar facing FORWARD (walk direction).
    Stored extrinsic det=+1 (proper rotation) => suspect fan-angle spin-direction
    convention. Fix from datasheet + this mount truth; falsifiable chirality test.
35. Pose/lidar rate sync (owner insight): ARCore poses ~30 Hz vs lidar slices ~6 ms —
    densify poses with phone IMU (gyro/accel) between ARCore samples for gait-frequency
    motion; record the phone IMU stream so offline re-resolve benefits too.

### Resolution — 2026-08-18 (0.6.0)

32. **Design sketch written, no implementation, as asked.** `android/NOTES.md` ROUND 9 §6
    carries it: read the keyframe index FIRST (if `kCameraFrameIndex` descriptors still
    hold ARCore poses, that is already a trajectory and the rest is unnecessary — this may
    make the item nearly free); otherwise incremental SfM over the keyframes
    (`scan-015` has 50 jpgs + `frames.idx`), scale fixed from the D6's own metric ranges
    as a single global factor, interpolate, and re-resolve through the EXISTING
    `post::D6ResolvePipeline` by synthesising a `kPoseAr` stream — no new assembler, and
    this round's chirality fix applies for free. Honest quality: keyframes at ~2 Hz are
    15x sparser than ARCore, so gait-band motion is not merely attenuated but **aliased**
    and is not recoverable by any post-process. Expect several centimetres of wall RMS at
    best. It should be offered as "recover a recognisable room from a scan you would
    otherwise throw away", never as survey grade, and the UI must say so.

33. **Fixed — and the stray was not coming from where it looked.** Entering Capture has
    created nothing since ROUND 5; leaving without recording was already clean, and the
    regression test added for it passed on the first run. The three real sources were:
    (a) a Start the engine REFUSES — the `.lscan` was created before the engine was asked
    to record, so a refusal left the directory, `project.json` and a spent series number
    behind (the code admitted this in a comment); now rolled back, and only when that call
    created it. (b) Stop with 0 points, kept deliberately since ROUND 7; now pruned, with
    the navigation-to-Projects suppressed so the shell cannot open a deleted project. A
    *failed* seal is still never pruned, so the "your raw data IS on the phone" banner can
    never be contradicted. (c) The legacy scan-012/-014 strays: the Projects list hides
    them (filtered in the ViewModel, not in the shared store), the count is shown quietly,
    and Settings gained a Scans section with a `keepEmptyScans` switch (default off) and a
    "Clean up N empty scans" action. Emulator-verified on b4_test: 16 tests, 0 failures.

34. **Root cause found in the vendor datasheet. It was one character, and it is fixed at
    the source rather than at the output.**
    (a) **What it was:** the vendor states the D6's angle convention in a **left-handed**
    coordinate system — `docs/bench/BENCH_SETUP.md` §3.1 has quoted the manual since
    Phase 0: *"left-hand coordinate system ... rotation angle increases clockwise ...
    zero-degree direction marked in the figure"*. S1 transcribed the datasheet's `(x, y)`
    verbatim into a right-handed frame. Keeping `x` and `y` while the handedness flips
    **reverses the sense of rotation about the spin axis**, so the fan swept the wrong way
    and every cloud came out reflected. The formula is now
    `p_lidar = (−d·sinθ, d·cosθ, 0)` with `+z` out of the BASE of the unit, derived in
    full in the new `engine/include/scanengine/drivers/d6/d6_fan.h` — the one place it is
    written down and the only place it is computed (it had been spelled out longhand in
    two files, and **neither said which end of the sensor `+z` came out of**).
    (b) **Why det=+1 proved nothing, and why eight rounds of tests missed it:** every D6
    return has `z == 0` exactly, and restricted to that plane the reflection `x → −x` is
    identical to `diag(−1,+1,−1)` — a **proper, det=+1** rotation of 180°. So the rigidity
    guard that exists precisely to catch a mirrored cloud could not fire. Separately,
    every geometry test in both trees measured a **sign-blind** quantity — axis extents,
    best-fit-plane RMS, point counts — and a mirrored room has identical extents and
    identical planarity. There was no test in the repository able to tell a room from its
    reflection.
    (c) **The CAD nominal did NOT change**, and that is load-bearing. Under the corrected
    frame your stated mount (0° beam UP, cap FORWARD, D6 on the back of a portrait phone)
    maps lidar `+y → camera +Y`, lidar `+z` (the base) `→ camera +Z` so the cap faces
    forward, lidar `+x → camera +X` — the identity rotation `BracketNominals` has always
    carried. Yawing the nominal 180° instead would have produced the same picture, and
    doing **both** would have been a no-op; exactly one of them was wrong. Fixing the
    formula also un-mirrors **every archived capture for free**, with no manifest
    migration, because `old_fan(θ) ≡ diag(−1,+1,−1)·new_fan(θ)`.
    (d) **The falsifiable proof** (`engine/tests/test_round9_chirality.cpp`): a corridor
    walk with a doorway cut into the wall on the operator's LEFT, with "left" computed as
    `up × forward` from the resolved trajectory rather than hard-coded. Fixed convention:
    **0 returns left of the walk in the doorway band, 1,440 right.** The same returns
    through the same assembler under the pre-fix convention: **exactly reversed.**
    (e) **Your scan-017, re-resolved** with the fix and its own unmodified manifest:
    15,631 points, 150/150 poses. The vertical extent — ARCore's world is gravity-aligned
    with +Y up, so this is floor-to-ceiling — drops from **4.16 m to 3.20 m**, and you
    describe a ~3.1 m room; the two strongest horizontal bands (floor and ceiling) hold
    18.4% of returns after the fix versus 13.0% before. Mean per-point movement 1.77 m.
    **Both of those point the right way but neither is proof — please look at the
    re-resolved cloud and confirm the handedness against your room.** The one link in the
    chain that came from a translated quotation rather than a measurement is whether the
    datasheet's figure is a top view; your eyes settle it.

35. **Implemented, and the numbers are large.** Your insight was right: measured on
    scan-017 itself, ARCore delivered **150 poses over 4.999 s — 29.8 Hz, median 33.33 ms**
    — while the D6 samples at 4000 Hz, so **one pose bracket covers ~133 lidar returns**
    whose trajectory was pure interpolation.
    (a) **Orientation is where it pays:** 1° of orientation error puts a 3 m return 5 cm
    out of place; 1 mm of position error puts it 1 mm out. So the gyro densifies rotation
    and position stays on the lerp.
    (b) **The method is drift-proof by construction.** Integrate the gyro across the
    bracket, form the closing error against the next ARCore pose, and distribute it
    linearly. At both ends the answer is *exactly* the ARCore pose, so the IMU can only
    choose the PATH between two points VIO has already fixed — it can never pull the
    trajectory away or accumulate drift. Gyro bias is estimated from that same closing
    error, and every query falls back to plain interpolation (counted, by reason) if the
    IMU stutters, the bracket is too wide, or the disagreement is implausible.
    (c) **Measured** (`engine/tests/test_round9_imu_densify.cpp`), walking past a flat wall
    with 1.5° of 12 Hz rotational jitter — deliberately *below* the 30 Hz pose Nyquist, so
    this is attenuation and not aliasing: wall plane-fit RMS **0.739 cm with plain slerp
    → 0.021 cm IMU-densified**, against an analytic-truth floor of 0.0007 cm. **36x
    better; 97.3% of the recoverable error closed**, 3,856 queries densified, 0 fallbacks.
    Controls: remove the jitter and the win collapses to 0.55x (there is nothing to
    recover); a realistic 0.01 rad/s gyro bias still beats plain slerp and is recovered to
    0.0146 of 0.0173 rad/s; a starved 5 Hz IMU falls back on every query and reproduces
    the plain answer instead of inventing a shape.
    (d) The phone IMU is recorded as its own stream (`streams/imu_phone.bin`) so an offline
    re-resolve gets the same benefit, and it is wired into both the live pushbroom and
    `D6ResolvePipeline`. Its extrinsic goes into `manifest.json` beside `mountCalibration`,
    because a container carrying gyro samples but not the frame they were measured in is
    self-contained only by accident — that was ROUND 8's lesson and it applies again here.
    Wiring it up also turned up a latent bug worth naming: `FileRecordReader` had a
    hard-coded list of stream files, so the new stream was written correctly and then never
    read back. Anything added to the format in future has to update that list, and nothing
    enforces it.
    (e) **Not yet verified on hardware** — every number above is from a synthetic bench,
    and the 12 Hz / 1.5° jitter model is an assumption about handheld gait rather than a
    measurement of your walk.

**Plus one refinement your D6 spec numbers exposed, which you did not ask for.** 10 Hz
rotation / 4000 Hz sampling / 230400 baud against a measured ~13.7 KB/s is **~60% wire
duty** — the D6 buffers a packet and transmits it ~1.7x faster than it samples (the packet
size falls straight out of those numbers: at 24 samples/packet the stream costs exactly
13,667 B/s). ROUND 7's byte-position back-dating is correct at packet granularity but
compresses time inside a packet. Returns are now dated when they were **sampled** — spaced
at the sampling period derived per packet from its own angle span and the reported scan
frequency, anchored on the packet's first byte. Going further: that anchor is itself
**biased late** by the duty cycle (up to ~120 ms at the head of a 4 KB read), so it is now
combined with the device's own sample rate through a min-delay estimator, which converges
with no tuning because every read's tail gives one tight anchor that the chain carries
forward. Derivation in `engine/docs/A2-d6-driver.md` §9.
