package com.lidarscan.core.capture

import com.lidarscan.core.render.GpuPageBudget

/**
 * ROUND 22 item 95 — **DETAIL: Auto / High / Max**, replacing the
 * Light / Optimal / Full presets on the Scan screen.
 *
 * The old names described the APP's effort ("Light" = the app does less work).
 * The new ones describe the RESULT the operator is choosing, which is the only
 * thing they can actually judge, and there are three of them rather than three
 * plus a `CUSTOM` state nobody selects.
 *
 * The mapping is deliberately thin — this is a rewording and a ceiling, not a
 * new performance model:
 *
 * | Detail | maps to | budget |
 * |---|---|---|
 * | **Auto** | [PerformancePreset.OPTIMAL] | the device tier's own safe limit |
 * | **High** | [PerformancePreset.OPTIMAL] | the old OPTIMAL budget, clamped |
 * | **Max**  | [PerformancePreset.FULL] | the old FULL budget, clamped |
 *
 * [PerformancePreset.LIGHT] is **not** dropped and is not reachable from this
 * control: it is the "the recording matters and the screen does not" setting,
 * which lives in the Advanced sheet with everything else power-user. No preset
 * has ever touched the recording, so none of this changes a single byte of any
 * `.lscan`.
 *
 * ## Every rung is clamped to the device (item 100)
 *
 * The owner: *"ceiling the LOD depends on the device. User will not able to
 * increase the LOD due to the detected hardware they are using."* So
 * [budgetPointsFor] runs every rung through
 * [GpuPageBudget.clampLodPointBudget], and [selectableOn] drops any rung whose
 * budget the device cannot hold — an option above the ceiling is **absent**,
 * not disabled, because a disabled control is something an operator argues
 * with. There is no override.
 *
 * A consequence worth stating: on a `MODEST` phone, High and Max clamp to the
 * same 6.3 M points, so only Auto is offered and the note says
 * "Limited by this device". That is the honest presentation of a device that
 * has one usable setting.
 */
enum class DetailLevel(val displayName: String) {
    /** The device tier's own safe limit — what a fresh install uses. */
    AUTO("Auto"),

    /** The old OPTIMAL budget, under item 91's fixed accounting and item 100's ceiling. */
    HIGH("High"),

    /** The old FULL budget, same two constraints. */
    MAX("Max"),
    ;

    /** Which performance preset this rung drives. */
    val preset: PerformancePreset
        get() = if (this == MAX) PerformancePreset.FULL else PerformancePreset.OPTIMAL
}

object DetailLevels {

    /** What a fresh install starts on, and what "I don't want to think about this" means. */
    val DEFAULT: DetailLevel = DetailLevel.AUTO

    /**
     * The **unclamped** budget each rung asks for, in points — taken from the
     * existing preset tuning so this control cannot drift away from what the
     * presets have always meant.
     *
     * [DetailLevel.AUTO] asks for the tier's own ceiling, which is why it can
     * never be clamped and never carries a note.
     */
    fun requestedPointsFor(level: DetailLevel, tier: DeviceTier, displayCeilingHz: Int = 60): Int =
        when (level) {
            DetailLevel.AUTO -> GpuPageBudget.maxSelectableLodPoints(tier)
            else -> PerformancePresets
                .tuningFor(level.preset, tier, displayCeilingHz)
                .lodBudgetMPoints * 1_000_000
        }

    /** The budget actually applied: [requestedPointsFor] through item 100's ceiling. */
    fun budgetPointsFor(level: DetailLevel, tier: DeviceTier, displayCeilingHz: Int = 60): Int =
        GpuPageBudget.clampLodPointBudget(requestedPointsFor(level, tier, displayCeilingHz), tier)

    /**
     * The rungs this device may be offered.
     *
     * A rung is dropped when it would deliver nothing more than the rung below
     * it — i.e. when the ceiling has already flattened them into the same
     * budget. [DetailLevel.AUTO] is always present: a device with one usable
     * setting still has one usable setting.
     */
    fun selectableOn(tier: DeviceTier, displayCeilingHz: Int = 60): List<DetailLevel> {
        val seen = LinkedHashMap<Int, DetailLevel>()
        for (level in DetailLevel.entries) {
            seen.putIfAbsent(budgetPointsFor(level, tier, displayCeilingHz), level)
        }
        val offered = seen.values.toList()
        return if (DetailLevel.AUTO in offered) offered else listOf(DetailLevel.AUTO) + offered
    }

    /**
     * The one short note under the Detail row, or null when nothing is being
     * limited. Item 98's wording law: four words, no jargon.
     */
    fun ceilingNote(tier: DeviceTier, displayCeilingHz: Int = 60): String? =
        if (selectableOn(tier, displayCeilingHz).size < DetailLevel.entries.size) {
            "Limited by this device"
        } else {
            null
        }

    /**
     * Which rung a stored point budget corresponds to, for showing the current
     * selection. Falls back to [DetailLevel.AUTO] — the honest answer for a
     * budget that came from somewhere else (an old CUSTOM preset, a project
     * saved on another phone) is "the safe one", not a rung it does not match.
     */
    fun levelForBudget(points: Int, tier: DeviceTier, displayCeilingHz: Int = 60): DetailLevel =
        DetailLevel.entries.firstOrNull {
            budgetPointsFor(it, tier, displayCeilingHz) == GpuPageBudget.clampLodPointBudget(points, tier)
        } ?: DetailLevel.AUTO
}
