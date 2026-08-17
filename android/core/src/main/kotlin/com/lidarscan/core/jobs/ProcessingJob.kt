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
 */
enum class ProcessingMode(val displayName: String, val summary: String) {
    LOCAL(
        "Process here",
        "Run the post pipeline on this phone, in a foreground service. No network, no upload — but it is the " +
            "slowest option and the one that heats the device. Mid-360 captures only.",
    ),
    CLOUD(
        "Send to cloud",
        "Upload the .lscan as a resumable zip; a Linux worker runs the same engine CLI and the results come back " +
            "into the project. Needs a server URL and token in Settings.",
    ),
    EXTRACT_FOR_TRANSFER(
        "Save to phone",
        "Package the whole capture as a .lscan.zip and save it to Downloads — no server, no account, nothing to " +
            "configure. The desktop app imports that zip directly. This is the way to get a scan off the phone.",
    ),
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
data class ActionGate(val enabled: Boolean, val reason: String?) {
    companion object {
        val allowed = ActionGate(true, null)
        fun blocked(reason: String) = ActionGate(false, reason)
    }
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
     */
    fun postProcess(hasRawStreams: Boolean, sensor: SensorType? = null): ActionGate = when {
        !hasRawStreams -> ActionGate.blocked(
            "Nothing recorded yet — capture first. Post-processing runs from the raw streams on disk, not from a live session.",
        )
        sensor == SensorType.COIN_D6 -> ActionGate.blocked(
            "Post-processing is the Mid-360 LIO pipeline, and this is a COIN-D6 scan. A D6 cloud is built live " +
                "by the pushbroom from the phone's own trajectory — what you saw while walking IS the registered " +
                "result, and it is what Export writes. An offline re-run needs the ARCore pose stream saved " +
                "inside the .lscan, which the engine cannot write yet.",
        )
        else -> ActionGate.allowed
    }

    fun colorize(
        hasKeyframes: Boolean,
        syncQuality: SyncQuality,
        allowPoorSync: Boolean,
        hasProcessedCloud: Boolean,
    ): ActionGate = when {
        !hasProcessedCloud -> ActionGate.blocked(
            "Post-process first. Colorization paints an existing cloud; there is nothing in memory to paint.",
        )
        !hasKeyframes -> ActionGate.blocked(
            "No camera frames in this capture (streams/frames/ is empty), so there is nothing to sample colour from. " +
                "Tech Spec §3.5 calls this gracefully unavailable, not a failure — enable camera keyframes in the " +
                "project's profile before the next capture.",
        )
        syncQuality == SyncQuality.UNKNOWN -> ActionGate.blocked(
            "Clock sync never converged for this capture, so the colorizer refuses (it fails closed on purpose — " +
                "A4 §7). A mis-timed projection paints colour onto the wrong points and nothing downstream can tell.",
        )
        syncQuality == SyncQuality.POOR && !allowPoorSync -> ActionGate.blocked(
            "Clock sync is poor (>15 ms). At that jitter, sync alone eats most of the reprojection budget (S6). " +
                "Turn on \"Allow poor sync\" to override — the result is worth looking at, not worth quoting.",
        )
        else -> ActionGate.allowed
    }

    fun export(hasProcessedCloud: Boolean, hasLiveCloud: Boolean): ActionGate = when {
        hasProcessedCloud || hasLiveCloud -> ActionGate.allowed
        // ROUND 7: this refusal used to be a dead end that read as "you need a
        // server". It is a refusal about POINT-CLOUD formats (PLY/LAS/PCD),
        // which need a resolved cloud in memory — and it now names the door that
        // is always open, because "Save to phone" needs none of that.
        else -> ActionGate.blocked(
            "No point cloud in memory to convert to PLY/LAS/PCD — that needs a post-process run first. " +
                "To get this scan off the phone right now, use \"Save to phone\": it packages the whole " +
                "capture as a .lscan.zip into Downloads, with no processing and no server.",
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
        format == ExportFormat.LAS14 && !georeferenced ->
            "This capture is not georeferenced, so the LAS will carry A9's local-frame placeholder CRS rather than a " +
                "real one. It opens fine; it will not land anywhere on a map."
        format == ExportFormat.PCD -> "PCD carries no CRS field at all."
        else -> null
    }
}
