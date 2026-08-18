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

## Round 10 — owner field test of 0.6.0 (2026-08-18, scan-020 + log 3)

Owner verdict: "the 0.6.0 mirror issue fixed, the scan quality much better and real now.
The thing on the left show on the left." Handedness chain is CLOSED — visually confirmed
against the real room. Remaining issues, owner's words distilled:

36. **Speed/latency — the headline.** "The scanning speed seems a bit slow and delay...
    currently i need to move very slow to capture the stable quality... I need to scan
    with a normal walking speed. current scan right when i go forward but when i turn
    around the scan position shifted." Diagnose from scan-020 (202 s, 584,315 pts,
    sections=1, imu_phone.bin @ 399.1 Hz recorded): the turn-around shift is the classic
    signature of a CONSTANT TIME OFFSET between the pose timeline and the lidar timeline
    (yaw rate x Δt = tangential smear; invisible in straight walking, glaring in turns).
    We now have the 400 Hz gyro recorded in the same clock domain — cross-correlate gyro
    yaw rate against lidar-derived / pose-derived yaw to MEASURE the offset on the
    owner's own capture, then correct it (per-capture estimate or fixed calibrated
    offset, whichever the data supports). Also audit: ARCore pose timestamp domain vs
    SystemClock vs D6 byte-arrival anchor; whether IMU densification is actually active
    in the LIVE pushbroom path (session logs say liveSlam=false on OPTIMAL preset —
    verify what that flag gates); live-view refresh/latency budget. Normal walking speed
    is the acceptance bar.

37. **IMU path display reversed.** "The display of my imu path also reversed." ROUND 9
    fixed point resolution at the formula level; the trajectory/trail render evidently
    still draws in the old frame (or its own projection). Find every consumer of poses
    for display and unify on the corrected convention. Acceptance: walk an L-shaped path,
    the on-screen trail turns the same way the operator turned.

38. **Capture lifecycle broken — unacceptable, owner's word.** "When i finish the capture
    and click stop, it will stay with the capture page but not heading to project. when
    click capture after capture, it still show with the previous capture. i can't start a
    new capture unless i close and reopen the app." Required flow: Stop → seal → navigate
    to Projects (scan visible) → Capture tab fully reset (cleared live view, fresh
    project name, ready to Start immediately). No residue, no restart.

39. **De-clutter for the camera-less rig.** "disable the follow and rgb since we dont use
    the camera now. default scan setting show be 3d orbit, intensity, grey scale...
    pause, disable and hide the colorize function and features." Default view mode = 3D
    Orbit; Follow hidden/disabled; RGB color mode hidden; colorize
    pipeline+UI hidden everywhere (capture defaults, review, export, settings). Keep the
    code paths (feature-flagged off), remove from UI. Intensity + grayscale already the
    default — keep.

40. **Capture log filename.** "the capture log please save with date and time in the file
    name." Export as lidarscan-capture-log-YYYY-MM-DD-HHMM.txt (device local time), so
    successive exports never overwrite and reports pair with scans.

## Round 11 — owner-approved quality/UX wave (2026-08-18), ships WITH round 10 as 0.7.0

Owner decision recorded: **0° stays facing UP** — coverage is identical either way (360°
fan + mount re-zero absorbs orientation); the proven configuration stays proven. No work
item, but the nominal bracket assumption must not change.

41. **Loop closure — the accuracy pillar.** ARCore drift over a 200 s walk leaves
    start/end disagreement. When the trajectory returns near its origin (or any
    previously-visited place), detect the revisit, compute the closing correction, and
    distribute it back along the path (deterministic, hand-rolled — no Eigen; same
    doctrine as everything else). Applies in offline re-resolve first (safe, provable on
    scan-020); live is stretch. Acceptance: walk a loop, the two ends of the wall meet;
    quantify start/end mismatch before/after on a real capture.

42. **Coverage coloring — the density pillar.** Live-view mode tinting the map by local
    return density (thin = warning tint, dense = normal) so the operator sees gaps while
    still in the room and revisits instead of walking slow everywhere. Cheap deterministic
    binning, honest at the point budget; live-only display concern, never written into
    the container.

43. **Haptic + audio cues — the operator pillar.** Phone is held facing forward; the
    operator cannot watch the screen. Vibrate + tone on: tracking degraded, section
    break, moving too fast for target ring density. Distinct patterns, debounced,
    toggleable in settings, default ON.

44. **Scan summary card on seal.** After stop (before/with the jump to Projects from item
    38): thumbnail, points, duration, path length, sections, tracking-drop count, and a
    plain quality grade so the owner knows keep-or-rescan in five seconds.

45. **Guided mount re-zero + auto-refresh.** Replace trial-and-error (log shows long
    MOVING-refusal streaks) with a hold-still progress ring ("hold steady… set"), same
    gate underneath. At capture start, if stationary and trim is stale, auto-recapture
    the trim so a stale reference never enters a scan silently.

### Resolution — 2026-08-18 (0.7.0)

36. **MEASURED, and the answer is that the clocks are fine — so the round went
    looking for what actually is slow, and found two things.**

    (a) **The offset, twice, on your own scan-020.** Two clock crossings, two
    independent measurements:
    * **phone IMU ↔ ARCore pose: −1.5 ms** (correlation r = 0.982). Your 400 Hz
      gyro's angular rate cross-correlated against the rate implied by the
      ARCore poses, over all 5,961 pose intervals of the 202 s walk. Stable
      across both halves of the walk and across the turns alone. So
      `Frame.getTimestamp()` and `SensorEvent.timestamp` really are the same
      clock — ROUND 7 §4 asserted that from documentation; this is the hardware
      agreeing.
    * **D6 lidar ↔ ARCore pose: +4 ms**, from a crispness sweep: the same
      container re-resolved at every offset from −150 ms to +150 ms through the
      production pipeline, scored by 3 cm voxel occupancy and by wall-probe
      thickness. **The whole ±30 ms window varies by 0.1 %.** +4 ms is 4 mm at
      1 m/s and 0.24° at a 60 °/s turn, against the **4.8 cm** wall thickness
      scan-020 actually has. The default therefore stays 0 rather than
      pretending a 4 mm correction is real; the knob and the tool
      (`engine_cli --d6-timesweep`) exist so a rig with a genuinely slow
      transport can be corrected from data instead of from reasoning.

    **So the turn-around shift is not a clock offset.** That is a null result
    and it is stated as one. What the round CAN show is what a real offset
    would look like, so this never has to be re-argued:
    `test_round10_time_offset.cpp` walks a wall with 40 ms of injected skew and
    measures **no change at all walking straight (9.9e-06 cm both ways)** and
    **8.81 cm at a 60 °/s turn** — a 260,000× difference from the same error,
    which is exactly why eight rounds of straight-line fixtures could not see
    this class of bug. The correction recovers it exactly, and a sweep's
    minimum lands on the injected truth.

    (b) **The delay that IS real: 2.8 seconds of it, in the live map.**
    `PushbroomConfig::batch_points` held **4096 points** before publishing any
    of them. That number was written for a Mid-360 (hundreds of thousands of
    points/s). Your D6 resolves **1,453 points/s** — measured, 293,524 in-range
    returns over 202.1 s — so the live map was showing you where you had been
    **2.8 seconds ago**, then jumping. Nothing downstream could be faster: not
    the 30 fps refresh, not the renderer, not a better phone. The batch is now
    bounded in **point time** as well as count (100 ms, one D6 revolution), and
    point time and not wall time on purpose — the assembler is documented as
    never reading a clock, which is what makes replay bit-identical to capture.
    Measured: **first point visible 2,825 ms → 100 ms**, and 2,560 of 2,560
    points bit-identical across the two batchings.

    (c) **The audit you asked for.**

    | crossing | domain | correction | verdict |
    | --- | --- | --- | --- |
    | ARCore `Frame.getTimestamp()` | CLOCK_BOOTTIME | none | correct; now measured against the gyro at −1.5 ms |
    | `SensorEvent.timestamp` (gyro/accel) | CLOCK_BOOTTIME | none | correct |
    | engine `timesync/clock.h` | `clock_gettime(CLOCK_BOOTTIME)` on Android | none | correct (bionic's `steady_clock` is CLOCK_MONOTONIC, which stops on suspend) |
    | D6 UART bytes | no device clock at all | ROUND 7 per-byte back-dating + ROUND 9 min-delay sample anchor + the app's 2 ms sensor-latency setting | correct to within the +4 ms measured above |
    | lidar → pose join | — | **new** `pose_time_offset_ns`, default 0 | the knob this round added, with a tool to set it from data |

    **`liveSlam=false` on OPTIMAL gates nothing on your rig.** Confirmed at the
    source: `Engine::start_session`'s `if (cfg.live_slam)` block builds a
    `LioOdometry` — the **Mid-360's** lidar-inertial odometry. A D6's live map
    comes from the pushbroom assembler, which is enabled separately and was
    enabled on every one of your captures (`pushbroomEnabled=true` in your own
    log). ROUND 8 already documented this; it is re-confirmed rather than
    re-litigated. **IMU densification IS active in the live path** — the
    densifier is constructed in `Engine::create` and wired between the pose
    source and the assembler unconditionally, live and offline alike.

    (d) **Two things found while measuring, both real bugs.**
    * **`imuCalibration` is `null` in scan-020's manifest** even though your
      session log for that capture records `camera_from_imu = Rz(+90)`. This is
      ROUND 8's `mountCalibration` bug one sensor down, with the same cause: the
      manifest is written when the session opens, and the app applies the IMU
      extrinsic ~24 ms later (your log: `[session] start` 11:06:25.799,
      `phone IMU start` 11:06:25.823). So **every offline re-resolve of every
      capture so far integrated the gyro in the wrong frame.** Fixed the way
      ROUND 8 fixed the mount — pushed at an already-open recorder, so the
      sealed manifest carries it.
    * **The point count you are shown is roughly double the truth.** Your log
      says `points=584315` for scan-020; re-resolving the same container yields
      **293,166** world points, and the pushbroom's own accounting says 293,524
      returns in, 293,166 out, **zero dropped**. The live counter is adding the
      raw sensor-frame preview stream to the resolved map stream. Not fixed
      this round (it is a reporting bug, not a data bug) — **backlog**.

37. **Fixed, and it was one line and one wrong comment.**
    The trail tile projected `screen_right = +X, screen_up = +Z`. In ARCore's
    right-handed, +Y-up world the axis pointing out of that screen is
    `X × Z = −Y` — **a view from underneath the floor looking up**. A bird's-eye
    view needs `X × (−Z) = +Y`, i.e. world +Z goes **down** the tile and, since
    canvas y already grows downward, there is **no flip at all**. What shipped
    was `y = 1 − nz`; it is now `y = nz`.
    Two sibling modules already had it right and disagreed with this one: the
    floor-plan canvas, and the engine's own `plan/occupancy.cpp` (for a Y-up
    cloud it uses `plan_x = world z, plan_y = world x` — the same chirality,
    tile rotated 90°). The trail was the outlier.
    **Every other pose consumer was audited and is clean**: the 3D orbit view
    hands world XYZ straight to Filament with no negation, no axis swap and a
    right-handed `lookAt`; the follow camera's `atan2(dz, dx)` is the exact
    inverse of its own `headingVector`, so the angle round-trips and its sense
    is unobservable; the review screen draws no path at all; the project
    thumbnail's "trajectory" is a hard-coded decorative curve drawn at alpha 0.
    There was exactly one 2-D projection of poses in the app and it was the
    mirrored one.
    **The old test asserted the bug.** `screen y is flipped so north is up`
    checked that larger +Z sat higher — the mirror, in an assertion. It is
    replaced by two: forward (−Z) must go **up** the tile, and your acceptance
    test verbatim — an **L-shaped walk turning LEFT must render a trail turning
    left**, with "left" derived as `up × forward = −X` rather than hard-coded.

38. **Fixed. Root cause: the live point cloud does not belong to the capture —
    it belongs to the app process.**
    `RealEngineBridge` creates one `scan_engine*` on first connect and holds it
    for the app's lifetime, and the engine's `PageStore` is created **with the
    engine, not with the session**. A capture is a session. So capture #2 opened
    on top of capture #1's pages — "it still show with the previous capture",
    exactly. Every ViewModel-level state WAS being reset correctly, which is why
    six rounds of tests stayed green: the one thing filling the screen was the
    one thing not in the ViewModel.
    The engine has shipped the fix since ABI 7 (`start_session()` calls
    `recycle_all()`, with a comment about "a preview + N record cycles on ONE
    connect all stacked into the same 64 pages") — but that reset is gated on
    page eviction being enabled, eviction is opt-in, and **the Android app never
    opted in**. There was no `nativeSetLivePageEviction` and no
    `nativeRecycleLivePages` in the JNI at all: two C-ABI calls that existed and
    were never bound. Both are bound now, eviction is on (which also means the
    live map can no longer dead-end when its page budget fills — it recycles),
    and the window is emptied on entering Capture and again on every seal.
    **The navigation had a second, independent cause.** `sealedProjectId` was a
    `replay = 0` `MutableSharedFlow`, justified by a comment claiming its buffer
    covered "a collector that is momentarily absent". That is not what
    `MutableSharedFlow` does: `extraBufferCapacity` is slack for a *slow*
    subscriber; with **zero** subscribers a `replay = 0` flow discards the value
    and `tryEmit` still returns true. The seal runs `NonCancellable` in
    `viewModelScope`, so it outlives the composition — an Activity recreation
    mid-seal loses the navigation while keeping the scan, which is precisely
    "sealed fine, never navigated". Now `replay = 1`, and both outcomes are
    logged (`navigate -> Projects id=…`, or the reason it stayed), so the next
    field log answers this without anyone guessing.
    Proven three ways: a JVM test that subscribes **after** the seal completes
    and still receives the id; a JVM test that runs capture → stop → capture in
    one ViewModel and asserts two sealed scans, a fresh auto-name, a cleared
    typed name and zeroed stats; and an **instrumented test on the emulator**
    that drives the real native engine — two sessions on one handle, real D6
    UART bytes through the production driver, and the live window asserted
    **empty** at the start of capture #2 while both projects seal listable.

39. **Done, behind one flag file (`core/FeatureFlags.kt`) rather than by
    deletion**, following the house style the AR overlay was retired with.
    Follow camera: hidden in **both** places it lived (the Display sheet's View
    row *and* a second Orbit/Follow pill on the viewport itself — two controls
    for one state, which is why both had to be found); default camera mode is
    now **3D orbit** for capture as well as replay. RGB: removed from the
    capture sheet, the review chip strip and the review Display panel (which
    enumerated the raw enum and so surfaced RGB and FIX_QUALITY whatever the
    rest of the app did), and it now carries a reason string in
    `colorModeAvailability` like every other unavailable mode. Colorize: the
    keyframe switch and rate row, the viewport KF chip, the Processing tab's
    Colorize action and the Settings "Processing" section are all gone, and the
    keyframe state is gated at the **flow** as well as the UI so a preset switch
    cannot turn the recorder back on behind a hidden control. `ColorMode.RGB`
    itself is untouched — its ordinal crosses the C ABI into the shader, and it
    is what every unsupported mode falls back to.
    **Defaults verified against your list, and one was wrong in three places at
    once.** 3D orbit — fixed (was Follow). Intensity — already correct. **Grey
    scale — was not**: the QUICK_SCAN profile said GRAYSCALE, `captureDefaults()`
    set no colormap and inherited SPECTRUM, and the ViewModel initialised its own
    flow to SPECTRUM, so which one you got depended on which screen you came
    through. There is now one constant (`DisplayParams.CAPTURE_COLORMAP`) and
    all three read it. 1 px / gamma 1 / brightness 1 — already correct.
    30 fps — correct on your Pixel (STANDARD tier); a MODEST-tier phone still
    starts at 15 fps by design, which is the conservative-defaults rule from
    ROUND 6 item 21 and is left alone.

40. **Fixed.** `lidarscan-capture-log-YYYY-MM-DD-HHMM.txt`, device local time,
    to the minute — the same format and the same clock `ScanAutoName` uses for
    scan names, so `Scan-020-2026-08-18-1106` and
    `lidarscan-capture-log-2026-08-18-1111.txt` are comparable by eye. The old
    name was a bare constant, which is why MediaStore had been de-duplicating
    exports into `lidarscan-capture-log (1).txt` — a real artifact of that is
    quoted in this repository's own source (`MountTrim.kt` cites it from your
    0.4.0 session). Seconds are deliberately absent for the same reason the scan
    names omit them.

### Resolution — 2026-08-18 (0.7.0, round 11)

41. **Built, proved against synthetic ground truth, and it REFUSES your
    scan-020 — which turned out to be the more useful result.**

    New `post::TrajectoryLoopCloser` (`slam/post/trajectory_loop.*`), hand-rolled,
    no Eigen, deterministic, wired into the offline `D6ResolvePipeline` behind
    `close_loops` (default OFF: this pipeline's contract is "replay == capture",
    and a closure deliberately produces different points, so it may never happen
    behind a caller's back) and driven by a new `engine_cli --d6-loopclose`.

    (a) **It is not A7's pose graph, on purpose.** A Mid-360's trajectory is
    ESTIMATED, so every keyframe pose is a free variable and a graph is the right
    answer. Yours was MEASURED by ARCore, whose error is a slow smooth walk of
    the world frame — excellent over one second (which is what makes ROUND 9's
    gyro densification work), large over 200. So the correction is one
    measurement spread back along the path by ARC LENGTH (drift accumulates with
    distance, not with seconds), `C(s) = Exp(s·Log(T_fix))`, exactly identity at
    the first visit and exactly the measured transform at the second.

    (b) **Synthetic loop with known truth**: one lap of a circle in an 8×8×3 m
    room, resolved twice from the same ranges — once against true poses, once
    against poses corrupted by 4° of yaw growing as `s^1.5` plus 0.30 m of
    translation, deliberately NOT the shape the correction is built from.
    Per-point error against truth: **mean 17.2 cm → 8.9 cm, worst 61.1 cm →
    16.0 cm** — 74 % of the worst-case error removed, with ICP measuring 3.99°
    against the 4.0° injected.

    (c) **On scan-020 it refuses, and the refusal is the finding.** Your walk is
    **10.8 m in 202 seconds — 5.3 cm/s** (that is what "i need to move very
    slow" turned out to mean), along a straight line 3.67 m out and back with
    24 cm of lateral extent over the whole capture. It is not a loop; the
    excursion gate stops it. Forced past that gate, ICP proposes a **176° flip**
    — the corridor-mistaken-for-itself failure — and the geometric gate refuses
    it. **Nothing was moved. The cloud is byte-for-byte what the app produces.**

    (d) **The guard is five gates and every one earned its place.** A one-way
    walk produces no candidate at all (structural, not a threshold). A rig
    shuffling in one corner is caught by an excursion floor. Then the gate this
    round exists because of: a D6 sweeps a PLANE perpendicular to the walk, so
    it sees no surface facing along the walk, so point-to-plane ICP has a **null
    space** there and does not fail but WANDERS — 4°/0.30 m of injected drift
    came back as 3.77° (right) and **2.74 m** of translation (fiction). Surface
    normals are now collected and eigen-decomposed and an unobservable direction
    is refused by name. Then A7's inlier/RMS gate unmodified. Then a magnitude
    bound of 0.60 m / 6°, set where the physics is: run with a generous
    1.5 m / 20°, ICP produced a 0.97 m / **17.0°** "closure" on scan-020 with
    77.8 % inliers whose local mismatch genuinely improved 77 cm → 12 cm and
    which blurred the whole map by 8.6 %. Locally right, globally a fold. Then a
    whole-map crispness check, asked only where the walk actually painted a
    place twice (on a single-lap loop, occupancy carries no signal and the gate
    abstains rather than voting on no evidence — otherwise it would veto every
    good closure there is).

    Every decision, including every refusal and which gate did the refusing, is
    logged as a stable string.

42. **Shipped. `ColorMode.COVERAGE`, live view only, never written to the
    container — and it is the one value in that enum whose ordinal must never
    cross the C ABI.**
    Every other colour mode is a shader branch. Coverage is not: the renderer
    counts returns into a fixed 25 cm lattice anchored at the world origin
    (deterministic — the answer cannot depend on arrival order) and writes the
    tint into **its own GPU copy** of the vertices, asking the shader for plain
    RGB pass-through. The engine's PageStore — which is also the map cache that
    gets sealed into the `.lscan` — is read and never touched.
    A fully covered point keeps its own shade **byte for byte**, so a
    well-scanned room in coverage mode is indistinguishable from the
    grayscale/intensity view you asked to be the default; thin regions are pulled
    toward amber (not red — red is the failure colour and "thin here" is not a
    failure) and brightened, because a thin region is by definition few points
    and a dim tint on few points is invisible. 25 cm comes from the sensor: a D6
    puts ~40 returns into that much wall in one pass at walking speed, so one
    pass and several passes land on opposite sides of the ramp. Toggle in the
    Capture settings sheet; not offered in Review, because "where have I not been
    yet" is a question about a walk in progress.

43. **Shipped, default ON, one switch in Settings.** Two firm buzzes for
    tracking degraded (repeating every 4 s while it lasts — it is a state you
    must act on), three short urgent ones for a section break (on the event,
    never on the level), one long soft one for moving too fast (every 3 s). They
    differ by COUNT rather than by length or timbre, because through a pocket at
    walking pace that is the only dimension that survives.
    All the deciding is in `:core` and unit-tested, because everything hard about
    a cue is timing: the first tick is always silent (ARCore is degraded at
    Start, and buzzing before you have taken a step is how a default-ON feature
    gets switched off); one cue at a time with the loser keeping its debounce;
    disabled cues still advance the state so switching them on does not replay a
    backlog; reset per session; and nothing fires unless a recording is running.
    The decision is made at the same evaluation point as the on-screen "moving
    too fast" hint, so the two can never disagree. `VIBRATE` is a normal
    permission (no prompt mid-capture) and every call is posted off the main
    thread.

44. **Shipped, and it composes with item 38's jump to Projects rather than
    fighting it.** Points (the FIXED count — see below), duration, metres walked,
    `.lscan` size, sections, tracking drops, points per metre, average rate, and
    one word: **GOOD SCAN / USABLE / RESCAN**, with a sentence naming the worst
    thing about the scan.
    Every threshold is a consequence of something measured. Sections: a break is
    ARCore relocalizing, so everything after it is in a different world frame —
    1 clean, ≤3 usable, >3 unfixable. Tracking drops: those points are EXCLUDED
    by the assembler, so every drop is a hole. Points per metre: your rig
    resolves 1,453 points/s, so points-per-metre is walking speed in disguise and
    does not depend on the room's size — 800/m is about 1.8 m/s, 400/m about
    3.6 m/s. A tripod scan is not punished for having no path.
    `sealedProjectId` is untouched (still `replay = 1`, so an Activity recreation
    mid-seal still recovers the navigation); the screen HOLDS the id until you
    dismiss the card. Sealing and jumping in the same instant would have shown
    the card for one frame.

45. **All three parts, and (c) is the number you asked for.**

    (c) **A 2.4° trim error — which is exactly what scan-020 was captured with,
    at the gate's own 2.5° ceiling — paints the same overhead feature 13.1 cm
    apart between the outbound and return legs of an out-and-back walk, and the
    split REVERSES with the walk direction.** Measured through the production
    assembler, not derived:

    | trim | split at 1.66 m | scaled to 3 m |
    | ---: | ---: | ---: |
    | **2.4°** | **13.1 cm** | **23.6 cm** |
    | 1.4° | 9.0 cm | 16.3 cm |
    | 0.8° | 3.7 cm | 6.6 cm |
    | 0.5° | 1.0 cm (the fixture's floor) | 1.8 cm |

    That is "current scan right when i go forward but when i turn around the
    scan position shifted", to the centimetre. **Two corrections to the ROUND 10
    guess, both of which matter:** it doubles a FEATURE and does not thicken a
    wall (the displacement is perpendicular to each return's own ray, so on a
    wall seen square-on the points slide along it and every wall-flatness metric
    this project has is blind to it); and only the component about the phone's
    RIGHT axis reverses — the component about the phone's UP axis displaces the
    whole room by 6.5 cm at 2.4° and displaces it the same way both ways, which
    is an error nobody ever reports. A one-way walk hides all of it: the same
    2.4° paints one beam, 6.1 cm out of place, with its depth spread unchanged.

    (a) **The hold-still ring.** Tap Re-zero and the mount chip becomes a
    progress bar. Same gate underneath — the ROUND 8 sampler, unchanged — asked
    ten times a second instead of once per tap, so it fills while the hold is
    good and **empties the instant it is not**. Your 0.4.0 log has seven MOVING
    refusals in forty-four seconds; a refusal is a verdict on a moment you have
    already finished, and a ring is something your hands can learn from.
    **One honest correction to the brief**: "keep refining toward ≤0.8° spread"
    is not achievable, because the spread measures the JITTER of individual
    ARCore frames and holding longer does not reduce it. What holding longer
    improves is the accuracy of the MEAN, which is what actually gets stored —
    and rather than model that, the app MEASURES it: it splits the hold in half,
    averages each half separately and reports the angle between the two answers.
    "Improving… 1.4°" is that number, it needs no assumptions, and it catches the
    slow correlated wander a textbook standard-error argument would miss. The
    trim is then averaged over the whole hold (~180 samples at 6 s) rather than
    over the gate's one second (~31).

    (b) **Auto-refresh at Start.** If the trim is older than 10 minutes or came
    from a previous app run, AND the rig is genuinely still right now, it is
    re-taken silently and logged. If it is not still, the old trim stays and an
    amber note says how old it is — refusing to Start would be worse, but a
    stale reference entering a scan mentioned nowhere but a log read afterwards
    is what this closes.

**Plus the ROUND 10 backlog item: the point count you were shown was ~2× the
truth, and it is fixed.** `POINTS_AVAILABLE` fires once per page-append per
stream and has carried the stream id since B2; the counter summed the count and
ignored the id, so during a D6 capture every return was counted once as a raw
sensor-frame preview point and again as a resolved map point. It now reports the
resolved map (raw fan points that never found a pose are not in the room) and the
log carries both halves — `points=293166 (map=293166 raw=293524 other=0)` — so
the next field report needs no re-derivation. This number also feeds
`pointCountEstimate` in the sealed manifest, the HUD and the summary card, all of
which were ~2× as well.

## Round 12 — owner field test of 0.7.0 (2026-08-18, scan-025/026/028 + log 1418)

Owner verdict, walking at normal pace: **"quality not so good, still shift."**

46. **The shift is real, it is NOT the mount trim, and ROUND 11's prediction is
    refuted by the owner's own A/B pair.** scan-026 (`spreadP90 0.44°`) and
    scan-028 (`spreadP90 2.40°`), same room, four minutes apart. Diagnose with a
    metric that works at walking pace — the existing wall-probe one selects
    ZERO probes on both — and adjudicate the trim hypothesis by resolving each
    capture's bytes with the other's mount extrinsic.

47. **The mount-trim quality number in the container is not an accuracy figure
    and is being read as one.** `spreadP90Deg` is a dispersion over whatever
    hold length was averaged, so a 1 s hold and an 8 s hold produce
    incomparable numbers. Store the split-half repeatability the refiner already
    computes, gate the whole-hold spread, and never let the auto-recapture at
    Start replace a better trim with a worse one.

48. **A capture must not start on a tracker that has not settled.** scan-025
    took a 2.015 m step in 33 ms, 6.9 s after Start, with ARCore reporting
    TRACKING and quality GOOD; scan-028 the same shape at 3.7 s.

49. **The summary card claimed more than it measured.** It graded scan-026
    GOOD SCAN on counts alone, and scan-026 is the worst-aligned of the three
    captures examined.

### Resolution — 2026-08-18 (0.7.1)

46. **Adjudicated, and outcome (b): both clouds shift and the trim is not why.**

    **The premise was broken before the experiment started.** The two "very
    different" trims are **1.33° apart**. scan-026's 0.44° is a one-second
    dispersion over 34 samples; scan-028's 2.40° is an eight-second one over
    244. ROUND 11's own notes say holding longer does not reduce the spread —
    and then shipped that spread as the container's only quality field.

    **A ruler had to be built.** `post::measure_map_consistency` (new,
    hand-rolled, no Eigen, bit-identical under point reordering): split the
    capture into 8 s windows, and for every pair that filled the same 25 cm
    cell, measure how far the later pass lands from the earlier pass's own
    plane **along that plane's normal**. Along the normal is the point — a D6's
    returns slide freely along a wall, which is exactly why every flatness
    metric this project owns is blind to the error the owner reports.
    `engine_cli --d6-selfcheck` runs it; `--d6-dump` exports points with their
    own timestamps for analysis outside the tool. Proved against injected truth
    in `test_round12_map_consistency.cpp`: 20 cm of slide ALONG a wall reads
    **0.00 cm** while 20 cm perpendicular reads **20.0 cm**, and a one-pass map
    returns *not measurable* rather than a reassuring zero.

    **The numbers.** Surfaces re-painted 8 s apart disagree by:

    | capture | trim `spreadP90` | walked | disagreement | floor |
    | --- | ---: | ---: | ---: | ---: |
    | scan-020 (5.3 cm/s crawl) | 2.40° | 10.8 m | **0.70 cm** | 0.29 cm |
    | scan-026 | **0.44°** | 15.3 m | **5.26 cm** | 0.99 cm |
    | scan-028 | **2.40°** | 15.8 m | **4.45 cm** | 0.70 cm |

    The capture with the *good* trim is the *worse* of the two. And the
    decisive experiment — re-resolve each capture with the OTHER one's mount
    extrinsic, the trim hypothesis with nothing else changed — moves scan-026
    from 5.26 to 5.00 cm and scan-028 from 4.45 to 4.21 cm, with occupied 3 cm
    voxels changing by **0.15 %**. Both get very slightly *better*, which is
    what noise looks like and is impossible if either trim were correct.

    **Three more suspects tested and killed.** A lidar↔pose clock offset:
    swept ±100 ms on both captures — flat to 7 %, and the two minima disagree
    in sign (ROUND 10's null result was measured on the one capture where
    100 ms is 5 mm; it now holds at six times the speed). Live-vs-offline
    divergence: the cached `map.bin` and an offline re-resolve have identical
    extents and voxel counts within 0.02 %, so what the owner looked at is what
    is on disk. ARCore orientation: it tracks the phone gyro at **r = 0.9994**,
    and rewriting the pose stream with a gyro complementary filter at time
    constants from 0.3 s to 30 s produced **no improvement at any of them** —
    recorded as a falsified hypothesis so it is not re-tried from reasoning.

    **What it is.** Local geometry at walking pace is *excellent*: the
    measurement's own floor — one 8 s window against itself — is 0.70–0.99 cm,
    which retires every per-return error class (fan formula, byte-position time
    slicing, sample cadence, mount extrinsic, pose interpolation). What grows is
    the disagreement *between passes*. **Both captures are loops the app never
    closes**: scan-026 ends 0.52 m from its start after 15.3 m, scan-028 0.80 m
    after 15.8 m. ROUND 11's loop closer finds the revisit on both and refuses
    both, correctly — the rotations ICP proposes (14–19°) are impossible against
    the gyro cross-check (≤1.1° over 16 s), which is precisely the pushbroom
    null-space wander its observability gate exists to catch. **The gate is
    right and the closure is therefore unavailable.** That is the honest state
    and it is the next round's headline.

    **Also fixed while here:** `--d6-timesweep` grew `--probe-min-points` /
    `--probe-elongation`, because its defaults (200 points in a 0.5 m cell)
    silently produce zero probes and a blank report at walking pace. Relaxed to
    40/6 it reads 8.66 cm of wall thickness on scan-026 against scan-020's
    4.86 cm.

47. **Root-caused to three defects, all fixed, all from the owner's own
    `scan-028` and log.** `sampleCount = 244` at 30 Hz is 8.1 s — exactly
    `DEFAULT_MAX_HOLD_MS`, so the hold ran to its timeout without converging.
    (a) The whole-hold mean was stored with its dispersion compared against
    nothing: the gate judged the trailing second, the refiner then stored an
    eight-second mean. It now must clear the same gate, and falls back to the
    (passing) one-second trim when it does not. (b) The split-half accuracy was
    computed for the ring and thrown away; `MountTrim.stabilityDeg` now reaches
    DataStore and `project.json` (defaulted to −1 so every 0.7.0 trim still
    decodes), with `accuracyDeg` / `accuracyIsPoor` / `qualityRank` as the API.
    `spreadP90Deg` keeps its name and meaning — ROUND 8's rule. (c) The
    auto-recapture at Start replaced the incumbent unconditionally, so a 0.35°
    guided hold could be overwritten by a 2.49° one-second sample — and because
    `fromPreviousRun` alone marks a trim stale, that path runs far more often
    than "10 minutes" suggests. It now compares `qualityRank` and keeps the
    better, saying which and why in the log. Two smaller ones from the same
    read: the ring reported `gate=true` on a hold that had not happened
    (`holdMs=0 samples=1` appears twice in the owner's log), and `evaluate()`
    did not filter `tracking` while `capture()` did.

48. **Fixed — `TrackingWarmup`, and the tracking flag was not the answer.**
    ARCore reported TRACKING with pose quality GOOD across every one of those
    jumps; all three containers decode with `tracking_lost = 0`. It does not
    report re-anchoring as a failure, it just moves. So the gate is a property
    of a WINDOW: two seconds of poses with no step a person could not take,
    reusing `PoseSectionTracker`'s own thresholds so the two can never disagree.
    It **waits and never refuses** — four seconds, then Start proceeds with an
    amber note saying why. A Start that can refuse is ROUND 10 item 38 arriving
    by another road.

49. **Fixed, in three ways that are all about not claiming more than was
    measured.** The GOOD sentence now ends *"Coverage checks passed; alignment
    is not measured on the phone"*. A mount trim measured worse than 1.0° caps
    the grade at FAIR with a named reason (ROUND 11 measured 1.4° = 16.3 cm of
    doubled feature at 3 m). And the walk's return-to-start gap is reported
    **under** the grade, never as part of it, with its condition attached: *"if
    you finished where you began, that gap is tracker drift and it is the
    largest error in this scan"* — because the app cannot know whether the
    operator meant to finish where they started, and quietly assuming it would
    be the same unearned confidence that produced item 49.

46. **Owner question (2026-08-18): drop ARCore/rear camera, IMU-only?** Answer on record:
    pure IMU dead-reckoning is physics-barred (double-integration, meters of error in
    30-60 s on phone MEMS — 10x worse than current drift). The real architecture: demote
    the camera from single-point-of-failure to one vote of three — ARCore primary while
    healthy; during degradation windows BRIDGE with (a) gyro heading (proven <1.1°/16 s),
    (b) pedestrian dead reckoning (step detect + stride, ~1-2% of distance), (c) D6
    scan-to-scan matching (fan-plane self-odometry; the reference video rig was
    camera-less). Bridged gaps replace section breaks; stitching becomes safety net.
    Scope for ROUND 14, shaped by round 13's keyframe verdict.

47. **Owner directive: Do Not Disturb during capture.** At capture start, enable DND
    (NotificationManager interruption filter; needs ACCESS_NOTIFICATION_POLICY granted
    via Settings — graceful ask-once flow, degrade politely if refused) and restore the
    user's previous filter at stop/seal — always, including crash/abandon paths. Note the
    physics: a notification buzz shakes the IMU and camera mid-measurement — this is a
    plausible contributor to IMPOSSIBLE_STEP breaks, not just UX. Our own cue haptics
    must still fire under DND (app-local Vibrator is unaffected). Log DND state at
    session start so field captures record whether the walk was protected.

### Resolution — 2026-08-18 (0.8.0, round 13)

**The headline: a section break is not a tracking failure. It is ARCore
re-anchoring, and the frame change it applied is written down in the pose
stream.**

Measured against the owner's own recorded 400 Hz gyro, over the same 33 ms as
each break:

| break | ARCore pose jump | gyro over the SAME 33 ms |
| --- | ---: | ---: |
| scan-030 #1 | 0.78 m / **13.53°** | 0.23° |
| scan-030 #2 | 0.97 m / **11.55°** | 1.16° |
| scan-030 #3 | 1.23 m / **11.58°** | 0.94° |
| scan-030 #4 | 1.06 m / **8.08°** | 0.49° |
| scan-029 #1 | 1.05 m / 0.54° | 0.44° |

The phone did not rotate. And there is **no ramp**: the rolling 1 s
ARCore-vs-gyro disagreement is 0.049° / 0.051° / 0.069° (median, 028/029/030)
and stays at 0.04–0.21° in the four seconds before every break, right up to the
frame it happens. Tracking does not degrade and then fail; the world frame
snaps. In a small flat walked in loops ARCore will keep recognising places and
keep snapping, and walking more slowly will not stop it.

**Four hypotheses tested and killed, each with the measurement that killed it.**

* **Camera occlusion by the D6/bracket — REFUTED, and this is the owner-hardware
  answer: the bracket does NOT block the rear camera.** scan-029/030 carry no
  keyframes (`captureCameraKeyframes=false` since ROUND 11 hid colorize), so the
  test ran on scan-020's 403 frames from the same rig and mount. Per-pixel over
  all 403: **not one pixel is dark in more than 80 % of frames**, and only 1.6 %
  are dark in more than half — and those form a smooth corner gradient
  (vignetting plus a shadowed floor), not a hard-edged occluder. A bracket in
  frame would read as a stationary silhouette at ~100 %. **No bracket redesign is
  indicated.**
* **Puck vibration blurring the camera — REFUTED, and backwards.** Gyro RMS in
  the 8–12 Hz D6-spin band: scan-020 2.82°, scan-026 2.47°, scan-028 2.63°,
  scan-029 2.21°, **scan-030 1.75°**. The capture that broke four times has the
  *least* vibration of the five.
* **CPU starvation from `liveSlam=true` — REFUTED at the source and in the
  data.** `Impl::on_page_update` forwards to the LIO only when
  `u.stream == StreamId::kLidarMid360`, so on a D6 the odometry thread is
  constructed, blocks on a condition variable and is handed nothing; the map
  store is the caller's, so not even an allocation. And the streams agree:
  scan-030's D6 chunk cadence (6.3 ms median), pose cadence (33.3 ms) and IMU
  cadence (2.5 ms) are identical to scan-028/029's, with **zero** pose gaps over
  200 ms against one each on the other two. The `liveSlam=true`/four-breaks
  coincidence is n = 1 with a refuted mechanism.
* **Turning too fast — explains two of four, not the rule.** Net rotation in the
  2 s before each scan-030 break: 2.5°, 8.9°, **133.5°**, 51.5°. Breaks 3 and 4
  follow a real 135°-in-2 s spin; breaks 1 and 2 follow almost no rotation at
  all. Instantaneous gyro rate at every break was 8–37 °/s, below the capture's
  own p90 of 38 °/s.

**One finding is ours, and it is actionable.** Every high-frequency vibration
burst in all three captures maps **1:1 onto one of the app's own logged cues**
(10 bursts / 10 cues on scan-030, 1/1 on scan-028 and scan-029) — so **no
external notification fired during any of them**, and item 47 is hygiene rather
than the explanation. But scan-030's fourth break at t = 27.63 s came **0.51 s
after the 130 ms haptic this app fired for the third break** at t = 27.12 s, and
it is the only one of the four with any high-frequency energy in the half second
before it (z = 178 against 2.0 / 7.3 / 10.2). Buzzing the phone at full
amplitude while ARCore is re-establishing itself is a self-inflicted risk.
**Backlogged** with the fix named: hold the section-break cue until the tracker
has re-settled, and drop its amplitude.

**What was built.**

* **`post::stitch_sections`** (`slam/post/section_stitch.{h,cpp}`, hand-rolled,
  no Eigen, deterministic) — the correction is **analytic, not a search**:
  `T_k = pose_after · pose_before⁻¹` is the frame change ARCore applied, so
  pushing section *k* through it lands it in section *k+1*'s frame, and the
  per-section correction composes to bring everything into the last (most
  recently re-anchored) frame. Wired into `D6ResolvePipeline` behind
  `stitch_sections` (default OFF — same "replay == capture" doctrine as
  `close_loops`) and it runs **before** loop closure, correcting the trajectory
  as well as the cloud, so the closer finally sees one walk instead of five.
  New `engine_cli --d6-stitch`.
* **The refinement solves for translation only, with rotation frozen at `T_k`.**
  That is ROUND 12's open item answered: ICP's 14–19° proposals were always the
  pushbroom null space, the rotation was never the unknown, and with three
  unknowns instead of six **the system matrix `Σ wᵢnᵢnᵢᵀ` *is* the
  observability** — λ_min/λ_max, no proxy needed. On scan-030 it reports 0.05 /
  0.09 / 0.42 / 0.65 and declines three of four seams **by name**.
* **`post::check_mount_consistency`** (owner item 48) + `engine_cli
  --d6-mountcheck`.

**The result on the owner's captures**, first six seconds and whole walk:

| | scan-028 | scan-029 | scan-030 |
| --- | ---: | ---: | ---: |
| sections | 2 | 2 | **5** |
| trajectory vertical extent, before → after | 0.167 → **0.102 m** | 0.637 → **0.359 m** | 0.820 → **0.271 m** |
| start→end gap, before → after | 0.799 → 0.576 m | 0.724 → **0.353 m** | 0.828 → 0.616 m |
| map self-consistency @8 s | 4.45 → 4.45 cm | 6.00 → **5.72 cm** | 5.85 → 5.83 cm |

**The vertical extent is the proof, and it is the only number here that does not
come from the same measurement that produced the correction.** The operator
walks on a flat floor. Applying `T_k` forward flattens scan-030's trajectory
from 0.82 m of vertical wander to 0.27 m — a phone moving in a hand. Applying
the inverse takes it to 1.55 m. The sign is not a matter of opinion.

**And an honest negative: `--d6-selfcheck` is nearly blind to a section break.**
It compares 25 cm cells filled twice, and two sections a metre apart fill
*different* cells — so there are no pairs to compare and the ruler reports
nothing wrong. That is why every geometric metric this project owns scored
scan-030 as ordinary while the owner's eyes saw a wrecked map, and it is why the
8 s number barely moves above. Stitching makes the sections overlap, which
*creates* cross-section pairs carrying ARCore's residual drift — so a few of the
longer-separation buckets get slightly worse while the map gets dramatically
better. The across-seam mismatch is the number that speaks: on scan-029 the
refinement took it from 14.65 cm to 12.56 cm.

48. **Built, and the check the brief asked for is not the check that works —
    the honest version is.** A "fan-vs-gravity" test cannot detect a rotated
    puck: every D6 return leaves the fan formula with `z == 0` exactly, so the
    returns lie in the *assumed* plane by construction and the test would
    compare an assumption with itself. What is observable is where the returns
    LAND. A vertical fan on a phone held at ~1.4 m paints floor to ceiling and
    **nothing can be further from the sensor vertically than the room is tall**.

    | capture | median per-revolution vertical extent | returns >2.5 m above/below sensor | median range |
    | --- | ---: | ---: | ---: |
    | scan-020 | 2.71 m | **0.00 %** | 1.29 m |
    | **scan-026** (owner rotated the puck) | **6.15 m** | **19.49 %** | 1.54 m |
    | scan-028 | 2.64 m | **0.00 %** | 1.58 m |
    | scan-029 | 2.59 m | **0.00 %** | 1.55 m |
    | scan-030 | 2.60 m | **0.00 %** | 1.59 m |

    Judged on the **first six seconds** only, which is the point. Exactly zero
    on four good captures and 19.5 % on the bad one at the *same median return
    range*, so this is not a taller room — it is the same ranges arriving where
    no room is. The gate is the impossible-elevation fraction (2 %, two orders
    of margin either side); the extent is supporting evidence that cannot fire
    alone, because a genuine atrium exceeds it honestly. It **warns and never
    refuses**. Remaining seam, named rather than hidden: the phone-side wiring
    needs an additive C ABI call (ABI 10) + JNI + the cue, exactly as ROUND 12
    scoped for `--d6-selfcheck`; the measurement, the gate and the proof are
    shipped and the CLI runs them.

49. **The card says what to DO, and the sentence that described the mechanism
    was wrong.** "N sections — tracking restarted too many times" is not what
    happens; the gyro says the tracker re-anchored. `ScanSummary.gradeReason`
    now names re-anchoring, says the pieces are about a metre apart *until you
    Process this scan*, and a new `nextWalkAdvice` line — separate on purpose,
    because the grade describes the walk that happened and this describes the
    next one — tells the operator to uncover the rear camera, add light, and
    slow through turns. A GOOD scan is given nothing to fix.

47. **Shipped.** `CaptureFocus` (`:core`, every decision, unit-tested) +
    `DoNotDisturbGuard` (`:app`, framework only). `INTERRUPTION_FILTER_PRIORITY`
    and deliberately not ALARMS or NONE: an interruption filter governs
    *notifications*, a foreground `Vibrator.vibrate()` is not one and is
    unaffected, but `ToneGenerator` plays on `STREAM_NOTIFICATION` — which
    ALARMS would mute, silencing the app's own audio cue. PRIORITY is the
    weakest filter that does the job. It engages only from
    `INTERRUPTION_FILTER_ALL`, because taking over a filter the user already set
    would mean *restoring a weaker one* at stop — this feature turning somebody's
    DND off. Restore is skipped if anything moved the filter mid-capture (a
    bedtime rule, the user, a work profile): that is a newer decision than ours.
    Released on Stop, on a failed seal, and in `onCleared` (the abandon path),
    all idempotent; `restoreOrphaned()` covers process death. Never blocks
    Start: without the grant the walk runs and the session-start log line
    records `dnd=unprotected-no-permission`, so a field report says whether the
    walk was protected without anyone remembering. Default ON, one Settings
    switch.

**Two shipped 0.7.1 bugs, and they were ONE bug in three places.**
`"a %f" + "b".format(x)` does not format `"a" + "b"` — a method call binds
tighter than `+`, so `.format()` applied to the last literal fragment only.
(A) The seal summary's `pathM=%.1f sections=%d drops=%d ptsPerM=%.0f` was never
substituted and printed verbatim; (B) the six arguments were then consumed by
the two `%s` that *were* in scope, so **`trimAccuracyDeg=15.99` was metres
walked and `loopEndGapM=2` was the section count** — which is why it sat beside
a stored `stabilityDeg=0.39` and looked like a unit error. Nothing was wrong
with the trim, and the summary **card** was never affected (it reads the
`ScanSummary` fields directly). A sweep of the same pattern across the whole app
found two more, both user-facing and both fixed: the IMPOSSIBLE_STEP section-
break sentence has been telling operators *"pose stepped %.2f m / %.1f°"*, and
the mount-calibration screen *"Pattern size: %.2f x %.2f m"*.

### Resolution addendum — 2026-08-18 (0.8.0): the Process button

The round's own report flagged this as ship-blocking and it is closed in the
same version. **`post::reprocess_d6_container` is the one implementation**;
the Android app reaches it through `processing_jni.cpp` (which links
`scanengine` directly, as it has since B6) and every other consumer through
**ABI 10** — `scan_lscan_reprocess_d6()`, `scan_lscan_has_stitched_cloud()`,
`scan_lscan_mount_check()`, additive, nothing existing changed.

**The corrected cloud is a derived file, not an overwrite.** It goes to
`processed/map_stitched.bin` beside a `processed/stitch.json` naming every seam
and its decision; `load_recorded_cloud()` prefers it at the one function every
reader already goes through, so Review draws the corrected map with no change
to any caller. After a full reprocess of scan-030, `lidar.bin`,
`poses_ar.bin`, `imu_phone.bin`, `map.bin`, `manifest.json` and `project.json`
are **byte-identical by checksum** — "replay == capture" still holds over the
raw data, and deleting the two derived files restores exactly what the phone
sealed. Verified on the emulator, not merely intended.

**What the owner sees**, produced by the real Android path on scan-030's actual
bytes:

> **5 pieces aligned — height spread 0.82 → 0.27 m.**
> The first piece moved 0.52 m to meet the last. The joins kept the camera's own
> correction — the walls here could not measure a better one. Your walk still
> ends 65 cm from where it began — that is the camera's own drift over the walk,
> and aligning the pieces does not remove it.

The height spread is the headline because it is the only number in the report
that is not self-referential; the end gap is stated and deliberately never sold
as an improvement.

**Two bugs came out of putting it on a device, and the second could not have
been found any other way.** `reprocess` assumed `processed/` existed (an
exported and re-imported `.lscan` has no empty directories). And the progress
callback crossed JNI as `CallBooleanMethod(obj, mid, fraction)` — **C varargs
promote a `float` to `double`, so a `(F)Z` method got eight bytes where it
expected four**; the call threw, the wrapper read the exception as "the caller
said stop", and the whole reprocess **cancelled silently** with `ran = 0` and no
error anywhere — only on the path that passes a progress callback, which is
every real one. `CallBooleanMethodA` with a typed `jvalue` fixes it. The
desktop harness over the same C entry point passed throughout.

48. **Also on the phone now.** `scan_lscan_mount_check` → `ProcessingRepository`,
    and it runs for free inside every reprocess, so the warning *"the lidar is
    not where the mount reference says it is — N% of returns landed at heights
    no room has"* reaches the operator. Silent on OK and NOT_MEASURABLE. Still
    sealed-container only; firing it live in the first seconds of a walk needs
    the statistic accumulated inside the pushbroom assembler.

**Plus the two quick fixes the diagnosis named.** The section-break haptic drops
from amplitude 255 to 150 and now imposes a **1.5 s quiet window in which
nothing buzzes at all** — the risk is to the tracker, not to the operator's
attention, so it covers every cue. (scan-030's fourth break arrived 0.51 s after
the third break's buzz, inside that window.) And `liveSlam` is **hidden on a D6
rig**: ROUND 10 established it gates the Mid-360's LIO, which a D6 capture can
never feed, and it was still the first thing anyone reached for when a D6 walk
went wrong.

Engine **584 cases / 2,503,041 assertions**, ctest 7/7 serial, werror clean;
`:core` **498**, `:app` **74**; emulator **19/19** with the native library
rebuilt from this round's sources, so `JNI_OnLoad` validated ABI 10 on device.
VERSION stays **0.8.0**.
