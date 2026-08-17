package com.lidarscan.app.ar

import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 6, owner item 19 — **"the AR overlay crush the app when enable".**
 *
 * `CaptureArController` needs a real ARCore `Session` and real GL surfaces, and
 * this project has no Robolectric harness and no ARCore-capable device. So the
 * ownership + lifecycle logic that actually decides whether a renderer thread
 * may call `Session.update()` was extracted into [ArSessionGate], which is plain
 * Kotlin — and this suite covers exactly the scenarios the brief names: rapid
 * toggling, background/foreground during a claim, and a surface destroyed
 * mid-claim.
 *
 * The assertion that matters most is [not resumed]: `Session.update()` on a
 * paused session throws `SessionPausedException`, unchecked, on a
 * `GLSurfaceView` render thread — which is an uncaught exception on a non-UI
 * thread, i.e. a dead process. Every test here that ends in
 * [ArSessionGate.Decision.NOT_RESUMED] is a crash that can no longer happen.
 */
class ArSessionGateTest {

    private val pump = ArSessionGate.Owner.POSE_PUMP
    private val overlay = ArSessionGate.Owner.OVERLAY

    private fun readyGate(owner: ArSessionGate.Owner = pump) = ArSessionGate().apply {
        claim(owner)
        onSessionCreated()
        onResumed()
    }

    // ── the crash paths ────────────────────────────────────────────────────

    @Test
    fun `a claimed owner may not drive before a session exists`() {
        val gate = ArSessionGate()
        gate.claim(overlay)
        assertEquals(ArSessionGate.Decision.NO_SESSION, gate.mayDrive(overlay))
        assertFalse(gate.mayDrive(overlay).mayProceed)
    }

    @Test
    fun `a created but never-resumed session may not be driven - this is the crash`() {
        // Reproduces the grant-the-permission path exactly: the old code called
        // createSession() and NOT resume(), while the overlay's GL thread was
        // already spinning at RENDERMODE_CONTINUOUSLY.
        val gate = ArSessionGate()
        gate.claim(overlay)
        gate.onSessionCreated()
        assertEquals(ArSessionGate.Decision.NOT_RESUMED, gate.mayDrive(overlay))
    }

    @Test
    fun `pausing shuts the gate immediately, before the session is actually paused`() {
        val gate = readyGate(overlay)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlay))
        gate.onPaused()
        assertEquals(
            "backgrounding while the GL thread is mid-frame must not reach Session.update()",
            ArSessionGate.Decision.NOT_RESUMED,
            gate.mayDrive(overlay),
        )
    }

    @Test
    fun `foreground - background - foreground during a claim ends up drivable again`() {
        val gate = readyGate(overlay)
        gate.onPaused()
        assertFalse(gate.mayDrive(overlay).mayProceed)
        gate.onResumed()
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlay))
        assertTrue(gate.isSessionRunning)
    }

    @Test
    fun `closing the session revokes drive rights even though the claim survives`() {
        val gate = readyGate(overlay)
        gate.onSessionClosed()
        assertEquals(ArSessionGate.Decision.NO_SESSION, gate.mayDrive(overlay))
        assertSame("the claim itself is untouched by a close", overlay, gate.currentOwner)
    }

    // ── ownership: the ROUND 5 black-camera race, now with tests ───────────

    @Test
    fun `only the current owner may drive`() {
        val gate = readyGate(pump)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(pump))
        assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(overlay))

        gate.claim(overlay)
        assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(pump))
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlay))
    }

    @Test
    fun `an out-of-order release from a superseded renderer cannot revoke the new owner`() {
        // The exact race: Compose creates the overlay's AndroidView (claim)
        // BEFORE the pump's onRelease runs on the same main thread.
        val gate = readyGate(pump)
        gate.claim(overlay)
        gate.release(pump) // the stale pump finally tears down
        assertSame("the overlay must still own the session", overlay, gate.currentOwner)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlay))
    }

    @Test
    fun `a surface destroyed mid-claim releases only that owner`() {
        val gate = readyGate(pump)
        gate.surfaceDestroyed(overlay) // an overlay that never owned it
        assertSame(pump, gate.currentOwner)

        gate.surfaceDestroyed(pump)
        assertNull(gate.currentOwner)
        assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(pump))
    }

    @Test
    fun `rapid toggling between pump and overlay always leaves exactly one owner able to drive`() {
        val gate = ArSessionGate()
        gate.onSessionCreated()
        gate.onResumed()
        repeat(200) { i ->
            val next = if (i % 2 == 0) overlay else pump
            val previous = if (i % 2 == 0) pump else overlay
            gate.claim(next)
            // The superseded renderer's teardown lands late, as it does in
            // Compose — after the new claim.
            gate.release(previous)
            assertEquals("iteration $i", ArSessionGate.Decision.PROCEED, gate.mayDrive(next))
            assertEquals("iteration $i", ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(previous))
        }
    }

    @Test
    fun `a toggle whose pause and claim interleave never opens a window on a paused session`() {
        // needsArSession going momentarily false: DisposableEffect's onDispose
        // pauses on the main thread while the new renderer is being created.
        val gate = readyGate(pump)
        gate.claim(overlay)
        gate.onPaused()
        assertEquals(
            "a fresh claim must not imply a resumed session",
            ArSessionGate.Decision.NOT_RESUMED,
            gate.mayDrive(overlay),
        )
        gate.onResumed()
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlay))
    }

    // ── failure handling: degrade, do not die ──────────────────────────────

    @Test
    fun `a failure blocks driving and is reported exactly once`() {
        val gate = readyGate(overlay)
        assertTrue("the first failure is the one worth showing", gate.fail("camera texture bind failed"))
        assertFalse("a per-frame throw must not flood", gate.fail("camera texture bind failed"))
        assertEquals(ArSessionGate.Decision.FAILED, gate.mayDrive(overlay))
        assertEquals("camera texture bind failed", gate.failure)
        assertFalse(gate.isSessionRunning)
    }

    @Test
    fun `re-claiming clears a failure - toggling back into AR is an explicit try again`() {
        val gate = readyGate(overlay)
        gate.fail("the AR camera stopped")
        assertNotNull(gate.failure)

        gate.claim(overlay)
        assertNull("a claim is the operator asking again", gate.failure)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlay))
    }

    @Test
    fun `clearFailure restores driving without needing a new claim`() {
        val gate = readyGate(pump)
        gate.fail("boom")
        gate.clearFailure()
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(pump))
    }

    // ── concurrency: main thread claims, GL thread reads ───────────────────

    @Test
    fun `concurrent claims, releases and mayDrive reads stay internally consistent`() {
        // The real concurrency contract: claims/releases come from the main
        // thread, `mayDrive` is polled from a GL thread. What must hold is that
        // a PROCEED is always consistent with the owner that was current at the
        // moment of that single read — never that two SEPARATE reads agree,
        // which they legitimately need not while ownership is being handed over.
        val gate = ArSessionGate()
        gate.onSessionCreated()
        gate.onResumed()
        val pool = Executors.newFixedThreadPool(3)
        val start = CountDownLatch(1)
        val done = CountDownLatch(3)
        val inconsistent = java.util.concurrent.atomic.AtomicBoolean(false)
        val failures = java.util.concurrent.atomic.AtomicInteger(0)

        pool.submit {
            runCatching {
                start.await()
                repeat(20_000) { gate.claim(if (it % 2 == 0) pump else overlay) }
            }.onFailure { failures.incrementAndGet() }
            done.countDown()
        }
        pool.submit {
            runCatching {
                start.await()
                repeat(20_000) { gate.release(if (it % 2 == 0) overlay else pump) }
            }.onFailure { failures.incrementAndGet() }
            done.countDown()
        }
        pool.submit {
            runCatching {
                start.await()
                repeat(40_000) {
                    // Only the values a SINGLE read can produce are asserted.
                    // Comparing two separate reads would be testing the test:
                    // ownership legitimately changes between them, which is the
                    // entire point of a hand-off.
                    val decision = gate.mayDrive(pump)
                    if (decision !in ALLOWED_UNDER_CONTENTION) inconsistent.set(true)
                }
            }.onFailure { failures.incrementAndGet() }
            done.countDown()
        }

        start.countDown()
        assertTrue("workers must finish", done.await(20, TimeUnit.SECONDS))
        pool.shutdownNow()
        assertEquals("no worker may throw", 0, failures.get())
        assertFalse(
            "a contended read must only ever return PROCEED or NOT_OWNER here — never a torn state",
            inconsistent.get(),
        )

        // After the storm the gate is still a single coherent value.
        val owner = gate.currentOwner
        if (owner != null) {
            assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(owner))
            val other = if (owner == pump) overlay else pump
            assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(other))
        } else {
            assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(pump))
            assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(overlay))
        }
    }

    private companion object {
        /**
         * The session is created and resumed for the whole of the contention
         * test and never fails, so the only two answers a read can legally give
         * are "you own it" and "you do not".
         */
        val ALLOWED_UNDER_CONTENTION = setOf(
            ArSessionGate.Decision.PROCEED,
            ArSessionGate.Decision.NOT_OWNER,
        )
    }
}
