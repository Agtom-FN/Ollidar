package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 19 — the summary card's coverage line (item 75) and the
 * measured-cause advice text (item 77's correction, applied to the card).
 *
 * The owner's correction is ON RECORD: his sessions were in good light, and
 * round 18 measured the real diet — close feature-poor surfaces and fast
 * turns. No advice string on the card may mention light as a cause again;
 * these tests make that a build failure rather than a review comment.
 */
class Round19CoverageAdviceTest {

    private fun summary(
        breaks: Int = 0,
        trackingDrops: Int = 0,
        coverageAdvice: String? = null,
        pathM: Double = 22.0,
        points: Long = 150_000L,
    ) = ScanSummary(
        pointsCaptured = points,
        elapsedMillis = 90_000L,
        pathLengthMeters = pathM,
        sections = breaks + 1,
        trackingDrops = trackingDrops,
        recordingSizeBytes = 4_000_000L,
        coverageAdvice = coverageAdvice,
        posesRecorded = 2_600L,
        engineStarted = true,
        worldPointsResolved = points,
    )

    @Test
    fun `coverage advice reaches the card when nothing more urgent is queued`() {
        val line = "The walls behind you are thin in the map (about 90° of the room) — " +
            "walk past them before stopping."
        val s = summary(coverageAdvice = line)
        assertEquals(line, s.nextWalkAdvice)
    }

    @Test
    fun `a tracking break outranks the coverage line`() {
        val s = summary(breaks = 2, coverageAdvice = "The walls behind you are thin…")
        assertNotNull(s.nextWalkAdvice)
        assertFalse(s.nextWalkAdvice!!.contains("thin in the map"))
    }

    @Test
    fun `no coverage line and healthy numbers mean a quiet card`() {
        assertNull(summary().nextWalkAdvice)
    }

    @Test
    fun `no advice string blames light — the owner correction is load-bearing`() {
        // Every branch of the advice chain that can fire on a D6 capture.
        val variants = listOf(
            summary(breaks = 3),
            summary(breaks = 1),
            summary(trackingDrops = 2),
            summary(coverageAdvice = "The walls to your left are thin in the map — walk past them before stopping."),
        )
        for (s in variants) {
            val line = s.nextWalkAdvice ?: continue
            assertFalse("advice blames light: $line", line.lowercase().contains("light."))
            assertFalse("advice blames light: $line", line.lowercase().contains("lights up"))
            assertFalse("advice blames light: $line", line.lowercase().contains("turn the lights"))
        }
    }

    @Test
    fun `the multi-break advice names the measured causes`() {
        val line = summary(breaks = 3).nextWalkAdvice
        assertNotNull(line)
        assertTrue("got: $line", line!!.contains("arm's length"))
        assertTrue("got: $line", line.contains("turn"))
    }
}
