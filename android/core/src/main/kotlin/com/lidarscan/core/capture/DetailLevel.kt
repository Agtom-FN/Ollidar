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
 * | Detail | asks for | STANDARD | shown |
 * |---|---|---|---|
 * | **Auto** | [PerformancePreset.OPTIMAL] for the tier | 5 M | `Fits this device` |
 * | **High** | the [PerformancePreset.FULL] budget, clamped | 16 M | `16 M` |
 * | **Max**  | the tier's own ceiling | 16 M | `16 M` |
 *
 * ## ROUND 29 item 172 — why this table is not the one round 22 shipped
 *
 * It was: `AUTO` asked for [GpuPageBudget.maxSelectableLodPoints] — the tier
 * **ceiling** — while `HIGH` asked for the `OPTIMAL` budget. On the owner's
 * STANDARD phone that read **Auto 16 M, High 5 M**, and [selectableOn] then
 * dropped `MAX` as a duplicate of `AUTO`, so the control offered two rungs in
 * descending order. His words: *"detail of auto is 16M while high is 5M"*.
 *
 * The defect is not the numbers, it is that `AUTO` was given **Max's**
 * meaning. "Auto" is the rung that adapts; "Max" is the rung that takes
 * everything the device has. So `AUTO` now asks for what the tier itself
 * recommends (`OPTIMAL`, which is also what a fresh install has always
 * *behaved* like), `HIGH` takes the `FULL` budget under item 100's clamp, and
 * `MAX` takes the ceiling — and the ladder ascends on every tier.
 *
 * **Auto shows no number** ([AUTO_READOUT]). It does not have one: it is
 * whatever this phone measured as good for it, and printing `5 M` beside a
 * `16 M` rung invites an operator to "fix" a setting that is already correct
 * for his hardware. Every other rung prints the budget it actually applies.
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
 * A consequence worth stating: on a `MODEST` and on a `STANDARD` phone, High
 * and Max clamp to the same number (6.3 M and 16.8 M), so two rungs are offered
 * rather than three and the note says "Limited by this device". A `FLAGSHIP`
 * gets all three — 8 M / 20 M / 33 M. That is the honest presentation of a
 * ladder whose top is the hardware.
 */
enum class DetailLevel(val displayName: String) {
    /**
     * What this device was measured to be good for — the tier's own `OPTIMAL`
     * recommendation, and what a fresh install uses. The rung that adapts, and
     * therefore the one rung with no fixed number to print.
     */
    AUTO("Auto"),

    /** The FULL budget, under item 91's fixed accounting and item 100's ceiling. */
    HIGH("High"),

    /** Everything this device can hold — item 100's ceiling itself. */
    MAX("Max"),
    ;

    /**
     * Which performance preset this rung drives.
     *
     * `MAX` has no preset of its own: it asks for the ceiling directly (see
     * [DetailLevels.requestedPointsFor]), and `FULL` is the closest tuning for
     * everything else the preset carries — refresh, keyframes, the trail.
     */
    val preset: PerformancePreset
        get() = if (this == AUTO) PerformancePreset.OPTIMAL else PerformancePreset.FULL
}

object DetailLevels {

    /** What a fresh install starts on, and what "I don't want to think about this" means. */
    val DEFAULT: DetailLevel = DetailLevel.AUTO

    /** The one thing [DetailLevel.AUTO] prints instead of a number. */
    const val AUTO_READOUT = "Fits this device"

    /**
     * The **unclamped** budget each rung asks for, in points — taken from the
     * existing preset tuning so this control cannot drift away from what the
     * presets have always meant.
     *
     * [DetailLevel.MAX] asks for the tier's own ceiling, which is why it can
     * never be clamped and never carries a note. ROUND 29 item 172: that used
     * to be [DetailLevel.AUTO]'s line, and it is the whole defect — see the
     * class header.
     */
    fun requestedPointsFor(level: DetailLevel, tier: DeviceTier, displayCeilingHz: Int = 60): Int =
        when (level) {
            DetailLevel.MAX -> GpuPageBudget.maxSelectableLodPoints(tier)
            else -> PerformancePresets
                .tuningFor(level.preset, tier, displayCeilingHz)
                .lodBudgetMPoints * 1_000_000
        }

    /**
     * What the Detail row reads out for [level] — `Fits this device` for
     * [DetailLevel.AUTO], `16 M` for every other rung.
     *
     * One function so the Scan sheet and the Settings row cannot print the
     * question differently, which is the same rule item 150 applied to the
     * point count.
     */
    fun readoutFor(level: DetailLevel, tier: DeviceTier, displayCeilingHz: Int = 60): String =
        if (level == DetailLevel.AUTO) {
            AUTO_READOUT
        } else {
            "${budgetPointsFor(level, tier, displayCeilingHz) / 1_000_000} M"
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
    fun levelForBudget(points: Int, tier: DeviceTier, displayCeilingHz: Int = 60): DetailLevel {
        // ROUND 29 item 172 — **compare in millions, because that is what is
        // stored.**
        //
        // The budget makes a round trip through `_lodBudgetMPoints`, which is
        // an Int **in millions**, so a rung whose budget is not a whole number
        // of millions cannot come back equal to itself: `MAX` on a STANDARD
        // phone is 16 777 216 points, is written as `16`, and read back as
        // 16 000 000 — which matches no rung, so the row said `Auto` a moment
        // after the operator picked `High`. It was invisible until this round
        // only because `AUTO` used to BE the ceiling, so the fallback happened
        // to be the right answer.
        //
        // Millions is the resolution the setting actually has. Anything finer
        // is a comparison against a number this app never persists.
        val stored = GpuPageBudget.clampLodPointBudget(points, tier) / 1_000_000
        return DetailLevel.entries.firstOrNull {
            budgetPointsFor(it, tier, displayCeilingHz) / 1_000_000 == stored
        } ?: DetailLevel.AUTO
    }
}
