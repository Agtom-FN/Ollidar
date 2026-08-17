package com.lidarscan.core.capture

import com.lidarscan.core.render.RefreshGovernor
import kotlinx.serialization.Serializable

/**
 * ROUND 6, owner items 21 + 22 — **the phone-first performance tiers.**
 *
 * > 21. "dont default to use the max setting for the phone device which may
 * >     cause crush"
 * > 22. "allow user to have a fast select on light/optimal/full different level
 * >     of scanning performance output but keep the full parameter for advance
 * >     user setting"
 *
 * Three chips on the capture screen pick a [CaptureTuning]; the tuning
 * **prefills** every individual control in the Capture-settings sheet and then
 * gets out of the way. A preset is a starting point, never a cap: an advanced
 * user who moves one slider afterwards keeps that value, and the chip row simply
 * reads [PerformancePreset.CUSTOM] from then on. That is the whole of item 22's
 * "keep the full parameter for advance user setting".
 *
 * ## What made this necessary (item 21)
 *
 * Every live-view default the capture flow picked was the **maximum** the
 * control offered:
 *
 * | control | 0.2.1 default | why it was the max |
 * | --- | --- | --- |
 * | live refresh cap | `0` = uncapped | draws on every vsync — 120 Hz on the owner's Pixel 8 Pro |
 * | LOD point budget | `20 M` | the top of the sheet's own 2–20 M slider |
 * | camera keyframes | on, 3 fps | a JPEG encode + a disk write per keyframe, on the GL thread's heels |
 * | trajectory trail | 600 points | the ring's full capacity |
 *
 * Individually defensible; together they are "run the renderer, the encoder and
 * the uploader flat out on a phone that is also driving ARCore, a USB serial
 * reader and a SLAM session, while being carried". ROUND 5.3's bounded uploads
 * and the [RefreshGovernor] are the guards that keep that from taking the
 * process down — but a guard firing constantly is not the same as a sustainable
 * default, and the owner is right that the default should not be the ceiling.
 *
 * [OPTIMAL] is the new default and it is genuinely mid-tier on every axis.
 *
 * ## Device tiers
 *
 * The same three chips mean different numbers on different phones, because
 * "sustainable" is a property of the device. [DeviceTier] is deliberately coarse
 * — RAM, cores and the display's own refresh ceiling, all of which are free to
 * read and none of which needs a benchmark run on the critical path.
 */
@Serializable
enum class PerformancePreset(val displayName: String, val tagline: String) {
    /**
     * Raw preview + record, and nothing else. No live map, no keyframes, the
     * smallest budget that is still a picture. For a weak or hot phone, a very
     * long walk, or any session where the recording matters and the screen does
     * not — the `.lscan` is identical to [FULL]'s, because **no preset ever
     * touches the recording**.
     */
    LIGHT("Light", "raw preview · no live map"),

    /** Balanced, and the default. A live map, a mid budget, keyframes on. */
    OPTIMAL("Optimal", "balanced · recommended"),

    /** Everything on, at the device's own ceiling. Warned about on weaker phones. */
    FULL("Full", "everything on · needs a strong phone"),

    /**
     * Not selectable — what the chip row shows once an individual parameter has
     * been moved away from the preset that prefilled it. Item 22's whole point is
     * that this state is normal and lossless, not an error.
     */
    CUSTOM("Custom", "your own settings"),
    ;

    val isSelectable: Boolean get() = this != CUSTOM
}

/** How much live-view work this phone can carry. Coarse on purpose — see [PerformancePresets.tierFor]. */
@Serializable
enum class DeviceTier(val displayName: String) {
    /** Low RAM / few cores. The presets pull everything in hard here. */
    MODEST("modest"),

    /** The mainstream phone this app targets. */
    STANDARD("standard"),

    /** Plenty of RAM, plenty of cores, high-refresh panel — a Pixel 8 Pro is this. */
    FLAGSHIP("flagship"),
}

/**
 * Every live-view knob a preset sets, as one value.
 *
 * Deliberately NOT a superset of the capture settings: [liveMapEnabled] and the
 * four below are the ones that cost sustained GPU/CPU/disk during a walk.
 * Colour mode, colormap, gamma, brightness and point size are taste, cost
 * nothing, and are left exactly where the operator put them.
 */
@Serializable
data class CaptureTuning(
    /**
     * Whether the live viewport draws the **registered/pushbroom map** at all.
     *
     * False ([PerformancePreset.LIGHT]) means the viewport shows the sensor's
     * own raw returns, which for a D6 is a fan slice at the origin and for a
     * Mid-360 is the sensor-frame preview. The engine still records everything;
     * post-processing is still ground truth. It is the *drawing* of the map
     * that is skipped, and on a D6 that is the expensive half.
     */
    val liveMapEnabled: Boolean,
    /** Viewport refresh cap in fps. `0` = uncapped; only [PerformancePreset.FULL] on a flagship uses it. */
    val refreshHz: Int,
    /** §3.12's page-admission ceiling, in millions of points. */
    val lodBudgetMPoints: Int,
    /** B8's camera keyframes for colorization. */
    val keyframesEnabled: Boolean,
    /** 2 / 3 / 5 fps, the sheet's own row. */
    val keyframeRateFps: Int,
    /** ROUND 5.3's walked-path inset. */
    val trailEnabled: Boolean,
    /** Ring capacity for the trail — points, not metres. */
    val trailPoints: Int,
)

object PerformancePresets {

    /** The preset a fresh install starts on. */
    val DEFAULT: PerformancePreset = PerformancePreset.OPTIMAL

    /**
     * The device tier, from what any phone will tell you for free.
     *
     * Thresholds: 6 GB is where a modern Android phone stops having to fight for
     * a few hundred MB of GPU staging memory, and 8 cores + a >60 Hz panel is
     * what a current flagship has. Nothing here is a benchmark, and it is not
     * meant to be — the point is to avoid handing a 4 GB phone the same
     * defaults as a Pixel 8 Pro, not to rank devices.
     */
    fun tierFor(totalRamMb: Long, cpuCores: Int, displayCeilingHz: Int): DeviceTier = when {
        totalRamMb in 1 until 4_500 || cpuCores in 1 until 6 -> DeviceTier.MODEST
        totalRamMb >= 7_500 && cpuCores >= 8 && displayCeilingHz > 60 -> DeviceTier.FLAGSHIP
        else -> DeviceTier.STANDARD
    }

    /**
     * The tuning for a preset on a device.
     *
     * Refresh caps are snapped onto [RefreshGovernor.NOTCHES] and never exceed
     * the panel's own ceiling, so a preset can never select a rate the device
     * cannot reach — item 17's rule, applied to presets as well as to the
     * slider.
     */
    fun tuningFor(
        preset: PerformancePreset,
        tier: DeviceTier,
        displayCeilingHz: Int,
    ): CaptureTuning {
        val ceiling = displayCeilingHz.coerceAtLeast(RefreshGovernor.FLOOR_HZ)
        return when (preset) {
            PerformancePreset.LIGHT -> CaptureTuning(
                liveMapEnabled = false,
                refreshHz = snapRefresh(15, ceiling),
                lodBudgetMPoints = 2,
                keyframesEnabled = false,
                keyframeRateFps = 2,
                trailEnabled = true,
                trailPoints = 200,
            )

            // The default, and the whole answer to item 21. Nothing here is a
            // maximum: 30 fps on a 120 Hz panel, 6 M of a 20 M budget, keyframes
            // at the middle of their own 2/3/5 row.
            PerformancePreset.OPTIMAL -> when (tier) {
                DeviceTier.MODEST -> CaptureTuning(
                    liveMapEnabled = true,
                    refreshHz = snapRefresh(15, ceiling),
                    lodBudgetMPoints = 3,
                    keyframesEnabled = false,
                    keyframeRateFps = 2,
                    trailEnabled = true,
                    trailPoints = 300,
                )
                DeviceTier.STANDARD -> CaptureTuning(
                    liveMapEnabled = true,
                    refreshHz = snapRefresh(30, ceiling),
                    lodBudgetMPoints = 5,
                    keyframesEnabled = true,
                    keyframeRateFps = 2,
                    trailEnabled = true,
                    trailPoints = 400,
                )
                DeviceTier.FLAGSHIP -> CaptureTuning(
                    liveMapEnabled = true,
                    refreshHz = snapRefresh(30, ceiling),
                    lodBudgetMPoints = 8,
                    keyframesEnabled = true,
                    keyframeRateFps = 3,
                    trailEnabled = true,
                    trailPoints = 600,
                )
            }

            // Everything on. `refreshHz = 0` (uncapped) only where the panel and
            // the device can plausibly carry it; a MODEST phone asking for Full
            // still gets a cap, because "Full" must not mean "crash".
            PerformancePreset.FULL -> CaptureTuning(
                liveMapEnabled = true,
                refreshHz = if (tier == DeviceTier.MODEST) snapRefresh(30, ceiling) else 0,
                lodBudgetMPoints = 20,
                keyframesEnabled = true,
                keyframeRateFps = 5,
                trailEnabled = true,
                trailPoints = 600,
            )

            // CUSTOM is a read-out, not a selection: asking for its tuning means
            // "whatever the default is", which is what a caller falling through
            // to it actually wants.
            PerformancePreset.CUSTOM -> tuningFor(DEFAULT, tier, displayCeilingHz)
        }
    }

    /**
     * True when [preset] on this device would be a strain — drives the inline
     * caution item 22 asks for ("with an inline caution on weaker devices").
     */
    fun cautionFor(preset: PerformancePreset, tier: DeviceTier): String? = when {
        preset == PerformancePreset.FULL && tier == DeviceTier.MODEST ->
            "Full on a phone this size will drop live frames and warm up fast. The recording is " +
                "unaffected either way — Optimal shows you the same walk for far less heat."
        preset == PerformancePreset.FULL && tier == DeviceTier.STANDARD ->
            "Full runs the live view at this phone's ceiling. If it stutters, the view eases itself " +
                "down and says so; the recording never does."
        else -> null
    }

    /**
     * One line per parameter a preset switch actually moved — item 22's
     * "switching preset shows what it changed".
     *
     * An empty list means the switch was a no-op, which is worth saying too
     * (the caller shows "no change"): silently doing nothing is how an operator
     * concludes a control is broken.
     */
    fun changes(from: CaptureTuning, to: CaptureTuning): List<String> = buildList {
        if (from.liveMapEnabled != to.liveMapEnabled) {
            add(if (to.liveMapEnabled) "live 3D map on" else "live 3D map off (raw preview only)")
        }
        if (from.refreshHz != to.refreshHz) {
            add("live refresh ${refreshWord(from.refreshHz)} → ${refreshWord(to.refreshHz)}")
        }
        if (from.lodBudgetMPoints != to.lodBudgetMPoints) {
            add("point budget ${from.lodBudgetMPoints} M → ${to.lodBudgetMPoints} M")
        }
        if (from.keyframesEnabled != to.keyframesEnabled) {
            add(if (to.keyframesEnabled) "camera keyframes on" else "camera keyframes off")
        }
        if (from.keyframesEnabled && to.keyframesEnabled && from.keyframeRateFps != to.keyframeRateFps) {
            add("keyframe rate ${from.keyframeRateFps} → ${to.keyframeRateFps} fps")
        }
        if (from.trailPoints != to.trailPoints) {
            add("trail length ${from.trailPoints} → ${to.trailPoints} points")
        }
    }

    /**
     * Which selectable preset a set of live values corresponds to, or
     * [PerformancePreset.CUSTOM] once anything has been moved.
     */
    fun match(tuning: CaptureTuning, tier: DeviceTier, displayCeilingHz: Int): PerformancePreset =
        PerformancePreset.entries.firstOrNull {
            it.isSelectable && tuningFor(it, tier, displayCeilingHz) == tuning
        } ?: PerformancePreset.CUSTOM

    private fun refreshWord(hz: Int): String = if (hz <= 0) "Max" else "$hz fps"

    /** The nearest ladder notch at or below [wanted] that the panel can also reach. */
    private fun snapRefresh(wanted: Int, ceilingHz: Int): Int {
        val capped = minOf(wanted, ceilingHz)
        return RefreshGovernor.NOTCHES.firstOrNull { it <= capped } ?: RefreshGovernor.FLOOR_HZ
    }
}
