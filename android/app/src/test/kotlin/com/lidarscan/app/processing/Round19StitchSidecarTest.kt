package com.lidarscan.app.processing

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 19 — the sidecar reads behind the debug log's yield line.
 *
 * The fixture JSON below is shaped exactly like `slam/post/reprocess.cpp`
 * writes it (the numbers are scan-050's real ones from this round), because
 * the writer and this reader live in the same repository and have to be
 * tested against each other, not merely against themselves — the same rule
 * `write_point_chunk_file` states engine-side.
 */
class Round19StitchSidecarTest {

    private fun projectDir(json: String?): File {
        val dir = File.createTempFile("round19sidecar", "").let {
            it.delete()
            it.mkdirs()
            it
        }
        if (json != null) {
            val processed = File(dir, "processed").apply { mkdirs() }
            File(processed, "stitch.json").writeText(json)
        }
        return dir
    }

    private val scan050Shape = """
        {
          "schema": 1,
          "points": 88501,
          "gapsExamined": [
            {"tMonoNs": 139815618259773, "gapS": 6.398, "decision": "gap-refused-gyro-disagrees", "reason": "x"}
          ],
          "rescues": [
            {"tMonoNs": 139815618259773, "gapS": 6.398, "gyroDeg": 115.631728, "rotationAppliedDeg": 85.698348,
             "translationM": 0.298, "coarseOverlap": 0.93, "observability": 0.3812, "solvedAxes": 3,
             "pairs": 18832, "decision": "rescued", "reason": "rescued"}
          ],
          "recoveries": [
            {"tBeforeNs": 1, "tAfterNs": 2, "candidates": 23609, "noGyro": 0, "admitted": 0,
             "rulerVetoed": true, "reason": "vetoed"}
          ],
          "recoveredPoints": 0,
          "yield": {"samples": 113037, "noReturns": 482, "outOfWindow": 0, "noPose": 445,
                    "flaggedExcluded": 23609, "otherDropped": 0, "resolved": 88501, "recovered": 0}
        }
    """.trimIndent()

    @Test
    fun `the yield block reads back exactly`() {
        val y = StitchSidecar.readYield(projectDir(scan050Shape))
        assertNotNull(y)
        assertEquals(113_037L, y!!.samples)
        assertEquals(482L, y.noReturns)
        assertEquals(445L, y.noPose)
        assertEquals(23_609L, y.flaggedExcluded)
        assertEquals(88_501L, y.resolved)
        assertEquals(0L, y.recovered)
        // The audit's own invariant: the rows sum to the samples.
        assertEquals(
            y.samples,
            y.noReturns + y.outOfWindow + y.noPose + y.flaggedExcluded + y.otherDropped + y.resolved,
        )
    }

    @Test
    fun `the rescue tally counts attempts and acceptances separately`() {
        val counts = StitchSidecar.rescueCounts(projectDir(scan050Shape))
        assertEquals(1 to 1, counts)
    }

    @Test
    fun `the log line carries every row by name`() {
        val line = StitchSidecar.yieldLine(projectDir(scan050Shape))
        assertNotNull(line)
        assertTrue("got: $line", line!!.contains("113037 samples"))
        assertTrue(line.contains("23609 flagged-excluded"))
        assertTrue(line.contains("88501 resolved"))
        assertTrue(line.contains("rescues=1/1"))
    }

    @Test
    fun `a pre-round-19 sidecar reads as nothing, never as zeros`() {
        val old = """{"schema": 1, "points": 5, "gapsExamined": []}"""
        assertNull(StitchSidecar.readYield(projectDir(old)))
        assertNull(StitchSidecar.yieldLine(projectDir(old)))
    }

    @Test
    fun `no sidecar at all is quiet`() {
        assertNull(StitchSidecar.yieldLine(projectDir(null)))
    }
}
