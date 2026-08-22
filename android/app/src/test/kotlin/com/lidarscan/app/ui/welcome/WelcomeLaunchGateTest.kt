package com.lidarscan.app.ui.welcome

import com.lidarscan.core.welcome.WelcomeAnimation
import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 32 item 177 — **"once per process" is one claim, and it is atomic.**
 *
 * [WelcomeLaunchGate] is four lines and every one of them is a decision the
 * item made in words: the event is the *process*, the claim is taken once, and
 * a second Activity — a rotation, a locale change, a launcher tap racing a
 * USB-attach intent — gets nothing.
 */
class WelcomeLaunchGateTest {

    @Before
    fun reset() = WelcomeLaunchGate.resetForTest()

    @After
    fun tidy() = WelcomeLaunchGate.resetForTest()

    @Test
    fun `the first caller claims the launch and nobody else does`() {
        assertFalse(WelcomeLaunchGate.claimedInThisProcess)
        assertTrue(WelcomeLaunchGate.claimFirstLaunch())
        assertTrue(WelcomeLaunchGate.claimedInThisProcess)
        repeat(5) { assertFalse("claim $it must have been refused", WelcomeLaunchGate.claimFirstLaunch()) }
    }

    /**
     * The reason it is an `AtomicBoolean` and not a `var`. Two Activities can
     * be created concurrently in this app — the launcher and the manifest's
     * `USB_DEVICE_ATTACHED` filter both start `MainActivity` — and a torn read
     * would play the film twice, over the top of itself.
     */
    @Test
    fun `exactly one of many concurrent claimants wins`() {
        val threads = 32
        val wins = java.util.concurrent.atomic.AtomicInteger(0)
        val start = java.util.concurrent.CountDownLatch(1)
        val done = java.util.concurrent.CountDownLatch(threads)
        repeat(threads) {
            Thread {
                start.await()
                if (WelcomeLaunchGate.claimFirstLaunch()) wins.incrementAndGet()
                done.countDown()
            }.start()
        }
        start.countDown()
        done.await()
        org.junit.Assert.assertEquals(1, wins.get())
    }

    /**
     * The gate and the decision, together — which is how `MainActivity` uses
     * them. The second composition in a process shows nothing whatever the
     * settings say, and that is what makes a tab switch and a rotation silent.
     */
    @Test
    fun `a second composition in the same process gets no film`() {
        fun decide() = WelcomeAnimation.variantFor(
            WelcomeAnimation.Launch(
                enabled = true,
                reducedMotion = false,
                firstInProcess = WelcomeLaunchGate.claimFirstLaunch(),
            ),
        )
        org.junit.Assert.assertEquals(WelcomeAnimation.Variant.LIDAR_FLIP, decide())
        assertNull(decide())
        assertNull(decide())
    }

    /**
     * **ROUND 34 item 181(e).** The launch gate and the egg are independent:
     * unlocking developer mode does not consume the launch claim, and the
     * launch claim does not suppress an egg.
     *
     * They are separate objects for a separate reason each — the launch is
     * once per PROCESS and the egg is once per TRANSITION — and the way that
     * goes wrong is somebody making one of them serve both.
     */
    @Test
    fun `the egg does not touch the launch claim`() {
        assertTrue(WelcomeLaunchGate.claimFirstLaunch())
        org.junit.Assert.assertEquals(
            WelcomeAnimation.Variant.LLAMA_SPIT,
            WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = false, to = true)),
        )
        assertFalse(
            "the egg must not have re-armed the launch",
            WelcomeLaunchGate.claimFirstLaunch(),
        )
    }
}
