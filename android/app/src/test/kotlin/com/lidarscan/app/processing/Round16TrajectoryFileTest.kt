package com.lidarscan.app.processing

import com.lidarscan.core.capture.TrajectoryRibbon
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 16 item 59 — the phone's decoder for `processed/trajectory.bin`.
 *
 * The engine test (`round16/reprocess_writes_the_corrected_trajectory_beside_the_cloud`)
 * checks the WRITER against an independent decoder written from the format's
 * description. This checks the READER the same way, from the same description,
 * with bytes this file builds by hand — so writer and reader are tested against
 * the FORMAT and never merely against each other.
 *
 * Every rejection case matters more than the happy one. This file is a derived
 * product on a phone's shared storage: it can be half-written when a process is
 * killed mid-reprocess, it can be a stale copy from a container that was
 * exported and re-imported, and it must never be a reason the viewer fails to
 * open a scan. "No path" is always an acceptable answer; a wrong path is not.
 */
class Round16TrajectoryFileTest {

    private fun encode(
        poses: List<Triple<Float, Float, Float>>,
        magic: String = "LSTRAJ01",
        countOverride: Int? = null,
        trailingBytes: Int = 0,
    ): ByteArray {
        val n = countOverride ?: poses.size
        val buf = ByteBuffer
            .allocate(16 + poses.size * 12 + trailingBytes)
            .order(ByteOrder.LITTLE_ENDIAN)
        buf.put(magic.toByteArray(Charsets.US_ASCII))
        buf.putInt(n)
        buf.putInt(0)
        poses.forEach { (x, y, z) ->
            buf.putFloat(x); buf.putFloat(y); buf.putFloat(z)
        }
        repeat(trailingBytes) { buf.put(0) }
        return buf.array()
    }

    private fun walk(n: Int) = (0 until n).map { Triple(it * 0.20f, 1.40f, 0f) }

    @Test
    fun `a well-formed file becomes a coloured ribbon`() {
        val ribbon = TrajectoryFile.decode(encode(walk(50)))
        assertTrue(ribbon.count >= 2)
        // 50 poses 20 cm apart is 9.8 m; the 12 cm stride keeps all of them.
        assertEquals(50, ribbon.count)
        assertEquals(TrajectoryRibbon.START_MARKER, ribbon.rgba.first())
        assertEquals(TrajectoryRibbon.END_MARKER, ribbon.rgba.last())
        // The geometry survives the round trip to the last float.
        assertEquals(0f, ribbon.xyz[0])
        assertEquals(1.40f, ribbon.xyz[1])
        assertEquals(49 * 0.20f, ribbon.xyz[(ribbon.count - 1) * 3])
    }

    @Test
    fun `the wrong magic is not a trajectory`() {
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(encode(walk(10), magic = "LSTRAJ02")))
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(encode(walk(10), magic = "XXXXXXXX")))
    }

    @Test
    fun `a truncated file is no path rather than a path into whatever follows`() {
        // The exact shape of a process killed mid-write: a header claiming 200
        // poses over a body holding 40.
        val bytes = encode(walk(40), countOverride = 200)
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(bytes))
    }

    @Test
    fun `a file with extra bytes after the poses is refused too`() {
        // Not paranoia: a length that does not match the count exactly means
        // one of the two is lying, and there is no way to tell which.
        assertEquals(
            TrajectoryRibbon.EMPTY,
            TrajectoryFile.decode(encode(walk(20), trailingBytes = 7)),
        )
    }

    @Test
    fun `an empty or single-pose trajectory draws nothing`() {
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(encode(emptyList())))
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(encode(walk(1))))
    }

    @Test
    fun `garbage shorter than a header is refused without reading past it`() {
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(ByteArray(0)))
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(ByteArray(15)))
    }

    @Test
    fun `an absurd count is refused before it is allocated`() {
        // A corrupted length field must not turn into a two-gigabyte
        // allocation on a phone.
        val bytes = encode(walk(4), countOverride = Int.MAX_VALUE)
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(bytes))
        val negative = encode(walk(4), countOverride = -12)
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.decode(negative))
    }

    @Test
    fun `a missing file is no path, not a crash`() {
        val missing = File.createTempFile("traj", ".bin").also { it.delete() }
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.read(missing))
        // ...and a directory where a file was expected is the same answer.
        val dir = File.createTempFile("trajdir", "").also { it.delete(); it.mkdirs() }
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryFile.read(dir))
        dir.delete()
    }

    @Test
    fun `a real file on disk reads back the same as its bytes`() {
        val f = File.createTempFile("traj", ".bin")
        try {
            val bytes = encode(walk(300))
            f.writeBytes(bytes)
            val fromDisk = TrajectoryFile.read(f)
            assertEquals(TrajectoryFile.decode(bytes), fromDisk)
            // 300 poses 20 cm apart is 60 m — thinned by the 12 cm stride to
            // the same 300, because the stride is smaller than the spacing.
            assertNotEquals(TrajectoryRibbon.EMPTY, fromDisk)
        } finally {
            f.delete()
        }
    }
}
