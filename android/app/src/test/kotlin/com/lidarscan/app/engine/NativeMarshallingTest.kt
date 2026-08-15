package com.lidarscan.app.engine

import com.lidarscan.app.merge.MergeRepository
import com.lidarscan.core.gnss.FixType
import com.lidarscan.core.gnss.GeorefRecord
import com.lidarscan.core.plan.OpeningKind
import com.lidarscan.core.plan.SillCheck
import com.lidarscan.core.plan.WallEvidence
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * B6/B9/B11/B12 — the flat-array JNI layouts.
 *
 * These exist because of a deliberate trade recorded in `processing_jni.cpp`:
 * records with no strings cross as a `double[]`/`int[]` with a documented index
 * layout rather than through a marshalling class, so there is no hand-typed
 * constructor descriptor to get wrong. The cost of that trade is that a shifted
 * or transposed layout is a wrong *number* instead of a load-time abort — which
 * is exactly the failure this file makes loud. Every case builds the array the
 * way the C++ says it does and checks the decoder reads it back.
 *
 * Plain JVM: no device, no `System.loadLibrary`, nothing native is called.
 */
class NativeMarshallingTest {

    // --- B9: scan_gnss_fix ----------------------------------------------------

    @Test
    fun `a fix decodes field for field from its documented indices`() {
        val a = DoubleArray(NativeGnssLayout.FIX_LEN)
        a[NativeGnssLayout.FIX_TYPE] = FixType.RTK_FIXED.code.toDouble()
        a[NativeGnssLayout.FIX_SATELLITES] = 21.0
        a[NativeGnssLayout.FIX_QUALITY_RAW] = 4.0
        a[NativeGnssLayout.FIX_LAT_DEG] = 22.3193
        a[NativeGnssLayout.FIX_LON_DEG] = 114.1694
        a[NativeGnssLayout.FIX_ALT_M] = 31.5
        a[NativeGnssLayout.FIX_HEIGHT_ELLIPSOID_M] = 33.7
        a[NativeGnssLayout.FIX_HDOP] = 0.8
        a[NativeGnssLayout.FIX_CORRECTION_AGE_S] = 1.2
        a[NativeGnssLayout.FIX_SIGMA_H_M] = 0.021
        a[NativeGnssLayout.FIX_SIGMA_FROM_GST] = 1.0
        a[NativeGnssLayout.FIX_SPEED_MPS] = 1.4
        a[NativeGnssLayout.FIX_UTC_UNIX_MILLIS] = 1_755_000_000_000.0
        a[NativeGnssLayout.FIX_HAS_FIX] = 1.0

        val f = NativeGnssLayout.decodeFix(a)
        assertEquals(FixType.RTK_FIXED, f.fix)
        assertEquals(21, f.satellites)
        assertEquals(22.3193, f.latDeg, 1e-9)
        // Orthometric and ellipsoidal are different fields and must not be
        // confused — A10 §6 calls mixing them "the classic 30-metre bug".
        assertEquals(31.5, f.altM, 1e-9)
        assertEquals(33.7, f.heightEllipsoidM, 1e-9)
        assertTrue(f.sigmaFromGst)
        assertTrue(f.hasFix)
        // Milliseconds on the wire, nanoseconds in the model.
        assertEquals(1_755_000_000_000_000_000L, f.utcUnixNs)
    }

    @Test
    fun `a null or short fix array decodes to the empty no-fix snapshot`() {
        assertFalse(NativeGnssLayout.decodeFix(null).hasFix)
        assertFalse(NativeGnssLayout.decodeFix(DoubleArray(3)).hasFix)
    }

    @Test
    fun `the fix-quality timeline lands in by_fix in FixType code order`() {
        val a = DoubleArray(NativeGnssLayout.STATS_LEN)
        for (i in 0 until 5) a[NativeGnssLayout.STATS_BY_FIX_0 + i] = (i + 1) * 10.0
        a[NativeGnssLayout.STATS_TIME_CONVERGED] = 1.0
        val s = NativeGnssLayout.decodeStats(a)
        assertEquals(10L, s.byFix[FixType.NONE.code])
        assertEquals(50L, s.byFix[FixType.RTK_FIXED.code])
        assertTrue(s.timeConverged)
    }

    @Test
    fun `ntrip stats decode including the negative unknown corrections age`() {
        val a = DoubleArray(NativeGnssLayout.NTRIP_LEN)
        a[NativeGnssLayout.NTRIP_STATE] = 2.0 // streaming
        a[NativeGnssLayout.NTRIP_RECEIVING] = 1.0
        a[NativeGnssLayout.NTRIP_CORRECTION_AGE_S] = -1.0
        a[NativeGnssLayout.NTRIP_FRAMES_OK] = 60.0
        a[NativeGnssLayout.NTRIP_FRAMES_CRC_FAILED] = 2.0
        val n = NativeGnssLayout.decodeNtrip(a)
        assertEquals(com.lidarscan.core.gnss.NtripState.STREAMING, n.state)
        assertTrue(n.receiving)
        assertEquals(-1f, n.correctionAgeS, 0f)
        assertEquals(60L, n.framesOk)
        assertEquals(2L, n.framesCrcFailed)
    }

    @Test
    fun `the georef matrix decodes row-major and keeps the ENU origin the caller supplied`() {
        val a = DoubleArray(NativeGnssLayout.GEOREF_LEN)
        a[NativeGnssLayout.GEOREF_CONVERGED] = 1.0
        a[NativeGnssLayout.GEOREF_EPSG] = 32650.0
        a[NativeGnssLayout.GEOREF_YAW_DEG] = 37.0
        for (k in 0 until 16) a[NativeGnssLayout.GEOREF_MATRIX_0 + k] = k.toDouble()
        a[NativeGnssLayout.GEOREF_HORIZONTAL_SIGMA_M] = 0.02
        a[NativeGnssLayout.GEOREF_DOMINANT_FIX] = FixType.RTK_FIXED.code.toDouble()

        val g = NativeGnssLayout.decodeGeoref(a, "", 22.3, 114.2, 30.0)
        assertNotNull(g)
        assertTrue(g!!.converged)
        assertEquals(32650, g.epsg)
        assertEquals(37.0, g.yawDeg, 1e-9)
        // Element order preserved exactly — the whole point of a row-major
        // contract stated at both ends.
        assertEquals(0.0, g.globalFromLocal[0], 0.0)
        assertEquals(15.0, g.globalFromLocal[15], 0.0)
        // The ENU frame is NOT in scan_georef_solution; B9 supplies it.
        assertEquals(22.3, g.enuOriginLatDeg, 1e-9)
        assertEquals(30.0, g.enuOriginHeightM, 1e-9)
        assertEquals(FixType.RTK_FIXED, g.dominantFix)
    }

    @Test
    fun `a null georef array decodes to null rather than a zeroed transform`() {
        // A zeroed transform would place the cloud at the centre of the earth
        // and look like data — the failure A13 warns about.
        assertNull(NativeGnssLayout.decodeGeoref(null, "", 0.0, 0.0, 0.0))
    }

    // --- B12: the merge georef encoding ---------------------------------------

    @Test
    fun `merge georef encoding round-trips through the documented stride`() {
        val r = GeorefRecord(
            converged = true, epsg = 32650, yawDeg = 12.0,
            globalFromLocal = DoubleArray(16) { it * 2.0 },
            enuOriginLatDeg = 22.5, enuOriginLonDeg = 114.0, enuOriginHeightM = 25.0,
            horizontalSigmaM = 0.03, verticalSigmaM = 0.05, cep95M = 0.07,
            samples = 200, inliers = 198, residualRmsM = 0.02, spanM = 40.0,
            dominantFix = FixType.RTK_FIXED, blocker = "",
        )
        val encoded = MergeRepository.encodeGeoref(listOf(null, r))
        assertEquals(2 * MergeRepository.GEOREF_STRIDE, encoded.size)

        // Session 0 had no georeference: `valid` must be 0, so the native side
        // skips it rather than reading zeros as a transform.
        assertEquals(0.0, encoded[MergeRepository.IDX_VALID], 0.0)

        val base = MergeRepository.GEOREF_STRIDE
        assertEquals(1.0, encoded[base + MergeRepository.IDX_VALID], 0.0)
        assertEquals(1.0, encoded[base + MergeRepository.IDX_CONVERGED], 0.0)
        assertEquals(32650.0, encoded[base + MergeRepository.IDX_EPSG], 0.0)
        for (k in 0 until 16) {
            assertEquals(k * 2.0, encoded[base + MergeRepository.IDX_MATRIX_0 + k], 0.0)
        }
        assertEquals(22.5, encoded[base + MergeRepository.IDX_ENU_LAT], 0.0)
        assertEquals(114.0, encoded[base + MergeRepository.IDX_ENU_LON], 0.0)
        assertEquals(25.0, encoded[base + MergeRepository.IDX_ENU_HEIGHT], 0.0)
        assertEquals(0.03, encoded[base + MergeRepository.IDX_SIGMA_H], 0.0)
    }

    @Test
    fun `a malformed transform falls back to the identity, never to zeros`() {
        val r = GeorefRecord(
            converged = true, epsg = 0, yawDeg = 0.0,
            globalFromLocal = DoubleArray(4), // wrong length
            enuOriginLatDeg = 0.0, enuOriginLonDeg = 0.0, enuOriginHeightM = 0.0,
            horizontalSigmaM = 0.0, verticalSigmaM = 0.0, cep95M = 0.0,
            samples = 0, inliers = 0, residualRmsM = 0.0, spanM = 0.0,
            dominantFix = FixType.NONE, blocker = "",
        )
        val e = MergeRepository.encodeGeoref(listOf(r))
        // Diagonal ones, off-diagonal zeros — an all-zero matrix is singular and
        // would collapse the session to a point.
        assertEquals(1.0, e[MergeRepository.IDX_MATRIX_0 + 0], 0.0)
        assertEquals(1.0, e[MergeRepository.IDX_MATRIX_0 + 5], 0.0)
        assertEquals(1.0, e[MergeRepository.IDX_MATRIX_0 + 10], 0.0)
        assertEquals(1.0, e[MergeRepository.IDX_MATRIX_0 + 15], 0.0)
        assertEquals(0.0, e[MergeRepository.IDX_MATRIX_0 + 1], 0.0)
    }

    // --- B11: the plan arrays -------------------------------------------------

    @Test
    fun `a plan decodes from the strides processing_jni documents`() {
        val wallsD = doubleArrayOf(
            // ax, ay, bx, by, thickness, rms, coverage, confidence
            0.0, 0.0, 4.0, 0.0, 0.15, 0.004, 0.92, 0.8,
            4.0, 0.0, 4.0, 3.0, 0.10, 0.006, 0.71, 0.6,
        )
        val wallsI = intArrayOf(
            // id, evidence, supportCells, snapped
            1, WallEvidence.PAIRED_FACES.code, 210, 1,
            2, WallEvidence.SINGLE_FACE.code, 150, 0,
        )
        val openingsD = doubleArrayOf(1.2, 0.0, 2.1, 0.0, 0.9, 0.7)
        val openingsI = intArrayOf(10, 1, OpeningKind.DOOR_CANDIDATE.code, SillCheck.OPEN_BELOW.code)
        val roomsD = doubleArrayOf(12.0, 14.0, 2.0, 1.5, 0.9)
        val roomsI = intArrayOf(1, 1, 4)
        val polys = doubleArrayOf(0.0, 0.0, 4.0, 0.0, 4.0, 3.0, 0.0, 3.0)
        val labels = arrayOf("R1")
        val summary = DoubleArray(NativePlanArrays.SUMMARY_LEN).also {
            it[NativePlanArrays.SUMMARY_MIN_X] = 0.0
            it[NativePlanArrays.SUMMARY_MAX_X] = 4.0
            it[NativePlanArrays.SUMMARY_MAX_Y] = 3.0
            it[NativePlanArrays.SUMMARY_BOUNDS_VALID] = 1.0
            it[NativePlanArrays.SUMMARY_SLICE_Z_MIN] = 1.0
            it[NativePlanArrays.SUMMARY_SLICE_Z_MAX] = 1.5
            it[NativePlanArrays.SUMMARY_GRID_RES] = 0.02
            it[NativePlanArrays.SUMMARY_OCCUPIED_CELLS] = 812.0
            it[NativePlanArrays.SUMMARY_POINTS_IN_BAND] = 41_000.0
            it[NativePlanArrays.SUMMARY_TOTAL_ROOM_AREA] = 12.0
        }

        val plan = NativePlanArrays.decode(wallsD, wallsI, openingsD, openingsI, roomsD, roomsI, polys, labels, summary)

        assertEquals(2, plan.walls.size)
        assertEquals(1, plan.walls[0].id)
        assertEquals(4.0, plan.walls[0].b.x, 0.0)
        assertEquals(0.15, plan.walls[0].thicknessM, 0.0)
        assertEquals(WallEvidence.PAIRED_FACES, plan.walls[0].evidence)
        assertTrue(plan.walls[0].snapped)
        assertEquals(WallEvidence.SINGLE_FACE, plan.walls[1].evidence)
        assertFalse(plan.walls[1].snapped)
        assertEquals(4.0, plan.walls[0].lengthM, 1e-9)

        assertEquals(1, plan.openings.size)
        assertEquals(OpeningKind.DOOR_CANDIDATE, plan.openings[0].kind)
        assertEquals(SillCheck.OPEN_BELOW, plan.openings[0].sill)
        assertEquals(1, plan.openings[0].wallId)

        assertEquals(1, plan.rooms.size)
        assertEquals("R1", plan.rooms[0].label)
        assertEquals(4, plan.rooms[0].polygon.size)
        assertEquals(3.0, plan.rooms[0].polygon[3].y, 0.0)
        assertEquals(12.0, plan.rooms[0].areaM2, 0.0)

        assertTrue(plan.bounds.valid)
        assertEquals(4.0, plan.bounds.width, 0.0)
        assertEquals(1.0, plan.sliceZMinM, 0.0)
        assertEquals(812, plan.stats.occupiedCells)
        assertFalse(plan.isEmpty)
    }

    @Test
    fun `room polygons are walked with per-room vertex counts, not guessed`() {
        val roomsD = doubleArrayOf(1.0, 4.0, 0.5, 0.5, 1.0, 2.0, 6.0, 3.0, 3.0, 1.0)
        val roomsI = intArrayOf(1, 1, 3, 2, 0, 4)
        val polys = doubleArrayOf(
            0.0, 0.0, 1.0, 0.0, 0.0, 1.0, // room 1: 3 vertices
            2.0, 2.0, 4.0, 2.0, 4.0, 4.0, 2.0, 4.0, // room 2: 4 vertices
        )
        val plan = NativePlanArrays.decode(
            DoubleArray(0), IntArray(0), DoubleArray(0), IntArray(0),
            roomsD, roomsI, polys, arrayOf("R1", "R2"), DoubleArray(NativePlanArrays.SUMMARY_LEN),
        )
        assertEquals(3, plan.rooms[0].polygon.size)
        assertEquals(4, plan.rooms[1].polygon.size)
        assertEquals(2.0, plan.rooms[1].polygon[0].x, 0.0)
        assertFalse(plan.rooms[1].fullyMeasured)
    }

    @Test
    fun `an empty plan decodes cleanly and diagnoses itself`() {
        val summary = DoubleArray(NativePlanArrays.SUMMARY_LEN)
        val plan = NativePlanArrays.decode(
            DoubleArray(0), IntArray(0), DoubleArray(0), IntArray(0),
            DoubleArray(0), IntArray(0), DoubleArray(0), emptyArray(), summary,
        )
        assertTrue(plan.isEmpty)
        assertTrue(plan.emptyDiagnosis().contains("cloud is empty"))
    }

    @Test
    fun `an empty plan from a real cloud blames the slice band, not the cloud`() {
        val summary = DoubleArray(NativePlanArrays.SUMMARY_LEN).also {
            it[NativePlanArrays.SUMMARY_POINTS_CONSIDERED] = 2_000_000.0
            it[NativePlanArrays.SUMMARY_POINTS_IN_BAND] = 0.0
            it[NativePlanArrays.SUMMARY_SLICE_Z_MIN] = 1.0
            it[NativePlanArrays.SUMMARY_SLICE_Z_MAX] = 1.5
        }
        val plan = NativePlanArrays.decode(
            DoubleArray(0), IntArray(0), DoubleArray(0), IntArray(0),
            DoubleArray(0), IntArray(0), DoubleArray(0), emptyArray(), summary,
        )
        assertTrue(plan.emptyDiagnosis().contains("slice band"))
    }
}
