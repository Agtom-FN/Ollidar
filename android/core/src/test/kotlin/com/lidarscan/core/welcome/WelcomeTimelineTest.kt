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

    // ══ B — the reference video's two sections ════════════════════════════

    /**
     * ROUND 36 item 188 — **the two sections, in the reference's own order.**
     *
     * Round 35's version of this test asserted a tell, two bursts and six marks
     * on the glass. None of that is in the video the owner handed over, so the
     * test is rewritten rather than relaxed: what it pins now is *expectation,
     * cut, jaw, and then nothing but goo.*
     */
    @Test
    fun `B leans in, cuts, grinds its jaw and then the lens is gone`() {
        // 1. the expectation: the side pose, leaning, with a blink in it.
        val early = WelcomeTimeline.frameB(0.10f)
        assertTrue("the side pose must be up", early.sideAlpha > 0.9f)
        assertEquals(0f, early.frontAlpha, 1e-4f)
        assertTrue("it must be leaning in by now: ${early.lean}", early.lean > 0.2f)
        assertEquals("at full reach", 1f, WelcomeTimeline.frameB(WelcomeTimeline.B_TAKE).lean, 1e-3f)

        // 2. the cut: side gone, front up, and it is a turn rather than a swap —
        // the pair pinches flat through it.
        val after = WelcomeTimeline.frameB(WelcomeTimeline.B_FRONT)
        assertEquals(0f, after.sideAlpha, 1e-4f)
        assertTrue(after.frontAlpha > 0.9f)
        assertTrue(WelcomeTimeline.frameB(WelcomeTimeline.B_TURN).turnScaleX < 0.3f)
        assertEquals(1f, WelcomeTimeline.frameB(1f).turnScaleX, 1e-3f)

        // 3. the reality: the jaw is working and the eyes are lidded.
        val staring = WelcomeTimeline.frameB(0.55f)
        assertTrue("the eyes must narrow", staring.eyeNarrow > 0.95f)
        assertTrue("the ears must be back", staring.earPin > 0.95f)

        // 4. nothing is in the air, and the lens is clean, until the hit.
        for (t in everyMillisecond.filter { it < WelcomeTimeline.B_HIT }) {
            val f = WelcomeTimeline.frameB(t)
            assertTrue("t=$t fired early", f.spray.isEmpty())
            assertTrue("t=$t covered the lens early", f.lens.isEmpty())
            assertEquals("t=$t washed the lens early", 0f, f.lensWash, 1e-4f)
        }

        // 5. …and after it, the lens stays gone to the last frame.
        assertEquals(1f, WelcomeTimeline.frameB(1f).lensWash, 1e-4f)
        assertEquals(WelcomeTimeline.B_LENS.size, WelcomeTimeline.frameB(1f).lens.size)

        // 6. the grin is last of all, and it is behind the patch that ran clear.
        assertEquals(0f, WelcomeTimeline.frameB(0.78f).grinAlpha, 1e-4f)
        assertEquals(1f, WelcomeTimeline.frameB(1f).grinAlpha, 1e-4f)
        assertTrue(WelcomeTimeline.frameB(1f).lensPatch > 0.99f)
    }

    /**
     * **SECTION ONE — the expectation.** The animal comes to you, blinks once,
     * and eases off again; nothing else happens in the first second, which is
     * the whole point of it. A set-up that is doing three things is not a
     * set-up.
     */
    @Test
    fun `section one is a lean and one soft blink, and nothing else`() {
        assertEquals("it starts standing", 0f, WelcomeTimeline.frameB(0f).lean, 1e-4f)

        // Reach, take, ease off — and never back to attention, because an
        // animal that has just been fed does not stand back up straight.
        val reach = everyMillisecond.filter { it <= WelcomeTimeline.B_TAKE }
        var last = -1f
        for (t in reach) {
            val lean = WelcomeTimeline.frameB(t).lean
            assertTrue("t=$t the lean went backwards", lean >= last - 1e-3f)
            last = lean
        }
        val leaving = WelcomeTimeline.frameB(WelcomeTimeline.B_CUT).lean
        assertTrue("it must ease off: $leaving", leaving in 0.15f..0.75f)

        // Exactly one blink, and it is inside section one.
        val shut = everyMillisecond.filter { WelcomeTimeline.frameB(it).blink > 0.5f }
        assertTrue("there must be a blink", shut.isNotEmpty())
        assertTrue(
            "the blink must be inside the first section",
            shut.all { it < WelcomeTimeline.B_CUT },
        )
        assertTrue(
            "the blink must be one closure, not a flutter",
            shut.last() - shut.first() < 0.05f,
        )
        assertEquals("and it must open again", 0f, WelcomeTimeline.frameB(0.30f).blink, 1e-3f)

        // …and the reality has not started: ears up, jaw still, eyes round.
        for (t in everyMillisecond.filter { it < WelcomeTimeline.B_CUT }) {
            val f = WelcomeTimeline.frameB(t)
            assertEquals("t=$t pinned its ears in section one", 0f, f.earPin, 1e-4f)
            assertEquals("t=$t ground its jaw in section one", 0f, f.jawGrind, 1e-4f)
            assertEquals("t=$t narrowed its eyes in section one", 0f, f.eyeNarrow, 1e-4f)
        }
    }

    /**
     * **SECTION TWO — the tell.** The reference's second animal chews *at* the
     * camera for six seconds: the jaw goes side to side, the eyes never leave
     * you, and the head does almost nothing. All four halves of that are here.
     */
    @Test
    fun `the jaw grinds side to side under a lidded stare`() {
        // Two to three grinds, counted as the times it changes direction, and
        // it must go BOTH ways — a jaw that only ever swings right is a head
        // that is turning.
        val grind = everyMillisecond
            .filter { it in WelcomeTimeline.B_FRONT..WelcomeTimeline.B_HIT }
            .map { WelcomeTimeline.frameB(it).jawGrind }
        assertTrue("it never went left: ${grind.min()}", grind.min() < -0.9f)
        assertTrue("it never went right: ${grind.max()}", grind.max() > 0.9f)
        var reversals = 0
        var rising = true
        for (i in 1 until grind.size) {
            val d = grind[i] - grind[i - 1]
            if (kotlin.math.abs(d) < 1e-5f) continue
            if ((d > 0f) != rising) {
                reversals++
                rising = d > 0f
            }
        }
        // Five reversals is two and a half round trips, which is the item's
        // "2-3 grinds". Fewer reads as a flinch; more reads as a wobble.
        assertTrue("$reversals reversals is not 2-3 grinds", reversals in 4..7)

        // The stare: lidded, and it never opens up again.
        for (t in everyMillisecond.filter { it >= 0.50f }) {
            assertTrue("t=$t stopped staring", WelcomeTimeline.frameB(t).eyeNarrow > 0.9f)
            assertTrue("t=$t put its ears up", WelcomeTimeline.frameB(t).earPin > 0.9f)
        }

        // The head barely moves while the jaw does all of it — the whole
        // difference between menace and a tantrum. The stare, up to the tenth
        // of a second the hit draws back in, stays inside a third of the snap
        // the hit itself makes.
        val duringRise = everyMillisecond
            .filter { it in WelcomeTimeline.B_FRONT..(WelcomeTimeline.B_HIT - 0.04f) }
            .maxOf { kotlin.math.abs(WelcomeTimeline.frameB(it).headRise) }
        val snap = everyMillisecond.maxOf { WelcomeTimeline.frameB(it).headRise }
        assertTrue("the head moved too much during the stare: $duringRise vs $snap", duringRise < snap / 3f)

        // …and the mouth is never shut while it chews: a jaw grinding behind a
        // closed mouth is a shape sliding about under the fleece.
        for (t in everyMillisecond.filter { it in 0.42f..(WelcomeTimeline.B_HIT - 0.01f) }) {
            assertTrue("t=$t chewed with its mouth shut", WelcomeTimeline.frameB(t).mouthOpen > 0.05f)
        }
        // It goes wide on the hit.
        assertTrue(WelcomeTimeline.frameB(WelcomeTimeline.B_HIT).mouthOpen > 0.95f)
    }

    /**
     * **THE HIT.** In the reference there is one smeared frame between a llama
     * and an opaque lens. The cone is therefore a *frame*, not a flight: every
     * particle leaves and arrives inside two of them, and the whole thing is
     * over before the cover is finished.
     */
    @Test
    fun `the cone is one frame, and it is a cone`() {
        val out = WelcomeTimeline.B_SPRAY.map { it.launch }
        assertTrue("something left before the hit", out.min() >= WelcomeTimeline.B_HIT)
        assertTrue(
            "the cone dribbles out instead of leaving at once",
            out.max() - out.min() < WelcomeTimeline.B_FRAME,
        )
        assertTrue(
            "the cone is still in the air after the lens is covered",
            WelcomeTimeline.B_SPRAY.maxOf { it.landing } < WelcomeTimeline.B_COVERED,
        )
        assertTrue(
            "the cone is not seen at all",
            WelcomeTimeline.frameB(WelcomeTimeline.B_HIT + WelcomeTimeline.B_FRAME / 2f)
                .spray.size >= 10,
        )

        // A cone, not a column: it opens both ways, and the sizes vary, which
        // is what stops one frame of it reading as a ring of identical dots.
        val wide = WelcomeTimeline.frameB(WelcomeTimeline.B_HIT + 0.008f).spray
        assertTrue("nothing went left", wide.any { it.dx < -80f })
        assertTrue("nothing went right", wide.any { it.dx > 80f })
        val radii = wide.filter { !it.mist }.map { it.radius }
        assertTrue("all one size", radii.max() > radii.min() * 1.8f)

        // 10-16 drops with 2-3 puffs of mist in them, and the mist is soft.
        val drops = WelcomeTimeline.B_SPRAY.count { !it.mist }
        val mist = WelcomeTimeline.B_SPRAY.count { it.mist }
        assertTrue("10-16 particles, not $drops", drops in 10..16)
        assertTrue("2-3 mist puffs, not $mist", mist in 2..3)
        assertTrue(
            "mist must be soft",
            wide.filter { it.mist }.all { it.alpha <= WelcomeTimeline.B_MIST_ALPHA },
        )
    }

    /**
     * Every particle comes **at the viewer** — down the screen and growing —
     * and is gone by the time it gets there. Round 35 asserted a handover to a
     * mark on the glass; item 188 has no marks, because the thing it hands over
     * to is the whole screen.
     */
    @Test
    fun `every particle comes at the viewer and is spent when it arrives`() {
        for (i in WelcomeTimeline.B_SPRAY.indices) {
            val shot = WelcomeTimeline.B_SPRAY[i]
            assertTrue("shot $i: not airborne at launch", WelcomeTimeline.sprayAt(i, shot.launch) != null)
            assertTrue(
                "shot $i: still airborne after landing",
                WelcomeTimeline.sprayAt(i, shot.landing + 0.001f) == null,
            )
            if (shot.mist) continue

            var lastDy = -1f
            var lastRadius = -1f
            // A tenth of a millisecond, because the whole flight is a frame.
            for (step in 0..200) {
                val t = shot.launch + shot.flight * step / 200f
                val d = WelcomeTimeline.sprayAt(i, t) ?: continue
                assertTrue("shot $i at t=$t went back up", d.dy >= lastDy - 1e-3f)
                assertTrue("shot $i at t=$t shrank", d.radius >= lastRadius - 1e-3f)
                assertTrue("shot $i at t=$t is not stretched along its flight", d.stretch >= 1f)
                lastDy = d.dy
                lastRadius = d.radius
            }
            val arrival = WelcomeTimeline.sprayAt(i, shot.landing)!!
            assertEquals(
                "shot $i must reach exactly its own travel",
                shot.reach * WelcomeTimeline.B_SPRAY_TRAVEL, arrival.dy, 1e-2f,
            )
            assertEquals("shot $i must be spent at the glass", 0f, arrival.alpha, 1e-3f)
            assertTrue(
                "shot $i must grow at least fourfold on the way",
                arrival.radius > 4f * WelcomeTimeline.sprayAt(i, shot.launch)!!.radius,
            )
        }
    }

    /**
     * **THE WIPEOUT.** The gag is a cut, so the assertion is about *frames*:
     * one clean one after the hit, and the lens gone two later — and it never
     * comes back.
     */
    @Test
    fun `the lens goes between two frames and stays gone`() {
        val hit = WelcomeTimeline.B_HIT
        assertEquals("the wash is early", 0f, WelcomeTimeline.frameB(hit).lensWash, 1e-4f)
        assertEquals(
            "the cone's own frame must be clean",
            0f, WelcomeTimeline.frameB(hit + WelcomeTimeline.B_FRAME).lensWash, 1e-4f,
        )
        assertEquals(
            "the lens must be gone by B_COVERED",
            1f, WelcomeTimeline.frameB(WelcomeTimeline.B_COVERED).lensWash, 1e-3f,
        )
        assertTrue(
            "the cover must not take longer than three frames",
            WelcomeTimeline.B_COVERED - hit <= 3f * WelcomeTimeline.B_FRAME + 1e-4f,
        )
        // Every blob, however late it starts, is complete at the same instant.
        for (i in WelcomeTimeline.B_LENS.indices) {
            val at = WelcomeTimeline.lensAt(i, WelcomeTimeline.B_COVERED)
            assertTrue("blob $i is not there when the lens is gone", at != null)
            assertEquals(
                "blob $i is still arriving",
                WelcomeTimeline.B_LENS_ALPHA, at!!.alpha, 1e-3f,
            )
        }
        // …and it is still there on the last frame of the film, a little dulled
        // so the grin behind the patch is the brightest thing left.
        val end = WelcomeTimeline.frameB(1f).lens
        assertEquals(WelcomeTimeline.B_LENS.size, end.size)
        // Dulled, but nowhere near gone — it dries, it does not evaporate.
        assertTrue(end.all { it.alpha > WelcomeTimeline.B_LENS_ALPHA * 0.6f })
        assertTrue(end.all { it.alpha < WelcomeTimeline.B_LENS_ALPHA })
        assertTrue(end.all { it.ink < WelcomeTimeline.B_LENS_INK })
    }

    /**
     * **~90 % of the screen**, measured rather than asserted by eye.
     *
     * The eighteen shapes are rasterised the way the composable draws them —
     * the same lumpy polygon out of the same tables, rotated then stretched —
     * onto a 21:9 handset, and the covered fraction is counted. Both bounds
     * matter: below the floor the cover has holes in it, and above the ceiling
     * the blobs have closed into a single sheet and stopped being blobs.
     */
    @Test
    fun `the blobs cover about ninety per cent of the screen`() {
        for (aspect in floatArrayOf(0.42f, 1080f / 2340f, 0.5625f)) {
            val covered = lensCoverage(aspect)
            assertTrue(
                "aspect $aspect: only ${(covered * 100).toInt()} % covered",
                covered >= WelcomeTimeline.B_LENS_COVERAGE,
            )
            assertTrue(
                "aspect $aspect: ${(covered * 100).toInt()} % is a sheet, not blobs",
                covered <= 0.985f,
            )
        }
    }

    /**
     * …and **nothing sits on the patch that runs clear.**
     *
     * The grin is seen through a thin place over the llama's mouth, and the
     * mouth moves down the screen as the art box grows on a squarer device. No
     * blob's centre may be within its own radius of it anywhere in that range —
     * a soft `DstOut` bite can thin the wash but it cannot dig an opaque core
     * out of the middle of it.
     */
    @Test
    fun `no blob sits on the patch the grin shows through`() {
        for (aspect in floatArrayOf(0.42f, 1080f / 2340f, 0.5f, 0.5625f)) {
            val mouth = 0.30f + 0.701f * minOf(0.80f * aspect, 0.46f)
            assertTrue(
                "the mouth at aspect $aspect is outside the stated range: $mouth",
                mouth in WelcomeTimeline.B_MOUTH_Y_MIN..WelcomeTimeline.B_MOUTH_Y_MAX,
            )
            for (i in WelcomeTimeline.B_LENS.indices) {
                val clearance = WelcomeTimeline.lensGapClearance(i, aspect, mouth)
                assertTrue(
                    "blob $i is on the mouth at aspect $aspect: clearance $clearance",
                    clearance > 0f,
                )
            }
        }
    }

    /**
     * **THE HOLD.** One or two drips, running for the whole of it, and the
     * patch opening after the cover rather than with it — the reference's first
     * covered frame is opaque and the shape behind it only swims back as the
     * stuff runs off.
     */
    @Test
    fun `the cover drips through the hold and the patch opens after it`() {
        val running = WelcomeTimeline.B_LENS.count { it.drip > 0f }
        assertTrue("1-2 drips, not $running", running in 1..2)

        fun drips(t: Float) = WelcomeTimeline.frameB(t).lens.sumOf { it.drip.toDouble() }
        assertEquals("nothing runs on the frame it lands", 0.0, drips(WelcomeTimeline.B_COVERED), 1e-6)
        assertTrue("a drip must actually run", drips(0.86f) > drips(0.78f))
        assertTrue("…and keep running", drips(1f) > drips(0.86f))

        // The patch is shut when the lens goes and open by the grin.
        assertEquals(0f, WelcomeTimeline.frameB(WelcomeTimeline.B_COVERED).lensPatch, 1e-4f)
        assertTrue(
            "the grin has nothing to show through",
            WelcomeTimeline.frameB(WelcomeTimeline.B_GRIN).lensPatch > 0.6f,
        )
    }

    /**
     * **The whole thing still fits in three seconds**, and the beats land where
     * item 188 put them (±0.2 s, which is the tuning room it allows).
     */
    @Test
    fun `B's beats land on the times item 188 names`() {
        fun seconds(t: Float) = t * WelcomeAnimation.DURATION_MS / 1000f
        assertEquals("the cut", 1.0f, seconds(WelcomeTimeline.B_CUT), 0.2f)
        assertEquals("the front pose", 1.2f, seconds(WelcomeTimeline.B_FRONT), 0.2f)
        assertEquals("the hit", 2.1f, seconds(WelcomeTimeline.B_HIT), 0.2f)
        assertEquals("the grin", 2.4f, seconds(WelcomeTimeline.B_GRIN), 0.2f)
        assertEquals("the whole film", 3.0f, WelcomeAnimation.DURATION_MS / 1000f, 1e-3f)
        // Section one is a second of it and the tell is nine tenths: any less
        // and the set-up does not land, any more and the stare is a pause.
        assertEquals("section one", 1.0f, seconds(WelcomeTimeline.B_CUT), 0.2f)
        assertEquals(
            "the tell",
            0.9f, seconds(WelcomeTimeline.B_HIT) - seconds(WelcomeTimeline.B_FRONT), 0.2f,
        )
    }

    /** B ends cleanly too — the overlay must not still be on screen at 3.0 s. */
    @Test
    fun `B ends at zero as well`() {
        assertEquals(1f, WelcomeTimeline.frameB(0f).overlayAlpha, 1e-4f)
        assertEquals(0f, WelcomeTimeline.frameB(1f).overlayAlpha, 1e-4f)
    }

    /**
     * The covered fraction of a screen of this [aspect] (width ÷ height),
     * rasterised from [WelcomeTimeline.B_LENS] exactly as `WelcomeOverlay`
     * draws it: each blob's lumpy polygon plus its welded lobes, rotated by its
     * own angle and then stretched along it.
     *
     * Everything is in **width units**, so a y in screen fractions has to be
     * divided by the aspect before it can be compared with a radius.
     */
    private fun lensCoverage(aspect: Float, nx: Int = 100, ny: Int = 220): Float {
        var hit = 0
        for (i in 0 until nx) {
            val u = (i + 0.5f) / nx
            for (j in 0 until ny) {
                val y = ((j + 0.5f) / ny) / aspect
                if (WelcomeTimeline.B_LENS.indices.any { k ->
                        val blob = WelcomeTimeline.B_LENS[k]
                        val dx = u - blob.x
                        val dy = y - blob.y / aspect
                        val a = blob.angleDeg * Math.PI / 180.0
                        val rx = dx * kotlin.math.cos(a) + dy * kotlin.math.sin(a)
                        val ry = -dx * kotlin.math.sin(a) + dy * kotlin.math.cos(a)
                        insideSplat(
                            (rx / (blob.radius * blob.stretch)).toFloat(),
                            (ry / blob.radius).toFloat(),
                            k,
                        )
                    }
                ) {
                    hit++
                }
            }
        }
        return hit.toFloat() / (nx * ny)
    }

    /** Is (`px`, `py`) inside the unit splat with this [seed]? */
    private fun insideSplat(px: Float, py: Float, seed: Int): Boolean {
        val n = WelcomeTimeline.SPLAT_LOBES.size
        val xs = FloatArray(n)
        val ys = FloatArray(n)
        for (i in 0 until n) {
            val r = WelcomeTimeline.splatLobe(seed, i)
            val a = WelcomeTimeline.splatAngle(seed, i)
            xs[i] = (kotlin.math.cos(a) * r).toFloat()
            ys[i] = (kotlin.math.sin(a) * r * WelcomeTimeline.SPLAT_SQUASH).toFloat()
        }
        var inside = false
        var j = n - 1
        for (i in 0 until n) {
            if ((ys[i] > py) != (ys[j] > py) &&
                px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i]) + xs[i]
            ) {
                inside = !inside
            }
            j = i
        }
        if (inside) return true
        for (b in WelcomeTimeline.SPLAT_BLOBS.indices) {
            val (dx, dy, dr) = WelcomeTimeline.splatBlob(seed, b)
            val ox = px - dx
            val oy = py - dy * WelcomeTimeline.SPLAT_SQUASH
            if (ox * ox + oy * oy <= dr * dr) return true
        }
        return false
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
