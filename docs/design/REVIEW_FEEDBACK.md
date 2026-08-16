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
