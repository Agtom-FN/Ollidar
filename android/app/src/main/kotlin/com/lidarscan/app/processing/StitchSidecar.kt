package com.lidarscan.app.processing

import java.io.File

/**
 * ROUND 19 — the few numbers the app reads back out of `processed/stitch.json`.
 *
 * The sidecar is WRITTEN by the engine (`slam/post/reprocess.cpp`) and its full
 * record is for people and bundles; what the app itself needs from it is one
 * debug-log line's worth: the yield audit (where the D6's 4,000 samples/s
 * went) and the rescue/recovery tallies. A whole JSON parser for that would be
 * a dependency doing a regex's job — the same judgement the engine's own
 * manifest reads make in the other direction (`d6_resolve.cpp`'s
 * `read_manifest_array`), and safe for the same reason: the shape is fixed and
 * written by one function in the same repository.
 */
object StitchSidecar {

    data class YieldAudit(
        val samples: Long,
        val noReturns: Long,
        val outOfWindow: Long,
        val noPose: Long,
        val flaggedExcluded: Long,
        val otherDropped: Long,
        val resolved: Long,
        val recovered: Long,
    )

    private fun sidecar(projectDir: File): File = File(projectDir, "processed/stitch.json")

    private fun longField(json: String, key: String): Long? =
        Regex("\"$key\"\\s*:\\s*(-?\\d+)").find(json)?.groupValues?.get(1)?.toLongOrNull()

    /** The `yield` object, or null when the sidecar predates round 19 or is absent. */
    fun readYield(projectDir: File): YieldAudit? {
        val f = sidecar(projectDir)
        if (!f.isFile) return null
        val json = runCatching { f.readText() }.getOrNull() ?: return null
        val start = json.indexOf("\"yield\"")
        if (start < 0) return null
        val block = json.substring(start)
        return YieldAudit(
            samples = longField(block, "samples") ?: return null,
            noReturns = longField(block, "noReturns") ?: 0L,
            outOfWindow = longField(block, "outOfWindow") ?: 0L,
            noPose = longField(block, "noPose") ?: 0L,
            flaggedExcluded = longField(block, "flaggedExcluded") ?: 0L,
            otherDropped = longField(block, "otherDropped") ?: 0L,
            resolved = longField(block, "resolved") ?: 0L,
            recovered = longField(block, "recovered") ?: 0L,
        )
    }

    /** How many rescues were attempted / applied, from the `rescues` array. */
    fun rescueCounts(projectDir: File): Pair<Int, Int>? {
        val f = sidecar(projectDir)
        if (!f.isFile) return null
        val json = runCatching { f.readText() }.getOrNull() ?: return null
        val start = json.indexOf("\"rescues\"")
        if (start < 0) return null
        val end = json.indexOf(']', start).let { if (it < 0) json.length else it }
        val block = json.substring(start, end)
        val attempted = Regex("\"decision\"").findAll(block).count()
        val applied = Regex("\"decision\"\\s*:\\s*\"rescued\"").findAll(block).count()
        return attempted to applied
    }

    // ── ROUND 20 item 80: the auto-level block ───────────────────────────────

    data class AutoLevel(
        val decision: String,
        val tiltBeforeDeg: Double,
        val tiltAfterDeg: Double,
        val correctionDeg: Double,
        val applied: Boolean,
    )

    private fun doubleField(json: String, key: String): Double? =
        Regex("\"$key\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)").find(json)?.groupValues?.get(1)?.toDoubleOrNull()

    /** The `autoLevel` object, or null when the sidecar predates round 20. */
    fun readAutoLevel(projectDir: File): AutoLevel? {
        val f = sidecar(projectDir)
        if (!f.isFile) return null
        val json = runCatching { f.readText() }.getOrNull() ?: return null
        val start = json.indexOf("\"autoLevel\"")
        if (start < 0) return null
        val end = json.indexOf('}', start).let { if (it < 0) json.length else it }
        val block = json.substring(start, end)
        val decision = Regex("\"decision\"\\s*:\\s*\"([^\"]*)\"").find(block)
            ?.groupValues?.get(1) ?: return null
        return AutoLevel(
            decision = decision,
            tiltBeforeDeg = doubleField(block, "tiltBeforeDeg") ?: 0.0,
            tiltAfterDeg = doubleField(block, "tiltAfterDeg") ?: 0.0,
            correctionDeg = doubleField(block, "correctionDeg") ?: 0.0,
            applied = block.contains(Regex("\"applied\"\\s*:\\s*true")),
        )
    }

    /** One debug-log line for the auto-level verdict, or null when unrecorded. */
    fun autoLevelLine(projectDir: File): String? {
        val a = readAutoLevel(projectDir) ?: return null
        return "auto-level: ${a.decision} — floor tilt %.2f -> %.2f deg, correction %.2f deg"
            .format(a.tiltBeforeDeg, a.tiltAfterDeg, a.correctionDeg)
    }

    /**
     * The one debug-log line item 66's file gets after auto-process: every
     * decoded sample accounted for by name, plus the rescue tally. Null when
     * there is nothing to read (no sidecar, or one written before 0.9.4).
     */
    fun yieldLine(projectDir: File): String? {
        val y = readYield(projectDir) ?: return null
        val rescues = rescueCounts(projectDir)
        val rescueNote = rescues?.let { (attempted, applied) ->
            if (attempted > 0) " rescues=$applied/$attempted" else ""
        } ?: ""
        return "d6 yield: ${y.samples} samples = ${y.noReturns} no-return + " +
            "${y.outOfWindow} out-of-window + ${y.noPose} no-pose + " +
            "${y.flaggedExcluded} flagged-excluded + ${y.otherDropped} other + " +
            "${y.resolved} resolved (+${y.recovered} recovered)$rescueNote"
    }
}
