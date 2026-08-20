package com.lidarscan.core

import com.lidarscan.core.model.SensorType
import com.lidarscan.core.render.DisplayProfile

/**
 * ROUND 22 item 97 — **what Simple mode shows, in one place.**
 *
 * The owner-approved simplification hides five things behind one Settings
 * switch (`AppSettings.advancedFeatures`, default **off**) and deliberately
 * does NOT hide two others. Scattering that decision across five screens would
 * mean five different answers to "is this on?" the first time someone changes
 * one of them, so every surface asks this object instead.
 *
 * Nothing here deletes anything. Each predicate is a visibility question; the
 * code behind every one of them stays compiled, stays reachable when the
 * switch is on, and stays under test — the same posture `FeatureFlags` has
 * taken since ROUND 10, for the same reason.
 *
 * `:core` and not `:app` so the rules are testable on a bare JVM and so
 * `:core` types (`DisplayProfile`, `SensorType`) can consult them directly.
 */
object SimpleMode {

    /**
     * The **Survey** display profile, and the capture-blocking GNSS gate rule
     * that travels with it.
     *
     * The gate is the reason this one matters more than a chip: the Survey
     * profile refuses to start a capture without a georeference, which on a
     * D6-in-a-flat walk is a capture that cannot be started for a reason the
     * operator did not ask for. Hidden means the profile cannot be selected,
     * so the rule cannot fire.
     */
    fun showsSurveyProfile(advanced: Boolean): Boolean = advanced

    /** The **Research** display profile — the 50 M-point one item 100 clamps anyway. */
    fun showsResearchProfile(advanced: Boolean): Boolean = advanced

    /**
     * The **Floor plan**: the Review screen's pill, the `Routes.PLAN`
     * destination and the "Floor plan" display-profile chip.
     *
     * ROUND 15 shipped it and was honest about what it is: a good scaled floor
     * MAP and a weak floor PLAN, whose gap is coverage rather than arithmetic.
     * That is a power-user output, not part of "scan a room and look at it".
     */
    fun showsFloorPlan(advanced: Boolean): Boolean = advanced

    /** The **Merge** screen — inherently multi-project, and off the simple path. */
    fun showsMerge(advanced: Boolean): Boolean = advanced

    /** **Cloud** processing mode. */
    fun showsCloudProcessing(advanced: Boolean): Boolean = advanced

    /**
     * The **Details, jobs & export** hub (`ProjectDetailScreen`) and the
     * separate **Processing** screen.
     *
     * In simple mode a project card opens the viewer directly (item 96) and
     * Export is a row on Review, so the hub has nothing left that is not
     * reachable in one tap. It is not deleted: with Advanced on, both screens
     * are back in navigation exactly as they are today.
     */
    fun showsProjectDetailHub(advanced: Boolean): Boolean = advanced

    /**
     * **RTK is NOT hidden by the switch** — it appears whenever a Mid-360 is
     * the selected sensor, whatever Simple mode says.
     *
     * The owner is testing Mid-360 + RTK shortly. Hiding the two screens that
     * trip is about behind a switch he would first have to discover would be
     * the simplification working directly against the person it is for. So the
     * rule is contextual rather than global: a D6-only operator never sees it,
     * and a Mid-360 operator always does.
     */
    fun showsRtk(advanced: Boolean, sensor: SensorType): Boolean =
        advanced || sensor == SensorType.MID360

    /** The **Mid-360 connect wizard** — same contextual rule, same reason. */
    fun showsMid360Connect(advanced: Boolean, sensor: SensorType): Boolean =
        advanced || sensor == SensorType.MID360

    /**
     * ROUND 23 item 106d — **the same three rules, applied to the chip that
     * actually selects a profile.**
     *
     * Round 22 hid Survey, Research and the floor plan; the Review screen's
     * Display panel went on enumerating `DisplayProfile.entries`, so all four
     * profiles stayed one tap away in Simple mode and `showsSurveyProfile`
     * answered "no" while a chip labelled "Survey" sat on the screen. Applying
     * a profile is how the Survey GNSS gate and the Research 50 M-point budget
     * get switched on, so this was the hidden feature reachable anyway — the
     * exact shape [showsFloorPlan]'s null-callback pairing exists to prevent.
     *
     * Quick scan is never hidden: it is the simple path's own profile, and a
     * panel with no profile chips at all is a panel with a dangling heading.
     */
    fun showsDisplayProfile(advanced: Boolean, profile: DisplayProfile): Boolean = when (profile) {
        DisplayProfile.SURVEY -> showsSurveyProfile(advanced)
        DisplayProfile.RESEARCH -> showsResearchProfile(advanced)
        DisplayProfile.FLOOR_PLAN -> showsFloorPlan(advanced)
        DisplayProfile.QUICK_SCAN -> true
    }

    /** The profile chips to draw, in the enum's own order. */
    fun displayProfiles(advanced: Boolean): List<DisplayProfile> =
        DisplayProfile.entries.filter { showsDisplayProfile(advanced, it) }
}
