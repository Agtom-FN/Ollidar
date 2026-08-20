package com.lidarscan.core.projects

import kotlinx.coroutines.CancellationException

/**
 * ROUND 23 item 104c — **what "export the 3 scans I picked" actually is.**
 *
 * The export pipeline is per-project and always has been: one A15 job, one
 * output file, one ROUND 7 delivery into `Downloads/LidarScan/`. Group export
 * is therefore a **sequencing** problem, not a second pipeline — and the only
 * interesting question in it is the one the owner's rounds keep returning to:
 * *what happens to job 3 when job 2 fails?*
 *
 * The answer this file encodes, and the reason it is in `:core` under test:
 *
 *  * **The rest still run.** Aborting the batch on the first failure would
 *    lose two good exports because of one bad scan, and the operator would
 *    have no way to tell which of the three they now have.
 *  * **The failure is reported, never dropped.** Every step ends in exactly
 *    one [BatchOutcome], success or failure, and the failure carries its
 *    reason. This is ROUND 7's "there is no third outcome" rule applied to a
 *    batch: a run of five that produces four files says so, out loud.
 *  * **Cancellation is not a failure.** A cancelled scope propagates (the
 *    ROUND 22 item 90 rule) instead of being recorded as five broken scans.
 *  * **One at a time.** The jobs are engine jobs against the same native
 *    handle and the same per-container lock; running them concurrently would
 *    only queue them behind each other with a less legible progress story.
 */
enum class BatchAction(val verb: String, val imperative: String) {
    /** Save each scan into `Downloads/LidarScan/`. */
    EXPORT("Exported", "Export"),

    /** Save each scan, then hand all of the produced files to ONE share sheet. */
    SHARE("Shared", "Share"),

    /** The long-press bar's destructive action. Still confirmed by a dialog. */
    DELETE("Deleted", "Delete"),
}

/** One project's turn in the batch. [index] is 0-based; [total] never changes mid-run. */
data class BatchStep(
    val projectId: String,
    val index: Int,
    val total: Int,
) {
    /** "2 of 3" — the chip on the card, three words at most. */
    val positionLabel: String get() = "${index + 1} of $total"
}

/** Exactly one of these per step. There is no "we are not sure" outcome. */
data class BatchOutcome(
    val projectId: String,
    /** Null on success; the reason on failure. Never blank when non-null. */
    val error: String? = null,
) {
    val ok: Boolean get() = error == null
}

/**
 * The end of a batch: what ran, what worked, and what did not.
 *
 * [summary] is the one line the operator sees. It is written to the ROUND 22
 * wording law — a report at most twelve words, and when something failed it
 * says what to tap next rather than stopping at "1 failed".
 */
data class BatchReport(
    val action: BatchAction,
    val outcomes: List<BatchOutcome>,
) {
    val succeeded: List<String> get() = outcomes.filter { it.ok }.map { it.projectId }
    val failed: List<BatchOutcome> get() = outcomes.filterNot { it.ok }
    val allSucceeded: Boolean get() = outcomes.isNotEmpty() && failed.isEmpty()

    /** The files the SHARE arm should put in one `ACTION_SEND_MULTIPLE`. */
    fun succeededIn(order: List<String>): List<String> =
        order.filter { id -> succeeded.contains(id) }

    fun summary(): String {
        if (outcomes.isEmpty()) return "Nothing selected."
        val done = succeeded.size
        val total = outcomes.size
        val noun = if (total == 1) "scan" else "scans"
        return when {
            failed.isEmpty() -> "${action.verb} $done $noun."
            done == 0 -> "Nothing was ${action.verb.lowercase()}. Tap ${action.imperative} to retry."
            else -> "${action.verb} $done of $total. Tap ${action.imperative} to retry ${failed.size}."
        }
    }

    /** The per-scan reasons, for the log. Not a screen string — see WordingLaw. */
    fun failureLogLines(): List<String> = failed.map { "${it.projectId}: ${it.error}" }
}

/**
 * Builds and runs the ordered plan. Pure apart from the caller's own step
 * function, so the whole failure story is testable on a bare JVM.
 */
object BatchExport {

    /** The ordered list of jobs. Duplicate ids collapse; order is the caller's. */
    fun plan(projectIds: List<String>): List<BatchStep> {
        val unique = projectIds.distinct()
        return unique.mapIndexed { i, id -> BatchStep(id, i, unique.size) }
    }

    /**
     * Runs [step] once per project, **in order, one at a time**, and keeps
     * going after a failure.
     *
     * @param step returns `Result.success` when that project's job produced its
     *   file, `Result.failure` when it did not. A thrown exception is treated
     *   exactly like a returned failure — except [CancellationException], which
     *   propagates untouched (ROUND 22 item 90: cancellation is never swallowed
     *   and never reported as a failure).
     */
    suspend fun run(
        action: BatchAction,
        projectIds: List<String>,
        onStep: (BatchStep) -> Unit = {},
        step: suspend (BatchStep) -> Result<Unit>,
    ): BatchReport {
        val outcomes = mutableListOf<BatchOutcome>()
        for (job in plan(projectIds)) {
            onStep(job)
            val result = try {
                step(job)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (t: Throwable) {
                Result.failure(t)
            }
            outcomes += result.fold(
                onSuccess = { BatchOutcome(job.projectId) },
                onFailure = { e -> BatchOutcome(job.projectId, reasonOf(e)) },
            )
        }
        return BatchReport(action, outcomes)
    }

    /** A non-blank reason, always — an exception with a null message still has a class name. */
    private fun reasonOf(e: Throwable): String =
        e.message?.takeIf { it.isNotBlank() } ?: e.javaClass.simpleName
}
