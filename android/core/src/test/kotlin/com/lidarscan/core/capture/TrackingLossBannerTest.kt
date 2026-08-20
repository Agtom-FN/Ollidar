package com.lidarscan.core.capture

import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 23 item 105 (owner request) — the "stop walking" banner's state
 * machine.
 *
 * Everything interesting here is timing, which is why it is in `:core`: how
 * long the green confirmation lingers, whether a tracker that flickers
 * produces one banner or six, and what a Stop mid-loss does. The owner's own
 * scan-070 is the reason the banner exists at all — a 4.1 s blindness with
 * 73.34° of gyro turn in it, refused by the ROUND 19 gate because no amount of
 * arithmetic can heal a gap that big.
 */
class TrackingLossBannerTest {

    private val none = TrackingBannerState()

    @Test
    fun `nothing is drawn while the tracker has the room`() {
        val s = TrackingLossBanners.next(none, recording = true, tracking = true, nowMillis = 1_000)
        assertEquals(TrackingBanner.NONE, s.banner)
    }

    @Test
    fun `a preview that is not recording never shouts`() {
        // Lining up a shot with poor tracking is normal and is not an error.
        val s = TrackingLossBanners.next(none, recording = false, tracking = false, nowMillis = 1_000)
        assertEquals(TrackingBanner.NONE, s.banner)
    }

    @Test
    fun `losing tracking during a recording raises the amber banner, once`() {
        val lost = TrackingLossBanners.next(none, recording = true, tracking = false, nowMillis = 1_000)
        assertEquals(TrackingBanner.LOST, lost.banner)
        assertEquals(1_000L, lost.sinceMillis)
        assertTrue(TrackingLossBanners.becameLost(none, lost))

        // Still blind 3 s later: the SAME banner, still counting from 1_000.
        val still = TrackingLossBanners.next(lost, recording = true, tracking = false, nowMillis = 4_000)
        assertEquals(TrackingBanner.LOST, still.banner)
        assertEquals("the count-up must not restart on every tick", 1_000L, still.sinceMillis)
        assertFalse("one loss is one haptic, not one per tick", TrackingLossBanners.becameLost(lost, still))
    }

    @Test
    fun `the banner does not clear itself while the tracker is still blind`() {
        var s = TrackingLossBanners.next(none, recording = true, tracking = false, nowMillis = 0)
        repeat(60) { i ->
            s = TrackingLossBanners.next(s, recording = true, tracking = false, nowMillis = 500L * i)
        }
        assertEquals(
            "\"until the tracking back\" is the owner's requirement, verbatim",
            TrackingBanner.LOST,
            s.banner,
        )
    }

    @Test
    fun `tracking coming back flips it green for exactly two seconds`() {
        val lost = TrackingLossBanners.next(none, recording = true, tracking = false, nowMillis = 1_000)
        val back = TrackingLossBanners.next(lost, recording = true, tracking = true, nowMillis = 5_100)
        assertEquals(TrackingBanner.REGAINED, back.banner)
        assertTrue(TrackingLossBanners.becameRegained(lost, back))

        val stillGreen = TrackingLossBanners.next(back, recording = true, tracking = true, nowMillis = 6_000)
        assertEquals(TrackingBanner.REGAINED, stillGreen.banner)

        val expired = TrackingLossBanners.next(
            back,
            recording = true,
            tracking = true,
            nowMillis = 5_100 + TrackingLossBanners.REGAINED_LINGER_MS,
        )
        assertEquals(TrackingBanner.NONE, expired.banner)
    }

    @Test
    fun `a second loss inside the green window goes straight back to amber`() {
        val lost = TrackingLossBanners.next(none, recording = true, tracking = false, nowMillis = 1_000)
        val back = TrackingLossBanners.next(lost, recording = true, tracking = true, nowMillis = 2_000)
        val lostAgain = TrackingLossBanners.next(back, recording = true, tracking = false, nowMillis = 2_500)
        assertEquals(TrackingBanner.LOST, lostAgain.banner)
        assertEquals(2_500L, lostAgain.sinceMillis)
        assertTrue("the second loss buzzes too", TrackingLossBanners.becameLost(back, lostAgain))
    }

    @Test
    fun `stopping mid-loss clears the banner rather than freezing it`() {
        val lost = TrackingLossBanners.next(none, recording = true, tracking = false, nowMillis = 1_000)
        val stopped = TrackingLossBanners.next(lost, recording = false, tracking = false, nowMillis = 1_400)
        assertEquals(TrackingBanner.NONE, stopped.banner)
    }

    @Test
    fun `the banner obeys the wording law`() {
        assertTrue(
            "${TrackingLossBanners.LOST_TEXT} is ${WordingLaw.wordCount(TrackingLossBanners.LOST_TEXT)} words",
            WordingLaw.isInstruction(TrackingLossBanners.LOST_TEXT),
        )
        assertTrue(WordingLaw.isInstruction(TrackingLossBanners.REGAINED_TEXT))
        assertTrue(WordingLaw.isInstruction(TrackingLossBanners.lostDetail(12_400)))
        // It must tell the operator what to DO, which is the whole point.
        assertTrue(WordingLaw.isActionable(TrackingLossBanners.LOST_TEXT))
        assertEquals("Lost for 12s.", TrackingLossBanners.lostDetail(12_400))
    }
}
