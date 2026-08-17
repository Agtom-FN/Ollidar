package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 9, owner item 35 — the phone-IMU logic that can be wrong, checked
 * without a device.
 *
 * The capture AVD has no real IMU, so an instrumented test could not exercise
 * any of this either; everything below is deliberately plain-JVM and lives in
 * `:core` for that reason. Three things are under test, and they are the three
 * that would each silently ruin the densifier in a different way:
 *
 *  1. the **two-stream merge** — a gyro integrator fed a repeated or
 *     out-of-order sample double-counts an interval;
 *  2. the **`SENSOR_ORIENTATION -> camera_from_imu` derivation** — a 90-degree
 *     error here bends the densified path sideways instead of along the walk;
 *  3. the **clock-domain sanity check** — samples from a different epoch than
 *     the poses they densify.
 */
class PhoneImuTest {

    // --- 1. the merge ------------------------------------------------------

    @Test
    fun `the gyro sets the cadence and the accel is a held value`() {
        val m = ImuStreamMerger()
        m.onAccel(1_000L, 0f, 9.81f, 0f)
        val a = m.onGyro(1_100L, 0.1f, 0.2f, 0.3f)
        // No new accel between the two gyro samples: the second sample carries
        // the SAME accel, and its own gyro timestamp — never a blend.
        val b = m.onGyro(1_200L, 0.4f, 0.5f, 0.6f)

        assertNotNull(a)
        assertNotNull(b)
        assertEquals(1_100L, a!!.tMonoNs)
        assertEquals(1_200L, b!!.tMonoNs)
        assertEquals(9.81f, a.ay, 1e-6f)
        assertEquals(9.81f, b.ay, 1e-6f)
        assertEquals(0.4f, b.gx, 1e-6f)
        assertEquals(2L, m.emitted)
    }

    @Test
    fun `an accel event on its own never emits a sample`() {
        val m = ImuStreamMerger()
        m.onAccel(1_000L, 0f, 9.81f, 0f)
        m.onAccel(1_005L, 0f, 9.80f, 0f)
        m.onAccel(1_010L, 0f, 9.79f, 0f)
        assertEquals(0L, m.emitted)
        assertEquals(3L, m.accelEvents)
        // Only now does one come out, and it carries the LATEST accel.
        val s = m.onGyro(1_020L, 0f, 0f, 0f)
        assertEquals(9.79f, s!!.ay, 1e-6f)
    }

    @Test
    fun `a gyro sample before any accel is dropped rather than emitted with zeros`() {
        val m = ImuStreamMerger()
        assertNull(m.onGyro(1_000L, 0.1f, 0f, 0f))
        assertEquals(1L, m.droppedNoAccel)
        assertEquals(0L, m.emitted)
        // Zeros would be a plausible-looking gravity vector pointing nowhere,
        // which is worse than a missing sample at the very start of a stream.
        m.onAccel(1_010L, 0f, 9.81f, 0f)
        assertNotNull(m.onGyro(1_020L, 0.1f, 0f, 0f))
    }

    @Test
    fun `an out-of-order or repeated gyro timestamp is dropped, matching what the engine does to poses`() {
        val m = ImuStreamMerger()
        m.onAccel(1_000L, 0f, 9.81f, 0f)
        assertNotNull(m.onGyro(2_000L, 0f, 0f, 0f))
        assertNull(m.onGyro(1_900L, 0f, 0f, 0f)) // older
        assertNull(m.onGyro(2_000L, 0f, 0f, 0f)) // repeated
        assertEquals(2L, m.droppedOutOfOrder)
        assertEquals(1L, m.emitted)
        // ...and the stream recovers on the next genuinely newer sample.
        assertNotNull(m.onGyro(2_100L, 0f, 0f, 0f))
    }

    @Test
    fun `an out-of-order ACCEL is accepted - a held value has no ordering requirement`() {
        val m = ImuStreamMerger()
        m.onAccel(2_000L, 0f, 9.81f, 0f)
        m.onAccel(1_900L, 0f, 5.00f, 0f) // stamped earlier, delivered later
        // The two sensor streams are not ordered against each other. Refusing
        // this would keep the OLDER reading, which is the opposite of the point.
        val s = m.onGyro(2_100L, 0f, 0f, 0f)
        assertEquals(5.00f, s!!.ay, 1e-6f)
    }

    @Test
    fun `a stale accel is counted but still emitted - a hole in the integration is worse`() {
        val m = ImuStreamMerger(maxAccelAgeNs = 15_000_000L)
        m.onAccel(0L, 0f, 9.81f, 0f)
        assertNotNull(m.onGyro(5_000_000L, 0f, 0f, 0f)) // 5 ms: fresh
        assertEquals(0L, m.staleAccel)
        assertNotNull(m.onGyro(100_000_000L, 0f, 0f, 0f)) // 100 ms: stale
        assertEquals(1L, m.staleAccel)
        assertEquals(2L, m.emitted)
    }

    @Test
    fun `a 200 Hz gyro against a 100 Hz accel emits at the GYRO rate`() {
        // The realistic case: the two sensors run at different rates. The merged
        // stream must carry every gyro sample (that is the integrand) and must
        // not be throttled to the slower stream.
        val m = ImuStreamMerger(maxAccelAgeNs = 30_000_000L)
        val meter = ImuRateMeter()
        val gyroStepNs = 5_000_000L // 200 Hz
        var nextAccel = 0L
        for (i in 0 until 200) {
            val t = i * gyroStepNs
            if (t >= nextAccel) {
                m.onAccel(t, 0f, 9.81f, 0f)
                nextAccel += 10_000_000L // 100 Hz
            }
            m.onGyro(t, 0f, 0f, 0f)?.let { meter.record(it.tMonoNs) }
        }
        assertEquals(200L, m.emitted)
        assertEquals(100L, m.accelEvents)
        assertEquals(200.0, meter.hz(), 0.5)
    }

    @Test
    fun `the rate meter says nothing until it can say something true`() {
        val meter = ImuRateMeter()
        assertEquals(0.0, meter.hz(), 0.0)
        meter.record(1_000_000L)
        assertEquals(0.0, meter.hz(), 0.0)
        meter.record(3_500_000L) // 2.5 ms later
        assertEquals(400.0, meter.hz(), 1e-6)
    }

    // --- 2. SENSOR_ORIENTATION -> camera_from_imu --------------------------

    /** Applies the derived rotation to a device-frame vector. */
    private fun toCamera(sensorOrientationDeg: Int, v: Vec3): Vec3 =
        CameraFromImu.rearCamera(sensorOrientationDeg).rotate(v)

    private fun assertVec(expected: Vec3, actual: Vec3, what: String) {
        assertTrue(
            "$what: expected $expected got $actual",
            abs(expected.x - actual.x) < 1e-9 &&
                abs(expected.y - actual.y) < 1e-9 &&
                abs(expected.z - actual.z) < 1e-9,
        )
    }

    @Test
    fun `at SENSOR_ORIENTATION 90 device up maps to camera minus X`() {
        // The physical check written out in CameraFromImu's KDoc: photograph a
        // standing person in portrait with a rear camera whose SENSOR_ORIENTATION
        // is 90. The raw image is landscape and needs a 90-degree CLOCKWISE
        // rotation to be upright, so in the RAW image the head is at the LEFT.
        // Head = device +Y. Image-left = -X_cam.
        assertVec(Vec3(-1.0, 0.0, 0.0), toCamera(90, Vec3(0.0, 1.0, 0.0)), "device +Y (up)")
        // And device +X (right) lands on image-up.
        assertVec(Vec3(0.0, 1.0, 0.0), toCamera(90, Vec3(1.0, 0.0, 0.0)), "device +X (right)")
    }

    @Test
    fun `the Z axis is shared for a rear camera at every orientation`() {
        // The rear camera looks along device -Z, and ARCore's -Z_cam IS the look
        // direction, so Z_cam == Z_dev. That is what makes the whole thing a
        // single rotation about one axis; if it ever stops holding, the
        // derivation in CameraFromImu is no longer valid and this says so.
        for (deg in listOf(0, 90, 180, 270)) {
            assertVec(Vec3(0.0, 0.0, 1.0), toCamera(deg, Vec3(0.0, 0.0, 1.0)), "device +Z at $deg")
        }
    }

    @Test
    fun `SENSOR_ORIENTATION 0 is identity and 180 is a half turn about Z`() {
        assertVec(Vec3(1.0, 0.0, 0.0), toCamera(0, Vec3(1.0, 0.0, 0.0)), "0 deg leaves +X alone")
        assertVec(Vec3(0.0, 1.0, 0.0), toCamera(0, Vec3(0.0, 1.0, 0.0)), "0 deg leaves +Y alone")
        assertVec(Vec3(-1.0, 0.0, 0.0), toCamera(180, Vec3(1.0, 0.0, 0.0)), "180 deg flips +X")
        assertVec(Vec3(0.0, -1.0, 0.0), toCamera(180, Vec3(0.0, 1.0, 0.0)), "180 deg flips +Y")
    }

    @Test
    fun `270 is the inverse of 90`() {
        val v = Vec3(0.3, -0.5, 0.81).normalized()
        val there = CameraFromImu.rearCamera(90)
        val back = CameraFromImu.rearCamera(270)
        assertVec(v, back.rotate(there.rotate(v)), "90 then 270 is the identity")
    }

    @Test
    fun `the derived quaternion is a unit quaternion in x y z w order`() {
        val q = CameraFromImu.rearCamera(90)
        assertEquals(1.0, q.norm, 1e-12)
        // Rz(+90) as (x, y, z, w) — a pure +Z rotation, so x and y are zero and
        // w == z == sin(45 deg). A (w, x, y, z) mix-up would show up right here.
        assertEquals(0.0, q.x, 1e-12)
        assertEquals(0.0, q.y, 1e-12)
        assertEquals(0.70710678, q.z, 1e-7)
        assertEquals(0.70710678, q.w, 1e-7)
        val xyzw = CameraFromImuExtrinsics(q, derived = true, why = "").toXyzw()
        assertEquals(4, xyzw.size)
        assertEquals(q.z, xyzw[2], 0.0)
        assertEquals(q.w, xyzw[3], 0.0)
    }

    @Test
    fun `an underivable extrinsic is identity AND says so, never a silent guess`() {
        // The three cases the field can actually produce. Each must come back
        // NOT derived, so the recorder logs its loud warning instead of letting
        // a guess pass for a measurement.
        val missing = CameraFromImu.resolve(null, frontFacing = false)
        assertFalse(missing.derived)
        assertEquals(Quat.IDENTITY, missing.quat)
        assertTrue(missing.why.contains("SENSOR_ORIENTATION"))

        val front = CameraFromImu.resolve(270, frontFacing = true)
        assertFalse(front.derived)
        assertEquals(Quat.IDENTITY, front.quat)
        assertTrue(front.why.contains("front-facing"))

        val nonsense = CameraFromImu.resolve(45, frontFacing = false)
        assertFalse(nonsense.derived)
        assertEquals(Quat.IDENTITY, nonsense.quat)
    }

    @Test
    fun `a rear camera at the usual 90 degrees resolves as derived`() {
        val r = CameraFromImu.resolve(90, frontFacing = false)
        assertTrue(r.derived)
        assertEquals(0.70710678, r.quat.z, 1e-7)
        // Negative/over-wrapped values normalise rather than being refused —
        // -90 and 270 are the same physical mounting.
        assertEquals(CameraFromImu.resolve(270, false).quat, CameraFromImu.resolve(-90, false).quat)
    }

    // --- 3. the clock-domain check -----------------------------------------

    @Test
    fun `a BOOTTIME sample a few ms in the past passes`() {
        val now = 12_345_678_901_234L
        // Sample-to-callback latency is milliseconds. Both ends of a realistic
        // range pass.
        assertTrue(ImuClockDomain.looksLikeBoottime(now - 500_000L, now))
        assertTrue(ImuClockDomain.looksLikeBoottime(now - 50_000_000L, now))
        assertTrue(ImuClockDomain.looksLikeBoottime(now, now))
    }

    @Test
    fun `a CLOCK_MONOTONIC sample on a phone that has slept trips the check`() {
        // CLOCK_MONOTONIC excludes deep sleep, so on a phone that has been
        // asleep for an hour it lags elapsedRealtime by that hour. That is the
        // realistic shape of this failure, and it must be caught.
        val boottime = 4_000_000_000_000L // ~66 min since boot
        val monotonic = boottime - 3_600_000_000_000L // one hour of sleep excluded
        assertFalse(ImuClockDomain.looksLikeBoottime(monotonic, boottime))
    }

    @Test
    fun `a vendor epoch and a zero timestamp both trip the check`() {
        val now = 4_000_000_000_000L
        // Unix-epoch nanoseconds — off by decades.
        assertFalse(ImuClockDomain.looksLikeBoottime(1_760_000_000_000_000_000L, now))
        // A device that stamps nothing at all.
        assertFalse(ImuClockDomain.looksLikeBoottime(0L, now))
        assertFalse(ImuClockDomain.looksLikeBoottime(-1L, now))
    }

    @Test
    fun `the tolerance is one second, either side`() {
        val now = 4_000_000_000_000L
        assertTrue(ImuClockDomain.looksLikeBoottime(now - ImuClockDomain.MAX_SKEW_NS, now))
        assertFalse(ImuClockDomain.looksLikeBoottime(now - ImuClockDomain.MAX_SKEW_NS - 1L, now))
        // Ahead of the delivery clock is just as wrong as behind it.
        assertFalse(ImuClockDomain.looksLikeBoottime(now + ImuClockDomain.MAX_SKEW_NS + 1L, now))
    }

    @Test
    fun `the status summary names the stream facts a field log needs`() {
        val s = PhoneImuStatus(
            running = true,
            samplesPushed = 1234,
            samplesRejected = 2,
            measuredHz = 398.7,
            sensorMaxHz = 400.0,
            requestedRateGranted = true,
            clockDomainOk = true,
            extrinsicsDerived = true,
            extrinsicsNote = "rear camera, SENSOR_ORIENTATION=90 deg",
        ).summary()
        assertTrue(s.contains("pushed=1234"))
        assertTrue(s.contains("rejected=2"))
        assertTrue(s.contains("granted=true"))
        assertTrue(s.contains("derived"))
    }
}
