package com.lidarscan.app.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 8 — the bitfield `nativeProcProbeProject` returns, decoded.
 *
 * A bitfield split across a JNI boundary is exactly the kind of contract that
 * drifts silently: nothing fails to compile when the two sides disagree, the
 * flags just start meaning other things. So the numbering is asserted here
 * literally, bit by bit, against the same comment
 * `app/src/main/cpp/processing_jni.cpp` carries — if someone renumbers one
 * side, this fails rather than the Review screen quietly deciding that every
 * project predates trajectory storage.
 */
class ProjectProbeTest {

    @Test
    fun eachBitDecodesToItsOwnFlag() {
        assertTrue(ProjectProbe.of(1L).opened)
        assertTrue(ProjectProbe.of(2L).isD6)
        assertTrue(ProjectProbe.of(4L).hasPoses)
        assertTrue(ProjectProbe.of(8L).hasRecordedMap)
        assertTrue(ProjectProbe.of(16L).hasMount)

        // ... and only its own flag.
        val onlyPoses = ProjectProbe.of(4L)
        assertFalse(onlyPoses.opened)
        assertFalse(onlyPoses.isD6)
        assertFalse(onlyPoses.hasRecordedMap)
        assertFalse(onlyPoses.hasMount)
    }

    @Test
    fun zeroIsTheAbsentProject() {
        assertEquals(ProjectProbe.NONE, ProjectProbe.of(0L))
        assertFalse(ProjectProbe.of(0L).canShow3d)
        // An unreadable directory is NOT "recorded before trajectory storage":
        // the two produce very different sentences on screen, and telling a
        // user their scan is too old when the file is simply missing would send
        // them looking in the wrong place.
        assertFalse(ProjectProbe.of(0L).predatesTrajectoryStorage)
    }

    @Test
    fun aPost050D6CaptureCanBeShownIn3d() {
        // opened + D6 + poses + map + mount
        val p = ProjectProbe.of(1L or 2L or 4L or 8L or 16L)
        assertTrue(p.canShow3d)
        assertFalse(p.predatesTrajectoryStorage)
    }

    @Test
    fun aPre050D6CaptureIsIdentifiedAsSuch() {
        // The owner's scan-015 exactly: opened, D6, returns, and nothing else.
        // Every capture this app made before 0.5.0 has this shape.
        val p = ProjectProbe.of(1L or 2L)
        assertFalse("a D6 capture without a trajectory cannot be shown in 3D", p.canShow3d)
        assertTrue("and Review has to say why", p.predatesTrajectoryStorage)
    }

    @Test
    fun aCachedMapAloneIsEnoughToDraw() {
        // A container whose pose stream was lost or stripped but whose resolved
        // cloud survived is still showable — the cache IS the resolved result.
        // Process would refuse it; Review should not.
        val p = ProjectProbe.of(1L or 2L or 8L)
        assertTrue(p.canShow3d)
        assertFalse(p.predatesTrajectoryStorage)
    }

    @Test
    fun aMid360ProjectIsNeverCalledPreTrajectoryStorage() {
        // A Mid-360 carries its own IMU and estimates its own trajectory, so
        // "no kPoseAr" says nothing about it. Applying the D6 sentence here
        // would be a confident lie about a sensor it does not describe.
        val p = ProjectProbe.of(1L)
        assertFalse(p.isD6)
        assertFalse(p.predatesTrajectoryStorage)
    }
}
