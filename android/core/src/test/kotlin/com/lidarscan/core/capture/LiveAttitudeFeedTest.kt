package com.lidarscan.core.capture

import com.lidarscan.core.calib.DeviceOrientation
import com.lidarscan.core.calib.StartOrientation
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 30 item 175 — **the feed's rules**, which are the half of this item
 * that no screenshot can check.
 *
 * A photograph of a correct needle proves the maths. It cannot prove that the
 * gravity listener was released when the screen left, that ARCore's pose
 * outranks the accelerometer while it is tracking and hands back when it stops,
 * or that a second surface holding the same feed does not switch it off for the
 * first. Those are the ways this feature costs battery or silently freezes
 * again, and they are all here.
 */
class LiveAttitudeFeedTest {

    private val g = 9.81
    private var now = 0L
    private fun feed() = LiveAttitudeFeed { now }

    private fun LiveAttitudeFeed.gravityAt(angleDeg: Double, forMs: Long = 4_000L, stepMs: Long = 10L) {
        val r = Math.toRadians(angleDeg)
        val end = now + forMs
        while (now <= end) {
            onGravity(g * sin(r), g * cos(r), 0.0, fused = true)
            now += stepMs
        }
    }

    // ── The reference count ───────────────────────────────────────────────

    @Test
    fun nothingIsListeningUntilSomethingIsWatching() {
        val f = feed()
        assertFalse("a feed nobody is watching must hold no sensor", f.isSensorWanted)
        assertEquals(0, f.subscriberCount)
        f.acquire()
        assertTrue(f.isSensorWanted)
        f.release()
        assertFalse("the screen left and the sensor stayed on", f.isSensorWanted)
    }

    @Test
    fun twoSurfacesShareOneListener() {
        // The hold-still card and the recording strip can be composed across
        // one another during the hand-off at Start. The first to leave must not
        // switch off the instrument the second is still drawing.
        val f = feed()
        val edges = mutableListOf<Boolean>()
        f.onSensorWantedChanged = { edges += it }
        f.acquire()
        f.acquire()
        f.release()
        assertTrue("one surface left and took the feed with it", f.isSensorWanted)
        f.release()
        assertFalse(f.isSensorWanted)
        assertEquals("the listener was churned rather than shared", listOf(true, false), edges)
    }

    @Test
    fun releaseIsFloored() {
        val f = feed()
        f.acquire()
        f.release()
        f.release()
        f.acquire()
        assertTrue("an unbalanced release must not make acquire a no-op", f.isSensorWanted)
    }

    @Test
    fun samplesBeforeAnyoneIsWatchingAreDropped() {
        val f = feed()
        f.gravityAt(30.0)
        assertNull("a feed nobody acquired must publish nothing", f.attitude.value)
    }

    @Test
    fun theLastReleaseClearsTheReading() {
        val f = feed()
        f.acquire()
        f.gravityAt(30.0)
        assertNotNull(f.attitude.value)
        f.release()
        assertNull("a stale needle survived the screen that drew it", f.attitude.value)
        // And the filter is reset, not merely hidden: re-entering at a
        // different attitude must not blend across the gap.
        f.acquire()
        now += 60_000
        f.onGravity(0.0, g, 0.0, fused = true)
        assertEquals(0.0, f.attitude.value!!.screenUpAngleDeg, 0.01)
    }

    // ── The IMU hand-off ──────────────────────────────────────────────────

    @Test
    fun theRoundNineImuStreamReplacesTheListenerAndGivesItBack() {
        val f = feed()
        f.acquire()
        assertTrue(f.isSensorWanted)
        f.onImuFeedStarted()
        assertFalse("a second accelerometer was opened on top of the round-9 stream", f.isSensorWanted)
        f.onImuFeedStopped()
        assertTrue("the recording ended and the instrument went deaf", f.isSensorWanted)
        f.release()
        assertFalse(f.isSensorWanted)
    }

    @Test
    fun theImuStreamStillFeedsTheNeedleWhileItOwnsTheSensor() {
        val f = feed()
        f.acquire()
        f.onImuFeedStarted()
        val r = Math.toRadians(25.0)
        val end = now + 4_000L
        while (now <= end) {
            f.onImuAccel(g * sin(r), g * cos(r), 0.0)
            now += 3
        }
        assertEquals(25.0, f.attitude.value!!.screenUpAngleDeg, 0.5)
    }

    @Test
    fun anImuFeedThatStartsWhileNobodyIsWatchingChangesNothing() {
        val f = feed()
        f.onImuFeedStarted()
        assertFalse(f.isSensorWanted)
        f.acquire()
        assertFalse(f.isSensorWanted)
        f.onImuFeedStopped()
        assertTrue(f.isSensorWanted)
    }

    // ── ARCore's priority ─────────────────────────────────────────────────

    @Test
    fun aTrackingPoseOutranksGravity() {
        val f = feed()
        f.acquire()
        // Settle on gravity at 30 deg, then have ARCore say portrait.
        f.gravityAt(30.0)
        assertEquals(30.0, f.attitude.value!!.screenUpAngleDeg, 0.5)
        val pose = StartOrientation.syntheticHold(DeviceOrientation.PORTRAIT, 90)
        val end = now + 2_000L
        while (now <= end) {
            f.onCameraPose(pose, 90, tracking = true)
            // Gravity keeps arriving and keeps being ignored.
            f.onGravity(g * sin(Math.toRadians(30.0)), g * cos(Math.toRadians(30.0)), 0.0, fused = true)
            now += 33
        }
        assertEquals("the pose stream did not win", 0.0, f.attitude.value!!.screenUpAngleDeg, 0.5)
    }

    @Test
    fun aPoseThatStopsArrivingHandsTheInstrumentBackToGravity() {
        // The freeze this whole item is about, in its most dangerous form: a
        // tracking session that dies mid-walk. Two hundred milliseconds later
        // the needle must be following gravity again, not holding ARCore's last
        // opinion for the rest of the scan.
        val f = feed()
        f.acquire()
        val pose = StartOrientation.syntheticHold(DeviceOrientation.PORTRAIT, 90)
        repeat(30) {
            f.onCameraPose(pose, 90, tracking = true)
            now += 33
        }
        assertEquals(0.0, f.attitude.value!!.screenUpAngleDeg, 0.5)
        now += LiveAttitudeFeed.POSE_PRIORITY_MS + 1
        f.gravityAt(35.0)
        assertEquals("the needle stayed frozen on a dead pose stream", 35.0, f.attitude.value!!.screenUpAngleDeg, 0.5)
    }

    @Test
    fun aPoseArcoreDoesNotBelieveIsNotUsedAndDoesNotClaimPriority() {
        val f = feed()
        f.acquire()
        f.gravityAt(35.0)
        val pose = StartOrientation.syntheticHold(DeviceOrientation.PORTRAIT, 90)
        f.onCameraPose(pose, 90, tracking = false)
        f.gravityAt(35.0, forMs = 200L)
        assertEquals(
            "an untracked pose silenced gravity",
            35.0,
            f.attitude.value!!.screenUpAngleDeg,
            0.5,
        )
    }

    @Test
    fun arcoreNeverSilencesTheSensorItOnlyOutranksIt() {
        // The deliberate asymmetry: ARCore's priority is a deadline on SAMPLES
        // and never touches the registration, because a session can stop
        // delivering with no notification at all.
        val f = feed()
        val edges = mutableListOf<Boolean>()
        f.onSensorWantedChanged = { edges += it }
        f.acquire()
        val pose = StartOrientation.syntheticHold(DeviceOrientation.PORTRAIT, 90)
        repeat(60) {
            f.onCameraPose(pose, 90, tracking = true)
            now += 33
        }
        assertTrue("ARCore released the fallback sensor it may need back", f.isSensorWanted)
        assertEquals(listOf(true), edges)
    }

    @Test
    fun posesBeforeAnyoneIsWatchingAreDropped() {
        val f = feed()
        val pose = StartOrientation.syntheticHold(DeviceOrientation.PORTRAIT, 90)
        repeat(10) {
            f.onCameraPose(pose, 90, tracking = true)
            now += 33
        }
        assertNull(f.attitude.value)
    }

    // ── The flow ──────────────────────────────────────────────────────────

    @Test
    fun theStateFlowMovesAsTheSourceMoves() = runTest {
        // The state-flow half of item 175: what the composable reads has to
        // CHANGE when the phone does. Round 28's wiring produced a flow with
        // one value in it for the whole capture, and that is exactly what this
        // rejects.
        val f = feed()
        f.acquire()
        val seen = mutableListOf<Double?>()
        val job = launch(UnconfinedTestDispatcher(testScheduler)) {
            f.attitude.collect { seen += it?.screenUpAngleDeg }
        }
        // A slow, deliberate tilt from level to 40 deg over two seconds.
        var angle = 0.0
        while (angle <= 40.0) {
            val r = Math.toRadians(angle)
            f.onGravity(g * sin(r), g * cos(r), 0.0, fused = true)
            now += 10
            angle += 0.2
        }
        job.cancel()

        val angles = seen.filterNotNull()
        assertTrue("the flow emitted ${angles.size} readings, expected the tilt to be visible", angles.size >= 20)
        assertTrue("the flow started level", abs(angles.first()) < 1.0)
        assertTrue("the flow never reached the tilt (${angles.last()})", angles.last() > 30.0)
        // Monotone, because the input was: a needle that jitters against a
        // steady tilt is the "jittery" half of the item's requirement.
        for (i in 1 until angles.size) {
            assertTrue("the needle went backwards during a one-way tilt", angles[i] >= angles[i - 1] - 1e-9)
        }
        // And it is throttled: 200 samples in, nothing like 200 values out.
        assertTrue("the flow published ${angles.size} times — the throttle is not working", angles.size < 60)
    }

    @Test
    fun theFlowRepublishesOnlyWhenTheReadingChanges() = runTest {
        // StateFlow conflates equal values, so a phone held perfectly still
        // costs one emission and not twenty a second.
        val f = feed()
        f.acquire()
        val collected = mutableListOf<Double?>()
        val job = launch(UnconfinedTestDispatcher(testScheduler)) {
            f.attitude.collect { collected += it?.screenUpAngleDeg }
        }
        f.gravityAt(0.0, forMs = 3_000L)
        val settled = collected.size
        f.gravityAt(0.0, forMs = 3_000L)
        job.cancel()
        assertEquals("a motionless phone kept republishing", settled, collected.size)
    }
}
