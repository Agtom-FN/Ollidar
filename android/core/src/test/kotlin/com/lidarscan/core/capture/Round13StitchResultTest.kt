package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 13 — the "Process this scan" result, decoded and put into words.
 *
 * The slot layout is a contract with `processing_jni.cpp`, and a flat
 * `DoubleArray` is exactly the kind of contract that rots silently: a wrong
 * index is a plausible number in the wrong place, not a crash. So the decode
 * is tested against the REAL numbers the engine produced for the owner's
 * scan-030, taken from the run in this round's notes.
 */
class Round13StitchResultTest {

    /**
     * scan-030 through `scan_lscan_reprocess_d6` — the actual values, in slot
     * order. If anyone renumbers the JNI table without renumbering
     * [StitchResult.fromNative], one of these assertions is wrong.
     */
    private val scan030 = doubleArrayOf(
        1.0,        // 0  ran
        1.0,        // 1  map_written
        5.0,        // 2  sections
        4.0,        // 3  seams
        0.0,        // 4  seams_refined
        78421.0,    // 5  points
        0.516952,   // 6  moved m
        0.819935,   // 7  vertical before
        0.270875,   // 8  vertical after
        0.489791,   // 9  end gap before
        0.648994,   // 10 end gap after
        5.532767,   // 11 moved deg
        0.0,        // 12 mount verdict (OK)
        0.0,        // 13 mount impossible fraction
        1210.0,     // 14 poses
        14.0,       // 15 poses untracked
    )

    @Test
    fun `scan-030 decodes into the numbers the engine actually produced`() {
        val r = StitchResult.fromNative(scan030)
        assertNotNull(r)
        requireNotNull(r)
        assertTrue(r.ran)
        assertTrue(r.mapWritten)
        assertEquals(5, r.sections)
        assertEquals(4, r.seams)
        assertEquals(0, r.seamsRefined)
        assertEquals(78421L, r.points)
        assertEquals(1210L, r.poses)
        // The 14 poses ARCore recorded at the origin before it had a frame —
        // the ones that used to be mistaken for a re-anchor.
        assertEquals(14L, r.posesUntracked)
        assertEquals(0.8199, r.verticalExtentBeforeM, 1e-3)
        assertEquals(0.2709, r.verticalExtentAfterM, 1e-3)
        assertEquals(MountVerdict.OK, r.mountVerdict)
        assertTrue(r.changedAnything)
    }

    @Test
    fun `the headline is the height spread, in the operator's words`() {
        val r = requireNotNull(StitchResult.fromNative(scan030))
        val h = r.headline
        assertTrue("expected the piece count: $h", h.contains("5 pieces"))
        assertTrue("expected the before value: $h", h.contains("0.82"))
        assertTrue("expected the after value: $h", h.contains("0.27"))
        assertFalse("a literal placeholder escaped: $h", h.contains("%"))
    }

    @Test
    fun `the end gap is stated but never sold as an improvement`() {
        val d = requireNotNull(requireNotNull(StitchResult.fromNative(scan030)).detail)
        // scan-030's gap gets WORSE by this measure (0.49 -> 0.65 m), because
        // before stitching it compared two points in different world frames and
        // meant nothing. The sentence must attribute it to the camera's drift
        // rather than imply this feature fixed or broke it.
        assertTrue(d.contains("drift"))
        assertTrue(d.contains("does not remove it"))
        assertFalse(d.contains("%"))
    }

    @Test
    fun `a one-section scan says there was nothing to do`() {
        val v = scan030.copyOf()
        v[2] = 1.0; v[3] = 0.0
        val r = requireNotNull(StitchResult.fromNative(v))
        assertFalse(r.changedAnything)
        assertTrue(r.headline.contains("one piece"))
        assertNull("a clean scan must not be given a lecture", r.detail)
    }

    @Test
    fun `a rotated mount is surfaced with the measured fraction`() {
        val v = scan030.copyOf()
        v[12] = 3.0   // SCAN_MOUNT_MISMATCH
        v[13] = 0.1949 // scan-026's measured value
        val r = requireNotNull(StitchResult.fromNative(v))
        assertEquals(MountVerdict.MISMATCH, r.mountVerdict)
        val w = requireNotNull(r.mountWarning)
        assertTrue(w.contains("19%"))
        assertTrue(w.contains("Re-zero"))
        // OK and NOT_MEASURABLE must stay silent — a watchdog that speaks when
        // nothing is wrong is a watchdog people learn to ignore.
        v[12] = 0.0
        assertNull(requireNotNull(StitchResult.fromNative(v)).mountWarning)
        v[12] = 1.0
        assertNull(requireNotNull(StitchResult.fromNative(v)).mountWarning)
    }

    @Test
    fun `a short or absent array is refused rather than half-decoded`() {
        assertNull(StitchResult.fromNative(null))
        assertNull(StitchResult.fromNative(DoubleArray(15)))
        assertNotNull(StitchResult.fromNative(DoubleArray(16)))
    }

    @Test
    fun `every SCAN_MOUNT ordinal maps to the verdict the C header declares`() {
        assertEquals(MountVerdict.OK, MountVerdict.of(0))
        assertEquals(MountVerdict.NOT_MEASURABLE, MountVerdict.of(1))
        assertEquals(MountVerdict.SUSPECT, MountVerdict.of(2))
        assertEquals(MountVerdict.MISMATCH, MountVerdict.of(3))
        // An ordinal from a newer engine must degrade to "say nothing", never
        // to a confident wrong answer.
        assertEquals(MountVerdict.NOT_MEASURABLE, MountVerdict.of(99))
    }

    // ── the cue quiet window (ROUND 13) ──────────────────────────────────────

    @Test
    fun `nothing buzzes while the tracker is recovering from a break`() {
        val s = CueScheduler()
        var t = 0L
        assertNull(s.tick(CueConditions(sectionBreaks = 0), t))          // baseline tick
        t += 100
        assertEquals(CueKind.SECTION_BREAK, s.tick(CueConditions(sectionBreaks = 1), t))

        // scan-030's fourth break arrived 0.51 s after the third break's cue.
        // Inside the quiet window nothing fires — not the break cue, and not
        // the degraded cue either, because the risk is to the TRACKER and not
        // to the operator's attention.
        t += 510
        assertNull(s.tick(CueConditions(sectionBreaks = 2, trackingDegraded = true), t))

        // Past the window, the world resumes.
        t += 1_200
        assertEquals(
            CueKind.TRACKING_DEGRADED,
            s.tick(CueConditions(sectionBreaks = 2, trackingDegraded = true), t),
        )
    }

    @Test
    fun `the section-break buzz is no longer the loudest thing the phone does`() {
        val break_ = CuePatterns.SECTION_BREAK
        val degraded = CuePatterns.TRACKING_DEGRADED
        assertTrue(
            "the break cue must not exceed the amplitude of the cue that repeats every 4 s",
            break_.amplitudes.max() <= degraded.amplitudes.max(),
        )
        assertTrue("it still has to be feelable through a pocket", break_.amplitudes.max() >= 120)
        // Three pulses is what distinguishes it; that must not have changed.
        assertEquals(3, break_.toneRepeats)
    }
}
