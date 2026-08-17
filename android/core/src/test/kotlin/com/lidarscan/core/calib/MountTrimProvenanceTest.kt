package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 7, field bug 1 — the trim's provenance, which is what turns "the trim
 * vanished" into "the trim is still here and here is how old it is".
 *
 * The owner's log is the specification:
 *
 * ```
 * 22:53:04 [ar]        mount re-zero captured: magnitude=132.44deg
 * 22:53:09 [pushbroom] extrinsic applied: source=nominal trim=132.81deg   <- scan-008, 216 k pts
 * 22:54:06 [pushbroom] extrinsic applied: source=nominal trim=none        <- scan-009, 57 s later
 * ```
 */
class MountTrimProvenanceTest {

    private val runA = "run-a"
    private val runB = "run-b"

    private fun trim(capturedAtMillis: Long, magnitudeDeg: Double = 132.8): MountTrim {
        // A rotation of the requested magnitude about a fixed axis, so
        // magnitudeDeg in the label is a real number and not a stub.
        val q = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), Math.toRadians(magnitudeDeg))
        return MountTrim(
            qx = q.x, qy = q.y, qz = q.z, qw = q.w,
            sensor = SensorType.COIN_D6,
            capturedAtEpochMillis = capturedAtMillis,
            sampleCount = 37,
            spreadDeg = 0.47,
        )
    }

    @Test
    fun `no trim reads as the CAD nominal and is not a warning`() {
        val p = MountTrimProvenances.describe(null, runA, nowMillis = 1_000_000L)
        assertEquals(null, p.trim)
        assertFalse(p.warn)
        assertFalse(p.fromPreviousRun)
        assertTrue(p.label.contains("CAD nominal"))
        assertEquals("trim=none", p.logSuffix)
    }

    @Test
    fun `a trim set in this run is a plain confirmation with its age`() {
        val now = 10 * 60_000L
        val p = MountTrimProvenances.describe(
            StoredMountTrim(trim(capturedAtMillis = now - 3 * 60_000L), runA),
            currentAppRunId = runA,
            nowMillis = now,
        )
        assertFalse(p.fromPreviousRun)
        assertFalse(p.warn)
        assertTrue(p.label.contains("3 min ago"))
        assertTrue(p.logSuffix.contains("trimSource=this-run"))
        assertEquals(3 * 60_000L, p.ageMillis)
    }

    @Test
    fun `a trim from a previous app run is still applied and says so`() {
        val now = 60 * 60_000L
        val p = MountTrimProvenances.describe(
            StoredMountTrim(trim(capturedAtMillis = now - 20 * 60_000L), runA),
            currentAppRunId = runB,
            nowMillis = now,
        )
        // The whole point: the trim SURVIVES. 0.3.0 threw it away silently and
        // ran 132 degrees off the nominal on the very next capture.
        assertTrue("a restored trim must still be in force", p.trim != null)
        assertTrue(p.fromPreviousRun)
        assertFalse("20 minutes old is not stale", p.warn)
        assertTrue(p.label.contains("restored from your last session"))
        assertTrue(p.logSuffix.contains("trimSource=restored-previous-run"))
    }

    @Test
    fun `a trim older than the stale window is a caution, and still applied`() {
        val now = 40L * 60 * 60 * 1000
        val p = MountTrimProvenances.describe(
            StoredMountTrim(trim(capturedAtMillis = now - 30L * 60 * 60 * 1000), runA),
            currentAppRunId = runB,
            nowMillis = now,
        )
        assertTrue(p.trim != null)
        assertTrue(p.stale)
        assertTrue(p.warn)
        assertTrue(p.label.contains("Re-zero if"))
    }

    @Test
    fun `the stale boundary is exactly the documented window`() {
        val now = 100L * 60 * 60 * 1000
        val justUnder = MountTrimProvenances.describe(
            StoredMountTrim(trim(now - MountTrimProvenances.STALE_AFTER_MILLIS + 1), runA),
            runA,
            now,
        )
        val justOver = MountTrimProvenances.describe(
            StoredMountTrim(trim(now - MountTrimProvenances.STALE_AFTER_MILLIS), runA),
            runA,
            now,
        )
        assertFalse(justUnder.stale)
        assertTrue(justOver.stale)
    }

    @Test
    fun `the stored trim round-trips through JSON`() {
        // This is the mechanism the persistence actually uses; a shape change
        // that silently stopped decoding would put us straight back in the
        // field bug, with the trim reading "none" again.
        val stored = StoredMountTrim(trim(1_700_000_000_000L), runA)
        val json = kotlinx.serialization.json.Json.encodeToString(StoredMountTrim.serializer(), stored)
        val back = kotlinx.serialization.json.Json.decodeFromString(StoredMountTrim.serializer(), json)
        assertEquals(stored, back)
        assertEquals(SensorType.COIN_D6, back.trim.sensor)
        assertEquals(132.8, back.trim.magnitudeDeg, 1e-6)
    }
}
