package com.lidarscan.core.model

import kotlinx.serialization.Serializable

/**
 * Workflow profiles are how the Tech Spec (§1 "Audience") covers all four
 * target segments with one app: each profile is a preset bundle of capture
 * and processing defaults rather than a separate mode. B5 (Profiles +
 * Settings) is expected to make these defaults user-editable; B1 only wires
 * the picker and persists the choice on the project.
 *
 * The one-line [description] strings below are this scaffold's reading of
 * the spec's audience mapping (§1 lists the four segments and their driving
 * features — RTK/CRS in §3.4, floor plan extraction in §3.6, record-always
 * raw-stream retention in §3.11, "Record now, process later" in §1) — the
 * spec itself doesn't spell out per-profile copy verbatim, so flag this for
 * product sign-off if the wording should change.
 */
@Serializable
enum class WorkflowProfile(val displayName: String, val description: String) {
    SURVEY(
        displayName = "Survey",
        description = "Georeferenced, RTK-accurate capture for professional surveying and as-built records.",
    ),
    FLOOR_PLAN(
        displayName = "Floor plan",
        description = "Interior walkthroughs tuned for wall and opening extraction into DXF/PDF plans.",
    ),
    RESEARCH(
        displayName = "Research",
        description = "Full-fidelity capture that keeps every raw stream for detailed offline analysis.",
    ),
    QUICK_SCAN(
        displayName = "Quick scan",
        description = "Minimal setup, single pass, live preview only — a fast look at a space.",
    ),
}
