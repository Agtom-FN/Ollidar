package com.lidarscan.core.projects

import com.lidarscan.core.WordingLaw
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 23 item 104c — **the sequencing, and above all what a failure in the
 * middle of it does.**
 *
 * The owner's rounds have converged on one rule for anything that touches
 * files: an operation ends in a visible success with a path or a visible
 * failure, and there is no third outcome (ROUND 7). A batch of five makes that
 * rule harder, not easier — the tempting implementation aborts on the first
 * failure and leaves the operator with two files, no message, and no way to
 * tell which two. These tests are the version that does not.
 */
class BatchExportTest {

    @Test
    fun `the plan is ordered, numbered and de-duplicated`() {
        val plan = BatchExport.plan(listOf("a", "b", "a", "c"))
        assertEquals(listOf("a", "b", "c"), plan.map { it.projectId })
        assertEquals(listOf(0, 1, 2), plan.map { it.index })
        assertTrue(plan.all { it.total == 3 })
        assertEquals("2 of 3", plan[1].positionLabel)
    }

    @Test
    fun `an empty selection plans nothing`() {
        assertTrue(BatchExport.plan(emptyList()).isEmpty())
    }

    @Test
    fun `jobs run one at a time, in order`() = runTest {
        val seen = mutableListOf<String>()
        var concurrent = 0
        var maxConcurrent = 0
        val report = BatchExport.run(BatchAction.EXPORT, listOf("a", "b", "c")) { step ->
            concurrent++
            maxConcurrent = maxOf(maxConcurrent, concurrent)
            seen += step.projectId
            concurrent--
            Result.success(Unit)
        }
        assertEquals(listOf("a", "b", "c"), seen)
        assertEquals(1, maxConcurrent)
        assertTrue(report.allSucceeded)
        assertEquals("Exported 3 scans.", report.summary())
    }

    @Test
    fun `onStep fires once per job before that job runs`() = runTest {
        val announced = mutableListOf<String>()
        BatchExport.run(
            action = BatchAction.EXPORT,
            projectIds = listOf("a", "b"),
            onStep = { announced += it.projectId },
        ) { Result.success(Unit) }
        assertEquals(listOf("a", "b"), announced)
    }

    // ── the one that matters ────────────────────────────────────────────────

    @Test
    fun `a failure in the MIDDLE does not stop the rest`() = runTest {
        val ran = mutableListOf<String>()
        val report = BatchExport.run(BatchAction.EXPORT, listOf("a", "b", "c")) { step ->
            ran += step.projectId
            if (step.projectId == "b") Result.failure(IllegalStateException("no room on the phone"))
            else Result.success(Unit)
        }
        assertEquals("job 3 must still run", listOf("a", "b", "c"), ran)
        assertEquals(listOf("a", "c"), report.succeeded)
        assertEquals(1, report.failed.size)
        assertFalse(report.allSucceeded)
    }

    @Test
    fun `the failure is REPORTED, with its reason, never silently dropped`() = runTest {
        val report = BatchExport.run(BatchAction.EXPORT, listOf("a", "b")) { step ->
            if (step.projectId == "b") Result.failure(IllegalStateException("no room on the phone"))
            else Result.success(Unit)
        }
        assertEquals("b", report.failed.single().projectId)
        assertEquals("no room on the phone", report.failed.single().error)
        assertTrue(report.failureLogLines().single().contains("no room on the phone"))
    }

    @Test
    fun `a THROWN exception is an outcome too, and the batch continues`() = runTest {
        val ran = mutableListOf<String>()
        val report = BatchExport.run(BatchAction.EXPORT, listOf("a", "b", "c")) { step ->
            ran += step.projectId
            if (step.projectId == "a") throw IllegalStateException("the engine refused")
            Result.success(Unit)
        }
        assertEquals(listOf("a", "b", "c"), ran)
        assertEquals("the engine refused", report.failed.single().error)
        assertEquals(2, report.succeeded.size)
    }

    @Test
    fun `an exception with no message still reports a non-blank reason`() = runTest {
        val report = BatchExport.run(BatchAction.EXPORT, listOf("a")) {
            Result.failure(IllegalStateException())
        }
        assertEquals("IllegalStateException", report.failed.single().error)
    }

    @Test
    fun `cancellation propagates — it is not five broken scans`() = runTest {
        var ran = 0
        var thrown: Throwable? = null
        try {
            BatchExport.run(BatchAction.EXPORT, listOf("a", "b", "c")) { step ->
                ran++
                if (step.projectId == "b") throw CancellationException("left the tab")
                Result.success(Unit)
            }
        } catch (e: CancellationException) {
            thrown = e
        }
        assertTrue("cancellation must not be recorded as a failure", thrown is CancellationException)
        assertEquals(2, ran)
    }

    @Test
    fun `every job produces exactly one outcome — there is no third result`() = runTest {
        val report = BatchExport.run(BatchAction.SHARE, listOf("a", "b", "c", "d")) { step ->
            if (step.index % 2 == 0) Result.success(Unit) else Result.failure(RuntimeException("x"))
        }
        assertEquals(4, report.outcomes.size)
        assertEquals(4, report.succeeded.size + report.failed.size)
    }

    // ── the sentence the operator reads ─────────────────────────────────────

    @Test
    fun `a clean run says how many, in three words`() = runTest {
        val export = BatchExport.run(BatchAction.EXPORT, listOf("a", "b")) { Result.success(Unit) }
        assertEquals("Exported 2 scans.", export.summary())
        val share = BatchExport.run(BatchAction.SHARE, listOf("a")) { Result.success(Unit) }
        assertEquals("Shared 1 scan.", share.summary())
        val delete = BatchExport.run(BatchAction.DELETE, listOf("a", "b", "c")) { Result.success(Unit) }
        assertEquals("Deleted 3 scans.", delete.summary())
    }

    @Test
    fun `a partial run names both halves AND what to tap`() = runTest {
        val report = BatchExport.run(BatchAction.EXPORT, listOf("a", "b", "c")) { step ->
            if (step.projectId == "b") Result.failure(RuntimeException("nope")) else Result.success(Unit)
        }
        assertEquals("Exported 2 of 3. Tap Export to retry 1.", report.summary())
        assertTrue(WordingLaw.isDetail(report.summary()))
        assertTrue("a report of a failure must say what to DO", WordingLaw.isActionable(report.summary()))
    }

    @Test
    fun `a run where everything failed still says what to tap`() = runTest {
        val report = BatchExport.run(BatchAction.SHARE, listOf("a", "b")) {
            Result.failure(RuntimeException("nope"))
        }
        assertEquals("Nothing was shared. Tap Share to retry.", report.summary())
        assertTrue(WordingLaw.isDetail(report.summary()))
        assertTrue(WordingLaw.isActionable(report.summary()))
    }

    @Test
    fun `an empty run says so rather than claiming zero successes`() {
        val report = BatchReport(BatchAction.EXPORT, emptyList())
        assertEquals("Nothing selected.", report.summary())
        assertFalse(report.allSucceeded)
    }

    @Test
    fun `SHARE hands the sheet only the files that were actually produced`() = runTest {
        val order = listOf("a", "b", "c")
        val report = BatchExport.run(BatchAction.SHARE, order) { step ->
            if (step.projectId == "b") Result.failure(RuntimeException("nope")) else Result.success(Unit)
        }
        assertEquals(listOf("a", "c"), report.succeededIn(order))
    }

    @Test
    fun `no summary carries design-document jargon`() = runTest {
        for (action in BatchAction.entries) {
            val mixed = BatchExport.run(action, listOf("a", "b")) { step ->
                if (step.index == 0) Result.success(Unit) else Result.failure(RuntimeException("x"))
            }
            assertTrue(mixed.summary(), WordingLaw.jargonIn(mixed.summary()).isEmpty())
            assertTrue(mixed.summary(), WordingLaw.isDetail(mixed.summary()))
        }
    }
}
