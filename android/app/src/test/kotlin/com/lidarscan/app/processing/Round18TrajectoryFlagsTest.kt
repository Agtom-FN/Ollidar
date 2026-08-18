package com.lidarscan.app.processing

import com.lidarscan.core.capture.TrajectoryRibbon
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 18 item 70 — "LSTRAJ02", the flags-carrying trajectory file.
 *
 * The round-16 format could not say "the tracker was blind here", so Review
 * and the floor-plan sheet drew the owner's 6-7 s freezes — frozen poses plus
 * the re-acquisition teleport — as an ordinary walked line. v2 records carry
 * a u32 of flags (bit 0 = untracked, bit 1 = the incoming segment is a blind
 * jump); untracked poses draw nothing and the jump lands as a BRIDGE vertex.
 *
 * Like Round16TrajectoryFileTest, the bytes here are built by hand from the
 * format's description, so the reader is tested against the FORMAT, not
 * against the writer.
 */
class Round18TrajectoryFlagsTest {

    private fun encodeV2(
        poses: List<Triple<Float, Float, Float>>,
        flags: List<Int>,
    ): ByteArray {
        val buf = ByteBuffer
            .allocate(16 + poses.size * 16)
            .order(ByteOrder.LITTLE_ENDIAN)
        buf.put("LSTRAJ02".toByteArray(Charsets.US_ASCII))
        buf.putInt(poses.size)
        buf.putInt(0)
        poses.forEachIndexed { i, (x, y, z) ->
            buf.putFloat(x); buf.putFloat(y); buf.putFloat(z); buf.putInt(flags[i])
        }
        return buf.array()
    }

    @Test
    fun `a v2 file with no flags draws exactly like a clean walk`() {
        val poses = (0 until 30).map { Triple(it * 0.20f, 1.40f, 0f) }
        val ribbon = TrajectoryFile.decode(encodeV2(poses, List(30) { 0 }))
        assertTrue(ribbon.count >= 2)
        assertTrue(ribbon.rgba.none { it == TrajectoryRibbon.BRIDGE })
        assertTrue(ribbon.rgba.none { it == TrajectoryRibbon.UNTRACKED })
    }

    @Test
    fun `frozen poses draw nothing and the teleport is a bridge, not a walk`() {
        // A metre of walking, a scan-046-shaped freeze (untracked poses frozen
        // in place), then re-acquisition 1.5 m away flagged as a jump.
        val poses = mutableListOf<Triple<Float, Float, Float>>()
        val flags = mutableListOf<Int>()
        for (i in 0 until 6) { poses.add(Triple(i * 0.20f, 1.40f, 0f)); flags.add(0) }
        repeat(8) { poses.add(Triple(1.0f, 1.40f, 0f)); flags.add(1) } // untracked, frozen
        poses.add(Triple(2.5f, 1.40f, 0f)); flags.add(2) // regained: jump-in
        for (i in 1 until 6) { poses.add(Triple(2.5f + i * 0.20f, 1.40f, 0f)); flags.add(0) }

        val ribbon = TrajectoryFile.decode(encodeV2(poses, flags))
        assertTrue(ribbon.count >= 2)
        // The frozen guesses are not vertices at all...
        for (i in 0 until ribbon.count) {
            val x = ribbon.xyz[i * 3]
            assertTrue("an untracked pose leaked into the ribbon at x=$x", x <= 1.0f + 1e-4f || x >= 2.5f - 1e-4f)
        }
        // ...and the segment across the blindness is a bridge.
        assertTrue(ribbon.rgba.any { it == TrajectoryRibbon.BRIDGE })
    }

    @Test
    fun `a v2 file whose length matches v1 records is refused`() {
        // 12-byte records under a v2 magic: the length check must refuse it
        // rather than shear every record by four bytes.
        val buf = ByteBuffer.allocate(16 + 10 * 12).order(ByteOrder.LITTLE_ENDIAN)
        buf.put("LSTRAJ02".toByteArray(Charsets.US_ASCII))
        buf.putInt(10)
        buf.putInt(0)
        repeat(10) { buf.putFloat(it * 0.3f); buf.putFloat(1.4f); buf.putFloat(0f) }
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(buf.array()))
    }

    @Test
    fun `an unknown future version is no path, not a guess at its record size`() {
        val buf = ByteBuffer.allocate(16 + 4 * 16).order(ByteOrder.LITTLE_ENDIAN)
        buf.put("LSTRAJ03".toByteArray(Charsets.US_ASCII))
        buf.putInt(4)
        buf.putInt(0)
        repeat(16) { buf.putFloat(0.5f) }
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(buf.array()))
    }
}
