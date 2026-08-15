package com.lidarscan.core.jobs

import com.lidarscan.core.model.ExportFormat
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * B6 — the Processing screen's action policy. Every refusal has to name its own
 * cause, because the underlying engine errors (`kNotSupported` from the sync
 * gate, `kNotFound` from a session with no camera) mean nothing to an operator.
 */
class ProcessingPolicyTest {

    @Test
    fun `job kind and state codes mirror the C++ enums`() {
        assertEquals(listOf(0, 1, 2, 3, 4), JobKind.entries.map { it.code })
        assertEquals(listOf(0, 1, 2, 3, 4), JobState.entries.map { it.code })
        assertEquals(JobKind.POST_PROCESS, JobKind.fromCode(0))
        assertEquals(JobKind.CLOUD_SUBMIT, JobKind.fromCode(4))
        // Unknown codes degrade rather than throw.
        assertEquals(JobKind.POST_PROCESS, JobKind.fromCode(99))
        assertEquals(JobState.QUEUED, JobState.fromCode(-1))
    }

    @Test
    fun `a cancelled job is failed with the cancelled error, not a sixth state`() {
        // A15 §2 and SCAN_JOB_*: "FIVE states, not six... a UI does not need a
        // second code path to notice a cancellation, it reads the error."
        val cancelled = job(state = JobState.FAILED, error = 9)
        assertTrue(cancelled.wasCancelled)
        assertEquals("Cancelled", cancelled.statusText)

        val genuinelyFailed = job(state = JobState.FAILED, error = 20, message = "disk full")
        assertFalse(genuinelyFailed.wasCancelled)
        assertEquals("disk full", genuinelyFailed.statusText)
    }

    @Test
    fun `a failed job with no message still says something`() {
        assertTrue(job(state = JobState.FAILED, error = 42).statusText.contains("42"))
    }

    @Test
    fun `running jobs show their stage label`() {
        assertEquals("Running — loop closure", job(state = JobState.RUNNING, stage = "loop closure").statusText)
    }

    @Test
    fun `post-process needs something recorded`() {
        assertTrue(ProcessingPolicy.postProcess(hasRawStreams = true).enabled)
        val blocked = ProcessingPolicy.postProcess(hasRawStreams = false)
        assertFalse(blocked.enabled)
        assertNotNull(blocked.reason)
    }

    @Test
    fun `colorize refuses in a specific order, each with its own reason`() {
        // No cloud first — there is nothing to paint.
        val noCloud = ProcessingPolicy.colorize(true, SyncQuality.GOOD, false, hasProcessedCloud = false)
        assertFalse(noCloud.enabled)
        assertTrue(noCloud.reason!!.contains("Post-process first"))

        // Then no camera: §3.5's "gracefully unavailable", explicitly NOT a failure.
        val noCamera = ProcessingPolicy.colorize(false, SyncQuality.GOOD, false, hasProcessedCloud = true)
        assertFalse(noCamera.enabled)
        assertTrue(noCamera.reason!!.contains("gracefully unavailable"))

        // Then the sync gate, which fails CLOSED at UNKNOWN.
        val unsynced = ProcessingPolicy.colorize(true, SyncQuality.UNKNOWN, false, hasProcessedCloud = true)
        assertFalse(unsynced.enabled)
        assertTrue(unsynced.reason!!.contains("never converged"))
    }

    @Test
    fun `poor sync refuses until the operator overrides it`() {
        assertFalse(ProcessingPolicy.colorize(true, SyncQuality.POOR, false, true).enabled)
        assertTrue(ProcessingPolicy.colorize(true, SyncQuality.POOR, true, true).enabled)
        // GATED is A11's "colorize, with motion-gated keyframes" — allowed.
        assertTrue(ProcessingPolicy.colorize(true, SyncQuality.GATED, false, true).enabled)
        // UNKNOWN is NOT unlocked by the poor-sync override: it means "no
        // estimate at all", which is a different claim from "a bad estimate".
        assertFalse(ProcessingPolicy.colorize(true, SyncQuality.UNKNOWN, true, true).enabled)
    }

    @Test
    fun `sync quality codes mirror SCAN_SYNC and fail closed at zero`() {
        assertEquals(0, SyncQuality.UNKNOWN.code)
        assertEquals(SyncQuality.UNKNOWN, SyncQuality.fromCode(0))
        assertEquals(SyncQuality.UNKNOWN, SyncQuality.fromCode(77))
    }

    @Test
    fun `export needs a cloud from somewhere`() {
        assertTrue(ProcessingPolicy.export(hasProcessedCloud = true, hasLiveCloud = false).enabled)
        assertTrue(ProcessingPolicy.export(hasProcessedCloud = false, hasLiveCloud = true).enabled)
        assertFalse(ProcessingPolicy.export(hasProcessedCloud = false, hasLiveCloud = false).enabled)
    }

    @Test
    fun `cloud submit names the missing configuration rather than 401-ing later`() {
        val unconfigured = ProcessingPolicy.cloudSubmit(hasRawStreams = true, cloudConfigured = false)
        assertFalse(unconfigured.enabled)
        assertTrue(unconfigured.reason!!.contains("Settings"))
        assertTrue(ProcessingPolicy.cloudSubmit(true, true).enabled)
    }

    @Test
    fun `LAS without a georeference is a note, never a block`() {
        val note = ProcessingPolicy.exportFormatNote(ExportFormat.LAS14, georeferenced = false)
        assertNotNull(note)
        assertTrue(note!!.contains("placeholder"))
        assertNull(ProcessingPolicy.exportFormatNote(ExportFormat.LAS14, georeferenced = true))
        assertNotNull(ProcessingPolicy.exportFormatNote(ExportFormat.PCD, georeferenced = true))
        assertNull(ProcessingPolicy.exportFormatNote(ExportFormat.PLY_BINARY, georeferenced = false))
    }

    private fun job(
        state: JobState,
        error: Int = 0,
        stage: String = "",
        message: String = "",
    ) = ProcessingJob(1, JobKind.POST_PROCESS, state, 0.5f, stage, error, message)
}
