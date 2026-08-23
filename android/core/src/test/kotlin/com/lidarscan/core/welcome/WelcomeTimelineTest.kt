package com.lidarscan.core.welcome

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
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

    /**
     * ROUND 35 item 187(a)(b) — **A does not fade out, and it ends on a lit
     * lidar.**
     *
     * Round 32's version of this test asserted the opposite: opaque until 90 %,
     * zero at the end. That fade is what the owner objected to — the film
     * dissolved into the app while the flash was still happening — so the claim
     * under test is now the resting pose the one-second hold is held on.
     */
    @Test
    fun `A stays opaque to its last frame and ends on a lit lidar`() {
        for (t in everyMillisecond) {
            assertEquals("t=$t", 1f, WelcomeTimeline.frameA(t).overlayAlpha, 1e-4f)
        }
        val rest = WelcomeTimeline.frameA(1f)
        assertEquals("the LED must still be on", 1f, rest.ledAlpha, 1e-4f)
        assertTrue("and the lidar must be its own colour", rest.puckDim == 0f)
        // …and everything that was in flight has landed or left: the puck is
        // home and square, and neither the sweep nor the rings are still up.
        assertEquals(0f, rest.puckDy, 1e-3f)
        assertEquals(1f, rest.puckScaleX, 1e-3f)
        assertEquals(1f, rest.puckScaleY, 1e-3f)
        assertEquals("the sweep is a thing that happens", 0f, rest.fanAlpha, 1e-4f)
        assertEquals(0f, rest.ring1Alpha, 1e-4f)
        assertEquals(0f, rest.ring2Alpha, 1e-4f)
    }

    /**
     * ROUND 35 item 184 — **there is one emit point, and it is the light's.**
     *
     * The geometry that uses it is in the composable, and the photograph is the
     * proof the item asks for; what belongs here is the claim underneath both —
     * that the emit point and the fan bitmap's own cone apex are the same
     * coordinate, and that it is a point **on the puck** rather than a number
     * somebody liked. Round 34 had four centres and this is the one that has to
     * stay true when any of them is edited again.
     */
    @Test
    fun `the emit point is the light's own anchor, on the puck`() {
        assertEquals(WelcomeTimeline.Art.FAN_ORIGIN_X, WelcomeTimeline.Art.EMIT_X, 0f)
        assertEquals(WelcomeTimeline.Art.FAN_ORIGIN_Y, WelcomeTimeline.Art.EMIT_Y, 0f)
        val a = WelcomeTimeline.Art
        assertTrue("emit x is off the puck", a.EMIT_X in a.PUCK_LEFT..(a.PUCK_LEFT + a.PUCK_WIDTH))
        assertTrue("emit y is off the puck", a.EMIT_Y in a.PUCK_TOP..a.PUCK_FOOT_Y)
        // …and it is NOT the puck's centre, which is the whole point of the
        // item: if these ever coincide, someone has quietly moved it back.
        assertTrue(a.EMIT_X != a.PUCK_CENTER_X || a.EMIT_Y != a.PUCK_CENTER_Y)
    }

    // ══ B — the unicorn egg ═══════════════════════════════════════════════
    //
    // ROUND 37 item 189. Round 36's eleven tests asserted a lean, a jaw grind
    // and a lens covered in goo; none of that exists any more, so they are
    // **rewritten and not relaxed**. What is pinned here is the four things the
    // item names — *ribbon orbit count, horn-growth window, gallop exit
    // reaching off-screen, total duration* — plus the properties underneath
    // them that a re-time could break silently.

    /**
     * **THE WHOLE FILM, in the storyboard's own order.** Ribbons, then the
     * horn, then the rear, then an empty stage.
     *
     * The four beats, each asserted where it is *and* where it is not: a
     * timeline test that only checks that a thing happens will pass on a film
     * in which it happens continuously.
     */
    @Test
    fun `B orbits, transforms, rears and gallops off in that order`() {
        // 1. the ribbons — up, and the only thing up.
        val early = WelcomeTimeline.frameB(0.20f)
        assertTrue("both ribbons must be orbiting", early.ribbons.all { it.isNotEmpty() })
        assertEquals("the puck is still a puck", 1f, early.puckAlpha, 1e-4f)
        assertEquals("nothing has grown yet", 0f, early.hornAlpha, 1e-4f)
        assertEquals("the fleece is its own colour", 0f, early.fleeceFlood, 1e-4f)
        assertEquals("it has not moved", 0f, early.gallopX, 1e-4f)

        // 2. the transformation — the ribbons are gone by the time it starts.
        for (t in everyMillisecond.filter { it >= WelcomeTimeline.B_TRANSFORM }) {
            assertTrue(
                "t=$t: a ribbon is still up during the transformation",
                WelcomeTimeline.frameB(t).ribbons.all { it.isEmpty() },
            )
        }
        val done = WelcomeTimeline.frameB(WelcomeTimeline.B_HORN_DONE)
        assertEquals("the puck must be gone", 0f, done.puckAlpha, 1e-4f)
        assertEquals("the horn must be up", 1f, done.hornAlpha, 1e-4f)
        assertEquals("…at its own length", 1f, done.hornGrow, 1e-3f)
        assertEquals("the fleece must be flooded", 1f, done.fleeceFlood, 1e-3f)
        assertEquals("the mane must be in", 1f, done.maneAlpha, 1e-3f)

        // 3. the rear — and it is over before the gallop starts.
        assertEquals(
            "it must be back down for the launch",
            0f, WelcomeTimeline.frameB(WelcomeTimeline.B_LAUNCH).bodyRotDeg, 1e-3f,
        )

        // 4. gone, and the stage empty, before the overlay is dismissed.
        assertEquals(1f, WelcomeTimeline.frameB(WelcomeTimeline.B_EXIT).gallopX, 1e-3f)
        val last = WelcomeTimeline.frameB(1f)
        assertTrue("the stage must be empty", last.ribbons.all { it.isEmpty() })
        assertTrue(last.sparkles.isEmpty())
        assertEquals("nothing may still be trailing", 0f, last.trailAlpha, 1e-4f)
        assertEquals("…or still dusty", 0f, last.dustAlpha, 1e-4f)
    }

    /**
     * **THE RIBBON ORBIT COUNT** — the first of the four the item names.
     *
     * *"two full orbits each, phase-offset"*. Counted in revolutions rather
     * than in peaks of a coordinate: a peak count measures the ellipse and
     * would still read two if somebody halved the orbit and doubled the
     * ellipse's aspect.
     */
    @Test
    fun `each ribbon goes round exactly twice, and they are offset`() {
        assertEquals("there must be two ribbons", 2, WelcomeTimeline.B_RIBBONS.size)
        for (i in WelcomeTimeline.B_RIBBONS.indices) {
            val start = WelcomeTimeline.ribbonTurns(i, 0f)
            val end = WelcomeTimeline.ribbonTurns(i, WelcomeTimeline.B_RIBBONS_END)
            assertEquals(
                "ribbon $i must go round exactly ${WelcomeTimeline.B_ORBITS} times",
                WelcomeTimeline.B_ORBITS.toFloat(), end - start, 1e-3f,
            )
            // …and forwards the whole way. A ribbon that eases would slow at
            // the crossings, which is the one place it must not.
            var previous = -1f
            for (t in everyMillisecond.filter { it <= WelcomeTimeline.B_RIBBONS_END }) {
                val turns = WelcomeTimeline.ribbonTurns(i, t)
                assertTrue("ribbon $i went backwards at t=$t", turns >= previous - 1e-4f)
                previous = turns
            }
        }
        assertEquals("exactly two orbits, as the storyboard's iteration count", 2, WelcomeTimeline.B_ORBITS)
        val offset = WelcomeTimeline.ribbonTurns(1, 0f) - WelcomeTimeline.ribbonTurns(0, 0f)
        assertTrue(
            "the two must be phase-offset by a real part of a turn: $offset",
            offset > 0.15f && offset < 0.85f,
        )
    }

    /**
     * …and **each one passes in front of the llama and behind it**, twice a
     * revolution, which is the thing the storyboard could not draw.
     *
     * The assertion is on the sign of `depth` and on the crossings *within one
     * band*: a ribbon that is wholly in front on one frame and wholly behind on
     * the next has teleported round the animal rather than gone round it.
     */
    @Test
    fun `the ribbons cross in front of and behind the llama on every orbit`() {
        for (i in WelcomeTimeline.B_RIBBONS.indices) {
            var crossings = 0
            var inFront: Boolean? = null
            var everSplit = false
            for (t in everyMillisecond.filter { it <= WelcomeTimeline.B_RIBBONS_END }) {
                val band = WelcomeTimeline.ribbonAt(i, t)
                if (band.isEmpty()) continue
                val head = band.first().depth > 0f
                if (inFront != null && head != inFront) crossings++
                inFront = head
                // A band that spans a crossing has samples on both sides of the
                // animal at once, which is the frame the layer swap exists for.
                if (band.any { it.depth > 0f } && band.any { it.depth < 0f }) everSplit = true
            }
            // A crossing is a HALF-turn boundary — the ellipse's two ends are
            // where near becomes far — so how many fall inside a ribbon's own
            // window is a function of its phase, and it is computed rather than
            // assumed: ribbon 0 starts at phase 0, which is itself a crossing,
            // and therefore shows one fewer than ribbon 1 does. Both are still
            // two per orbit, which is what the floor below pins.
            val start = 2f * WelcomeTimeline.ribbonTurns(i, 0f)
            val end = 2f * WelcomeTimeline.ribbonTurns(i, WelcomeTimeline.B_RIBBONS_END)
            val expected = kotlin.math.ceil(end).toInt() - kotlin.math.floor(start).toInt() - 1
            assertTrue(
                "the arithmetic under this test is wrong, not the film: $expected",
                expected >= 2 * WelcomeTimeline.B_ORBITS - 1,
            )
            assertEquals(
                "ribbon $i crossed $crossings times, not $expected",
                expected, crossings,
            )
            assertTrue("ribbon $i is never half in front and half behind", everSplit)
        }
        // The band is a band: enough samples to be one, tapering to its tail.
        val band = WelcomeTimeline.ribbonAt(0, 0.20f)
        assertEquals(WelcomeTimeline.B_RIBBON_SEGMENTS, band.size)
        assertTrue("the tail must be fainter than the head", band.last().alpha < band.first().alpha)
        assertTrue("…and thinner", band.last().halfWidth < band.first().halfWidth)
        // …and it is a rainbow along its length, not one colour.
        assertTrue("the band must run through the ramp", band.map { it.hue }.distinct().size > 10)
    }

    /**
     * **THE HORN-GROWTH WINDOW** — the second of the four.
     *
     * *"1.3–1.6s … THE LIDAR PUCK SPRITE STRETCHES INTO THE SPIRAL HORN — a
     * morph anchored at the puck's seat (scaleY growth from the puck's base)"*.
     *
     * Four claims, and the third is the one that makes it a morph: the puck
     * **stretches** as it goes rather than being swapped out, and it is still
     * on screen while the horn is arriving.
     */
    @Test
    fun `the horn grows out of the stretching puck between 1_3 and 1_6 seconds`() {
        fun seconds(t: Float) = t * WelcomeAnimation.DURATION_MS / 1000f
        assertEquals("the window opens", 1.3f, seconds(WelcomeTimeline.B_TRANSFORM), 0.05f)
        assertEquals("…and shuts", 1.6f, seconds(WelcomeTimeline.B_HORN_DONE), 0.05f)

        // Nothing before it: the puck is whole, unstretched, and there is no horn.
        for (t in everyMillisecond.filter { it <= WelcomeTimeline.B_TRANSFORM }) {
            val f = WelcomeTimeline.frameB(t)
            assertEquals("t=$t grew a horn early", 0f, f.hornAlpha, 1e-4f)
            assertEquals("t=$t stretched the puck early", 1f, f.puckStretch, 1e-3f)
            assertEquals("t=$t faded the puck early", 1f, f.puckAlpha, 1e-4f)
        }

        // The morph: the puck stretches up and narrows while it fades, so the
        // shape it hands over is already horn-shaped.
        val mid = WelcomeTimeline.frameB(0.455f)
        assertTrue("the puck must still be there mid-morph", mid.puckAlpha > 0.05f)
        assertTrue("…and stretched by then: ${mid.puckStretch}", mid.puckStretch > 1.4f)
        assertTrue("…and narrowed: ${mid.puckNarrow}", mid.puckNarrow < 0.8f)
        assertTrue("the horn must be arriving under it", mid.hornAlpha > 0.05f)

        // scaleY growth, and it OVERSHOOTS — the storyboard's
        // cubic-bezier(.3,1.4,.5,1) with its 1.12 stop, which is the difference
        // between a horn that grew and a horn that was placed.
        val peak = everyMillisecond.maxOf { WelcomeTimeline.frameB(it).hornGrow }
        assertTrue("the horn must overshoot its own length: $peak", peak > 1.05f)
        assertTrue("…but not by a lot: $peak", peak < 1.25f)
        assertEquals(
            "…and settle at exactly its length",
            1f, WelcomeTimeline.frameB(WelcomeTimeline.B_HORN_DONE).hornGrow, 1e-3f,
        )
        assertEquals("…and stay there", 1f, WelcomeTimeline.frameB(1f).hornGrow, 1e-3f)
        // It never shrinks below the sixth of itself it starts at, which is
        // where it is when the ribbons hand over.
        assertTrue(everyMillisecond.all { WelcomeTimeline.frameB(it).hornGrow >= 0.14f })
    }

    /** …and the flood, the mane, the flash and the three sparkles land with it. */
    @Test
    fun `the flash, the sparkles, the flood and the mane are all one event`() {
        // The flash: one peak, inside the window, and gone.
        val lit = everyMillisecond.filter { WelcomeTimeline.frameB(it).flash > 0.5f }
        assertTrue("there must be a flash", lit.isNotEmpty())
        assertTrue(
            "the flash must be inside the transformation: ${lit.first()}‥${lit.last()}",
            lit.first() >= WelcomeTimeline.B_TRANSFORM && lit.last() <= WelcomeTimeline.B_HORN_DONE + 0.02f,
        )
        assertEquals("it must go out", 0f, WelcomeTimeline.frameB(WelcomeTimeline.B_REAR_PEAK).flash, 1e-3f)

        // Three sparkles, item 189's own count, and they are staggered.
        assertEquals(3, WelcomeTimeline.B_SPARKLES.size)
        assertEquals(
            "they must not all arrive together",
            3, WelcomeTimeline.B_SPARKLES.map { it.delay }.distinct().size,
        )
        val most = everyMillisecond.maxOf { WelcomeTimeline.frameB(it).sparkles.size }
        assertEquals("all three must be up at once at some point", 3, most)
        for (i in WelcomeTimeline.B_SPARKLES.indices) {
            assertNull("sparkle $i must not be up at the start", WelcomeTimeline.sparkleAt(i, 0.2f))
            assertNull("sparkle $i must not be up at the end", WelcomeTimeline.sparkleAt(i, 1f))
            val peak = everyMillisecond.mapNotNull { WelcomeTimeline.sparkleAt(i, it) }
            assertTrue("sparkle $i never reached full", peak.maxOf { it.alpha } > 0.95f)
            // It over-sizes and comes back, which is the storyboard's 1.2 stop.
            assertTrue("sparkle $i never over-sized", peak.maxOf { it.radius } > WelcomeTimeline.B_SPARKLES[i].size)
            // …and it turns the whole way through.
            assertTrue("sparkle $i does not turn", peak.last().spinDeg > peak.first().spinDeg + 20f)
        }

        // The flood is monotonic — a front that runs down the animal and does
        // not come back up — and it is finished with the horn.
        var previous = -1f
        for (t in everyMillisecond) {
            val flood = WelcomeTimeline.frameB(t).fleeceFlood
            assertTrue("the flood receded at t=$t", flood >= previous - 1e-4f)
            previous = flood
        }
        assertEquals(1f, WelcomeTimeline.frameB(WelcomeTimeline.B_HORN_DONE).fleeceFlood, 1e-3f)
        // …and the mane arrives with it rather than after it, which is the one
        // place the owner's note overrules the storyboard's own 54–60 % stops.
        assertEquals(1f, WelcomeTimeline.frameB(WelcomeTimeline.B_HORN_DONE).maneAlpha, 1e-3f)
        assertEquals(0f, WelcomeTimeline.frameB(WelcomeTimeline.B_TRANSFORM).maneAlpha, 1e-4f)
    }

    /**
     * **THE REAR-UP.** Between 1.6 s and 2.0 s, front lifted 6–10°, about the
     * base — and it is a *rear*, so the weight goes down before it goes up.
     */
    @Test
    fun `it rears up six to ten degrees between 1_6 and 2_0 seconds`() {
        val window = everyMillisecond.filter {
            it in WelcomeTimeline.B_HORN_DONE..WelcomeTimeline.B_LAUNCH
        }
        val lift = window.minOf { WelcomeTimeline.frameB(it).bodyRotDeg }
        // Negative because the animal faces right: lifting the front is
        // anticlockwise on a screen whose y runs down.
        assertTrue("it must rear 6-10°, not ${-lift}", -lift in 6f..10f)
        assertEquals("…and item 189's own figure", -9f, WelcomeTimeline.B_REAR_DEG, 1e-4f)

        // The crouch: it dips before it lifts, which is what stops the rear
        // reading as a hinge opening.
        val crouch = window.maxOf { WelcomeTimeline.frameB(it).bodyDy }
        assertTrue("it must load before it lifts: $crouch", crouch > 3f)
        val rise = window.minOf { WelcomeTimeline.frameB(it).bodyDy }
        assertTrue("…and then come up: $rise", rise < -20f)
        // The crouch is BEFORE the lift and not after it.
        val crouchAt = window.maxByOrNull { WelcomeTimeline.frameB(it).bodyDy }!!
        val riseAt = window.minByOrNull { WelcomeTimeline.frameB(it).bodyDy }!!
        assertTrue("it lifted before it crouched", crouchAt < riseAt)

        // Nothing rears during the transformation, and nothing is left over at
        // the launch.
        assertEquals(0f, WelcomeTimeline.frameB(WelcomeTimeline.B_HORN_DONE).bodyRotDeg, 1e-3f)
        assertEquals(0f, WelcomeTimeline.frameB(WelcomeTimeline.B_LAUNCH).bodyDy, 1e-3f)
    }

    /**
     * **THE GALLOP EXIT REACHING OFF-SCREEN** — the third of the four, and the
     * one that is a claim about a *device* rather than about a curve.
     *
     * `gallopX` is a fraction, exactly as animation A's ring scale is, because
     * the storyboard's `translateX(430px)` is true of a 340 px stage and of
     * nothing else. So there are two halves to check: that the fraction gets to
     * 1 before the film ends, and that 1 is genuinely off the right-hand edge —
     * on the widest and the narrowest art box this app can produce.
     */
    @Test
    fun `the gallop leaves the screen to the right, on every screen`() {
        // The curve: forwards only, arriving before the stage has to be empty.
        var previous = -1f
        for (t in everyMillisecond) {
            val x = WelcomeTimeline.frameB(t).gallopX
            assertTrue("it went backwards at t=$t", x >= previous - 1e-4f)
            previous = x
        }
        assertEquals("still standing at the launch", 0f, WelcomeTimeline.frameB(WelcomeTimeline.B_LAUNCH).gallopX, 1e-4f)
        assertEquals("gone by the exit", 1f, WelcomeTimeline.frameB(WelcomeTimeline.B_EXIT).gallopX, 1e-3f)

        // …and 1 is off the edge. The art box is `min(0.80 w, 0.46 h)` centred
        // horizontally, so this is every phone from a 4:3 tablet to a 21:9
        // handset, plus a square one that does not exist to catch the case
        // where the height is what binds.
        for ((w, h) in listOf(
            1080f to 2340f, 1440f to 3120f, 1080f to 2520f, 1200f to 1600f, 1000f to 1000f,
        )) {
            val side = minOf(w * 0.80f, h * 0.46f)
            val left = (w - side) / 2f
            val scale = side / WelcomeTimeline.Art.CANVAS
            val travel = WelcomeTimeline.exitTravelMasterUnits(w, left, scale)
            // The last pixel of the animal is the master canvas's own left edge.
            val trailingEdge = left + travel * scale
            assertTrue(
                "at ${w.toInt()}×${h.toInt()} the llama is still on screen: $trailingEdge vs $w",
                trailingEdge > w,
            )
            // …and it has not been sent to the next county, which would spend
            // the whole second of gallop off-screen.
            assertTrue("at ${w.toInt()}×${h.toInt()} it overshoots absurdly", trailingEdge < w + side)
        }
    }

    /** …with two to three bounces on the way out, and a roll with each one. */
    @Test
    fun `the gallop bounces two to three times and rolls with each stride`() {
        assertTrue("2-3 bounces", WelcomeTimeline.B_GALLOP_BOUNCES in 2..3)
        val run = everyMillisecond.filter { it in WelcomeTimeline.B_LAUNCH..WelcomeTimeline.B_EXIT }
        val bob = run.map { WelcomeTimeline.gallopBob(it) }
        assertTrue("it never left the ground: ${bob.min()}", bob.min() < -30f)
        assertTrue("it never came back down: ${bob.max()}", bob.max() > 30f)
        // Peaks, counted as sign changes of the difference. Two per cycle —
        // and the direction is SEEDED from the first real step rather than
        // assumed, because a bounce starts by going up and a test that assumes
        // it starts by going down counts the launch itself as a peak.
        var reversals = 0
        var rising: Boolean? = null
        for (i in 1 until bob.size) {
            val d = bob[i] - bob[i - 1]
            if (kotlin.math.abs(d) < 1e-5f) continue
            if (rising != null && (d > 0f) != rising) reversals++
            rising = d > 0f
        }
        assertEquals(
            "$reversals reversals is not ${WelcomeTimeline.B_GALLOP_BOUNCES} bounces",
            2 * WelcomeTimeline.B_GALLOP_BOUNCES, reversals,
        )
        // It starts and ends level, so the bounce cannot leave the animal
        // hanging when the stage is supposed to be empty.
        assertEquals(0f, WelcomeTimeline.gallopBob(WelcomeTimeline.B_LAUNCH), 1e-3f)
        assertEquals(0f, WelcomeTimeline.gallopBob(WelcomeTimeline.B_EXIT), 1e-3f)
        assertEquals("nothing bounces before it goes", 0f, WelcomeTimeline.gallopBob(0.5f), 1e-4f)
        // The roll goes with it, and it never rolls the wrong way — a gallop
        // that pitches nose-down is a fall.
        assertTrue(run.all { WelcomeTimeline.gallopTilt(it) >= -1e-3f })
        assertTrue(run.maxOf { WelcomeTimeline.gallopTilt(it) } > WelcomeTimeline.B_GALLOP_TILT_DEG)
    }

    /**
     * **The trail follows it out and the dust stays where it pushed off.**
     *
     * The dust is the one thing in the beat that must *not* move with the
     * animal: dust that travels is smoke. That is a property of the timeline
     * only in that it must fire on the launch and be gone before the end; where
     * it is drawn is the composable's, and it is drawn on the base pivot.
     */
    @Test
    fun `the trail runs out behind it and the dust is spent at the launch point`() {
        assertEquals("nothing trails before it goes", 0f, WelcomeTimeline.frameB(0.5f).trailAlpha, 1e-4f)
        assertEquals("no dust before it goes", 0f, WelcomeTimeline.frameB(WelcomeTimeline.B_LAUNCH).dustAlpha, 1e-4f)
        assertTrue("the dust must fire on the launch", WelcomeTimeline.frameB(0.71f).dustAlpha > 0.5f)
        assertTrue("the trail must be up mid-run", WelcomeTimeline.frameB(0.78f).trailAlpha > 0.5f)
        // Both are gone before the stage has to be empty.
        assertEquals(0f, WelcomeTimeline.frameB(WelcomeTimeline.B_EXIT).trailAlpha, 1e-3f)
        assertEquals(0f, WelcomeTimeline.frameB(WelcomeTimeline.B_EXIT).dustAlpha, 1e-3f)
        // The dust spreads once and does not pulse.
        var previous = -1f
        for (t in everyMillisecond) {
            val spread = WelcomeTimeline.frameB(t).dustSpread
            assertTrue("the dust contracted at t=$t", spread >= previous - 1e-4f)
            previous = spread
        }
    }

    /**
     * **The llama watches**, and only while there is something to watch.
     *
     * Item 189 makes the follow optional, so what is asserted is the shape of
     * it rather than its existence: if it is there, it tracks the ribbon in
     * front, it is small, and it stops dead when they do — an eye still
     * wandering during the gallop is a llama with something in it.
     */
    @Test
    fun `the llama watches the ribbons and stops when they go`() {
        val looks = everyMillisecond
            .filter { it < WelcomeTimeline.B_RIBBONS_END }
            .map { WelcomeTimeline.eyeFollow(it) }
        assertTrue("the eye must move at all", looks.any { kotlin.math.abs(it.first) > 4f })
        assertTrue("…both ways", looks.any { it.first < -4f } && looks.any { it.first > 4f })
        assertTrue(
            "the follow must stay inside the face",
            looks.all { kotlin.math.abs(it.first) <= 16f && kotlin.math.abs(it.second) <= 11f },
        )
        for (t in everyMillisecond.filter { it >= WelcomeTimeline.B_RIBBONS_END }) {
            val (x, y) = WelcomeTimeline.eyeFollow(t)
            assertEquals("t=$t still watching", 0f, x, 1e-4f)
            assertEquals("t=$t still watching", 0f, y, 1e-4f)
        }
        // The watch-tilt is subtle, and it is over before the rear needs the
        // same track: nothing may be left of it at 1.3 s.
        assertTrue(
            "the watch tilt is not subtle",
            everyMillisecond.filter { it < WelcomeTimeline.B_RIBBONS_END }
                .all { kotlin.math.abs(WelcomeTimeline.frameB(it).bodyRotDeg) <= WelcomeTimeline.B_WATCH_TILT_DEG + 1e-3f },
        )
        assertEquals(0f, WelcomeTimeline.frameB(WelcomeTimeline.B_TRANSFORM).bodyRotDeg, 1e-4f)
    }

    /**
     * **THE TOTAL DURATION** — the last of the four the item names, and the
     * beats inside it, ±0.2 s, which is the tuning room item 189 allows.
     */
    @Test
    fun `B's beats land on the times item 189 names`() {
        fun seconds(t: Float) = t * WelcomeAnimation.DURATION_MS / 1000f
        assertEquals("the whole film", 3.0f, WelcomeAnimation.DURATION_MS / 1000f, 1e-3f)
        assertEquals("the ribbons end", 1.3f, seconds(WelcomeTimeline.B_RIBBONS_END), 0.2f)
        assertEquals("the transformation starts", 1.3f, seconds(WelcomeTimeline.B_TRANSFORM), 0.2f)
        assertEquals("…and is done", 1.6f, seconds(WelcomeTimeline.B_HORN_DONE), 0.2f)
        assertEquals("the rear peaks", 1.8f, seconds(WelcomeTimeline.B_REAR_PEAK), 0.2f)
        assertEquals("the gallop starts", 2.0f, seconds(WelcomeTimeline.B_LAUNCH), 0.2f)
        // …and the stage is empty for about a fifth of a second before the
        // overlay is dismissed, which is a beat the owner asked for by name.
        val empty = 3.0f - seconds(WelcomeTimeline.B_EXIT)
        assertEquals("the empty stage", 0.2f, empty, 0.1f)
    }

    /** B ends cleanly too — the overlay must not still be on screen at 3.0 s. */
    @Test
    fun `B ends at zero as well`() {
        assertEquals(1f, WelcomeTimeline.frameB(0f).overlayAlpha, 1e-4f)
        assertEquals(
            "it must still be opaque while the stage empties",
            1f, WelcomeTimeline.frameB(WelcomeTimeline.B_EXIT).overlayAlpha, 1e-4f,
        )
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
