package com.lidarscan.core.jobs

import com.lidarscan.core.model.ExportFormat
import com.lidarscan.core.model.SensorType

/**
 * B6 — the processing-queue vocabulary, mirroring
 * `engine/include/scanengine/jobs/job_types.h` (A15).
 *
 * Kept in `:core` (plain Kotlin, no Android) so the queue's *policy* — which
 * actions are offered, which are gated and on what — is unit-testable without a
 * device, exactly like B3's `Mid360Settings` validation. The JNI layer
 * (`processing_jni.cpp`) marshals `scanengine::jobs::Job` into [ProcessingJob]
 * field for field.
 */

/** Mirror of `jobs::JobKind`. [code] must stay in lock-step with the C++ enum. */
enum class JobKind(val code: Int, val displayName: String) {
    POST_PROCESS(0, "Post-process"),
    COLORIZE(1, "Colorize"),
    EXPORT_POINTS(2, "Export"),
    TRANSFER_EXPORT(3, "Transfer bundle"),
    CLOUD_SUBMIT(4, "Cloud submit"),
    ;

    companion object {
        fun fromCode(code: Int): JobKind = entries.firstOrNull { it.code == code } ?: POST_PROCESS
    }
}

/**
 * Mirror of `jobs::JobState` — **five** states, not six.
 *
 * A15 §2 and `scanengine_c.h`'s `SCAN_JOB_*` block both spell out why: a
 * cancelled job settles into [FAILED] with `error == SCAN_ERR_CANCELLED`, the
 * same convention the whole engine uses, so a UI reads the error rather than
 * carrying a second code path. [ProcessingJob.wasCancelled] is that read, in one
 * place.
 */
enum class JobState(val code: Int, val displayName: String) {
    QUEUED(0, "Queued"),
    RUNNING(1, "Running"),
    CANCELLING(2, "Cancelling"),
    DONE(3, "Done"),
    FAILED(4, "Failed"),
    ;

    val isTerminal: Boolean get() = this == DONE || this == FAILED
    val isActive: Boolean get() = this == QUEUED || this == RUNNING || this == CANCELLING

    companion object {
        fun fromCode(code: Int): JobState = entries.firstOrNull { it.code == code } ?: QUEUED
    }
}

/** One row of the Processing screen's queue. */
data class ProcessingJob(
    val id: Long,
    val kind: JobKind,
    val state: JobState,
    /** 0..1, monotone within one run. */
    val progress: Float,
    /** A15's stable per-kind stage label (e.g. a `PostStage::to_string()`). */
    val stage: String,
    /** `scan_error_t`; 0 = OK. */
    val error: Int,
    val message: String,
) {
    /** SCAN_ERR_CANCELLED == 9. See [JobState]'s note on why this is not a state. */
    val wasCancelled: Boolean get() = state == JobState.FAILED && error == 9

    val statusText: String get() = when {
        wasCancelled -> "Cancelled"
        state == JobState.FAILED -> message.ifBlank { "Failed (error $error)" }
        state == JobState.DONE -> "Done"
        stage.isNotBlank() -> "${state.displayName} — $stage"
        else -> state.displayName
    }
}

/** Tech Spec §3.8's three processing modes. */
/**
 * ROUND 7 (owner field item) — **the names say where the scan goes.**
 *
 * The owner tried to get `scan-008` off the phone, got *"No cloud to export"*,
 * and concluded — correctly, from what the screen said — that getting a scan out
 * of this app required a configured server. It never did: `EXTRACT_FOR_TRANSFER`
 * has always packaged the whole project as a `.lscan.zip` with no server
 * involved. It was called "Extract for transfer", sat third in a row next to
 * "Cloud", and its only delivery route was a share sheet.
 *
 * So the two things a person actually wants to choose between are now named as
 * such — **Save to phone** and **Send to cloud** — and "Local" is named for what
 * it is (processing that happens here), not for where the file ends up.
 *
 * ── ROUND 28 item 163: [summary] is a row's detail line, not an essay ───────
 *
 * These three were a stacked segmented control on the Jobs tab with a 38-word
 * paragraph under it — a settings screen wearing a status tab's name (J7, J9).
 * The chooser moved to Review's Export sheet as a single **Destination** row,
 * where a mode gets one ≤12-word line and nothing else, so [summary] is now
 * written to that budget. Every fact that went missing said "this is how the
 * bytes move", which is not what someone choosing a destination is asking.
 */
enum class ProcessingMode(val displayName: String, val summary: String) {
    LOCAL("Process here", "Slowest, heats the phone. Mid-360 scans only."),
    CLOUD("Send to cloud", "Needs a server set up in Settings."),
    EXTRACT_FOR_TRANSFER("Save to phone", "Saves to Downloads. No server, no account."),
}

/**
 * Whether a given action can run right now, and — when it cannot — the sentence
 * that says why.
 *
 * A refusal that names its own cause is the whole point: A11 §"gracefully
 * unavailable" and Tech Spec §3.5 both require colorization to be *reported* as
 * unavailable rather than fail, and the colorizer's own sync gate
 * (`SCAN_SYNC_UNKNOWN` fails closed) produces a bare `SCAN_ERR_NOT_SUPPORTED`
 * that means nothing to an operator.
 */
data class ActionGate(
    val enabled: Boolean,
    val reason: String?,
    val tone: GateTone = GateTone.BAD,
    /**
     * The long form, for the capture log only. Never rendered.
     *
     * ROUND 28 item 163: the D6/STL-27L refusal below used to be 62 words of
     * pipeline archaeology **on the screen**. The archaeology is genuinely
     * useful — to whoever is reading a log a year from now asking why an
     * offline re-run was never offered — so it moved here rather than being
     * deleted. [reason] is what a person reads; this is what a developer reads.
     */
    val logReason: String? = null,
) {
    companion object {
        val allowed = ActionGate(true, null)

        /** Something is wrong or missing and the operator can act on it. Red. */
        fun blocked(reason: String, logReason: String? = null) =
            ActionGate(false, reason, GateTone.BAD, logReason)

        /**
         * The action does not apply to this scan. Nothing failed, nothing was
         * lost, there is nothing to fix — so it is [GateTone.NEUTRAL] and the
         * caller must not paint it in `bad`.
         */
        fun notApplicable(reason: String, logReason: String? = null) =
            ActionGate(false, reason, GateTone.NEUTRAL, logReason)
    }
}

/**
 * ROUND 28 item 163 (review findings J1/J2) — **"unavailable" is not "failed".**
 *
 * Every refusal on the Processing screen was rendered `Hint(gate.reason, color =
 * SemBad)`, because [ActionGate] had one shape and red was it. The screen
 * therefore told an operator holding a perfectly good D6 scan, in error red,
 * that his scan could not be post-processed — when the message actually means
 * *"this scan type is already final"*. Red means an operation failed and the
 * operator lost something. A colour that means both things means neither, so
 * the gate now carries which one it is and the caller reads it instead of
 * guessing.
 */
enum class GateTone {
    /** An operation failed, or something the operator needs is missing. `bad`. */
    BAD,

    /** Not applicable here. `inkMute`, never red. §C.6's "not applicable" row. */
    NEUTRAL,
}

/**
 * Mirror of `SCAN_SYNC_*` (`scanengine_c.h`, A11/A4). **Fails closed**:
 * [UNKNOWN] is what an unconverged estimator reports *and* what a caller who
 * never wired A4 gets, and the colorizer refuses both.
 */
enum class SyncQuality(val code: Int, val label: String) {
    UNKNOWN(0, "not converged"),
    GOOD(1, "good (≤5 ms)"),
    GATED(2, "gated (≤15 ms)"),
    POOR(3, "poor (>15 ms)"),
    ;

    companion object {
        fun fromCode(code: Int): SyncQuality = entries.firstOrNull { it.code == code } ?: UNKNOWN
    }
}

/**
 * The Processing screen's action policy, in one testable place.
 *
 * @param hasRawStreams the project has something recorded at all
 * @param hasKeyframes `streams/frames/` exists and is non-empty (B8's output)
 * @param syncQuality what A4 reports for the lidar stream — the colorizer's own go/no-go input
 * @param allowPoorSync the operator's explicit override behind [SyncQuality.POOR]
 * @param hasProcessedCloud a finished post-process job's `PageStore` is available in this app session
 * @param cloudConfigured a server URL and token are set in Settings
 */
object ProcessingPolicy {

    /**
     * ROUND 7, item 4 — **this gate now knows which sensor it is gating.**
     *
     * `PostSlamPipeline` is a Mid-360 pipeline and says so in its own header:
     * its decode loop counts `kLidarMid360` and `kImu` chunks and returns
     * `kNotFound` when there are none (`engine/src/post/post_pipeline.cpp`). A
     * COIN-D6 `.lscan` has neither — it holds raw UART bytes and camera frames
     * — so tapping Post-process on a phone-D6 scan submitted a job that failed,
     * and failed with the engine's bare `"not found"`, because `JobQueue`
     * discards the pipeline's own explanatory message. An enabled button that
     * produces a two-word error is worse than a disabled one that explains
     * itself, and this file's own header ("every refusal has to name its own
     * cause") already said so.
     *
     * The D6's registered cloud is produced **live**, by A8's pushbroom, from
     * the ARCore trajectory — that is what the capture screen draws and what the
     * project preview shows. There is no offline re-resolve today for one
     * concrete, nameable reason: the pose stream is never written to the
     * `.lscan` (`ChunkType::kPoseAr` is defined and mapped in `lscan.cpp` and
     * has no writer anywhere), so the trajectory does not survive the session.
     * Until it does, "post-process a D6 scan" has nothing to run on, and saying
     * that is the honest gate. See android/NOTES.md ROUND 7 §4.
     *
     * ── ROUND 28 item 163 (review J1/J2): honest, and now also short ────────
     *
     * All of the above stayed true and none of it belonged on the screen. The
     * pushbroom refusal shipped as **62 words** naming `Mid-360 LIO pipeline`,
     * `pushbroom`, `trajectory`, `registered result`, `ARCore pose stream` and
     * `.lscan` — against a six-word instruction law — and it was painted in
     * error red, so the one screen an operator visits after a good walk told
     * him in red that something was wrong with his scan. Nothing is wrong with
     * it. It is finished.
     *
     * "Already final. Processed while you walked." is that fact, in the words
     * the operator would use: he watched the cloud build as he walked, and what
     * he watched is what Export writes. The paragraph is not deleted — it moves
     * to [ActionGate.logReason], where the person who needs `kPoseAr` will find
     * it. And the gate is [ActionGate.notApplicable], so the colour says
     * "nothing to do here" rather than "you lost your scan".
     */
    fun postProcess(hasRawStreams: Boolean, sensor: SensorType? = null): ActionGate = when {
        !hasRawStreams -> ActionGate.blocked("Nothing recorded yet — capture first.")
        // ROUND 25 item 119: the STL-27L falls in here too, and for exactly the
        // reason the log line still gives — it is a 2-D pushbroom whose cloud
        // is assembled live from the phone's trajectory, and that trajectory is
        // still not written to the `.lscan`. The pipeline fact is unchanged;
        // only its audience moved.
        sensor != null && sensor.isPhoneTrackedPushbroom -> ActionGate.notApplicable(
            "Already final. Processed while you walked.",
            logReason = "post-process not offered: ${sensor.displayName} is a phone-tracked pushbroom. Its cloud " +
                "is built live from the ARCore trajectory and is what Export writes; an offline re-run needs the " +
                "pose stream saved inside the .lscan (ChunkType::kPoseAr has no writer). See NOTES.md ROUND 7 §4.",
        )
        else -> ActionGate.allowed
    }

    fun colorize(
        hasKeyframes: Boolean,
        syncQuality: SyncQuality,
        allowPoorSync: Boolean,
        hasProcessedCloud: Boolean,
    ): ActionGate = when {
        // ROUND 28 item 163: same four refusals, same order, same policy — the
        // paragraphs move to `logReason`. Every one of these carried a "§" the
        // wording law lists as jargon, and three of the four were the "not
        // applicable" case wearing red. What the operator gets is the fact; the
        // spec citation goes where a spec citation is useful.
        !hasProcessedCloud -> ActionGate.blocked("Post-process first — nothing to paint.")
        !hasKeyframes -> ActionGate.notApplicable(
            "No camera frames in this scan.",
            logReason = "colorize unavailable: streams/frames/ is empty, so there is nothing to sample colour " +
                "from. Tech Spec §3.5 calls this gracefully unavailable, not a failure — camera keyframes are " +
                "enabled in the project's capture profile.",
        )
        syncQuality == SyncQuality.UNKNOWN -> ActionGate.notApplicable(
            "Camera and lidar clocks never matched.",
            logReason = "colorize refused: clock sync never converged for this capture and the colorizer fails " +
                "closed on purpose (A4 §7). A mis-timed projection paints colour onto the wrong points and " +
                "nothing downstream can tell.",
        )
        syncQuality == SyncQuality.POOR && !allowPoorSync -> ActionGate.blocked(
            "Clock sync is poor. Turn on \"Allow poor sync\".",
        )
        else -> ActionGate.allowed
    }

    fun export(hasProcessedCloud: Boolean, hasLiveCloud: Boolean): ActionGate = when {
        hasProcessedCloud || hasLiveCloud -> ActionGate.allowed
        // ROUND 7: this refusal used to be a dead end that read as "you need a
        // server". It is a refusal about POINT-CLOUD formats (PLY/LAS/PCD),
        // which need a resolved cloud in memory — and it names the door that is
        // always open, because "Save to phone" needs none of that.
        //
        // ROUND 28 item 163: 45 words down to 10, and the door it names is the
        // half that mattered. The format archaeology is the log's.
        else -> ActionGate.blocked(
            "No point cloud yet. Use \"Save to phone\" instead.",
            logReason = "export refused: no resolved cloud in memory to convert to PLY/LAS/PCD, which needs a " +
                "post-process run. \"Save to phone\" packages the capture as a .lscan.zip into Downloads with " +
                "no processing and no server.",
        )
    }

    fun transferBundle(hasRawStreams: Boolean): ActionGate =
        if (hasRawStreams) {
            ActionGate.allowed
        } else {
            ActionGate.blocked("Nothing to package — this project has no recorded streams.")
        }

    fun cloudSubmit(hasRawStreams: Boolean, cloudConfigured: Boolean): ActionGate = when {
        !hasRawStreams -> ActionGate.blocked("Nothing to upload — this project has no recorded streams.")
        !cloudConfigured -> ActionGate.blocked("Set the cloud server URL and token in Settings first.")
        else -> ActionGate.allowed
    }

    /**
     * LAS 1.4 without a converged georeference is legal and is what A9
     * documents (an `ENGCRS` local-frame placeholder), but a survey deliverable
     * that opens in QGIS at the wrong place is a bad surprise. This is a note,
     * never a block.
     */
    fun exportFormatNote(format: ExportFormat, georeferenced: Boolean): String? = when {
        // ROUND 28 item 163: 32 words and a "CRS" down to a detail line. What
        // the operator needs to know is that the file opens but lands nowhere
        // on a map; A9's placeholder is the reason, not the message.
        format == ExportFormat.LAS14 && !georeferenced ->
            "Opens fine, but lands nowhere on a map."
        format == ExportFormat.PCD -> "Carries no map position."
        else -> null
    }
}
