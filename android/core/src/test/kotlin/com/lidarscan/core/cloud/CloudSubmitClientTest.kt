package com.lidarscan.core.cloud

import java.io.File
import kotlin.random.Random
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * D3. Plain-JVM tests for the cloud submit client, driven entirely by
 * [FakeCloudServer] behind the [HttpTransport] seam — no network, no service,
 * always runnable.
 *
 * They deliberately mirror the case list `engine/docs/A15-jobs.md` §6 gives for
 * `tests/test_jobs.cpp` (happy path, token reject, size cap before any
 * request, mid-upload disconnect + resume, poll to completion, result
 * download), because the value of this client is that it is
 * indistinguishable from the shipped C++ one to a server. The end-to-end test
 * against a *real* `cloud/service` lives in `CloudSubmitE2eTest`.
 *
 * All the waits are virtual: the client's backoff uses `delay`, and the
 * transport call is dispatched onto the test scheduler, so a case that
 * exhausts five retries costs no wall-clock time.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class CloudSubmitClientTest {

    @Rule
    @JvmField
    val tmp = TemporaryFolder()

    private val token = "test-token"

    private fun config(
        chunkBytes: Int = 4096,
        maxRetries: Int = 2,
        maxUploadBytes: Long = 2L * 1024 * 1024 * 1024,
    ) = CloudSubmitConfig(
        baseUrl = "http://fake",
        token = token,
        chunkBytes = chunkBytes,
        maxUploadBytes = maxUploadBytes,
        maxRetries = maxRetries,
        backoffInitialMs = 500,
        backoffMaxMs = 30_000,
        pollIntervalMs = 1_000,
    )

    /** 10 KiB of deterministic noise: three 4 KiB chunks, the last one short. */
    private fun sampleZip(name: String = "scan.lscan.zip", size: Int = 10 * 1024): File {
        val f = tmp.newFile(name)
        f.writeBytes(Random(42).nextBytes(size))
        return f
    }

    // ------------------------------------------------------------ happy path

    @Test
    fun `submit uploads every byte and reports progress up to 1_0`() = runTest {
        val server = FakeCloudServer(token)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val file = sampleZip()
        val progress = mutableListOf<Float>()

        val jobId = client.submit(file, progress = { progress += it }).getOrThrow()

        assertEquals("job00000001", jobId)
        assertArrayEquals(file.readBytes(), server.uploadedBytes(jobId))
        assertEquals(1f, progress.last(), 0f)
        assertTrue("progress must be monotonic: $progress", progress.zipWithNext().all { it.first <= it.second })
        // POST /jobs + 3 chunk PUTs (4096 + 4096 + 2048).
        assertEquals(4, server.served.size)
        assertEquals(
            listOf("bytes 0-4095/10240", "bytes 4096-8191/10240", "bytes 8192-10239/10240"),
            server.served.mapNotNull { it.header("Content-Range") },
        )
    }

    @Test
    fun `poll parses the contract fields and ignores the service's extra keys`() = runTest {
        val server = FakeCloudServer(token)
        server.pollScript = mutableListOf(Triple("processing", 0.42f, "optimizing"))
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val jobId = client.submit(sampleZip()).getOrThrow()

        val status = client.poll(jobId).getOrThrow()

        assertEquals(jobId, status.id)
        assertEquals(CloudJobState.PROCESSING, status.state)
        assertEquals(0.42f, status.progress, 1e-6f)
        assertEquals("optimizing", status.message)
    }

    @Test
    fun `awaitCompletion walks queued to done and downloads the result byte-exact`() = runTest {
        val server = FakeCloudServer(token)
        val expected = Random(7).nextBytes(64 * 1024)
        server.resultBytes = expected
        server.pollScript = mutableListOf(
            Triple("queued", 0f, "queued for the worker"),
            Triple("processing", 0.5f, "optimizing"),
            Triple("done", 1f, "complete"),
        )
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val jobId = client.submit(sampleZip()).getOrThrow()
        val seen = mutableListOf<CloudJobState>()

        val final = client.awaitCompletion(jobId, pollIntervalMs = 10, onStatus = { seen += it.state }).getOrThrow()

        assertEquals(CloudJobState.DONE, final.state)
        assertEquals(
            listOf(CloudJobState.QUEUED, CloudJobState.PROCESSING, CloudJobState.DONE),
            seen,
        )
        val dest = File(tmp.root, "result.zip")
        val n = client.downloadResult(jobId, dest).getOrThrow()
        assertEquals(expected.size.toLong(), n)
        assertArrayEquals(expected, dest.readBytes())
    }

    // ---------------------------------------------------------- token reject

    @Test
    fun `a rejected token fails with PermissionDenied and is never retried`() = runTest {
        val server = FakeCloudServer(token)
        val cfg = config().copy(token = "wrong-token")
        val client = CloudSubmitClient(server, cfg, UnconfinedTestDispatcher(testScheduler))

        val error = client.submit(sampleZip()).exceptionOrNull()

        assertTrue("expected PermissionDenied, got $error", error is CloudError.PermissionDenied)
        // The rule that matters: a 401 is a decision the server made, so
        // exactly ONE request went out even though maxRetries is 2.
        assertEquals(1, server.attempted.size)
    }

    @Test
    fun `a 413 from the server is not retried either`() = runTest {
        // Server cap below the file size: POST /jobs answers 413.
        val server = FakeCloudServer(token, maxUploadBytes = 1024)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))

        val error = client.submit(sampleZip()).exceptionOrNull()

        assertTrue("expected TooLarge, got $error", error is CloudError.TooLarge)
        assertEquals(1, server.attempted.size)
    }

    // -------------------------------------------------------------- size cap

    @Test
    fun `the size cap rejects before a single request reaches the transport`() = runTest {
        val server = FakeCloudServer(token)
        val client = CloudSubmitClient(server, config(maxUploadBytes = 4096), UnconfinedTestDispatcher(testScheduler))

        val error = client.submit(sampleZip()).exceptionOrNull()

        assertTrue("expected TooLarge, got $error", error is CloudError.TooLarge)
        // A15 §5, "Size cap": zero requests. On a phone this is the difference
        // between wasting 2 GiB of mobile data and wasting none.
        assertEquals(0, server.attempted.size)
    }

    // ------------------------------------------- disconnect mid-upload, resume

    @Test
    fun `a lost ack exhausts retries, issues ONE resume probe and finishes byte-exact`() = runTest {
        val server = FakeCloudServer(token)
        // The middle chunk's bytes land every time; its ack never comes back.
        // maxRetries = 2 means 3 attempts, all of them "successful" server-side.
        server.dropAcksFor("bytes 4096-8191/", times = 3)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val file = sampleZip()
        val progress = mutableListOf<Float>()

        val jobId = client.submit(file, progress = { progress += it }).getOrThrow()

        assertArrayEquals(file.readBytes(), server.uploadedBytes(jobId))
        assertEquals(1f, progress.last(), 0f)

        val ranges = server.attempted.mapNotNull { it.header("Content-Range") }
        assertEquals(
            listOf(
                "bytes 0-4095/10240",
                "bytes 4096-8191/10240", // attempt 1
                "bytes 4096-8191/10240", // retry 1
                "bytes 4096-8191/10240", // retry 2 — retries now exhausted
                "bytes */10240", // the ONE resume probe
                "bytes 8192-10239/10240", // resumed from the server's offset, not the client's
            ),
            ranges,
        )
        // Exactly one probe, never a second.
        assertEquals(1, ranges.count { it == "bytes */10240" })
        // The client jumped to 8192 because the SERVER said so — it had no way
        // to know its own bytes had landed.
        assertTrue("progress should show the resume jump: $progress", progress.contains(8192f / 10240f))
    }

    @Test
    fun `a dead connection resumes from the offset the server actually has`() = runTest {
        val server = FakeCloudServer(token)
        // These sends never leave the client, so the server's offset stays at
        // 4096 and the probe must send the client BACK to re-upload chunk 2.
        server.failSendsFor("bytes 4096-8191/", times = 3)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val file = sampleZip()

        val jobId = client.submit(file).getOrThrow()

        assertArrayEquals(file.readBytes(), server.uploadedBytes(jobId))
        val ranges = server.attempted.mapNotNull { it.header("Content-Range") }
        assertEquals(1, ranges.count { it == "bytes */10240" })
        // Probe reported 4096, so chunk 2 was sent a fourth time — and this
        // time it landed.
        assertEquals(4, ranges.count { it == "bytes 4096-8191/10240" })
    }

    @Test
    fun `a 5xx is retried, unlike a 4xx, and the upload then completes`() = runTest {
        val server = FakeCloudServer(token)
        server.serverErrorFor("bytes 4096-8191/", times = 2)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val file = sampleZip()

        val jobId = client.submit(file).getOrThrow()

        assertArrayEquals(file.readBytes(), server.uploadedBytes(jobId))
        val ranges = server.attempted.mapNotNull { it.header("Content-Range") }
        assertEquals(3, ranges.count { it == "bytes 4096-8191/10240" })
        // A 5xx is retried in place; it never triggers the resume probe.
        assertEquals(0, ranges.count { it == "bytes */10240" })
    }

    // --------------------------------------------------------- total outage

    @Test
    fun `a total outage fails with Network after retries`() = runTest {
        val server = FakeCloudServer(token)
        server.totalOutage = true
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))

        val error = client.submit(sampleZip()).exceptionOrNull()

        assertTrue("expected Network, got $error", error is CloudError.Network)
        // POST /jobs, tried maxRetries + 1 times, and nothing after it.
        assertEquals(3, server.attempted.size)
    }

    @Test
    fun `an outage that starts mid-upload fails once the resume probe cannot answer either`() = runTest {
        val server = FakeCloudServer(token)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val file = sampleZip()
        var chunksSeen = 0

        val error = client.submit(
            file,
            progress = {
                // Pull the plug after the first chunk lands.
                if (++chunksSeen == 1) server.totalOutage = true
            },
        ).exceptionOrNull()

        assertTrue("expected Network, got $error", error is CloudError.Network)
        assertTrue(
            "the failure must name the probe: ${error?.message}",
            error!!.message!!.contains("resume probe"),
        )
    }

    // --------------------------------------------------------- cancellation

    @Test
    fun `cancelling mid-upload stops between chunks and reports Cancelled`() = runTest {
        val server = FakeCloudServer(token)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        var uploaded = 0

        val error = client.submit(
            sampleZip(),
            progress = { uploaded++ },
            cancelled = { uploaded >= 1 },
        ).exceptionOrNull()

        assertTrue("expected Cancelled, got $error", error is CloudError.Cancelled)
        // POST + exactly one chunk: the flag is checked at the top of the
        // chunk loop, so the second chunk never goes out.
        assertEquals(2, server.attempted.size)
    }

    @Test
    fun `cancelling while polling stops promptly`() = runTest {
        val server = FakeCloudServer(token)
        server.pollScript = mutableListOf(Triple("processing", 0.1f, "optimizing"))
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val jobId = client.submit(sampleZip()).getOrThrow()
        var polls = 0

        val error = client.awaitCompletion(
            jobId,
            pollIntervalMs = 30_000,
            onStatus = { polls++ },
            cancelled = { polls >= 2 },
        ).exceptionOrNull()

        assertTrue("expected Cancelled, got $error", error is CloudError.Cancelled)
        assertEquals(2, polls)
    }

    @Test
    fun `cancel issues DELETE and the job settles into failed with the word in the message`() = runTest {
        val server = FakeCloudServer(token)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val jobId = client.submit(sampleZip()).getOrThrow()

        val status = client.cancel(jobId).getOrThrow()

        // There is no `cancelled` on the wire (service README) — it is `failed`
        // plus the word in `message`, exactly like the engine's own job model.
        assertEquals(CloudJobState.FAILED, status.state)
        assertTrue(status.message.contains("cancelled"))
        assertEquals(HttpMethod.DELETE, server.served.last().method)
    }

    // ------------------------------------------------ server-reported failure

    @Test
    fun `a job that fails on the server surfaces as JobFailed carrying its message`() = runTest {
        val server = FakeCloudServer(token)
        server.pollScript = mutableListOf(
            Triple("processing", 0.3f, "optimizing"),
            Triple("failed", 0.3f, "worker exited 1: registration diverged"),
        )
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val jobId = client.submit(sampleZip()).getOrThrow()

        val error = client.awaitCompletion(jobId, pollIntervalMs = 10).exceptionOrNull()

        assertTrue("expected JobFailed, got $error", error is CloudError.JobFailed)
        val failed = error as CloudError.JobFailed
        assertEquals(CloudJobState.FAILED, failed.status.state)
        assertEquals("worker exited 1: registration diverged", failed.status.message)
    }

    @Test
    fun `downloading a result that is not ready is NotFound, not a crash`() = runTest {
        val server = FakeCloudServer(token)
        server.pollScript = mutableListOf(Triple("processing", 0.3f, "optimizing"))
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))
        val jobId = client.submit(sampleZip()).getOrThrow()

        val error = client.downloadResult(jobId, File(tmp.root, "nope.zip")).exceptionOrNull()

        assertTrue("expected NotFound, got $error", error is CloudError.NotFound)
    }

    @Test
    fun `polling an unknown job is NotFound`() = runTest {
        val server = FakeCloudServer(token)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))

        val error = client.poll("job99999999").exceptionOrNull()

        assertTrue("expected NotFound, got $error", error is CloudError.NotFound)
    }

    // ------------------------------------------------------------- local IO

    @Test
    fun `a missing local zip fails before any request`() = runTest {
        val server = FakeCloudServer(token)
        val client = CloudSubmitClient(server, config(), UnconfinedTestDispatcher(testScheduler))

        val error = client.submit(File(tmp.root, "does-not-exist.zip")).exceptionOrNull()

        assertTrue("expected Io, got $error", error is CloudError.Io)
        assertEquals(0, server.attempted.size)
    }
}

/** The header/response value semantics the whole retry rule is built on. */
class HttpMessageTest {

    @Test
    fun `header lookup is case-insensitive in both directions`() {
        val resp = HttpResponse.ok(308, mapOf("upload-offset" to "4096"))
        assertEquals("4096", resp.header("Upload-Offset"))
        assertEquals("4096", resp.header("UPLOAD-OFFSET"))
        assertEquals(null, resp.header("Content-Range"))

        val req = HttpRequest(HttpMethod.PUT, "http://x", mapOf("Content-Range" to "bytes */10"))
        assertEquals("bytes */10", req.header("content-range"))
    }

    @Test
    fun `a transport failure carries no status at all`() {
        val f = HttpResponse.transportFailure()
        // The point of the flag: "no answer" is not status 0 and not a 5xx —
        // it is the absence of a status, which is what the client retries and
        // then resumes from.
        assertEquals(false, f.transportOk)
        assertEquals(0, f.status)
        assertEquals(0, f.body.size)
    }

    @Test
    fun `bodies compare by content so a fake can match on a request`() {
        val a = HttpRequest(HttpMethod.PUT, "http://x", body = byteArrayOf(1, 2, 3))
        val b = HttpRequest(HttpMethod.PUT, "http://x", body = byteArrayOf(1, 2, 3))
        assertEquals(a, b)
        assertEquals(a.hashCode(), b.hashCode())
    }

    @Test
    fun `state parsing accepts the five wire strings and nothing else`() {
        assertEquals(CloudJobState.QUEUED, CloudJobState.fromWire("queued"))
        assertEquals(CloudJobState.UPLOADING, CloudJobState.fromWire("uploading"))
        assertEquals(CloudJobState.PROCESSING, CloudJobState.fromWire("processing"))
        assertEquals(CloudJobState.DONE, CloudJobState.fromWire("done"))
        assertEquals(CloudJobState.FAILED, CloudJobState.fromWire("failed"))
        // There is no `cancelled` on the wire; a server that invented one is
        // UNKNOWN rather than silently mapped onto something plausible.
        assertEquals(CloudJobState.UNKNOWN, CloudJobState.fromWire("cancelled"))
        assertEquals(CloudJobState.UNKNOWN, CloudJobState.fromWire(null))
        assertTrue(CloudJobState.DONE.isTerminal && CloudJobState.FAILED.isTerminal)
        assertTrue(!CloudJobState.PROCESSING.isTerminal)
    }

    @Test
    fun `baseUrl loses its trailing slash so paths concatenate cleanly`() {
        val cfg = CloudSubmitConfig(baseUrl = "http://host:8080/", token = "t")
        assertEquals("http://host:8080", cfg.normalizedBaseUrl)
        assertEquals(2L * 1024 * 1024 * 1024, cfg.maxUploadBytes)
    }
}
