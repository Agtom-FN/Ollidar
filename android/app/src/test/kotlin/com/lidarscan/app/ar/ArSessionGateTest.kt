package com.lidarscan.app.ar

import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNotSame
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

    /**
     * ROUND 22 item 89: a ready gate AND the claim it granted. Every assertion
     * below now names a CLAIM rather than a role, because a role stopped being
     * an identity the moment the AR overlay was archived — see the class doc.
     */
    private data class Ready(val gate: ArSessionGate, val claim: ArSessionGate.Claim)

    private fun readyGate(owner: ArSessionGate.Owner = pump): Ready {
        val gate = ArSessionGate()
        val claim = gate.claim(owner)
        gate.onSessionCreated()
        gate.onResumed()
        return Ready(gate, claim)
    }

    // ── the crash paths ────────────────────────────────────────────────────

    @Test
    fun `a claimed owner may not drive before a session exists`() {
        val gate = ArSessionGate()
        val claim = gate.claim(overlay)
        assertEquals(ArSessionGate.Decision.NO_SESSION, gate.mayDrive(claim))
        assertFalse(gate.mayDrive(claim).mayProceed)
    }

    @Test
    fun `a created but never-resumed session may not be driven - this is the crash`() {
        // Reproduces the grant-the-permission path exactly: the old code called
        // createSession() and NOT resume(), while the overlay's GL thread was
        // already spinning at RENDERMODE_CONTINUOUSLY.
        val gate = ArSessionGate()
        val claim = gate.claim(overlay)
        gate.onSessionCreated()
        assertEquals(ArSessionGate.Decision.NOT_RESUMED, gate.mayDrive(claim))
    }

    @Test
    fun `pausing shuts the gate immediately, before the session is actually paused`() {
        val (gate, claim) = readyGate(overlay)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(claim))
        gate.onPaused()
        assertEquals(
            "backgrounding while the GL thread is mid-frame must not reach Session.update()",
            ArSessionGate.Decision.NOT_RESUMED,
            gate.mayDrive(claim),
        )
    }

    @Test
    fun `foreground - background - foreground during a claim ends up drivable again`() {
        val (gate, claim) = readyGate(overlay)
        gate.onPaused()
        assertFalse(gate.mayDrive(claim).mayProceed)
        gate.onResumed()
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(claim))
        assertTrue(gate.isSessionRunning)
    }

    @Test
    fun `closing the session revokes drive rights even though the claim survives`() {
        val (gate, claim) = readyGate(overlay)
        gate.onSessionClosed()
        assertEquals(ArSessionGate.Decision.NO_SESSION, gate.mayDrive(claim))
        assertSame("the claim itself is untouched by a close", claim, gate.currentClaim)
    }

    // ── ownership: the ROUND 5 black-camera race, now with tests ───────────

    @Test
    fun `only the current owner may drive`() {
        val (gate, pumpClaim) = readyGate(pump)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(pumpClaim))

        val overlayClaim = gate.claim(overlay)
        assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(pumpClaim))
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlayClaim))
    }

    @Test
    fun `an out-of-order release from a superseded renderer cannot revoke the new owner`() {
        // The exact race: Compose creates the overlay's AndroidView (claim)
        // BEFORE the pump's onRelease runs on the same main thread.
        val (gate, pumpClaim) = readyGate(pump)
        val overlayClaim = gate.claim(overlay)
        gate.release(pumpClaim) // the stale pump finally tears down
        assertSame("the overlay must still own the session", overlayClaim, gate.currentClaim)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlayClaim))
    }

    @Test
    fun `a surface destroyed mid-claim releases only that owner`() {
        val (gate, pumpClaim) = readyGate(pump)
        val strayOverlay = ArSessionGate().claim(overlay) // never owned THIS gate
        gate.surfaceDestroyed(strayOverlay)
        assertSame(pumpClaim, gate.currentClaim)

        gate.surfaceDestroyed(pumpClaim)
        assertNull(gate.currentClaim)
        assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(pumpClaim))
    }

    @Test
    fun `rapid toggling between pump and overlay always leaves exactly one owner able to drive`() {
        val gate = ArSessionGate()
        gate.onSessionCreated()
        gate.onResumed()
        var previous: ArSessionGate.Claim? = null
        repeat(200) { i ->
            val next = gate.claim(if (i % 2 == 0) overlay else pump)
            // The superseded renderer's teardown lands late, as it does in
            // Compose — after the new claim.
            gate.release(previous)
            assertEquals("iteration $i", ArSessionGate.Decision.PROCEED, gate.mayDrive(next))
            if (previous != null) {
                assertEquals("iteration $i", ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(previous))
            }
            previous = next
        }
    }

    @Test
    fun `a toggle whose pause and claim interleave never opens a window on a paused session`() {
        // needsArSession going momentarily false: DisposableEffect's onDispose
        // pauses on the main thread while the new renderer is being created.
        val (gate, _) = readyGate(pump)
        val overlayClaim = gate.claim(overlay)
        gate.onPaused()
        assertEquals(
            "a fresh claim must not imply a resumed session",
            ArSessionGate.Decision.NOT_RESUMED,
            gate.mayDrive(overlayClaim),
        )
        gate.onResumed()
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(overlayClaim))
    }

    // ── failure handling: degrade, do not die ──────────────────────────────

    @Test
    fun `a failure blocks driving and is reported exactly once`() {
        val (gate, claim) = readyGate(overlay)
        assertTrue("the first failure is the one worth showing", gate.fail("camera texture bind failed"))
        assertFalse("a per-frame throw must not flood", gate.fail("camera texture bind failed"))
        assertEquals(ArSessionGate.Decision.FAILED, gate.mayDrive(claim))
        assertEquals("camera texture bind failed", gate.failure)
        assertFalse(gate.isSessionRunning)
    }

    @Test
    fun `re-claiming clears a failure - toggling back into AR is an explicit try again`() {
        val (gate, _) = readyGate(overlay)
        gate.fail("the AR camera stopped")
        assertNotNull(gate.failure)

        val again = gate.claim(overlay)
        assertNull("a claim is the operator asking again", gate.failure)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(again))
    }

    @Test
    fun `clearFailure restores driving without needing a new claim`() {
        val (gate, claim) = readyGate(pump)
        gate.fail("boom")
        gate.clearFailure()
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(claim))
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

        val live = java.util.concurrent.atomic.AtomicReference<ArSessionGate.Claim?>(null)
        pool.submit {
            runCatching {
                start.await()
                repeat(20_000) { live.set(gate.claim(if (it % 2 == 0) pump else overlay)) }
            }.onFailure { failures.incrementAndGet() }
            done.countDown()
        }
        pool.submit {
            runCatching {
                start.await()
                // Stale releases, exactly as a superseded AndroidView issues
                // them: whatever claim this thread happens to observe, released
                // some microseconds later.
                repeat(20_000) { gate.release(live.get()) }
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
                    val decision = gate.peekDrive(live.get())
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
        val held = gate.currentClaim
        if (held != null) {
            assertEquals(ArSessionGate.Decision.PROCEED, gate.peekDrive(held))
        }
        // A claim taken and then immediately superseded can never drive again,
        // whatever the storm left behind.
        val superseded = gate.claim(pump)
        gate.claim(overlay)
        assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.peekDrive(superseded))
        assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.peekDrive(null))
    }


    // ── ROUND 22 item 89: ownership is an INSTANCE, not a role ─────────────

    /**
     * **The regression test for the owner's "tracking lost until app restart".**
     *
     * `CaptureScreen.AR_OVERLAY_ARCHIVED` has been true since ROUND 7, so every
     * claim and every release in the shipping app is `POSE_PUMP`. Navigation
     * Compose keeps the outgoing destination composed through the transition,
     * so the sequence below is what happened on every trip back into the Scan
     * tab: new pump claims, old pump releases. With the pre-round-22
     * enum-valued CAS, `release(POSE_PUMP)` succeeded against the NEW claim,
     * the gate went ownerless, and `mayDrive` answered NOT_OWNER for the rest
     * of the process.
     */
    @Test
    fun `a stale pump releasing after a new pump claimed cannot revoke the new claim - same role`() {
        val gate = ArSessionGate()
        gate.onSessionCreated()
        gate.onResumed()

        val stalePump = gate.claim(pump)            // the outgoing ArPosePumpView
        val livePump = gate.claim(pump)             // the incoming one's factory
        gate.surfaceDestroyed(stalePump)              // the outgoing one's onRelease, LATE

        assertSame("the live pump must still own the session", livePump, gate.currentClaim)
        assertEquals(
            "this is the owner's stuck-forever NOT_OWNER, and it must not happen",
            ArSessionGate.Decision.PROCEED,
            gate.mayDrive(livePump),
        )
        assertEquals(ArSessionGate.Decision.NOT_OWNER, gate.mayDrive(stalePump))
    }

    @Test
    fun `two claims by the same owner are distinct identities`() {
        val gate = ArSessionGate()
        val first = gate.claim(pump)
        val second = gate.claim(pump)
        assertNotSame("a role is not an identity", first, second)
        assertEquals(pump, first.owner)
        assertEquals(pump, second.owner)
        assertTrue("serials are monotonic, so a log line can order them", second.serial > first.serial)
        assertEquals("POSE_PUMP#${second.serial}", second.toString())
    }

    @Test
    fun `releasing a null claim is a no-op - a view whose factory never ran`() {
        val (gate, claim) = readyGate(pump)
        gate.release(null)
        gate.surfaceDestroyed(null)
        assertSame(claim, gate.currentClaim)
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(claim))
    }

    @Test
    fun `a claim from another gate can never release this gate`() {
        val (gate, claim) = readyGate(pump)
        val foreign = ArSessionGate().claim(pump)
        gate.release(foreign)
        assertSame(claim, gate.currentClaim)
    }

    // ── ROUND 22 item 89: the silence was the other half of the bug ────────

    @Test
    fun `every non-PROCEED decision is reported, and PROCEED is not`() {
        val lines = mutableListOf<String>()
        var now = 0L
        val gate = ArSessionGate()
        gate.decisionSink = { lines += it }
        gate.clockMillis = { now }

        val claim = gate.claim(pump)
        assertEquals(ArSessionGate.Decision.NO_SESSION, gate.mayDrive(claim))
        assertEquals(1, lines.size)
        assertTrue(lines[0], lines[0].contains("NO_SESSION"))
        assertTrue(lines[0], lines[0].contains("asked=POSE_PUMP#"))

        gate.onSessionCreated()
        gate.onResumed()
        now += ArSessionGate.LOG_INTERVAL_MS
        assertEquals(ArSessionGate.Decision.PROCEED, gate.mayDrive(claim))
        assertEquals("a working frame writes nothing", 1, lines.size)
    }

    @Test
    fun `refusals are rate limited to one line per second and carry the suppressed count`() {
        val lines = mutableListOf<String>()
        var now = 1_000L
        val gate = ArSessionGate()
        gate.decisionSink = { lines += it }
        gate.clockMillis = { now }
        gate.onSessionCreated()
        gate.onResumed()

        val stale = gate.claim(pump)
        gate.claim(pump) // supersede it; `stale` is now the stuck renderer

        // One second of a 60 Hz render thread asking and being refused.
        repeat(60) { gate.mayDrive(stale) }
        assertEquals("60 refusals in one window is ONE line", 1, lines.size)

        now += ArSessionGate.LOG_INTERVAL_MS
        gate.mayDrive(stale)
        assertEquals(2, lines.size)
        assertTrue(
            "the second line must carry the rate the first one no longer does: " + lines[1],
            lines[1].contains("(+59 more since the last line)"),
        )
        assertTrue(lines[1], lines[1].contains("NOT_OWNER"))
    }

    @Test
    fun `peekDrive answers without logging - the diagnostics strip must not flood`() {
        val lines = mutableListOf<String>()
        val gate = ArSessionGate()
        gate.decisionSink = { lines += it }
        val claim = gate.claim(pump)
        repeat(100) { assertEquals(ArSessionGate.Decision.NO_SESSION, gate.peekDrive(claim)) }
        assertTrue("peek is a question, not an event", lines.isEmpty())
    }

    @Test
    fun `a sink that throws cannot kill the render thread`() {
        val gate = ArSessionGate()
        gate.decisionSink = { throw IllegalStateException("the logger is broken") }
        val claim = gate.claim(pump)
        assertEquals(ArSessionGate.Decision.NO_SESSION, gate.mayDrive(claim))
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
