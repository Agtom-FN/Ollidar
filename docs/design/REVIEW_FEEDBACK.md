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

## Round 15 — owner-approved (2026-08-18): make breaks invisible, make output shareable

54. **Live re-anchor healing.** ARCore announces every re-anchor and the stitch correction
    is analytic (T_k = pose_after · pose_before⁻¹, round 13). Apply it the instant a
    break is detected DURING capture: live map stays continuous, no shattering, section
    bookkeeping still recorded for the offline refine. The deferred/softened cue then only
    fires for what the operator must actually act on. Determinism: live healing must not
    change what is RECORDED (raw streams untouched; healing is a live-view/live-map
    transform with provenance, replay still bit-identical).

55. **Auto-Process on seal.** Stop → seal → reprocess (stitch + refine + mount check)
    runs automatically with progress on the summary card; card shows POST-process
    numbers. Integrates with round 10's Stop→Projects flow (card first, then Projects).
    Skip gracefully when there is nothing to do (1 section, no warnings, fast path).

56. **Floor-plan export on the phone.** Engine A12 (walls w/ thickness, doors/windows,
    room areas, DXF R12 + PDF) has never been wired to Android. Project files already
    carry planSliceMinM/MaxM. Add "Floor plan" action in Review/Export: renders plan →
    share as PNG/PDF/DXF. The single most shareable artifact an indoor scanner produces.

57. **Self-check on the card.** Expose the round-12 ruler (measure_map_consistency)
    through the existing reprocess path (ABI additive) and put the number on the summary
    card in plain words: "surfaces repeat within X cm" (or "not measurable — single
    pass"). The honest accuracy figure, replacing vibes with measurement.

### Resolution — 2026-08-18 (0.9.0, round 15)

**Headline: the break is now invisible while you walk, the scan processes
itself when you stop, and the plan the app has been able to draw since A12 was
being cut on the wrong axis.**

54. **Fixed, and the direction is the opposite of the offline one.**

    ROUND 13's correction was already analytic — `T_k = pose_after ·
    pose_before⁻¹`, ARCore's own re-anchor written down in the pose stream —
    and everything needed to apply it live was already on the phone. Offline,
    sections are brought into the LAST section's frame because that is the
    frame ARCore currently believes. **Live, the new frame is mapped onto the
    OLD one** (`C ← C · T⁻¹`), because the map already on screen is what the
    operator's hands are steering by: nothing drawn moves, and the points
    arriving after the snap land where they are expected.

    It is applied **before** the pose that announced the break is pushed
    (`CaptureArController.publishPose`), so not even one frame of shattered map
    reaches the display. `Engine::push_pose` now records the RAW pose and feeds
    the CORRECTED one to the interpolator, the densifier and the assembler.

    **Proof that the recording did not change** (`test_round15_live_heal.cpp`):
    the same synthetic capture — a ROUND 8 walk past a wall with a 0.89 m /
    11° re-anchor injected at t = 2.0 s — recorded twice, healed and unhealed.
    `streams/lidar.bin` and `streams/poses_ar.bin` are compared byte for byte
    after the stream header, plus a field-by-field check of the header itself
    (it carries `t_start_utc_ns`, the wall clock, which two runs cannot share);
    and the offline re-resolve of the two containers is **bit-identical,
    including the discontinuity**. `streams/map.bin` — the resolved cache
    `reprocess.h` already documents as a cache — is the one artifact that
    legitimately differs, and the test asserts that too, so it is a stated
    property rather than an accident.

    | | seam offset |
    | --- | ---: |
    | unhealed live map | **1.463 m** |
    | healed live map | **0.027 m** |
    | control (no break at all) | 0.007 m |

    The 2.7 cm residual is not slop: `T` is measured across a 33 ms interval in
    which the operator was also moving, so their own ~1° of gait yaw is inside
    it (11° injected, 12.0° recovered). That is the term ROUND 13 bounded and
    deliberately did not try to remove, and it is why the offline stitch — with
    submaps, refinement and a flat-floor referee — still has a job.

    **The cue now fires only for a break that could NOT be healed.** The engine
    refuses a bracket that cannot define a rigid transform (a pose the tracker
    disowned, a degenerate rotation, non-increasing stamps) and refuses it
    WITHOUT changing the accumulated correction; `CueConditions.sectionBreaks`
    is fed from `unhealedSectionBreaks`, not from the section count. Every
    break is still recorded, still goes in the manifest and still gets stitched
    offline.

55. **Done, and the ordering is the whole design.** Stop → seal → verify →
    **process** → card → Projects:

    1. Stop seals the container. Nothing about ROUND 10 changes: the card is
       what holds navigation (`CaptureScreen` navigates when `sessionSummary`
       goes null), so it is also the only place a progress bar can live without
       inventing a second modal.
    2. Processing starts **after** `projectStore.open()` has re-read the sealed
       container and **before** `_sealedProjectId.tryEmit`. Both halves matter:
       nothing that runs after a successful verify can lose the scan, and the
       navigation is emitted whatever processing does.
    3. It runs on `Dispatchers.IO` against the sealed **directory**.
       `scan_lscan_reprocess_d6` is handle-less and opens its own PageStore, so
       it shares nothing with the capture engine — which is what makes it safe
       for the tab to have already re-armed and for ROUND 14's
       `resetWorldFrame()` to be rebuilding the ARCore session on a new Start
       while it works.
    4. The card grows a line and a bar, then swaps to the POST-process numbers:
       sections joined, height spread, the mount warning if the watchdog fired,
       and item 57's repeat-accuracy line.

    **Fast path:** one section and no warning skips the stitch and the second
    cloud write but still runs the ruler — a clean capture is exactly the one
    whose owner will believe the number.

    **Failure never loses a scan.** Any throw, any null, and any `ran == false`
    (ROUND 13's silent-cancel signature) all land on *"Processing failed — the
    scan is saved. Open it and tap Process to try again."* Dismissing the card
    does not cancel a run in flight.

56. **Wired — and the plan the app could already draw was being cut on the
    wrong axis.**

    `processing_engine.cpp` hard-coded `plan::UpAxis::kZ`. That is right for a
    Mid-360, whose session is gravity-aligned into a Z-up frame, and wrong for
    every D6 capture this project has ever taken, because a D6 session's world
    frame is ARCore's, where **+Y is up**. A Z cut takes a 50 cm VERTICAL slab
    through the room and produces a plausible-looking drawing of a wall: it
    yields walls, it yields a scale, and it can never close a room. Measured on
    scan-033 the Z cut holds **9,211 points over an 8.4 × 2.8 m "footprint"**
    (2.8 m being the ceiling) against **21,143 over 14.5 × 9.4 m** for Y.
    Fixed, and the new path takes the axis as an option and defaults it to +Y.

    New: `post::floor_plan_from_lscan()` (container in, plan out — prefers
    `processed/map_stitched.bin`, then the live cache, then a re-resolve),
    `plan::write_plan_png()` (a hand-rolled deterministic PNG writer: stored
    deflate blocks, no zlib anywhere in the tree), ABI 10 → **11** with
    `scan_lscan_floor_plan`, `nativeProcFloorPlan`, and a Review → Floor plan
    screen that previews the PNG and shares PNG/PDF/DXF through MediaStore
    Downloads like every other export. (The old plan export called
    `ShareTargets.shareFile` on a file in private storage only — the exact
    "file went nowhere" failure ROUND 7 fixed everywhere else.)

    **And the honest part.** A COIN-D6's fan is vertical and its 10 Hz
    revolution paints a LINE. A 50 cm horizontal band is thin evidence even on
    the best walk: scan-033's plan slice holds 21,143 of 220,438 points in
    1,327 cells over a 14 m room, which fits 9 walls / 15.0 m and **closes no
    room**. So the pipeline has a stated ladder — `{3,2,1}` points per cell,
    then coarser grids — and a second source of geometry, the **floor map**: a
    5 cm downward projection in which a cell counts as structure only when its
    returns span ≥ 0.60 m vertically. A wall is hit from skirting to coving as
    the fan sweeps past; a floor, a tabletop and a sofa back are not. That one
    test turns an unusable grey blob into a room outline. On scan-033 it gives
    **10 walls / 24.9 m from 99,373 points in 679 structure cells**, tracing
    three sides of the flat and an interior partition, over a 14.65 × 9.80 m
    extent at 122 px/m. When nothing can be fitted at all the result is
    `MODE density` and the picture is the returns themselves at a stated
    scale — labelled on its own face, because an empty sheet called "floor
    plan" is a lie and a scaled picture of real returns is a measurement.

    Across all seven of the owner's captures the plan produces walls; **only
    scan-029 closes a room, and that room is 1.60 m²**. Honest one-liner:
    *this is a good, scaled floor MAP and a weak floor PLAN — the outlines are
    real and measurable, the room polygons are not there yet.*

57. **Done, and "not measurable" is an answer.** ROUND 12's
    `measure_map_consistency` is now computed inside
    `reprocess_d6_container()` — free, since the cloud and its point times are
    already in hand — carried on `ReprocessReport::consistency`, written into
    `processed/stitch.json`, and exposed through a new
    `scan_lscan_reprocess_d6_ex()` plus six appended `double[]` slots (16–21).
    Appended, not inserted, and `StitchResult.fromNative` still accepts a
    16-long array, so a native library that has not been rebuilt reports no
    self-check instead of reading a slot that is not there.

    The card says **"Surfaces repeat within X cm (measured over N s; this
    measurement's own floor is Y cm)."**, or, when the scan never covered the
    same surface twice, **"Repeat accuracy: not measurable — nothing in this
    scan was covered twice. Walk past the same wall again and it can be
    measured."** A single pass down a corridor paints nothing twice, and a card
    that printed 0.0 cm for that would be claiming a perfect map on no
    evidence.

    On the owner's fixtures, every one measurable: **0.70 cm** (scan-020, the
    crawl), **1.74 cm** (scan-035), **1.97 cm** (scan-033), **2.45 cm**
    (scan-034), **4.45 cm** (scan-028), **5.26 cm** (scan-026), **5.85 cm**
    (scan-030), **6.00 cm** (scan-029) — against measurement floors of 0.42 to
    2.17 cm. Unchanged to the second decimal from what ROUND 12 and ROUND 14
    measured: item 57 moved the number onto a card, it did not move the number.

VERSION **0.9.0**. Engine **603 cases / 2,509,102+ assertions**, ctest serial;
`:core` **538**, `:app` **85**. ABI **10 → 11, additive only** — ABI 10's
`scan_reprocess_options` and `scan_reprocess_result` are byte-identical, which
is exactly why the ruler arrives on a new entry point rather than as two more
fields on the old one.

## Round 14 — owner field test of 0.8.0 (2026-08-18, scan-033/034/035 + log 1909)

> *"The new scan is much better when i go around … but its not good with tilting
> and moving around the phone."*
> *"Does the origin and imu data offset zero every time when the capture start?"*

50. **Tilt/sweep robustness.** Quality collapses when the owner stands and
    sweeps the phone instead of walking it. Diagnose against scan-033 (the
    26.6 m walk, the best capture yet) and say what specifically degrades. Fix
    what software can fix; if part of the answer is technique, quantify it and
    surface it live — the too-fast cue keys on LINEAR speed. And `ptsPerM` is
    misleading for a standing capture (scan-034 graded on 50,124 points/metre
    because the path was 2.7 m).

51. **Origin / IMU zero at start.** The owner's direct question. Verify in code
    whether the ARCore session is recreated per capture or reused, whether the
    first pose is re-zeroed, whether densifier state is reset — and fix any
    state that leaks between captures.

52. **DND broken as shipped.** Every 0.8.0 session logs
    `dnd=unprotected-no-permission`; the round-13 "ask-once flow" never reached
    the owner. Root-cause and build the ask, a Settings status row, and a
    visible unprotected note. Never block capture.

53. **Mid-360 on Android.** scan-032 recorded `bytesIn=0`. Wire discovery over
    the Ethernet interface, deal with the phone's interface IP, and preflight
    the capture instead of sealing zero bytes.

### Resolution — 2026-08-18 (0.8.1, round 14)

**The headline: sweeping does not degrade the geometry. It starves the camera
of parallax — and 0.8.0 had been handing every new capture the previous
capture's map to fall back on.**

Every metric this project owns says the sweeps are as good as the walk:

| | scan-033 walk | scan-034 sweep | scan-035 sweep |
| --- | ---: | ---: | ---: |
| map self-consistency @ 8 s | 1.97 cm | 2.45 cm | 1.74 cm |
| (that measurement's own floor) | 0.99 cm | 1.03 cm | 1.40 cm |
| impossible-elevation fraction | 0.00 % | 0.00 % | 0.00 % |
| IMU densifier fallback rate | 31.3 % | 31.9 % | 31.8 % |
| resolved points per second | 1,985 | 2,001 | 2,006 |

**Four candidate mechanisms tested and killed.** *Pose lag under rotation* —
cross-correlating ARCore's per-frame rotation against the recorded 400 Hz gyro
puts the best-fit lag at **−5 ms**, i.e. none, on all three captures.
*Orientation fidelity at speed* — pooled per-frame ARCore-vs-gyro disagreement
is 0.031° p90 below 10 °/s and still only **0.221° p90 at 90–150 °/s**, which
is 1.16 cm at 3 m; there is no rate below 150 °/s at which the tracker stops
being faithful. *Densification fallback* — flat across all three, so it is not
rate-driven. *Intra-revolution smear* — at the D6's 10 Hz revolution the
owner's median sweep advances the fan **0.92–1.03° between revolutions against
a 0.90° within-fan pitch**, which is as close to isotropic sampling as this
sensor gets. **His sweep rate is fine. "Sweep slower" is not the advice.**

**What is different is parallax.** Over 1 s windows:

| | translation per degree turned | median linear speed |
| --- | ---: | ---: |
| scan-033 (walk) | **2.43 cm/°** | 25.9 cm/s |
| scan-034 (sweep) | **0.53 cm/°** | 4.6 cm/s |
| scan-035 (sweep) | **0.56 cm/°** | 3.9 cm/s |

A monocular visual-inertial tracker gets depth from translation. Rotating on
the spot changes every bearing and creates no baseline, so nothing new can be
triangulated and the tracker has to lean on the map it already has. **scan-035's
section break is that failure at full size: 1.631 m / 162.57° of pose change in
33 ms while the phone's own gyro integrated 1.56° over the same 33 ms.** A 104×
disagreement — round 13's worst was 13.53° against 0.23°.

51. **Answered, and it was a real defect. NO — the origin did not zero.**

    The ARCore `Session` was created once per PROCESS and only ever *paused*
    between captures. `CaptureArController.close()` existed and had **no caller
    anywhere in `src/main`**. So capture N+1 opened in capture N's origin,
    holding capture N's feature map and relocalisation database. The owner's own
    containers prove it: **scan-035's first tracked pose sits 7 cm from
    scan-034's last, 49 s and a Stop/Start apart**, and scan-034's sits 0.42 m
    from scan-033's. Three scans of one small flat, one accumulating map — which
    is precisely what a 162° relocalisation hypothesis needs to exist.

    Two more leaks are visible in artifacts the owner already has.
    **scan-033's manifest lists one sensor, scan-034's lists three and
    scan-035's lists six** — two bugs with one symptom: the record writer never
    cleared its sensor list between containers, and the phone-GNSS fallback
    registered a fresh RTK-rover device on every capture without ever removing
    the last one (scan-035 lists the rover as both id 2 and id 3). Both fixed. And the seal summaries read `drops=1`, `drops=2`, `drops=3`
    across those same three captures, while the pose streams show each of the
    three had exactly one tracking-loss episode of its own — the counter was
    accumulating, because `ArStatus.trackingLossEpisodes` was reset only in the
    `close()` nobody called.

    **Fixed.** Every Start now calls `CaptureArController.resetWorldFrame()`:
    close the session, build a fresh one, hand it back the renderer's existing
    GL texture, resume. `ArSessionGate` is what makes that safe from the render
    thread — `sessionCreated`/`resumed` both go false for the duration, so an
    in-flight `onFrame()` returns null instead of calling `update()` on a
    closing session, which is exactly the state machine round 6 built. It runs
    **before** the round-12 warmup gate, so the gate judges the session that
    will actually record. The cost is one the app was already paying: a
    pause/resume was re-acquiring tracking in ~0.5 s anyway — scan-034 and
    scan-035 each open with 14–15 recorded poses at exactly (0,0,0) with
    `tracking_lost = 1`. Engine-side, `start_session()` now clears the sensor
    registry, the `ExternalPoseSource` ring, the IMU densifier (bias, sample
    ring, stats) and the georef ENU origin.

50. **Fixed in four places, and one of them is a metric that was lying.**

    * **`ParallaxWatch`** (`:core`, unit-tested) — a rolling 2 s window that
      fires when rotation ≥ 20° carries less than 0.8 cm of travel per degree.
      Swept against the owner's own captures that threshold fires on 5.5 % of
      the good walk and 46.7 % / 39.7 % of the two sweeps, an ~8× separation; it
      is the only row in the sweep that catches BOTH bad captures at a
      comparable rate, so it is fitted to the technique rather than to the snap.
      It drives an inline hint and a new **lowest-priority** `PARALLAX_STARVED`
      cue at the gentlest amplitude with a 12 s repeat — every buzz is itself a
      shake of the tracker this cue exists to protect.
    * **The advice is "keep walking while you sweep", not "sweep slower"**, and
      the summary card now says so for a standing capture.
    * **`ScanSummary.isFromTheSpot`** — a capture over 20 s with under 5 m of
      path is judged on **points per second**, not points per metre. scan-034's
      card said 50,124 points/metre and the grader used it; it now reads 2,001
      points per second. The floors (1,000/400 per second) exist only to catch a
      puck that has stopped delivering — a fixed viewpoint cannot be told to
      sweep faster for more returns.
    * **The densifier's fallback accounting**, on the backlog since round 12:
      82 % of scan-034's 63,805 fallbacks had no reason recorded.

52. **Root cause: the ask flow was never built.** `DoNotDisturbGuard`'s API is
    complete and correct — `hasPolicyAccess`, `policyAccessIntent()`, the whole
    engage/restore machine — and **`policyAccessIntent()` had zero call sites in
    the repository**. Its own doc comment says "the caller shows this once";
    there was no caller. The manifest permission was declared, `engage()` ran on
    every Start and correctly returned `NO_PERMISSION`, and the log token was
    right. Compounding it, `CaptureFocus.note()` computed the explanatory
    sentence on every Start and **no composable ever collected `dndNote`** — it
    was a write-only flow, and it was wiped at seal as well. The Settings switch
    meanwhile read *"Needs Do Not Disturb access"*, stating a prerequisite it
    gave the owner no way to satisfy and no way to check.

    **Now:** a first-entry explainer (never at Start — a modal mid-walk is the
    worst possible interruption) that leads with the physics rather than the
    permission, with **Open settings** / **Not now**; asked exactly once and
    remembered, because declining is an answer. A Settings row that states which
    side of the grant you are on and re-reads it on every resume (the system
    screen returns `RESULT_CANCELED` whatever happens, so the grant must be
    re-read, never inferred). And a persistent amber note on the capture screen
    that **survives the seal** and clears only when the grant actually arrives.
    Capture is never blocked.

    **The one manual step, for the owner:** *when the dialog appears, tap **Open
    settings**, find **LidarScan** in the Do Not Disturb access list, and turn it
    on. Android does not let any app grant this for you.*

53. **Preflighted and diagnosed, not yet fixed — and the 0.8.0 advice was
    wrong.** scan-031 and scan-032 are two sealed scans of nothing, each
    followed 2 s later by `bytesIn=0` and a seal message telling the owner to
    re-seat a cable that was almost certainly fine. `connect()` returning
    success proved nothing: a Mid-360 connect is `Engine::add_device`, which
    constructs a driver and performs **no I/O at all**, and `start_session()`
    logs a driver that fails to start and carries on.

    The addressing is the likely fault. A Mid-360 **unicasts to a host IP it has
    persisted** — on the owner's unit `192.168.1.5`, which it also advertises in
    the heartbeat it broadcasts on UDP 56201. Android USB-Ethernet DHCPs, so the
    phone holds something like `192.168.1.100` and the packets are addressed to
    a machine that is not on the cable. **This cannot be fixed in software:**
    `EthernetManager`/`StaticIpConfiguration` are `@SystemApi` behind
    signature-level `MANAGE_ETHERNET_NETWORKS`. So `Mid360Preflight` (`:core`,
    unit-tested against the owner's real addresses) refuses the session before a
    byte is recorded and hands over the value to type:

    > **The Mid-360 sends its data to 192.168.1.5, and this phone's Ethernet is
    > 192.168.1.100. Nothing would be recorded.**
    > Settings > Network & internet > Ethernet > IP settings > Static.
    > IP address 192.168.1.5, netmask 255.255.255.0. Then come back and press Start.

    It checks addressing **before** heartbeat, deliberately: being told "no
    heartbeat, check the power" when the real fault is the IP is the
    wrong-diagnosis failure it exists to end. A new `[net]` log tag records the
    verdict, so a field report now carries the diagnosis — the owner's whole
    0.8.0 log contains no network line at all. The NO-DATA watchdog stays as the
    backstop.

    **Named and NOT fixed:** even with the addressing right, the SDK2 backend
    creates its own sockets inside the vendored Livox SDK and nothing binds them
    to the Ethernet `Network`, so with Wi-Fi up the kernel may still choose the
    wrong interface (`android/NOTES.md` §8 finding 3). The engine's C ABI has
    carried `mid360_prebound_fd` / `mid360_prebound_imu_fd` since ABI 5 and the
    Android capture path fills 3 of ~30 fields of `scan_device_config`; three
    Android-side comments claiming the ABI lacks these fields are stale. That
    rewire is the next round's work and it needs the owner's hardware to
    validate. **This is why the version is 0.8.1 and not 0.9.0: Mid-360 on
    Android is diagnosed, not delivered.**

Engine **589 cases / 2,503,884 assertions**, ctest 7/7 serial, werror clean;
`:core` **527**, `:app` **80**; emulator **19/19** with the native library
rebuilt from this round's engine sources. **Replay is bit-identical** —
`--d6-selfcheck` on all three captures returns the same self-consistency
(1.97 / 2.45 / 1.74 cm) and the same resolved point counts (220,445 / 135,820 /
124,817) before and after, because every change in item 51 is session lifecycle
and none of it touches offline resolve. VERSION **0.8.1**.

## Round 16 — owner field test of 0.9.0 (2026-08-18, scan-036/037/038/039 + log 2251)

> *"scan look ok but not much improved"*
> *"i want to see the path of mine showing in the pointcloud too for me to check
> if the scan is right"*
> *"for the merge process button the pop up window the upper cornor radius too
> larger and there are some tab and window show the same too."*

The session's three exported captures split cleanly into a win and a
regression. scan-036 (45 s) and scan-038 (108 s) both healed every break live —
one of them a 1.39 m / 26.64° re-anchor — auto-processed on seal, and measured
3.43 cm and 2.73 cm of self-consistency. DND held (`dnd=protected`) on all four.
And **scan-039 recorded no poses at all**: 51 s, 184,454 points, `pathM=0.0`, no
`poses_ar.bin` and no `map.bin` in the exported bundle, graded **FAIR**.

58. **POSE-LOSS REGRESSION — the headline.** Find why scan-039 recorded zero
    poses; the prime suspect is a race in the round-14 `resetWorldFrame`. Three
    layers are required, not one: (a) the race itself; (b) a POSE WATCHDOG
    during capture — recording on, lidar bytes flowing, no accepted pose for N
    seconds → buzz and an on-screen warning, the round-7 no-data pattern applied
    to poses; (c) the seal must be honest — a capture with zero poses is
    labelled 2D-only on the card and never "FAIR", and auto-process says WHY it
    cannot run. Reproduce the race if possible; if not, harden the ordering
    deterministically and say so.

59. **Trajectory in the point cloud.** Render the walked path inside the 3D
    cloud — Review (from the corrected poses after Process) and the live view
    (the healed trajectory). A polyline distinct from points, with start and end
    markers and a time gradient, toggleable, default ON, and drawn in the same
    frame as the cloud so healing and stitching move them together. On the
    floor-plan PNG too.

60. **Loop-end closure — claim the backlog lever.** Apply the gyro-locked,
    translation-only solver at loop ends. Offline only. Prove on scan-033
    (0.45 m gap), scan-036 (0.65) and scan-038 (0.52) with before/after
    self-check and loop gap; the round-11 false-positive gates stay in force and
    scan-034/035/039 must still refuse.

61. **UI polish.** (a) The process/merge popup's top corner radius is too large;
    match the app's design system and make the sheets consistent. (b) Audit for
    duplicated surfaces — one surface per function — and list what was merged.

62. **Sections count mismatch.** scan-038's seal said `sections=3` and
    auto-process said `sections=2`. Unify: the card shows one truth
    (post-process), the log carries both under distinct labels.

### Resolution — 2026-08-19 (0.9.1, round 16)

**58 — THE RACE WAS REAL, IT IS IN THE ROUND-14 FIX, AND THE APP DIAGNOSED IT
AND STARTED ANYWAY.**

Root cause, from the owner's own log rather than from a hypothesis. The five
world-frame resets in that session split two ways:

```
22:41:30.600 world frame reset        22:41:33.840 start gate: cleared  -> scan-036 OK
22:43:20.686 world frame reset        22:43:24.701 start gate: TIMED OUT blocker=NO_POSES
                                      -> scan-037, 4 x "cue: tracking_degraded", abandoned
22:44:23.495 world frame reset        22:44:26.526 start gate: cleared  -> scan-038 OK
22:47:10.106 world frame reset        22:47:14.129 start gate: TIMED OUT blocker=NO_POSES
                                      -> scan-039, 13 x "cue: tracking_degraded", 51 s, 0 poses
```

`resetWorldFrame()` returned **true** both times it failed — the session was
rebuilt and `resume()` succeeded — and then the camera never delivered a frame
to it. `publishPose` returns at its first line on a frame whose timestamp is
zero, which is what ARCore hands out before the first camera image, so not one
pose reached `motion`, the section tracker or `nativePushPose`. The container is
exactly what that predicts: `lidar.bin` and 899 KB of 400 Hz `imu_phone.bin`
present, `poses_ar.bin` and `map.bin` absent, `sectionBreaks` empty.

**The mechanism: `ArSessionGate.mayDrive` is a CHECK, and a check is not a
lock.** `onFrame` reads it at the top and then proceeds into `Session.update()`;
nothing stopped the main thread from calling `Session.close()` in the
microseconds after that check passed. `Session.close()` concurrent with
`Session.update()` is undefined by ARCore's contract, and on this Pixel it
leaves a camera that never binds to the replacement session.

Round 6 did not need a lock, and said so: the only lifecycle call on the hot
path was `pause()`, and shutting the gate first is enough to keep a LATER
`update()` off a paused session. `close()` existed with, in round 6's own words,
*"zero callers anywhere in `src/main`"*. **Round 14 gave it a caller on the
hottest path there is** — every Start — while the pose pump drives the session
at 60 Hz. The fix for the origin-leak bug created this one, and it is
intermittent because it is a race: two of five resets lost it.

Three layers, all shipped:

* **(a) The race.** `CaptureArController` grows a `ReentrantLock`. `onFrame`
  takes it with **`tryLock`** and yields the frame if it cannot have it — a
  render thread must never block on the main thread, and a few dropped frames
  during a rebuild is the right price. `resetWorldFrame` takes it and HOLDS it
  across close / create / re-bind / resume, so no half-built session is ever
  reachable from a GL thread; `pause()` and `close()` take it too, reentrantly.
  Deadlock-free by construction: only the main thread ever blocks, for at most
  one in-flight frame. Frames yielded are counted and logged
  (`framesYielded=`), so the mechanism is visible in a field log instead of
  being a claim in a comment.
  Plus **hardening by construction**, because the lock cannot be proved on an
  emulator with no ARCore: the rebuild retries once (`RESET_ATTEMPTS = 2`), and
  the start gate no longer walks past its own diagnosis — a timeout whose
  blocker is `NO_POSES` (as distinct from `NOT_TRACKING` / `IMPOSSIBLE_STEP`,
  which mean the tracker is alive and unsettled) now triggers ONE more rebuild
  and ONE more wait before recording is armed. It still never refuses to start;
  round 12's rule stands.
  **Stated honestly: the race is not reproduced.** ARCore is stubbed on the
  emulator, so there is no way to make `Session.update()` and `Session.close()`
  overlap in this environment. What is shipped is the mutual exclusion the
  concurrency requires, an explicit retry, and a gate that acts on the signal it
  was already printing.

* **(b) The pose watchdog.** `startPoseWatchdog()`, armed in the same breath as
  the round-7 no-data watchdog and built to the same shape. It fires only when
  **points are arriving AND no pose has been accepted for 3 s** — the exact
  scan-039 signature, and never when nothing is arriving at all, because that is
  the no-data banner's diagnosis and its instruction is different. Three seconds
  and not two: ARCore does go quiet through a hard turn, and a banner that cries
  wolf on a corner is a banner the operator learns to ignore. The counter is new
  — `CaptureArController.acceptedPoseCount` / `lastAcceptedPoseAtMillis`,
  incremented in `publishPose` after the ordering filter — because nothing
  measured "is the tracker alive right now": `posesPushed` counts only poses
  that reached an engine handle, and is zero before a recording starts.
  **No new buzz, and that is a decision.** The haptic for this condition already
  exists and already fired — `TRACKING_DEGRADED` is scheduled from
  `status.tracking == false`, and scan-039's log carries thirteen of them. The
  buzz was never the missing half; the operator felt it and had no way to know
  it meant *"this scan has no room in it"* rather than *"this corner is dim"*.
  A second cue on the same condition would also spend the round-13 cue budget
  twice, and round 13 measured a cue buzz causing the very break the next cue
  reported.

* **(c) The honest seal.** `ScanSummary.posesRecorded` (nullable) and
  `isTwoDimensionalOnly`. scan-039's numbers, run through the shipped grader,
  came out **FAIR**: 51 s with a 0 m path made it "from the spot", 3,578
  points/second beat every density floor, one section, no drops, and the only
  thing keeping it off GOOD was a 1.68° mount trim. It is now **POOR**, the card
  says **2D ONLY — NO ROOM**, and the sentence says the returns have no
  positions and that nothing on the phone can recover it. The verdict is FIRST
  in the grade's `when`, above sections and drops, because every other
  measurement is meaningless without a trajectory. Auto-process no longer
  attempts a run it knows will fail: it reports *why* — every stage needs the
  trajectory — instead of the round-15 "open it and tap Process", an instruction
  that could only ever fail again.
  The field is **nullable on purpose**, and the `?:` that is not there is the
  point: a rig with no AR controller (a Mid-360 session, a replay, a unit test)
  has not measured this, and "not measured" is not "measured zero". Defaulting
  it to 0 made every such capture 2D-only, which is how this line first broke
  four round-15 tests within a minute of being written.

**59 — THE PATH IS IN THE CLOUD, IN THREE PLACES.**

* **Live.** `TrajectoryTrail.Point` gains `y`, which the recorder had been
  throwing away at the door (`pose.tx()`, `pose.tz()`, and nothing else) because
  the trail only ever fed a 108 dp bird's-eye tile. `snapshot()` has existed
  since round 5 with **no callers at all**; the accessor was the missing piece.
  A new `trail.mat` (compiled by the existing `compileMaterials` glob — no build
  change) draws a `LINE_STRIP` renderable in the same Filament scene, depth
  tested and depth writing, so the path is genuinely IN the cloud and is
  occluded by the wall you walked behind. Rebuilt on change, not per frame: the
  trail only grows when the operator has walked 15 cm, about five times a second
  against 60 Hz. Round 5.3 refused to build this and priced it at *"a second
  material, a second geometry upload path and a per-frame rebuild"*; two of the
  three were right.
* **Review.** From `processed/trajectory.bin`, a new derived product
  `reprocess_d6_container` writes beside `processed/map_stitched.bin`. That
  pairing is the whole argument for a file over an ABI call: the trajectory in
  it is the CORRECTED one — section-stitched, and loop-end-closed when item 60's
  closer fires — written by the same pass that wrote the cloud beside it. A path
  drawn from an uncorrected trajectory over a corrected cloud would be a lie
  shaped exactly like a diagnosis, and the operator is going to use the
  agreement between path and room to judge the scan. Deleting the file returns
  the container to what the phone sealed, like every other derived product.
  The phone-side decoder is independent of the writer, and every malformed case
  — bad magic, truncated body, count/length mismatch, absurd count — returns "no
  path" rather than throwing, because a derived file must never be a reason the
  viewer fails to open a scan.
* **The floor plan.** `Canvas::line` and `Canvas::disc` already existed;
  `PlanRasterOptions` gains the polyline (in PLAN coordinates, so `plan/` keeps
  knowing nothing about world frames), the extent expands to include it, and it
  is drawn after the walls and before the frame. It makes the sheet
  self-explanatory: on scan-033 the walk reads as a loop through the flat with
  the start and end discs a visible gap apart — the loop-end gap, at true scale,
  on the drawing.

The colouring is `:core`'s (`TrajectoryRibbon`) so live and Review draw by one
rule: teal at the start, ember at the end, brighter markers at both ends, and
**red where tracking was lost** — which outranks the markers, because a walk
that lost tracking in its first half-metre must not have the one warning colour
hidden under a start marker. The toggle is `DisplayParams.showTrajectory`, which
has been persisted per project since the desktop viewer under a switch whose own
subtitle said *"the overlay itself is desktop-only so far"*. It is not any more,
and adding a second Boolean beside it would have been exactly the duplication
item 61 is about.

**60 — THE LEVER IS CLAIMED, AND IT FIRES ON ONE CAPTURE IN SIX.**

Round 11's closer is in the tree, works, and refuses all of these — for two
reasons, neither of them a disagreement about geometry:

```
scan-033  geometry-rejected  ICP rms 0.285 m > 0.25, and it proposed 5.72 deg
scan-036  no-excursion       furthest-from-start 3.58 m < min_excursion_m 4.0
scan-038  no-excursion       furthest-from-start 3.68 m < 4.0
```

`min_excursion_m = 4.0` is a corridor's number and **the owner scans a flat**.
The gate exists so a rig shuffling on the spot cannot count as a loop, and that
purpose is served by how far the walk went *compared to how far it missed by*.
So it is now scale-aware: `>= 3.0 m` **and** `>= 4x the closing gap`. scan-035
(10.1 m of path inside a 1.55 m neighbourhood — the sweep) still refuses on the
absolute floor, which is the exact case round 11 wrote it for.

And six-DoF ICP is the wrong estimator on a pushbroom, which round 12 proved and
round 13 acted on for seams. `post/loop_end.h` draws the same conclusion for
loop ends: the closing transform is **constrained** to a pure translation — the
rotation half of the se(3) vector is structurally zero, so `Exp(s·xi)` cannot
produce a rotation at any `s`. "Gyro-locked" is a property of the type, not a
tolerance, and the test asserts `correction_rotation_deg == 0.0` exactly.

**One number in the config is not round 13's, and finding out why was the round's
second real bug.** `plane_radius_m` was 0.25 m, inherited from the seam refiner.
At a seam the analytic transform has already removed the jump and what remains
is centimetres; at a loop end there is no analytic transform and what remains is
the whole accumulated drift. With a 25 cm radius every surface whose normal
points ALONG the drift is displaced clean out of correspondence range and
contributes nothing, while the surfaces perpendicular to it — which carry no
information about it — match perfectly. The solver is then handed a system
matrix that is singular in exactly the direction the answer lies. Measured on
this round's fixture: the weakest direction came back as (1.000, −0.001, −0.002)
— the axis carrying 0.30 m of the 0.36 m injected drift — at an observability of
0.022. At 0.60 m it is 0.265, and the real captures went 0.12 → 0.27 and 0.39 →
0.36 with the correction changing accordingly. The radius has to be able to
reach across the error being measured; that is geometry, not tuning.

**A seventh gate had to be added, and it changed one answer.** Every gate round
11 wrote asks the SOLVER, or the two submaps the solver chose, whether the
closure is plausible — and on scan-036 all six said yes: the two ends came 4.8 cm
together (35.6 → 30.8 cm), observability was a healthy 0.358, the correction was
a plausible 0.336 m, and the trajectory's end gap fell 0.30 m. Then round 12's
ruler — same-surface disagreement measured over the WHOLE map, over every pair
of windows — went **3.43 → 4.52 cm**. The map got worse. A translation that
slides a cloud until it lands on SOME nearby surface always reduces the mean
nearest-neighbour distance between the two clouds it was fitted to; that number
is the solver's own residual wearing a different hat and it cannot referee
itself. So the ruler votes last, on the metric the summary card already prints,
and a closure can only ship if the number the operator is shown improves.

Measured, through the production path (`engine_cli --d6-loopend`, both legs
stitched, scored with the round-12 ruler and the round-10 crispness metric at
the same wall probes):

| scan | decision | correction | self-check | loop gap | occupancy |
|---|---|---|---|---|---|
| 033 | **closed** | 0.118 m, 0.000° | **1.97 → 1.66 cm** (−15 %) | 0.581 → 0.566 m | −1.01 % |
| 036 | ruler-says-worse | 0.336 m proposed | would be 3.43 → 4.52 cm | refused | — |
| 038 | correction-too-big | 1.389 m proposed, bound 1.00 m | refused | — | — |
| 034 | no-revisit | 5.4 m of path, floor 8 m | — | — | — |
| 035 | no-excursion | 10.1 m inside 1.55 m | — | — | — |
| 039 | no-trajectory | no poses were recorded (item 58) | — | — | — |

**One closure in six, and that is the honest state of it: this claims the lever,
it does not yet claim the room.** Two things must be said plainly rather than
sold. First, the five refusals are the product working — each names a gate and a
number, and scan-036 is the case where the last gate overruled the first six.
Second, **the loop GAP is not the target and must not be reported as one**: on
scan-033 the geometry says the walk genuinely ended 0.57 m from where it
started, so most of that gap is where the operator stopped walking, not drift.
What the closure removes is the part the map disagrees with itself about, and
that is the 15 % the ruler measures. Run inside `Process`, ON by default there
and nowhere else, because it moves points the live pass could not have moved.

`post_geom.h` was extracted on the way: round 11 wrote the Jacobi eigensolver,
the submap cutter, the plane fitter, the normal-coverage metric and the occupied
voxel count; round 13 copied four of the five; round 16 needed all of them and a
third copy is where a shared routine stops being shared. It is a pure move, and
the round-11 and round-13 fixtures assert the same numbers to the same decimals
afterwards — which is what makes the move safe to have made.

**61 — THE RADIUS, AND WHAT WAS ACTUALLY DUPLICATED.**

(a) The cause is one line. `LidarScanShapes.extraLarge` is
`RoundedCornerShape(percent = 50)`, deliberately a PILL so un-restyled `Button`s
and `FilterChip`s round like the hand-built ones — and Material 3 hands that same
token to `ModalBottomSheet` (`BottomSheetDefaults.ExpandedShape`) and to
`AlertDialog` (`AlertDialogDefaults.shape`). Fifty per cent of a full-width
sheet's short side is an enormous curve. Three of the app's five sheets were
already passing `RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp)` by hand
and two were not, which is exactly why *some* windows looked right — "there are
some tab and window show the same too". Fixed with a token
(`ScanDims.SheetRadius` / `DialogRadius`, both 20 dp) rather than two more
literals, applied to the four surfaces that were inheriting the pill: the
**session-summary sheet** (the one the merge/auto-process progress lives in —
the one the owner pointed at), Review's **display panel** sheet, the **delete
confirmation** dialog and the **DND explainer** dialog. The pill stays on
controls, where it belongs.

(b) Duplicated surfaces, merged:

* **The process result, laid out twice.** `CaptureScreen.AutoProcessPanel` and
  `ReviewScreen.ProcessSectionsCard` each rendered the same three `StitchResult`
  sentences — `detail`, `selfCheckLine`, `mountWarning` — in two files, at two
  type scales, with two spacings. Review's own comment said the sentences
  *"can never drift apart"*, and it was right about the sentences and wrong
  about everything around them. Now one `ProcessResultLines` composable, with
  the per-surface test tags passed in so the instrumented tests keep asserting
  what they assert. Item 60's loop-end result is exactly the fourth line that
  would have been added to one of the two.
* **The trajectory toggle.** Not added. `DisplayParams.showTrajectory` already
  existed, was already persisted per project, and its switch already sat in
  Review's panel with the subtitle *"Persisted; the overlay itself is
  desktop-only so far."* Both the live view and Review now read that one flag,
  so the setting the operator changes in Review is the setting they get on the
  next walk. The subtitle now describes what it does.

Found, deliberately NOT merged, and named so the next round does not rediscover
them:

* **Three "process" surfaces over two different pipelines.** Review's inline
  card and Capture's auto panel both drive `ProcessingRepository.reprocessD6`;
  the Jobs tab's `GatedAction("Post-process")` queues `JobKind.POST_PROCESS`,
  which is a different pipeline with a different queue and a Cancel button. Same
  word, two engines, three chromes. Consolidating them is a round of its own
  because it is a decision about what "Process" means, not a layout change.
* **Review reachable twice** — Projects' "Open in viewer" and ProjectDetail's
  "Review" card — with different chrome (`UnderTabBar` on one route and not the
  other). Removing a navigation route mid-round risks the deep-link and
  back-stack behaviour round 10 spent a session fixing.
* **Two display panels** (`CaptureSettingsSheet` and Review's `DisplayPanel`)
  with genuinely divergent ranges — point size 0.1–3.0 px against 0.5–12 px, LOD
  2–20 M against 0.5–50 M — and different widget kits. The divergence is a bug;
  merging the panels is a bigger change than this round can carry honestly.

**62 — ONE TRUTH ON THE CARD, BOTH NUMBERS IN THE LOG.**

Neither count was wrong. The live detector splits on every discontinuity as it
arrives; the offline one re-derives the seams from the recorded stream, where
scan-038's 1.6 s `TRACKING_REGAINED` gap is bridged rather than split. The
operator does not have two detectors, they have one card. The rule is
**whichever detector last spoke**: before processing the live count is all there
is, after it the processed count describes the file that now exists on disk and
replaces it. The log keeps both, as `sectionsLive=` and `sectionsProcessed=`,
which is the only place the distinction is useful.

**IS scan-039 RESCUABLE?** Not today, and it is not unrescuable either — the
distinction matters and the answer should not be rounded off in either
direction. What is on disk is `lidar.bin` (854 KB of ranges with their
timestamps) and `imu_phone.bin` (899 KB, 20,401 samples at 399.1 Hz, `rejected=0
dropped=2`), plus `gnss.bin`. What is missing is the only thing that turns a
range into a point: a world pose per instant. A rescue would need a trajectory
built from the IMU alone, which is the item-46 bridge — gyro integration gives
orientation to about a degree over tens of seconds (round 14 measured 0.221° p90
against ARCore), and that is enough to de-rotate each fan; **the translation is
the problem**, because double-integrated accelerometer bias diverges as t² and
over 51 s that is metres, not centimetres. A pushbroom with correct rotation and
unknown translation resolves to a cloud that is locally right and globally
smeared along the walk — the same null space item 60 spends its round refusing
to invent. So a rescue is possible in exactly one shape: bootstrap the
translation from the LIDAR itself (scan-to-scan registration between successive
fans, which needs overlap the D6's single sweeping plane barely provides), and
constrain it with the gyro. That is a real project, not a flag. The container is
kept and the raw streams are untouched, so nothing about that project is
foreclosed.

---

## ROUND 17 (v0.9.2) — the owner's 0.9.1 field session, 2026-08-19

> *"the scan is not good. the shift of my position shifted quite a lot. my path
> not show in the point cloud. its just a 2d map of my path."*

Three sentences, three separate bugs, and the first one is the largest thing
this project has shipped wrong on purpose. Items 63–67.

**63 — THE HEALING WAS RIGHT FOR 33 MILLISECONDS AND CATASTROPHIC FOR SIX
SECONDS.**

The seal log for scan-040 has the whole failure in one line:

```
HEALED live jump=0.678m/66.21deg gapMs=6065
```

Round 13 derived the transform and stated its own assumption in the same
breath: `T_k = pose_after · pose_before⁻¹`, *"the operator's own motion during
the 33 ms gap is inside T_k too, but the gyro bounds it at ~1 deg and the
trajectory bounds the translation at ~1 cm, so T_k is the frame change to
within that."* Every break round 13 measured was **one ARCore frame wide**, and
over one frame a person is a statue: whatever the pose did, the world did.

Over 6.065 seconds a person is not a statue. Measured on scan-040's own bytes
against its own recorded 399.2 Hz gyro:

| | |
|---|---|
| ARCore, last tracked → first re-acquired | **66.21°** |
| gyro, integrated across the same 6.065 s | **144.94°** raw / **142.75°** bias-corrected |
| ARCore's motion *during* the loss (181 poses) | **0.00° / 0.000 m** |

ARCore **froze** — all 181 poses inside the blind window carry the last good
pose verbatim, which is not something a phone in a walking hand does. The gyro
did not freeze. So the 66.21° is not a frame correction; it is the leftover of
a 145° turn the tracker only saw the end of, and round 15 applied all of it as
a world rotation to every point already on screen. That is *"the shift of my
position shifted quite a lot"*, exactly, and it is our arithmetic rather than
his walking.

**The gyro is trustworthy over exactly this span, and it is the only witness to
the blind window.** Re-measured here on all three of his captures, against
ARCore itself over every clean 1 s window:

| scan | windows | median | p90 | max |
|---|---|---|---|---|
| 040 | 2188 | 0.109° | 0.363° | 2.430° |
| 041 | 1058 | 0.152° | 0.494° | 1.637° |
| 042 | 667 | 0.465° | 0.711° | 5.173° |

So `poses/reanchor.h`: **predict, then heal only the residual.**

```
q_pred = q_before · q_gyro          the operator's real rotation
p_pred = p_after − û · excess       see below
T      = M_after · M_pred⁻¹         what is left over IS the frame
```

Translation is **not predicted, it is bounded**. Double-integrating a consumer
accelerometer over six seconds gives metres of nonsense (item 46's note,
restated), and inventing a displacement is the null space item 60 spent its
round refusing to fill. What can be said without inventing anything is how far
a person could possibly have walked — `1.8 m/s × gap + 0.30 m` — and only the
EXCESS beyond that is charged to the frame. On scan-040 the bound is 11.2 m
against a reported 0.678 m, so the translation correction is exactly **zero**,
which is the honest answer: nobody knows where he was, and the tracker's claim
is not impossible.

Gated by gap duration, one policy, two callers (`core/engine.cpp` live and
`slam/post/section_stitch.cpp` offline — item 62's lesson about two detectors
applies twice as hard to two DECISIONS):

| gap | verdict |
|---|---|
| ≤ 100 ms | `snap` — round 13's transform, unchanged, bit-identical |
| > 100 ms, gyro agrees | `bridged` — the residual only |
| > 100 ms, gyro agrees and residual < 1 cm / 0.25° | `negligible` — the jump WAS the operator; nothing applied and **no cue** |
| no continuous gyro | refused |
| > 8 s | refused |
| residual > 25° or > 2 m | **refused** ← scan-040, at 76.77° |

(Two gyro figures because there are two: 144.94° integrating the raw stream,
142.75° once the resolve's own measured bias is taken out. The production path
uses the second and the engine unit test pins the first, since it is given the
quaternion directly. Either way the residual is ~77–79° and the verdict is the
same; quoting one number and hiding the other would be the kind of tidiness
that costs a reader the ability to reproduce it.)

**scan-040 is refused, and that is the headline.** 76.77° is not a re-anchor:
every one round 13 measured was 8–14°, and ARCore correcting itself by 79
degrees against its own session map is not a thing that happens. Something else
is true — the tracker restarted its frame, or the gyro is lying, or both — and
nothing in the container can say which. The answer is not a better guess; it is
a refusal, a recorded seam, and an operator who is told.

**Offline, taught the same lesson — and it turned out to be blind as well.**
`stitch_sections` derived seams from RATES only, and 0.678 m over 6.065 s is
0.11 m/s: under every threshold. So the offline pass found **one section** in a
capture with a 66-degree fold in the middle of it. (Item 62 recorded this and
called it *"bridged rather than split"*; that was true and was not the whole
truth — nothing was bridged, the seam was simply invisible.) A run of poses the
tracker disowned between two it owned is now a candidate on its own, resolved
by the same policy, and **the ruler votes last**, exactly as item 60's seventh
gate does. That gate earned itself immediately: scan-041 has a 468 ms gap where
the gyro says the phone turned 13.90° and the tracker says 0.78°, so the bridge
proposes a perfectly defensible 14.18° correction that makes the map *worse*
(self-check 2.86 → 3.39 cm, flat-floor vertical wander 0.50 → 0.79 m). Refused
by the surfaces, which is the referee.

Measured through the production path (`engine_cli --d6-reprocess`), before and
after, on the owner's real bytes:

| scan | sections | self-check | trajectory extents | outcome |
|---|---|---|---|---|
| 040 | 1 → 1 | 2.64 → **2.64 cm** | 0.134 m vert / 4.182 m end gap, unchanged | gap **named** and refused (6.065 s blind, residual 76.77°) |
| 041 | 2 → 2 | 2.86 → **2.86 cm** | 0.496→0.811 m, unchanged | bridge refused by the ruler (25.29 cm applied vs 24.48 cm left alone) |
| 042 | 1 → 1 | 3.54 → **3.54 cm** | unchanged | one gap negligible (5.72° gyro vs 5.70° tracker), one thin-submap |

**Every offline number is identical to round 16's, on purpose.** A correction
nobody can check is exactly what item 63 exists to stop applying, so the
offline pass gained a diagnosis and not a change — while the LIVE pass stopped
rotating his room by 66 degrees, which is the fix he asked for. What the
container now says out loud is `longest blind stretch 6.065 s; 1 gap(s)
refused`, where before it said nothing at all.

ABI 11 → 12, additive: `scan_engine_last_reanchor()` + `scan_gap_verdict_str()`
carry the six numbers a `Status` cannot, so the capture log can print *why*
while the walk is still happening.

**64 — TWO STARTS FOR ONE PROJECT, AND A WRECK GRADED GOOD (scan-045).**

Root cause, and it is not the operator's finger. `startCapture()` had **no
re-entry guard of any kind**: it never looked at `captureState`, the Start
button was never disabled, and the round-12 tracking gate holds the press for
four to eight seconds during which — because `_startWarmup` was computed and
rendered **nowhere** — nothing on the screen changed at all. A person who
presses a button that does not respond presses it again. That is not operator
error; it is a button that lied.

The second press fell through both `if (!startPending)` blocks and started the
capture, leaving the *first* press's `startGateJob` alive (it is cancelled only
when a NEW gate opens, which never happened). Four seconds later that orphan
re-entered `startCapture()` mid-recording and, before the engine finally
refused it with `invalid state`, had already run `resetPoseCounters()`,
`resetWorldFrame()` — destroying and rebuilding the live ARCore session in the
middle of the walk — and `trailRecorder.clear()`.

That last call is `pathM=0.0`, and here is the part that matters most: **a zero
path did not merely lose a number, it flipped the grading axis.** Round 14's
`isFromTheSpot` reads "≥ 20 s elapsed and < 5 m walked" as a deliberate
from-the-spot sweep and grades on points per SECOND, which 55,228 returns over
28 s passes comfortably. One section, no drops, 225 poses (so round 16's
zero-pose check could not fire) → **GOOD SCAN**, on a bundle with no `map.bin`
and no `processed/` in it.

Fixed in three places, because the failure needed all three:

* **The button.** One `AtomicBoolean`, claimed by the first press and held
  across the whole gate wait; the gate's own re-entry walks through it. Plus a
  `starting` flow the transport button actually renders — dimmed, inert, with a
  spinner and the label "Starting — waiting for tracking". The operator now both
  cannot and no longer needs to press twice.
* **The grade.** `ScanSummary.engineStarted` (what `scan_engine_start`
  answered) and `worldPointsResolved` (read off `streams/map.bin`, 16 bytes per
  `PointVertex` — the file, not a counter, because a counter is what got reset).
  Both rank **above** density, sections and everything else. Headlines
  "NOT RECORDED" and "NO ROOM — NOTHING WAS PLACED", for round 16's reason:
  "POOR" invites "walk it again more carefully", and neither of these gets
  better for anything the operator does differently.
* **The refusal.** Auto-process now names which of the three reasons it
  skipped for. `auto-process FAILED` with no error text reads as a bug in
  Process and was nothing of the kind.

Both new fields are **nullable, and the `?:` that is not there is the point** —
the same rule round 16 wrote for `posesRecorded`, and it caught the same
mistake within the hour: a Mid-360 session, a replay and four round-15 unit
tests have no pushbroom writing `map.bin`, and reading its absence as zero made
every one of them "NO ROOM".

**65 — THE PATH WAS BUILT AND NEVER PUBLISHED. ONE LINE.**

Round 16 built all of it: the metric flow, its accessor, the `:core` vertex
builder, the Filament material, a `LINE_STRIP` drawn in the **same 3D pass** as
the points with the same depth state, and the Review-side file reader. What it
never did was publish the metric flow as the walk happened.
`_worldPoints` was written by `setCapacity()` and by `clear()` and **by nothing
else** — and `setCapacity()` is called when the performance preset changes, so
the live 3D ribbon held whatever the walk looked like at the moment a preset
was last touched, which on every real capture is the empty list. The 108 dp
bird's-eye tile, published two lines away in the same method, kept updating.

So the operator got exactly one view of his path and it was the 2D one. *"its
just a 2d map of my path"* is a precise bug report.

The renderer, the frame, the material and the file were all correct, and
scan-040's `processed/trajectory.bin` sits comfortably inside its own cloud —
so nothing here points at the shader or the coordinate frame. It was one
missing assignment, and it had no test because everything it lived in took a
`com.google.ar.core.Frame`. `onPose(x, y, z, tracking)` is now split out of
`onFrame` for exactly that reason, and five bare-JVM tests walk it.

Two things beside it, both found on the way:

* **Review said nothing when there was no path.** Round 16's comment claims it
  *"says so rather than drawing a straight line"*; it did not — a missing
  `trajectory.bin` produced `EMPTY`, the entity was removed from the scene in
  silence, and the result is indistinguishable from a broken renderer. **Every
  container processed by a pre-round-16 engine is in that state**, which is
  every scan the owner already owns. There is now a line under the toggle
  saying which it is and what to do.
* **The device path had never been asserted.** The round-16 commit touched zero
  files under `src/androidTest/`. `Round17TrajectoryOnDeviceTest` now drives the
  real `libscanengine_jni.so` over staged scan-030 bytes and checks that the
  engine writes the file, that its length is exactly `16 + 12 × poses`, that
  the phone's own independent decoder turns it into ≥ 2 finite vertices with
  real extent, and that a second Process does not corrupt it.

**66 — A DEBUG LOG THAT TRAVELS WITH THE SCAN.**

`filesDir/logs/capture.log` is an app-lifetime rolling log that has to be
exported separately and then matched up by timestamp against a scan that may be
one of five taken that evening. `<proj>.lscan/debug/capture-debug.log` needs no
matching up: the bundle a person sends **is** the whole story. It carries every
`[ar]`/`[session]`/`[seal]`/`[net]` line plus capture-only verbose events — pose
acceptance, re-anchor decisions with their six numbers (item 63's, via the new
ABI-12 call), watchdog transitions, cues, preset changes.

Opened *before* the engine start, so a capture whose engine refuses still
leaves a bundle that says why — the scan-045 case exactly. Closed on both seal
arms, before the empty-scan prune that may delete the directory it lives in.
Capped at 5 MB × 2 files: a developer toggle must not be a way to fill a phone.

**Explicitly not a stream, and a `debug/README.txt` beside it says so in the
bundle**: not chunked, not CRC'd, not in the manifest, not part of the replay
guarantee. `record/replay` walks `streams/`, so byte-identical replay is
untouched by anything written here — which is the property that lets the log be
as verbose as it likes.

Developer Mode is the standard seven taps on the version footer (which already
had a stable test tag and an unused `clickable` import). Android's own idiom,
used for its own reason: a setting nobody can reach by accident needs no warning
beside it.

**67 — CAMERA HONESTY, AND ONE REAL LEAK FOUND WHILE LOOKING.**

Audited every path that can touch an ARCore image: `KeyframeRecorder`, the
calibration wizard, the GL background renderer, the C++ colorizer, `frames_idx`,
and the container's chunk types. Findings:

* The GL background renderer is GPU-only — no `glReadPixels`, no `PixelCopy`,
  no `MediaCodec` anywhere in `app/` or `core/`.
* The calibration wizard takes the **luma plane only**, into a heap array, and
  closes the image immediately; it persists angles, never pixels.
* No engine code writes an image file, and **no chunk type can carry image
  bytes** — `kCameraFrameIndex` is a path + pose + intrinsics + timestamp.

**And one real leak.** `CaptureViewModel` gated the UI flow with
`FeatureFlags.COLORIZE_ENABLED` and then called
`keyframeRecorder.setEnabled(tuning.keyframesEnabled)` on the line *below*
without it — at two sites. Three performance presets set `keyframesEnabled =
true`, and the preset picker is reachable **during a recording**, so changing
preset mid-walk re-armed the recorder and started writing
`streams/frames/kf_*.jpg` while the app's own HUD, reading the correctly-gated
flow, went on reporting keyframes as off. Both sites now pass the gated flow —
and `KeyframeRecorder` itself now defaults to `false` and enforces the flag **at
the source**, so with colorization off there is no argument to `setEnabled()`
that turns the camera writer on. That is what makes the class of bug impossible
rather than fixed twice.

With that closed, the sentence is true unconditionally, and it is now on the
capture sheet and in a Settings section of its own:

> **Camera is used for position tracking only — no images are saved.**

Worth stating why it was needed. The app asks for CAMERA permission and holds
the camera open for every second of every walk, because ARCore's
visual-inertial odometry *is* what places each lidar return. Nothing anywhere
said so, and the one place the camera was mentioned — the profile reference
card — said "no camera", which is true about storage and false about the lens.
An app that keeps your camera on for eighty seconds in your own home and does
not explain itself has earned the suspicion.

## ROUND 18 (v0.9.3) — the owner's 0.9.2 field session, 2026-08-19, 03:11–03:25

> *"the path record seems not so accurate."*

Seven captures (scan-046…053), the first two with the round-17 debug log the
owner found by himself (developer mode, seven taps). scan-053's debug log
contains the round's smoking gun in one line, and the owner's one sentence is
item 70. Items 68–72. **Mid-round owner correction:** the room had GOOD
lighting — the dim-light theory this round briefly held is refuted and nothing
below assumes it.

**68 — THE GYRO BRIDGE NEVER ENGAGED, AND THE REASON WAS 46 MILLISECONDS.**

scan-053's debug log:

```
section break: reason=TRACKING_REGAINED gapMs=1553 jump=0.010m/0.28deg healed=false
  :: verdict=refused: no continuous gyro across the gap ... gyroUsed=false
```

…while the same capture's IMU line reads `pushed=22401 rate=399.1Hz dropped=3`.
The gyro ran continuously through every gap of the session, and round 17's
bridge refused every long gap anyway. Root cause, proven on the recorded bytes
of all seven captures:

* **SensorManager delivers its first event 44–70 ms after `registerListener`**,
  while ARCore (already running from the preview) delivers poses immediately —
  measured on the streams: first gyro sample lands **+46.3 ms** (053),
  **+68.1 ms** (047), **+44.3 ms** (050), **+69.6 ms** (046) after the first
  pose (048's IMU happened to start early, −450 ms).
* His captures **lose tracking at capture start** (the world-frame reset's
  re-acquisition; every first break sits at t≈+0.6 s), so the bracket's
  `t_before` IS the first pose.
* `integrate_()` required the ring to reach within `max_imu_gap_ns` (25 ms) of
  BOTH ends of the interval — so a missing 46 ms sliver at the edge of a
  1554 ms window it covered 97 % of refused the ENTIRE bridge. Same failure
  offline: the recorded stream starts at the same instant, so Process refused
  the same gaps for the same non-reason.

Fix, three parts, all measured on his bytes:

* `ImuDensifyConfig::bridge_edge_slack_ns` = 100 ms (deliberately
  `snap_gap_ns`: same physical claim — over ≤100 ms a walking human is a
  statue to round 13's budget, so an uncovered EDGE slice that short
  contributes zero rotation). Edges only, `relative_rotation()` only: an
  interior hole is still fatal, and `sample_at()`'s bracket densification
  still requires full coverage and is bit-identical.
* `set_imu_extrinsics` now CARRIES the ring across its rebuild (the samples
  are raw sensor-frame measurements; the extrinsic is applied at integration
  time) — the capi smoke test's step 198 asserts the new contract.
* The Kotlin seal line's unhealed arm printed a FABRICATED reason — `NOT
  healed (no usable bracket)` — for every refusal including the engine's own
  verdicts. It now prints the verdict.

**The owner's gaps, before → after, on the untouched originals** (engine_cli
`--d6-reprocess` on copies; `streams/` byte-identical throughout):

| scan | gap | tracker says | gyro says | residual | 0.9.2 verdict | 0.9.3 verdict |
|---|---|---|---|---|---|---|
| 053 | 1.554 s | 0.010 m / 0.29° | **0.99°** | 0.88° | refused: no gyro | bridge candidate; ruler: thin-submap (capture-start gap — nothing on the far side to move). LIVE: bridges at 0.88° |
| 047 #1 | 1.629 s | 0.022 m / 2.50° | **4.55°** | 2.18° | refused: no gyro | bridge candidate; ruler: thin-submap, nothing moved |
| 046 | 6.897 s | 0.595 m / 72.28° | **178.63°** | 106.54° | refused (live: "no usable bracket") | refused-gyro-disagrees, with the numbers above |
| 050 | 6.398 s | 0.809 m / 29.94° | **115.63°** | 85.70° | refused (live: "no usable bracket") | refused-gyro-disagrees |
| 047 #2 | 0.663 s | 0.369 m / 37.40° | 16.21° | 53.55° | refused-disagree | unchanged |

046 and 050 are the honest refusals working as designed — the operator really
turned 116–179° during those blind windows (he was pacing tight turns ~1 m
from the walls; see item 70) and the tracker's report is irreconcilable with
it. Their maps, self-checks (2.23 / 1.77 cm) and loop gaps (4.94 / 5.70 m) are
**byte-identical** before→after. The sidecar now records every examined gap
(`stitch.json` → `gapsExamined[]`, additive): until this round a refused gap
left NO trace in the bundle, so scan-046's stitch.json was indistinguishable
from a clean walk's.

**69 — THE SNAP PATH WAS UNGATED, AND ON scan-047 IT MEASURABLY HURT.**

scan-047 break #3: `HEALED live IMPOSSIBLE_STEP jump=0.371m/56.85deg gapMs=33`
— an implied **1,720°/s**. The capture's own gyro over that frame: **0.67°**.
Round 13 measured real ARCore re-anchors at 8–13.5°; the ≤100 ms snap path
nevertheless applied T wholesale with no check at all, because round 13's
argument ("nobody moves in 33 ms, so the jump IS the frame") was never asked
the follow-up: *is a 57° one-frame frame-change a thing ARCore does?* It is
not — that is a relocalisation or a frame restart, exactly the class item 63
refuses at 25°.

The snap fast-path now applies only at or under `max_residual_rotation_deg`
(25°); past it the pair takes the same gyro-checked route as a bridged gap
(residual ≈ reported when the gyro reads ~0, so scan-047 #3 →
refused-gyro-disagrees at 56.21° residual; a hypothetical genuine wrist-flick
the gyro CONFIRMS resolves as negligible — the operator, not the frame; no
gyro at all → refused in plain words). Translation is deliberately NOT gated:
the gyro cannot witness it, and round 13 verified large translation-only
snaps (scan-030's 1.118 m) against gravity. Every snap at round-13's measured
sizes is bit-identical — asserted in `test_round18_snap_gate.cpp` against the
exact analytic transform.

**Measured verdict on scan-047's bytes: the heal hurt.** With the 56.85° snap
applied (0.9.2): 2 sections, first section rotated 56.85°, self-check
**6.92 cm** — the session's worst. With it refused (0.9.3): 1 section,
self-check **3.42 cm**. The offline ruler had already said so
(`map-got-worse`, 17.2 cm across-seam mismatch either way) and was overruled
by its own pipeline keeping the analytic seam; now the seam never forms.

**70 — "THE PATH RECORD SEEMS NOT SO ACCURATE" — HE IS RIGHT, STRUCTURALLY.**

During his 6–7 s tracking losses ARCore freezes (round 17: 181 consecutive
poses of 0.000 m/0.00° in scan-040). The frozen poses and the re-acquisition
teleport went into the trail, into pathM, into trajectory.bin, into Review and
onto the floor-plan sheet as ordinary walked lines: the drawn path holds still
while he walks, then teleports 0.6–1.8 m in one frame, in confident teal.
Refused-heal offsets (unhealed IMPOSSIBLE_STEPs) teleport the same way with
tracking green throughout.

The fix is verdicts, carried end to end, so all four surfaces agree:

* **Live trail**: a kept point whose incoming segment crossed blindness (any
  lost pose since the last kept point, either endpoint untracked, or an
  implied speed over PoseSectionTracker's 6 m/s) carries `jump`. pathM no
  longer counts those metres; they are reported separately (`jumpM=` beside
  `pathM=` in the seal, `ScanSummary.jumpLengthMeters`). The 2D tile draws
  them dashed red; the 3D ribbon draws them as `BRIDGE` (a darkened red —
  deliberately NOT alpha, `trail.mat` is `blending: opaque` and a translucent
  strip would silently render opaque).
* **trajectory.bin → "LSTRAJ02"**: 16-byte records, xyz + u32 flags (bit 0
  untracked, bit 1 jump-in; jump = >150 ms between poses, >6 m/s, or the step
  out of an untracked run). Verified on scan-046's actual bytes: 206 lost
  poses flagged, 3 jump-in flags, index 1352 = the 72.28° re-acquisition.
  Both readers (`TrajectoryFile.kt`, `lscan_plan.cpp`) accept v1 and v2;
  unknown versions draw no path rather than guessing a record size.
* **Review**: untracked poses draw nothing (their positions are held
  guesses); the bridge lands as its own colour.
* **Floor plan**: `PlanRasterOptions::trajectory_breaks` — blind segments
  drawn as red dashed bridges (verified on scan-046's regenerated sheet).

**Why he loses tracking at all — measured, after the owner's correction.** Not
light. The five seconds before every long loss, from the streams:

| loss | prior-5 s motion | cm/° | median lidar range | returns <1.5 m |
|---|---|---|---|---|
| 046 @38.9 s (6.8 s) | 50.6° / 0.87 m | 1.71 | **1.00 m** | **70 %** |
| 050 @26.1 s (6.3 s) | 47.3° / 1.05 m | 2.22 | **1.23 m** | **67 %** |
| 053 @52.8 s (3.7 s) | 50.5° / 1.10 m | 2.17 | **1.21 m** | **63 %** |
| 047 @16.1 s (0.6 s) | **127.3°** / 1.08 m | 0.85 | 1.64 m | 37 % |

The long losses share one signature: ~1 m from surfaces with two-thirds of
returns under 1.5 m — round 13's measured ARCore re-anchor diet (close,
feature-poor walls), with healthy parallax. 047's short loss is the other
known diet: fast turning at the 0.8 cm/° parallax edge. The remaining first
breaks (+0.6 s into 047/048/053) are the capture-start world-frame reset
still re-acquiring — app-caused, and now healed trivially by item 68.
**No low-light warning ships** (the evidence against it is above); what ships
instead is the missing signal: ARCore's own `TrackingFailureReason` is now
written to the capture debug log at every loss transition, so the next
session's diagnosis is read, not reconstructed.

**71 — scan-053 WAS EXPORTED AS A HALF-BUNDLE, SILENTLY.**

His exported scan-053 contains `processed/preview.f32` and nothing else — no
`trajectory.bin`, no `map_stitched.bin`, no `stitch.json` — because the export
raced the seal's auto-process (or its silent failure) and shipped whatever
happened to exist. Three-part fix:

* `ProcessingRepository.reprocessD6` takes a **per-container lock**, so an
  export that needs processing WAITS for a still-running auto-process instead
  of racing it (idempotent job; the second run rewrites the same bytes).
* `transferBundle` now **ensures processing before zipping**: already
  processed → proceed; processable → process now ("Processing before
  export…"); failed or unprocessable → `processed/UNPROCESSED.txt` written
  into the bundle naming exactly what is missing and why. No third outcome.
* The **capture debug log now stays open through auto-process** and closes
  with its verdicts (or its refusal/failure) inside — round 17 closed it at
  the seal, which is why scan-053's log ends one line before the answer the
  owner's question needed. Guarded by project (`endCaptureDebugFor`): a new
  capture can begin while the old auto-process runs, and a late completion
  must never scribble into the wrong bundle. Still 5 MB-capped, still not a
  stream, still outside the replay guarantee. A scan about to be pruned still
  closes at the seal (its directory is deleted).

**72 — rank=100.55 WAS A PENALTY CONSTANT LEAKING INTO A LOG LINE.**

`MountTrim.qualityRank` is lower-is-better and an UNMEASURED trim ranks as
`UNMEASURED_RANK_BASE (100) + spreadP90` — that is where `candidate
rank=100.55 spreadP90=0.55` came from: 100 + 0.55. The COMPARISON was right
(a measured 0.78° split-half accuracy beats a one-second sample that cannot
be split-half checked at all); the line calling the candidate "worse" and
printing raw ranks was wrong twice. It now says which won and why in plain
words, and the note distinguishes "less steady" from "unverifiable from so
short a hold".

The 03:15:59 re-zero (magnitude 91.21°, spreadP90 2.24°, accepted; scan-050
then carries trimAccuracyDeg=1.35): **the acceptance was correct** — the
movement gate (p90 ≤ 2.50°) answers "was the rig still enough to average",
and it was. The round-10 refine goal (0.8°) is a different claim — the
split-half accuracy of the resulting MEAN — and 1.35° misses it. The two bars
were never contradictory; what was missing was the sentence at acceptance
time. A captured trim whose measured accuracy exceeds `WARN_STABILITY_DEG`
now warns immediately ("set, but its measured accuracy is 1.35° — past the
0.8° goal; it will be used…"), and the captured log line carries
`accuracyDeg=` so the field log answers this question by itself. (The ~91°
magnitude is the bracket's geometry, unchanged all session, and not a bug.)

**Numbers.** Engine 621 cases / 2,517,351 asserts, ctest 7/7 serial, capi
smoke updated for the ring carry-over; ABI stays 12 (no new entry point —
every engine change is inside poses/ + post/ + plan/). `:core` 560, `:app`
108 (+9: path honesty, trajectory flags). Owner-capture regression: 046/048/
050/051 maps and self-checks byte-identical; 047 changes BY DESIGN (6.92 →
3.42 cm, the refused harmful heal); 053 processes to 3.20 cm / loop-end gap
2.55 m (it had no processed results at all before this round). stitch.json
gains `gapsExamined[]` and trajectory.bin moves to v2 — both derived files,
both additive-or-versioned, `streams/` untouched everywhere.

## ROUND 19 (v0.9.4) — owner-approved: carry the trajectory through the blindness

Owner-approved scope (his items 1, 2, 6, 7 plus the recovery item his
correction earned). Context on record: the sessions were in GOOD light, the
losses were close feature-poor surfaces (63–70 % of returns under 1.5 m) and
fast turning, the D6 is capable hardware, and the strategy is to make
lidar + gyro carry the trajectory through camera outages. Items 73–77.

**73 — GYRO-CONSTRAINED LIDAR-TO-LIDAR GAP RESCUE (the headline).**

Round 18 left three honest refusals on the table: scan-046 (6.897 s blind,
gyro 178.63° vs the tracker's 72.28°, residual 106.54°), scan-050 (6.398 s,
115.63° vs 29.94°, residual 85.70°) and scan-040 (76.77° residual). Refusing
to APPLY the pose jump was right — and it is not the end of the evidence. The
D6 painted through every loss, the gyro measured through every loss, and the
walls on the far side of a blind window in a small flat are mostly the same
walls as on the near side. Two rigid maps of one room plus a witness to the
rotation between them is a registration problem with ONE unknown vector.

`slam/post/gap_rescue.h` is that primitive, reusable and gated:

* **Rotation LOCKED, never solved** — round 12 measured what a free rotation
  does on a pushbroom (invents 14–19° in the null space), so
  `R = R_after · (R_before · R_gyro)ᵀ` is constructed from the gyro's witness
  and applied, exactly the reanchor derivation with the refusal turned into a
  registration.
* **Translation solved from the walls the two sides share**: a deterministic
  coarse grid (±2.4 m / 0.4 m horizontal, ±0.6 m vertical, ties biased toward
  zero so an unobservable direction stays put) seeds round 13's point-to-plane
  solve at round 16's 0.60 m plane radius — with the step taken IN THE
  OBSERVABLE SUBSPACE of the system matrix. A direction below the 0.05 gate
  takes no step and is reported by name (`solvedAxes`, `weakAxis`); fewer than
  two observable directions refuses outright.
* **Refusals by name**: no-gyro, no-anchor (a disowned pose is not a frame),
  thin-submap, no-overlap (the sides paint no shared surface within reach),
  unobservable, not-converged, correction-too-big, no-improvement — and
  **the ruler votes last** (gate 7 doctrine, verbatim from loop_end.h): a
  rescue that makes the whole-map self-check worse is refused however good
  its own residual looks.

Wired into `Process` (`ReprocessOptions::rescue_gaps`, ON — this is the one
pipeline allowed to move points the live pass could not), after stitching and
before loop-end, applied exactly like a section seam through the `processed/`
provenance channel. `streams/` byte-identical everywhere; the sidecar gains a
`rescues[]` array with every attempt and its numbers.

**On the owner's real bytes** (engine_cli --d6-reprocess on copies):

| scan | verdict | numbers |
|---|---|---|
| **050** | **RESCUED** | 115.63° locked from the gyro, 0.298 m solved from the walls (overlap 0.93, observability 0.38, 3 axes, 18,832 pairs); the two sides 32.2 → 12.1 cm; **selfCheck 1.77 → 1.40 cm**; **loop-end gap 5.70 → 3.39 m** |
| 046 | refused: ruler-says-worse | the registration itself is excellent (sides 39.2 → 12.3 cm, overlap 0.95, 3 axes) but the whole-map number would go 2.23 → 2.46 cm. Structural honesty: a fold hides from the ruler (its two halves share no cells, so the 2.23 was measured blind to the fold), and unfolding exposes the cross-gap residual to the metric for the first time. The seam stays, the numbers are recorded, the map is byte-identical |
| 040 | refused: ruler-says-worse | sides only 41.8 → 29.6 cm at dt 1.029 m, selfCheck 2.64 → 2.79 — a genuinely weaker registration, correctly refused |
| 047 #2/#3 | refused: no-improvement | with the gyro-locked rotation applied the two sides agree LESS (8.8 → 15.0 cm, 14.6 → 20.1 cm) — geometric confirmation that those one-frame jumps were transients the pose stream recovered from on its own, exactly what round 18's snap gate claimed |
| 047 #1, 053 | refused: thin-submap | capture-start gaps — there is nothing painted on the near side of the blindness to register with |
| **039** | **refused: no-anchor** | the precise gate: `rescue-no-anchor` — a rescue needs one TRACKED pose on each side of the blindness and scan-039 has none anywhere (zero poses recorded). The gyro orientation is fine (round 16 measured it); the translation has no witness and lidar-to-lidar across single 2D fans is the very null space this module refuses to invent. Unrescuable today, nothing foreclosed |
| **045** | refused before the rescue | every one of its 268 poses is disowned (the round-17 double-start reset), so 0 points resolve and no side of any gap exists. The zero-point refusal now accounts for every return: *"223 had no pose covering their timestamp, 34,213 were painted during tracking loss and excluded"* — the old message explained 223 of 34,436 and let the rest vanish |

**74 — THE LOSS-WINDOW RETURNS COME BACK, RULER PERMITTING.**

`exclude_flagged` throws away ~10 k returns per long loss (scan-046: 26,428
excluded of 148,921 decoded — 17.7 % of the capture). Once a gap is bridged
(round 18) or rescued (item 73) its two ends are in ONE trusted frame, and the
returns painted inside it can be re-resolved instead of discarded: orientation
integrated from the gyro with the closing error distributed linearly (the
densifier's own model, step 3), position linearly interpolated between the
trusted endpoints — the honest statement of what is known. Offline only
(`ReprocessOptions::recover_gap_points`, ON; the LIVE default is untouched by
design), admitted points carry `flagged_alpha` as provenance, and **the ruler
votes per gap**: scan-050's 23,609 candidates were re-resolved and then VETOED
(selfCheck 1.40 → 1.96 cm — a 6.4 s position lerp under a 116° pacing turn
smears, and the ruler said so), so they stay excluded and the sidecar says
exactly that (`recoveries[]`, `rulerVetoed: true`). The synthetic end-to-end
fixture (corridor, 7 s blindness, 30° frame restart) proves the admit path:
5,000+ candidates recovered and kept when the interpolation is actually right.

**75 — LIVE COVERAGE GUIDANCE.** Rooms rarely close because of coverage, not
math — only scan-029 ever closed. `CoverageCompass` (:core) is the census the
grid could not give: twelve 30° azimuth sectors around the walked path,
counted relative to where the operator STOOD when each return resolved, thin
judged against the sector mean (scale-free), nothing claimed below 10 k
returns. Surfaced twice, both quiet: **amber arcs on the trail tile's edge**
pointing at the uncovered walls (the tile is already a top-down world map, so
a world azimuth is a canvas angle; polled at 1 Hz, visual only, zero new
cues — the round-13 budget applied to pixels), and **one sentence on the
summary card** naming the largest thin arc relative to the walk ("The walls
behind you are thin in the map (about 90° of the room) — walk past them
before stopping"), slotted into the existing advice chain below tracking
problems. Honest by construction: coverage of what the D6 could see from the
walked path, no pretense of global completeness, and silence when healthy or
unmeasured.

**76 — SURFACE CONSOLIDATION.** The round-16 list, closed:

* **One process pipeline for a D6.** The Jobs tab's "Post-process" queued
  `JobKind.POST_PROCESS` — a plain re-resolve with none of the stitch /
  rescue / loop-end corrections and nothing written to `processed/` — while
  Review's card and the seal's auto-process ran `reprocessD6`. Same word,
  different result on disk. Every D6 "Process" is now `reprocessD6` (same
  per-container lock, same derived files, same verdicts); the queue remains
  what it truly is: the Mid-360's LIO re-run.
* **One Review chrome.** Both doors stay (the seal → Projects handoff needs
  the list door; removing a route is the back-stack risk round 16 named) but
  the destination is now wrapped in `UnderTabBar` like every sibling project
  screen — the "different chrome" is gone.
* **One display truth.** Round 18's finding, fixed at the root:
  `CaptureViewModel.displayParams` no longer SYNTHESIZES a fresh block from
  five controls — it `copy()`s them onto a persisted DEVICE display block
  (`SettingsRepository.displayParams`), loaded at construction and saved
  debounced on change. Review's panel writes the same store beside its
  per-project manifest write. Consequences, all previously broken: Review's
  walked-path toggle finally reaches the live view; `showPoseGraph`, EDL, the
  clip block and `fixQualityColors` survive project creation instead of
  resetting; and the divergent slider ranges (0.1–3.0 px vs 0.5–12 px;
  2–20 M vs 0.5–50 M) collapse into `DisplayLimits` constants both panels
  read. (Consciously left: the floor plan's path overlay has no per-user
  toggle — it draws the corrected trajectory with round-18 verdicts
  unconditionally, and a toggle there would be a capi change this round does
  not need.)

**77 — PRE-SCAN CHECKLIST.** One compact sheet on the FIRST Start press per
device (skippable; "don't show again" persisted, one-way, like the DND ask):
mount trim age + measured accuracy against the 0.8° goal (amber past round
18's 1.0° warn), tracking readiness in the start gate's own words, DND
status, and ONE technique line built from measured causes — *"Keep about an
arm's length or more from blank close surfaces, ease through the turns, and
walk a loop that ends where it started."* Light is not mentioned, anywhere:
the owner's correction is on record, and this round also purged the two
summary-card advice strings that still said "turn the lights up" / "more
light" (they now name the measured diet), with a :core test that makes
"light" in an advice string a build failure. The checklist READS existing
state only — the round-12/16 start gate is untouched; its own Start button
continues the intercepted press (`startCapture(skipChecklist = true)`), so
nothing new gates and nothing waits.

**D6 YIELD AUDIT** (no UI; `yield` in stitch.json, one line in the item-66
debug log after auto-process, printed by `engine_cli --d6-reprocess`). Where
every decoded sample went, on the owner's captures:

| scan | samples | no-return | out-of-window | no-pose | flagged-excluded | resolved |
|---|---|---|---|---|---|---|
| 046 | 148,921 | 1,471 (1.0 %) | **0** | 263 | **26,428 (17.7 %)** | 120,759 (81.1 %) |
| 047 | 63,600 | 542 | 0 | 199 | 7,937 (12.5 %) | 54,922 |
| 048 | 83,639 | 392 | 0 | 1,862 | 3,243 | 78,142 |
| 050 | 113,037 | 482 | 0 | 445 | **23,609 (20.9 %)** | 88,501 |
| 051 | 77,986 | 601 | 0 | 20 | 58 | 77,307 (99.1 %) |
| 053 | 119,426 | 644 | 0 | 265 | 19,243 (16.1 %) | 99,274 |
| 040 | 186,301 | 1,890 | 0 | 204 | 22,742 | 161,465 |
| 045 | 34,436 returns | — | 0 | 223 | **34,213 (99.4 %)** | **0** |

The verdict the numbers give: **the range window is not the problem** —
out-of-window is zero on every capture, so there is no cheap win to retune
there, and nothing was silently retuned. True no-returns are 0.3–1.3 %. The
one large recoverable loss is the flagged-excluded block during long losses
(12–21 % of everything the sensor said on the loss captures), which is
precisely the territory items 73/74 opened.

**Numbers.** Engine 629 cases / 2,527,464 asserts, ctest 7/7 serial; ABI
stays **12** (every change is inside post/ + the sidecar; the app reads the
new sidecar fields with a fifteen-line field reader, the same judgement the
engine's own manifest reads make in the other direction). `:core` 573
(+13), `:app` 119 (+11). Owner-capture regression: 046/040/047/048/051/053
maps and self-checks byte-identical to 0.9.3 (all rescues there refused);
050 changes BY DESIGN (the rescue: 1.77 → 1.40 cm, loop gap 5.70 → 3.39 m).
`stitch.json` gains `rescues[]`, `recoveries[]`, `recoveredPoints`, `yield`
— all additive. Backlog, noted and NOT built: capture-start tracking loss is
app-caused (start-reset re-acquisition) — its avoidance is round 20.
