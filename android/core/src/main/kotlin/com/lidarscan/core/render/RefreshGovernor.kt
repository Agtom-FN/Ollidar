package com.lidarscan.core.render

/**
 * ROUND 5.3 (item 17): keeps the **live view** inside what the device can
 * actually sustain, and never lets a display decision touch the recording.
 *
 * The owner's case is concrete: a Pixel 8 Pro reports a 120 Hz display, and the
 * point renderer may not sustain 120 Hz on a multi-million-point cloud. So the
 * refresh control's ceiling is the *hardware's* ([optionsFor]), and when measured
 * frame times sustain an overrun the governor **downshifts one notch** and says so
 * inline ([onFrameInterval]).
 *
 * Two rules that are deliberately not negotiable:
 *
 *  1. **Downshift only.** The governor never raises the cap on its own. A
 *     renderer that oscillates between 120 and 60 because the load hovers at the
 *     threshold looks broken and measures nothing; the operator can always raise
 *     it by hand, which is an explicit decision rather than a guess.
 *  2. **Sustained, not spiky.** One long frame is a page upload, a GC, or the
 *     shade being pulled down. Only an overrun that persists for
 *     [SUSTAINED_WINDOW_MS] counts, so a transient hitch never changes what the
 *     operator chose.
 */
class RefreshGovernor(
    /** The device's own display ceiling in Hz — `Display.getRefreshRate()`, rounded. */
    private val deviceCeilingHz: Int,
    /** Never downshift below this: a live view slower than this is not a live view. */
    private val floorHz: Int = FLOOR_HZ,
) {
    /** The cap the operator asked for: 0 = "Max", meaning [deviceCeilingHz]. */
    private var requestedHz: Int = 0

    /** The cap actually in force after any auto-downshift. Always ≥ [floorHz]. */
    var effectiveHz: Int = 0
        private set

    // Booleans + values, not "0 means unset": a caller whose clock legitimately
    // starts at 0 (a test harness, or a renderer that timestamps from process
    // start) would otherwise never accumulate an overrun window at all. That was
    // a real bug here, caught by the first test that passed nowNs = 0.
    private var overrunActive: Boolean = false
    private var overrunSinceNs: Long = 0L
    private var haveDownshifted: Boolean = false
    private var lastDownshiftNs: Long = 0L

    /** True while the governor is running below what was asked for. */
    val isDownshifted: Boolean
        get() = effectiveTarget() < resolvedRequest()

    /** The one-line inline note, or null when nothing has been downshifted. */
    fun note(): String? =
        if (!isDownshifted) {
            null
        } else {
            "Live view eased to $effectiveHz fps — this phone could not sustain " +
                "${resolvedRequest()} fps on this cloud. Recording is unaffected."
        }

    /** The operator moved the control. Clears any downshift: their choice wins until proven otherwise. */
    fun request(hz: Int) {
        requestedHz = if (hz in 1 until deviceCeilingHz.coerceAtLeast(1)) hz else 0
        effectiveHz = requestedHz
        overrunActive = false
        overrunSinceNs = 0L
        haveDownshifted = false
        lastDownshiftNs = 0L
    }

    /**
     * Feeds one measured frame interval. Returns the new cap when it decided to
     * downshift (so the caller can apply it and show [note]), else null.
     *
     * [intervalNs] is the wall time between two *rendered* frames. A frame that
     * was deliberately skipped by the cap is not an overrun, so the caller must
     * only report frames it actually drew.
     */
    fun onFrameInterval(nowNs: Long, intervalNs: Long): Int? {
        val target = effectiveTarget()
        if (target <= floorHz) return null
        val targetIntervalNs = 1_000_000_000L / target
        // 1.35x: comfortably past vsync quantisation (a 120 Hz target missing one
        // vsync measures 1.0x → 2.0x with nothing in between, and a 30 Hz target
        // on a 60 Hz panel jitters ±8 ms) but well short of "half rate".
        val overrunning = intervalNs > targetIntervalNs * 135 / 100

        if (!overrunning) {
            overrunActive = false
            return null
        }
        if (!overrunActive) {
            overrunActive = true
            overrunSinceNs = nowNs
            return null
        }
        if (nowNs - overrunSinceNs < SUSTAINED_WINDOW_MS * 1_000_000L) return null
        // One notch per settle window, so a phone that is far too slow walks down
        // in steps the operator can read rather than collapsing to the floor.
        if (haveDownshifted && nowNs - lastDownshiftNs < SUSTAINED_WINDOW_MS * 1_000_000L) return null

        val next = nextNotchBelow(target)
        overrunActive = false
        haveDownshifted = true
        lastDownshiftNs = nowNs
        if (next == effectiveHz) return null
        effectiveHz = next
        return next
    }

    /**
     * What the OPERATOR asked for, in real Hz — 0 ("Max") resolved against the
     * hardware ceiling. Deliberately independent of [effectiveHz]: mixing the two
     * made `isDownshifted` compare a value with itself and always read false,
     * which is exactly the silent-no-op the tests caught.
     */
    private fun resolvedRequest(): Int =
        if (requestedHz == 0) deviceCeilingHz.coerceAtLeast(floorHz) else requestedHz

    /** The cap currently being aimed at — the downshifted one if there is one. */
    private fun effectiveTarget(): Int = if (effectiveHz == 0) resolvedRequest() else effectiveHz

    private fun nextNotchBelow(hz: Int): Int =
        NOTCHES.firstOrNull { it < hz && it >= floorHz } ?: floorHz

    companion object {
        /** How long an overrun has to persist before it counts. */
        const val SUSTAINED_WINDOW_MS = 2_000L

        /** The slowest the governor will ever choose by itself. */
        const val FLOOR_HZ = 10

        /** The downshift ladder, descending. */
        val NOTCHES: List<Int> = listOf(120, 90, 60, 45, 30, 20, 15, 10)

        /**
         * The refresh options to offer on a device whose display runs at
         * [deviceCeilingHz]: `0` ("Max") plus every ladder notch the hardware can
         * actually reach. A 60 Hz phone therefore never shows a 120 fps choice —
         * item 17's "the max IS the device's real capability".
         */
        fun optionsFor(deviceCeilingHz: Int): List<Int> {
            val ceiling = deviceCeilingHz.coerceAtLeast(FLOOR_HZ)
            return listOf(0) + NOTCHES.filter { it <= ceiling }
        }
    }
}
