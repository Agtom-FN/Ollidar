package com.lidarscan.core.welcome

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 32 item 177 — **the storyboard's waypoints, driven by a clock.**
 *
 * This is the test the item asks for by name: *a clock-driven test pinning key
 * waypoints of A (puck rotation completes 360°, light only after landing)*. It
 * walks the three seconds in one-millisecond steps and asserts the properties
 * the owner stated in words, not the numbers a particular frame happens to
 * have — so the film can be re-timed without rewriting the test, and cannot be
 * re-timed into something that breaks what he approved.
 */
class WelcomeTimelineTest {

    /** Every millisecond of the film, as the fraction the timeline is indexed by. */
    private val everyMillisecond: List<Float> =
        (0..WelcomeAnimation.DURATION_MS).map { it.toFloat() / WelcomeAnimation.DURATION_MS }

    // ══ A — the flip ══════════════════════════════════════════════════════

    /**
     * **Exactly one** 360° rotation. The owner wrote the word "exactly", and
     * the three ways to get this wrong are all covered: stopping short, going
     * round twice, and wobbling backwards on the way.
     */
    @Test
    fun `the puck turns through exactly one revolution and never back`() {
        var previous = -1f
        for (t in everyMillisecond) {
            val deg = WelcomeTimeline.frameA(t).puckRotationDeg
            assertTrue("t=$t went backwards: $deg after $previous", deg >= previous - 1e-3f)
            assertTrue("t=$t overshot one turn: $deg", deg <= WelcomeTimeline.A_FULL_TURN_DEG + 1e-3f)
            previous = deg
        }
        assertEquals(0f, WelcomeTimeline.frameA(0f).puckRotationDeg, 1e-4f)
        assertEquals(
            "it must have completed the turn by the end",
            WelcomeTimeline.A_FULL_TURN_DEG,
            WelcomeTimeline.frameA(1f).puckRotationDeg,
            1e-3f,
        )
    }

    /** It sits still on the head until it is tossed. */
    @Test
    fun `the puck does not move before the toss`() {
        for (t in everyMillisecond.filter { it <= WelcomeTimeline.A_LAUNCH }) {
            val f = WelcomeTimeline.frameA(t)
            assertEquals("t=$t", 0f, f.puckRotationDeg, 1e-4f)
            assertEquals("t=$t", 0f, f.puckDy, 1e-4f)
        }
    }

    /**
     * **The light only after landing.** Nothing orange — LED, fan dots, either
     * ring — may exist while the puck is in the air, and the puck itself must
     * be drawn dark for every one of those frames.
     */
    @Test
    fun `nothing is lit until the puck is back down`() {
        for (t in everyMillisecond.filter { it < WelcomeTimeline.A_LIGHT_ON }) {
            val f = WelcomeTimeline.frameA(t)
            assertFalse("t=$t claimed to be lit", f.lightOn)
            assertEquals("t=$t LED", 0f, f.ledAlpha, 1e-4f)
            assertEquals("t=$t fan", 0f, f.fanAlpha, 1e-4f)
            assertEquals("t=$t ring 1", 0f, f.ring1Alpha, 1e-4f)
            assertEquals("t=$t ring 2", 0f, f.ring2Alpha, 1e-4f)
            assertEquals("t=$t the puck must be dark in the air", 1f, f.puckDim, 1e-4f)
        }
    }

    /**
     * …and the moment it is allowed to be lit is the moment the puck is home:
     * the turn is complete and it is at or below its anchor (the storyboard's
     * 3 px overshoot is the contact).
     */
    @Test
    fun `the light comes on at the landing, not before it`() {
        val landing = WelcomeTimeline.frameA(WelcomeTimeline.A_LIGHT_ON)
        assertTrue(landing.lightOn)
        assertEquals(WelcomeTimeline.A_FULL_TURN_DEG, landing.puckRotationDeg, 1e-3f)
        assertTrue(
            "the puck must have arrived, not still be falling: dy=${landing.puckDy}",
            landing.puckDy >= 0f,
        )
        // And it is genuinely lit shortly after, rather than merely permitted to be.
        val after = WelcomeTimeline.frameA(0.62f)
        assertTrue(after.ledAlpha > 0.9f)
        assertTrue(after.fanAlpha > 0f)
        assertEquals(0f, after.puckDim, 1e-4f)
    }

    /** The apex is one, it is airborne, and it is where the storyboard put it. */
    @Test
    fun `the flip has a single apex, well above the head`() {
        val lowestDy = everyMillisecond.minOf { WelcomeTimeline.frameA(it).puckDy }
        assertTrue("the puck must actually leave the head: $lowestDy", lowestDy < -250f)
        // The storyboard's -78 storyboard px, converted. Within a unit.
        assertEquals(-78f * WelcomeTimeline.STORYBOARD_TO_MASTER, lowestDy, 1f)

        // One apex across the FLIGHT — up, then down, with no second hop. The
        // check stops at the landing on purpose: after it, dy legitimately goes
        // back up as the 3 px contact overshoot recovers, and asserting
        // monotonicity through that would be asserting the squash away.
        val flight = everyMillisecond
            .filter { it <= WelcomeTimeline.A_LIGHT_ON }
            .map { WelcomeTimeline.frameA(it).puckDy }
        val apex = flight.indexOf(flight.min())
        assertTrue("the apex is not at an end", apex > 0 && apex < flight.lastIndex)
        for (i in 1..apex) assertTrue("t index $i", flight[i] <= flight[i - 1] + 1e-3f)
        for (i in apex + 1..flight.lastIndex) assertTrue("t index $i", flight[i] >= flight[i - 1] - 1e-3f)
    }

    /** The landing squashes: flatter than it is tall, once, and settled by the end. */
    @Test
    fun `it lands with a squash and recovers`() {
        val flattest = everyMillisecond.minOf { WelcomeTimeline.frameA(it).puckScaleY }
        assertTrue("no squash happened: $flattest", flattest < 0.9f)
        val atFlattest = everyMillisecond.first { WelcomeTimeline.frameA(it).puckScaleY == flattest }
        assertTrue("the squash must be at the landing, not in the air", atFlattest > WelcomeTimeline.A_LAUNCH)
        val end = WelcomeTimeline.frameA(1f)
        assertEquals(1f, end.puckScaleX, 1e-3f)
        assertEquals(1f, end.puckScaleY, 1e-3f)
    }

    /** The rings run out to the storyboard's full scale, which is the screen's corner. */
    @Test
    fun `two rings expand to the full scale, the second behind the first`() {
        assertEquals(
            WelcomeTimeline.A_RING_FULL_SCALE,
            WelcomeTimeline.frameA(1f).ring1Scale,
            1e-3f,
        )
        // The second ring is the first, read late — so at any moment after they
        // have both started it is strictly smaller.
        for (t in everyMillisecond.filter { it > 0.70f && it < 0.98f }) {
            val f = WelcomeTimeline.frameA(t)
            assertTrue("t=$t: ring2 ${f.ring2Scale} must trail ring1 ${f.ring1Scale}", f.ring2Scale < f.ring1Scale)
        }
    }

    /** The eye follows the puck up while it is away, by a few pixels, and comes back. */
    @Test
    fun `the eye follows the puck up and returns`() {
        assertEquals(0f, WelcomeTimeline.frameA(0f).eyeBob, 1e-4f)
        assertTrue("the eye must look up mid-flight", WelcomeTimeline.frameA(0.3f).eyeBob < -10f)
        assertEquals("and be level again at the end", 0f, WelcomeTimeline.frameA(1f).eyeBob, 1e-4f)
    }

    /** The overlay is opaque until the flash's tail, and gone at the end. */
    @Test
    fun `A fades out on the tail and ends at zero`() {
        for (t in everyMillisecond.filter { it <= 0.90f }) {
            assertEquals("t=$t", 1f, WelcomeTimeline.frameA(t).overlayAlpha, 1e-4f)
        }
        assertEquals(0f, WelcomeTimeline.frameA(1f).overlayAlpha, 1e-4f)
    }

    // ══ B — the spit ══════════════════════════════════════════════════════

    /** The four beats of B, in the order the owner listed them. */
    @Test
    fun `B twinkles, turns, spits and splats, in that order`() {
        // 1. twinkle — before the turn, and only then.
        assertTrue(WelcomeTimeline.frameB(0.10f).twinkleAlpha > 0.9f)
        assertEquals(0f, WelcomeTimeline.frameB(0.50f).twinkleAlpha, 1e-4f)

        // 2. turn — side pose gone, front pose up.
        val before = WelcomeTimeline.frameB(0.20f)
        assertTrue(before.sideAlpha > 0.9f)
        assertEquals(0f, before.frontAlpha, 1e-4f)
        val after = WelcomeTimeline.frameB(0.40f)
        assertEquals(0f, after.sideAlpha, 1e-4f)
        assertTrue(after.frontAlpha > 0.9f)
        // …and it is a turn, not a cut: the pair pinches flat through it.
        assertTrue(WelcomeTimeline.frameB(WelcomeTimeline.B_TURN).turnScaleX < 0.3f)
        assertEquals(1f, WelcomeTimeline.frameB(1f).turnScaleX, 1e-3f)

        // 3. cheeks puff, then the droplet leaves — puff first.
        assertTrue(WelcomeTimeline.frameB(0.50f).cheekScaleY > 1.05f)
        assertEquals("nothing has been spat yet", 0f, WelcomeTimeline.frameB(0.50f).dropletAlpha, 1e-4f)

        // 4. splat, and not one frame before the droplet is nearly there. The
        // storyboard starts it blooming at 82 % — the last leg of the flight —
        // and it is fully on at B_SPLAT, so the claim under test is "nothing on
        // the glass while the droplet is still crossing the room".
        for (t in everyMillisecond.filter { it <= 0.82f }) {
            assertEquals("t=$t splatted early", 0f, WelcomeTimeline.frameB(t).splatAlpha, 1e-4f)
        }
        assertTrue(WelcomeTimeline.frameB(WelcomeTimeline.B_SPLAT).splatAlpha > 0.8f)
        assertTrue(WelcomeTimeline.frameB(1f).splatAlpha > 0.5f)

        // …and the grin is last of all.
        assertEquals(0f, WelcomeTimeline.frameB(0.80f).grinAlpha, 1e-4f)
        assertEquals(1f, WelcomeTimeline.frameB(1f).grinAlpha, 1e-4f)
    }

    /**
     * "TOWARD THE VIEWER (translate down + scale up ~4x)" — both halves, and
     * both monotonic, because a droplet that hesitates on its way at you is not
     * coming at you.
     */
    @Test
    fun `the droplet comes at the viewer, down and four times bigger`() {
        val launch = WelcomeTimeline.frameB(WelcomeTimeline.B_SPIT)
        val arrival = WelcomeTimeline.frameB(WelcomeTimeline.B_SPLAT)
        assertTrue("it must travel down the screen", arrival.dropletDy > launch.dropletDy)
        assertEquals(WelcomeTimeline.B_DROPLET_TRAVEL, arrival.dropletDy, 1e-3f)
        assertTrue(
            "it must grow about fourfold: ${launch.dropletScale} -> ${arrival.dropletScale}",
            arrival.dropletScale / launch.dropletScale > 10f && arrival.dropletScale > 4f,
        )
        var lastDy = -1f
        var lastScale = -1f
        for (t in everyMillisecond.filter { it in WelcomeTimeline.B_SPIT..WelcomeTimeline.B_SPLAT }) {
            val f = WelcomeTimeline.frameB(t)
            assertTrue("t=$t dy went back", f.dropletDy >= lastDy - 1e-3f)
            assertTrue("t=$t shrank", f.dropletScale >= lastScale - 1e-3f)
            lastDy = f.dropletDy
            lastScale = f.dropletScale
        }
    }

    /** B ends cleanly too — the overlay must not still be on screen at 3.0 s. */
    @Test
    fun `B ends at zero as well`() {
        assertEquals(1f, WelcomeTimeline.frameB(0f).overlayAlpha, 1e-4f)
        assertEquals(0f, WelcomeTimeline.frameB(1f).overlayAlpha, 1e-4f)
    }

    // ══ the machinery ═════════════════════════════════════════════════════

    /**
     * Out-of-range time is clamped rather than extrapolated. Worth a test
     * because the caller is a frame clock: a late frame handing over 1.004 must
     * give the last frame of the film, not a puck three degrees past home.
     */
    @Test
    fun `time outside the film is clamped at both ends`() {
        assertEquals(WelcomeTimeline.frameA(0f), WelcomeTimeline.frameA(-2f))
        assertEquals(WelcomeTimeline.frameA(1f), WelcomeTimeline.frameA(1.004f))
        assertEquals(WelcomeTimeline.frameB(0f), WelcomeTimeline.frameB(-0.5f))
        assertEquals(WelcomeTimeline.frameB(1f), WelcomeTimeline.frameB(9f))
    }

    /**
     * The storyboard-to-master conversion, re-derived from the two landmarks
     * rather than trusted, because every distance in both films is multiplied
     * by it and a wrong value would look plausible and be uniformly wrong.
     */
    @Test
    fun `the storyboard conversion matches the two shared landmarks`() {
        val masterSpan = kotlin.math.hypot(
            (WelcomeTimeline.Art.EYE_CENTER_X - WelcomeTimeline.Art.PUCK_CENTER_X).toDouble(),
            (WelcomeTimeline.Art.EYE_CENTER_Y - WelcomeTimeline.Art.PUCK_CENTER_Y).toDouble(),
        )
        // The storyboard's own puck centre (206,114) and eye (236,158).
        val storyboardSpan = kotlin.math.hypot(236.0 - 206.0, 158.0 - 114.0)
        assertEquals(
            (masterSpan / storyboardSpan).toFloat(),
            WelcomeTimeline.STORYBOARD_TO_MASTER,
            0.01f,
        )
    }
}
