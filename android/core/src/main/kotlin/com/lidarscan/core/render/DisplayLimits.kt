package com.lidarscan.core.render

/**
 * ROUND 5: the ranges the live-view display controls actually offer, in one
 * place, because the same numbers appear in a slider, in a clamp and in a
 * read-out and drifting between them is how a slider ends up unable to reach its
 * own minimum.
 *
 * The point-size range is an owner decision from the round-5 mockup review
 * (**0.1 – 3.0 px, 0.1 steps**), not A14's — see [POINT_SIZE_FLOOR_PX] and the
 * note in [clamped] for how that lands against the engine's own
 * `clamp_display_params()` range of [0.5, 64].
 */
object DisplayLimits {

    /** Owner-specified live point-size range, in pixels. */
    const val POINT_SIZE_MIN_PX = 0.1f
    const val POINT_SIZE_MAX_PX = 3.0f
    const val POINT_SIZE_STEP_PX = 0.1f

    /**
     * Discrete slider steps *between* the ends, which is what Compose's
     * `Slider(steps = …)` counts: (3.0 − 0.1) / 0.1 = 29 intervals, so 28
     * interior stops. Computed rather than typed so changing the step above
     * cannot leave a slider that snaps to the wrong grid.
     */
    val POINT_SIZE_STEPS: Int =
        (((POINT_SIZE_MAX_PX - POINT_SIZE_MIN_PX) / POINT_SIZE_STEP_PX).toInt() - 1).coerceAtLeast(0)

    /** Snaps a raw slider value onto the 0.1 grid and into range. */
    fun snapPointSize(px: Float): Float {
        val steps = Math.round((px - POINT_SIZE_MIN_PX) / POINT_SIZE_STEP_PX)
        val snapped = POINT_SIZE_MIN_PX + steps * POINT_SIZE_STEP_PX
        // Round to one decimal so the read-out never shows 0.30000001 px.
        val rounded = Math.round(snapped * 10f) / 10f
        return rounded.coerceIn(POINT_SIZE_MIN_PX, POINT_SIZE_MAX_PX)
    }

    /**
     * A14's own gamma/brightness ranges (`display_params.h`), named here so the
     * sheet's sliders and [clamped]'s coercion cannot disagree.
     */
    const val GAMMA_MIN = 0.1f
    const val GAMMA_MAX = 4.0f
    const val BRIGHTNESS_MIN = 0.1f
    const val BRIGHTNESS_MAX = 3.0f

    /**
     * The live-view refresh caps the capture screen offers, in fps. `0` is
     * "uncapped" (every vsync) and is the default; the rest are a throttle on
     * the *viewport only* — the engine keeps decoding and recording at full rate
     * either way.
     */
    val REFRESH_HZ_OPTIONS: List<Int> = listOf(0, 30, 15, 10, 5)

    fun refreshLabel(hz: Int): String = if (hz <= 0) "Max" else "$hz fps"
}
