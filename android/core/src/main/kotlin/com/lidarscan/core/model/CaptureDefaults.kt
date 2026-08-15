package com.lidarscan.core.model

import com.lidarscan.core.gnss.FixType
import com.lidarscan.core.render.DisplayProfile
import kotlinx.serialization.Serializable

/**
 * B5 — what a [WorkflowProfile] actually *sets* (Tech Spec §3.9: "profiles set
 * defaults"; §1 "Audience: all four segments via workflow profiles").
 *
 * B1 shipped the picker and persisted the choice; nothing read it. This is the
 * table that makes the choice mean something: every value below is applied at
 * **project creation time**, written into the project's manifest, and from
 * then on belongs to the project — changing the app-wide profile defaults
 * later does not silently re-write an existing capture's settings. That is the
 * same rule §3.9 states for display parameters ("settings persist per
 * project"), applied to the capture side as well, and it is the difference
 * between a profile and a global mode.
 *
 * ### Where each field ends up
 *
 * | field | consumed by |
 * | --- | --- |
 * | [liveSlam] | `scan_session_config.live_slam` (ABI 2) at capture start |
 * | [exportFormat] | the Processing screen's export sheet's pre-selection |
 * | [displayProfile] | `profile_defaults()` (A14 §5) → the project's [com.lidarscan.core.render.DisplayParams] |
 * | [captureCameraKeyframes] | whether B8's keyframe recorder is armed |
 * | [colorizeAfterProcessing] | whether the Processing screen pre-checks Colorize |
 * | [requireRtkFixForCapture] | §3.4's "capture UX warns/blocks below fix-quality threshold" |
 * | [minFixForCapture] | the gate that warning/block is measured against |
 * | [planSliceMinM]/[planSliceMaxM] | B11's floor-plan slice band (A12's `SliceOptions`) |
 *
 * ### `liveSlam` is a Mid-360 concept, and the D6 case is not a bug
 *
 * `scan_session_config.live_slam` starts **one LIO for the session's Mid-360
 * stream** (`core/engine.h`'s own wording). A D6 session has no Mid-360 stream
 * and no IMU, so the engine does nothing with the flag; the D6's "live" path is
 * A8's pushbroom assembler, which is gated on a mount calibration instead (see
 * [ProjectManifest.mountCalibration]). The flag is still recorded per project
 * because a project's sensor can only be chosen once, and because it is what a
 * Mid-360 project's Capture screen pre-selects.
 */
@Serializable
data class CaptureDefaults(
    /** Live-SLAM (true) vs Record-only (false) — Tech Spec §3.1's capture toggle. */
    val liveSlam: Boolean,
    /** Pre-selection in the export sheet. Not a restriction — every format stays available. */
    val exportFormat: ExportFormat,
    /** Which A14 display profile seeds this project's render settings (B10). */
    val displayProfile: DisplayProfile,
    /** Arm B8's 2–5 fps camera keyframe recorder during capture (§3.5). */
    val captureCameraKeyframes: Boolean,
    /** Pre-check "Colorize" on the Processing screen (only meaningful with keyframes). */
    val colorizeAfterProcessing: Boolean,
    /** §3.4: block (true) or merely warn (false) when the fix is below [minFixForCapture]. */
    val requireRtkFixForCapture: Boolean,
    /** The §3.4 ladder position capture is gated at. Compared numerically — [FixType] is ordered. */
    val minFixForCapture: FixType,
    /** A12 `SliceOptions::z_min_m` for this project's floor plan (B11). */
    val planSliceMinM: Float,
    /** A12 `SliceOptions::z_max_m`. */
    val planSliceMaxM: Float,
) {
    companion object {
        /**
         * The four profiles' capture-side defaults.
         *
         * Every choice below is derived from a spec line, and the ones that are
         * this task's own reading (rather than something the spec states) are
         * called out as such — §3.9 says "profiles set defaults" without
         * enumerating them, exactly as A14 §5 found for the display side.
         *
         * * **Survey** — §3.4's georeferenced workflow. Live SLAM on (a
         *   surveyor needs to see coverage while walking), **LAS 1.4** because
         *   it is the only export A9 gives a real CRS, keyframes + colorization
         *   on (an as-built record wants colour), and the RTK gate **blocks**
         *   at RTK Float: §3.4's "capture UX warns/blocks below fix-quality
         *   threshold", and for the profile whose whole point is georeferenced
         *   accuracy, blocking is the honest reading. Float rather than Fixed
         *   is deliberate — a hard Fixed-only gate makes the app unusable under
         *   canopy, and A10 §5 measures Float at 29 mm worst-corner error,
         *   which is still a usable survey product; the status strip keeps
         *   showing which one you actually had.
         * * **Floor plan** — §3.6. Live SLAM on, PLY out (the *plan* is
         *   exported as DXF/PDF separately, from B11), no camera pipeline at
         *   all: colour does not enter wall extraction, and the keyframe
         *   recorder costs GL-thread time and disk for nothing. Slice band
         *   1.0–1.5 m, matching A12's `SliceOptions` default exactly. RTK not
         *   gated — this is an indoor workflow.
         * * **Research** — §1's "keeps every raw stream for detailed offline
         *   analysis". **Record-only**: the raw streams are what this profile
         *   is for, live SLAM spends thermal budget on a preview that will be
         *   re-run at full density anyway (A7's post pipeline is "a second,
         *   better run from the same bytes"), and a phone that throttles
         *   mid-capture is the failure this avoids. Keyframes on (they cannot
         *   be recovered after the fact), colorization on, PLY out.
         * * **Quick scan** — §1's "minimal setup, single pass, live preview
         *   only". Live SLAM on, everything optional off, PLY out.
         */
        fun forProfile(profile: WorkflowProfile): CaptureDefaults = when (profile) {
            WorkflowProfile.SURVEY -> CaptureDefaults(
                liveSlam = true,
                exportFormat = ExportFormat.LAS14,
                displayProfile = DisplayProfile.SURVEY,
                captureCameraKeyframes = true,
                colorizeAfterProcessing = true,
                requireRtkFixForCapture = true,
                minFixForCapture = FixType.RTK_FLOAT,
                planSliceMinM = 1.0f,
                planSliceMaxM = 1.5f,
            )
            WorkflowProfile.FLOOR_PLAN -> CaptureDefaults(
                liveSlam = true,
                exportFormat = ExportFormat.PLY_BINARY,
                displayProfile = DisplayProfile.FLOOR_PLAN,
                captureCameraKeyframes = false,
                colorizeAfterProcessing = false,
                requireRtkFixForCapture = false,
                minFixForCapture = FixType.NONE,
                planSliceMinM = 1.0f,
                planSliceMaxM = 1.5f,
            )
            WorkflowProfile.RESEARCH -> CaptureDefaults(
                liveSlam = false,
                exportFormat = ExportFormat.PLY_BINARY,
                displayProfile = DisplayProfile.RESEARCH,
                captureCameraKeyframes = true,
                colorizeAfterProcessing = true,
                requireRtkFixForCapture = false,
                minFixForCapture = FixType.NONE,
                planSliceMinM = 1.0f,
                planSliceMaxM = 1.5f,
            )
            WorkflowProfile.QUICK_SCAN -> CaptureDefaults(
                liveSlam = true,
                exportFormat = ExportFormat.PLY_BINARY,
                displayProfile = DisplayProfile.QUICK_SCAN,
                captureCameraKeyframes = false,
                colorizeAfterProcessing = false,
                requireRtkFixForCapture = false,
                minFixForCapture = FixType.NONE,
                planSliceMinM = 1.0f,
                planSliceMaxM = 1.5f,
            )
        }

        /**
         * The engine's `scan_session_config.profile` string for a profile.
         * `scanengine_c.h` documents the four values only in a header comment
         * (`survey | floorplan | research | quickscan`) and gives no enum, so
         * this is the single place the app spells them — B2 used to pass the
         * literal `"quickscan"` unconditionally regardless of the project.
         */
        fun engineProfileString(profile: WorkflowProfile): String = when (profile) {
            WorkflowProfile.SURVEY -> "survey"
            WorkflowProfile.FLOOR_PLAN -> "floorplan"
            WorkflowProfile.RESEARCH -> "research"
            WorkflowProfile.QUICK_SCAN -> "quickscan"
        }
    }
}

/**
 * Mirror of `engine/include/scanengine/export/exporter.h`'s `ExportFormat`.
 * The numeric [code] values are the ones the JNI passes down, so they must not
 * drift from the C++ enum.
 */
@Serializable
enum class ExportFormat(
    val code: Int,
    val displayName: String,
    val extension: String,
    val description: String,
) {
    PLY_BINARY(0, "PLY (binary)", "ply", "Binary PLY with RGB. Opens everywhere; no CRS."),
    LAS14(1, "LAS 1.4", "las", "Georeferenced LAS 1.4 with RGB. The survey deliverable."),
    PCD(2, "PCD", "pcd", "Point Cloud Library format, for PCL/ROS toolchains."),
    DXF(3, "DXF", "dxf", "Floor-plan vectors (A12). Not a point-cloud format."),
    PDF(4, "PDF", "pdf", "Scaled floor-plan sheet (A12). Not a point-cloud format."),
    ;

    /** True for the three formats [com.lidarscan.core.jobs.JobKind.EXPORT_POINTS] can write. */
    val isPointCloud: Boolean get() = this == PLY_BINARY || this == LAS14 || this == PCD

    companion object {
        val pointCloudFormats: List<ExportFormat> get() = entries.filter { it.isPointCloud }
    }
}
