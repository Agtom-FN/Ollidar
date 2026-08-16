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
