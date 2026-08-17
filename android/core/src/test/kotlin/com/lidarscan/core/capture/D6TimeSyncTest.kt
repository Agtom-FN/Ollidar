package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 7, item 2 — the numbers behind the one D6 timing knob.
 *
 * The important thing this file pins is the *scale* the code claims: that a
 * 4096-byte USB read really is ~178 ms of wire time, which is what made the old
 * one-stamp-per-chunk behaviour a 15-cm-per-chunk error at walking pace, and
 * what the engine's per-byte back-dating now removes.
 */
class D6TimeSyncTest {

    @Test
    fun `the phone's 4 KB read is nearly two D6 revolutions of wire time`() {
        val ns = D6TimeSync.chunkWireNanos(4096)
        val ms = ns / 1e6
        assertTrue("expected ~178 ms, got $ms ms", ms > 176.0 && ms < 180.0)
        // Two full 10 Hz revolutions is 200 ms — this chunk spans 1.8 of them,
        // which is why collapsing it onto one pose shingles a wall.
        assertTrue(ms > 1.5 * 100.0)
    }

    @Test
    fun `the engine's 64-byte time slice is under 3 ms`() {
        val ms = D6TimeSync.chunkWireNanos(64) / 1e6
        assertTrue("expected ~2.8 ms, got $ms ms", ms > 2.5 && ms < 3.0)
        // At 1 m/s that is 2.8 mm of rig travel: an order of magnitude under the
        // D6's own range noise, which is the bar the slice size was picked for.
        assertTrue(ms / 1000.0 * 1.0 < 0.004)
    }

    @Test
    fun `latency clamps to the documented range in both directions`() {
        assertEquals(D6TimeSync.MAX_SENSOR_LATENCY_MS, D6TimeSync.clampLatencyMs(9999))
        assertEquals(D6TimeSync.MIN_SENSOR_LATENCY_MS, D6TimeSync.clampLatencyMs(-9999))
        assertEquals(7, D6TimeSync.clampLatencyMs(7))
        // Negative is legal on purpose (see the class header).
        assertTrue(D6TimeSync.MIN_SENSOR_LATENCY_MS < 0)
    }

    @Test
    fun `the default is small enough to be irrelevant at walking pace`() {
        val metres = D6TimeSync.displacementMetres(D6TimeSync.DEFAULT_SENSOR_LATENCY_MS)
        assertTrue("default displaces ${metres * 1000} mm", metres < 0.005)
    }

    @Test
    fun `the settings row states the displacement, not just the milliseconds`() {
        val line = D6TimeSync.describe(20)
        assertTrue(line.contains("20 ms"))
        // 20 ms at 1 m/s is 2 cm — the number the owner can judge.
        assertTrue(line, line.contains("2.0 cm"))
    }

    @Test
    fun `a zero or nonsense baud reports no wire time rather than dividing by zero`() {
        assertEquals(0L, D6TimeSync.chunkWireNanos(4096, baud = 0))
    }
}
