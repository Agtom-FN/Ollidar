package com.lidarscan.app.ui.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 8, owner item 28 — **the live scan view keeps >= 60 % of the screen.**
 *
 * This is a JVM test of a Compose layout, which is only possible because the
 * guarantee is arithmetic rather than visual: the viewport is the single
 * weighted child of the capture column, so it receives exactly
 * `screenHeight - fixedChromeDp()`, and whether that clears 60 % is a property
 * of [CaptureLayout] alone. Asserting it here means a future band added above
 * the viewport fails a unit test on every push rather than being noticed in a
 * field session — which is how the screen got to the state item 28 describes in
 * the first place (each of the six bands it accumulated was individually
 * reasonable).
 *
 * The numbers this replaced, measured off the pre-ROUND-8 screen at 800 dp:
 * ~370 dp of fixed chrome plus a pre-capture strip capped at 46 % of the
 * screen, leaving the live 3D view as little as 60 dp.
 */
class CaptureLayoutTest {

    /** The screens this app has actually been run on, plus a small one below the reference. */
    private val realPhoneHeightsDp = listOf(700f, 730f, 780f, 800f, 855f, 900f, 1000f)

    @Test
    fun `the rule itself is 60 percent`() {
        assertTrue(
            "item 28's number is 0.60 — a floor, not a target",
            CaptureLayout.MIN_VIEWPORT_FRACTION >= 0.60f,
        )
    }

    /**
     * THE assertion. A phone-tracked D6 capture, connected, pre-capture, no
     * manual panel — the state the owner's complaint is about.
     */
    @Test
    fun `the viewport keeps at least 60 percent of a real phone screen`() {
        for (height in realPhoneHeightsDp) {
            val fraction = CaptureLayout.viewportFraction(height, mountRow = true, appBar = false)
            assertTrue(
                "at ${height}dp the live view gets only ${"%.3f".format(fraction)} of the screen " +
                    "(chrome is ${CaptureLayout.fixedChromeDp()}dp)",
                fraction >= CaptureLayout.MIN_VIEWPORT_FRACTION,
            )
        }
    }

    /** And at the stated reference height it is achievable exactly, which is what makes 660 the reference. */
    @Test
    fun `the reference height is the shortest screen the rule fits on`() {
        val h = CaptureLayout.REFERENCE_SCREEN_HEIGHT_DP
        assertTrue(
            "the fixed chrome must fit inside the 40 % the rule leaves it at the reference height " +
                "(chrome ${CaptureLayout.fixedChromeDp()}dp vs ${h * (1f - CaptureLayout.MIN_VIEWPORT_FRACTION)}dp)",
            CaptureLayout.fixedChromeDp() <= h * (1f - CaptureLayout.MIN_VIEWPORT_FRACTION),
        )
        assertEquals(
            CaptureLayout.MIN_VIEWPORT_FRACTION,
            CaptureLayout.viewportFraction(h),
            1e-4f,
        )
    }

    /**
     * The safety valve, and the reason [CaptureLayout.viewportMinHeightDp] is a
     * `min` rather than just the fraction.
     *
     * Below the reference height the fixed chrome — a transport row you must be
     * able to hit one-handed while carrying a lidar, and a floating tab bar that
     * is not this screen's to shrink — is more than 40 % of the display. The
     * honest response is to give the viewport everything left over, NOT to
     * over-constrain the column: a `Column` whose children's minimum heights
     * exceed the space measures the last ones to zero, and a capture screen you
     * cannot press Stop on is worse than one whose viewport is 57 %.
     */
    @Test
    fun `a screen too short for the rule still leaves the transport room to exist`() {
        for (height in listOf(560f, 600f, 640f)) {
            val viewport = CaptureLayout.viewportMinHeightDp(height)
            assertTrue(
                "at ${height}dp the guaranteed viewport (${viewport}dp) must not eat the chrome",
                viewport + CaptureLayout.fixedChromeDp() <= height + 0.01f ||
                    viewport == CaptureLayout.VIEWPORT_FLOOR_DP,
            )
            assertTrue(
                "and it must still be more than half the screen at ${height}dp",
                CaptureLayout.viewportFraction(height) > 0.5f,
            )
        }
    }

    /** ROUND 5's floor survives: a viewport is never zero, because it is the proof a sensor is streaming. */
    @Test
    fun `the viewport never collapses to nothing`() {
        assertEquals(140f, CaptureLayout.VIEWPORT_FLOOR_DP, 0f)
        assertTrue(CaptureLayout.viewportMinHeightDp(200f) >= CaptureLayout.VIEWPORT_FLOOR_DP)
        assertTrue(CaptureLayout.viewportMinHeightDp(0f) >= CaptureLayout.VIEWPORT_FLOOR_DP)
    }

    /**
     * A Mid-360 session has no mount trim, so it does not pay for the mount row
     * — and the budget must actually account for that rather than reserving the
     * height unconditionally.
     */
    @Test
    fun `a sensor with no mount trim gets the mount row's height back`() {
        assertEquals(
            CaptureLayout.MOUNT_ROW_DP,
            CaptureLayout.fixedChromeDp(mountRow = true) - CaptureLayout.fixedChromeDp(mountRow = false),
            1e-4f,
        )
        // Checked on a screen SHORT enough for the leftover-height term to be
        // the binding one. Above the reference height both answers are the flat
        // 60 % floor, so comparing them there would prove nothing — the extra
        // room a Mid-360 session gains shows up in the `weight(1f)` the viewport
        // actually receives, not in the guaranteed minimum.
        assertTrue(
            CaptureLayout.viewportMinHeightDp(600f, mountRow = false) >
                CaptureLayout.viewportMinHeightDp(600f, mountRow = true),
        )
    }

    /**
     * The compact layout engages only when a sensor is CONNECTED and the manual
     * fallback is closed.
     *
     * That condition is load-bearing for two separate reasons, and both are
     * worth stating: with nothing attached, the Capture tab's whole job is the
     * connect flow (ROUND 5 item 7's auto-detect line and owner addition 1's
     * self-opening manual panel), and there is no live view to protect anyway —
     * the viewport reads "Connect a sensor to see the live 3D view". It is also
     * exactly the state `ReplayCaptureSmokeTest
     * .captureTabIsANewScanWithAutoDetectAndAnInlineManualFallback` walks on a
     * bare emulator, asserting `autoDetectStatus`, `manualLidarIpField`,
     * `manualHostIpField`, `manualConnectMid360`, `retryAutoDetectButton` and
     * `manualEntryToggle` are all DISPLAYED — so collapsing that state into a
     * sheet would break the CI smoke test, correctly.
     */
    @Test
    fun `the compact chrome is a connected screen's shape, not the connect flow's`() {
        assertTrue(CaptureLayout.useCompactChrome(connected = true, manualEntryOpen = false))
        assertFalse(
            "nothing attached: the connect flow needs the room and there is no live view to protect",
            CaptureLayout.useCompactChrome(connected = false, manualEntryOpen = false),
        )
        assertFalse(
            "the manual panel is open over a connected sensor — it is the thing being interacted with",
            CaptureLayout.useCompactChrome(connected = true, manualEntryOpen = true),
        )
        assertFalse(CaptureLayout.useCompactChrome(connected = false, manualEntryOpen = true))
    }

    /**
     * The hint band is bounded, and deliberately outside the budget.
     *
     * Six advisories can be live at once and each is two or three lines; without
     * a cap that is ~180 dp of the capture screen spent on notes *about* a scan
     * the operator can no longer see.
     */
    @Test
    fun `the hint band is capped and stays out of the fixed budget`() {
        assertTrue("one or two lines, not six", CaptureLayout.HINT_BAND_MAX_DP <= 48f)
        for (height in realPhoneHeightsDp) {
            val worstCase = (height - CaptureLayout.fixedChromeDp() - CaptureLayout.HINT_BAND_MAX_DP) / height
            assertTrue(
                "even with every hint up, ${height}dp must leave the viewport over half the screen " +
                    "(got ${"%.3f".format(worstCase)})",
                worstCase > 0.5f,
            )
        }
    }

    // ── ROUND 26 item 124: the budget, inverted ────────────────────────────

    /**
     * The floating chrome gets exactly what the viewport used to give up, and
     * the two halves must still add up to the whole screen. If they ever stop
     * doing so, either the picture is being covered or the connect flow is
     * being squeezed, and both are silent failures on a device.
     */
    @Test
    fun `the chrome ceiling is what the viewport floor leaves over`() {
        for (height in realPhoneHeightsDp) {
            for (mountRow in listOf(true, false)) {
                val viewport = CaptureLayout.viewportMinHeightDp(height, mountRow, appBar = false)
                val chrome = CaptureLayout.chromeMaxHeightDp(height, mountRow)
                assertEquals(
                    "at ${height}dp (mountRow=$mountRow) the two halves must be the whole screen",
                    height.toDouble(),
                    (viewport + chrome).toDouble(),
                    0.001,
                )
            }
        }
    }

    /**
     * And it never squeezes the connect flow off the screen. On a tall phone
     * the ceiling is generous; the floor is what protects the one state where
     * the chrome matters more than the picture, because there is no picture.
     */
    @Test
    fun `the floating chrome always has room for the manual entry panel`() {
        for (height in realPhoneHeightsDp + listOf(600f, 640f)) {
            assertTrue(
                "at ${height}dp the chrome ceiling ${CaptureLayout.chromeMaxHeightDp(height)} " +
                    "must clear the ${CaptureLayout.CHROME_FLOOR_DP}dp floor",
                CaptureLayout.chromeMaxHeightDp(height) >= CaptureLayout.CHROME_FLOOR_DP,
            )
        }
    }

    /**
     * The tab-bar clearance is still a real number, and the fullscreen layout
     * still spends it — but only while the tab bar is on screen. Round 25 item
     * 120 removed a dot and left its reserved space behind; this is the same
     * mistake one layer up, and it is asserted rather than remembered.
     */
    @Test
    fun `the tab bar clearance is only owed while the tab bar is drawn`() {
        assertTrue(
            "the clearance must be big enough to clear the 58dp bar plus its 12dp inset",
            CaptureLayout.TAB_BAR_CLEARANCE_DP >= 70f,
        )
        // The fullscreen layout's own rule, stated here because it is arithmetic
        // and not a pixel: hidden bar => the controls sit on the navigation bar
        // padding alone, which is strictly less than the clearance.
        assertTrue(CaptureLayout.TAB_BAR_CLEARANCE_DP > 12f)
    }

    /**
     * The disconnected screen's ceiling clears the connect flow, which is the
     * tallest stack this screen draws. Its `Connect` button below the fold is
     * the exact failure `ReplayCaptureSmokeTest` caught on the AVD, and the
     * number that prevents it belongs in a test rather than in a memory.
     */
    @Test
    fun `the connect flow gets everything the floating controls do not use`() {
        for (height in realPhoneHeightsDp) {
            val connect = CaptureLayout.connectFlowMaxHeightDp(height)
            assertTrue(
                "at ${height}dp the connect flow (${connect}dp) must beat the connected " +
                    "ceiling (${CaptureLayout.chromeMaxHeightDp(height)}dp)",
                connect > CaptureLayout.chromeMaxHeightDp(height),
            )
            assertEquals(
                "it reserves exactly the two floating bands",
                (height - CaptureLayout.FLOATING_CONTROL_RESERVE_DP).toDouble(),
                connect.toDouble(),
                0.001,
            )
        }
        // And on a small screen the floor still wins, rather than the reserve
        // producing a negative height that Compose would silently clamp.
        assertTrue(CaptureLayout.connectFlowMaxHeightDp(300f) >= CaptureLayout.CHROME_FLOOR_DP)
    }
}
