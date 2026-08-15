package com.lidarscan.core.cloud

import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import kotlin.random.Random
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * D3, end to end against a **real running `cloud/service`** — a real socket, a
 * real uvicorn, a real worker process, a real SQLite job row.
 *
 * The fake-transport tests ([CloudSubmitClientTest]) prove the *algorithm*;
 * they cannot prove that [UrlConnectionHttpTransport] puts the right bytes on
 * the wire, that `HttpURLConnection` surfaces a `308` instead of swallowing it
 * as a redirect, or that the service and this client agree on
 * `Upload-Offset`. Only this test does, so it is worth the setup.
 *
 * **It skips (does not fail) when the service is not configured**, so
 * `./gradlew :core:test` stays green for anyone without one:
 *
 * ```bash
 * # terminal 1 — the service, with a worker that writes something into {output}
 * cd cloud/service
 * LIDARSCAN_TOKEN=dev-token \
 * LIDARSCAN_DATA_DIR=/tmp/lidarscan-e2e \
 * LIDARSCAN_WORKER_CMD="/path/to/engine_cli --post {input} --out {output}" \
 *   .venv/bin/uvicorn lidarscan_service.asgi:app --host 127.0.0.1 --port 8080
 *
 * # terminal 2 — the test
 * cd android
 * LIDARSCAN_E2E_BASE_URL=http://127.0.0.1:8080 LIDARSCAN_E2E_TOKEN=dev-token \
 *   ./gradlew :core:test --tests '*CloudSubmitE2eTest*'
 * ```
 *
 * `LIDARSCAN_E2E_ZIP` optionally points at a real `.lscan.zip`; without it the
 * test builds a ~3 MB one (a `manifest.json` plus incompressible noise, the
 * layout `find_lscan_root` accepts) so the upload is genuinely multi-chunk.
 */
class CloudSubmitE2eTest {

    @Rule
    @JvmField
    val tmp = TemporaryFolder()

    private lateinit var baseUrl: String
    private lateinit var token: String

    @Before
    fun requireService() {
        val url = System.getenv("LIDARSCAN_E2E_BASE_URL")
        val tok = System.getenv("LIDARSCAN_E2E_TOKEN")
        if (url.isNullOrBlank() || tok.isNullOrBlank()) {
            println(
                """
                |[skip] CloudSubmitE2eTest needs a running cloud/service.
                |       Set LIDARSCAN_E2E_BASE_URL (e.g. http://127.0.0.1:8080) and
                |       LIDARSCAN_E2E_TOKEN to the service's LIDARSCAN_TOKEN, then re-run.
                |       See this class's KDoc for the exact uvicorn command.
                |       Skipping, NOT failing: the fake-transport suite
                |       (CloudSubmitClientTest) covers the algorithm without a network.
                """.trimMargin()
            )
        }
        assumeTrue(!url.isNullOrBlank() && !tok.isNullOrBlank())
        baseUrl = url!!
        token = tok!!
    }

    private fun config(chunkBytes: Int = 512 * 1024, maxRetries: Int = 2) = CloudSubmitConfig(
        baseUrl = baseUrl,
        token = token,
        chunkBytes = chunkBytes,
        maxRetries = maxRetries,
        // Short backoffs: these are real wall-clock waits, not virtual ones.
        backoffInitialMs = 50,
        backoffMaxMs = 500,
        pollIntervalMs = 250,
        // ~90 s ceiling so a wedged worker fails the test instead of hanging it.
        maxPollAttempts = 360,
    )

    /** A ~3 MB `.lscan.zip`: `MyScan.lscan/manifest.json` + incompressible noise. */
    private fun lscanZip(): File {
        System.getenv("LIDARSCAN_E2E_ZIP")?.takeIf { it.isNotBlank() }?.let { return File(it) }
        val f = tmp.newFile("e2e.lscan.zip")
        ZipOutputStream(f.outputStream().buffered()).use { zip ->
            zip.putNextEntry(ZipEntry("MyScan.lscan/manifest.json"))
            zip.write("""{"schema":"lscan/1","name":"MyScan","sensor":"mid360"}""".encodeToByteArray())
            zip.closeEntry()
            val rnd = Random(20260815)
            for (i in 0 until 3) {
                zip.putNextEntry(ZipEntry("MyScan.lscan/raw/chunk_$i.bin"))
                zip.write(rnd.nextBytes(1024 * 1024))
                zip.closeEntry()
            }
        }
        return f
    }

    // ------------------------------------------------------------- full round

    @Test
    fun `create, chunked upload, poll to done, download the result`() = runBlocking {
        val client = CloudSubmitClient(UrlConnectionHttpTransport(), config())
        val zip = lscanZip()
        val progress = mutableListOf<Float>()

        val jobId = client.submit(zip, progress = { progress += it }).getOrThrow()
        println("[e2e] POST /jobs -> job $jobId; uploaded ${zip.length()} bytes in ${progress.size} chunk acks")

        assertEquals(1f, progress.last(), 0f)
        assertTrue(progress.zipWithNext().all { it.first <= it.second })

        val states = mutableListOf<String>()
        val final = client.awaitCompletion(
            jobId,
            onStatus = { states += "${it.state}@${it.progress}" },
        ).getOrThrow()
        println("[e2e] states seen: ${states.joinToString(" -> ")}")
        assertEquals(CloudJobState.DONE, final.state)

        val dest = File(tmp.root, "result.zip")
        val n = client.downloadResult(jobId, dest).getOrThrow()
        println("[e2e] GET /jobs/$jobId/result -> $n bytes")
        assertEquals(dest.length(), n)
        assertTrue("result must not be empty", n > 0)

        // It is the worker's output, zipped by the service — so assert it is a
        // readable zip with content rather than guessing at entry names.
        val entries = ZipFile(dest).use { z -> z.entries().toList().map { it.name } }
        println("[e2e] result entries: $entries")
        assertTrue("result zip should have entries", entries.isNotEmpty())

        // Byte-exact: the same bytes come back a second time.
        val again = File(tmp.root, "result-again.zip")
        client.downloadResult(jobId, again).getOrThrow()
        assertArrayEquals(dest.readBytes(), again.readBytes())
    }

    // ------------------------------------------------------- the real resume

    /**
     * The disconnect/resume path against the live service.
     *
     * [LostAckTransport] lets the chunk's bytes reach the server and throws the
     * *response* away — the one failure a real server cannot be asked to
     * produce, and the reason the resume probe exists at all. So the client
     * really exhausts its retries, really sends `Content-Range: bytes *&#47;<total>`
     * to uvicorn, and the offset it resumes from is the service's own
     * `Upload-Offset`, computed off the `.part` file on disk.
     */
    @Test
    fun `a lost ack against the live service resumes from its Upload-Offset`() = runBlocking {
        val zip = lscanZip()
        val chunk = 512 * 1024
        // Drop every ack for the second chunk: 1 attempt + 2 retries.
        val transport = LostAckTransport(UrlConnectionHttpTransport(), "bytes $chunk-", times = 3)
        val client = CloudSubmitClient(transport, config(chunkBytes = chunk))

        val jobId = client.submit(zip).getOrThrow()

        val ranges = transport.ranges
        println("[e2e] Content-Ranges sent: $ranges")
        println("[e2e] resume probe answered Upload-Offset=${transport.probeOffset}")

        val probes = ranges.filter { it.startsWith("bytes */") }
        assertEquals("exactly ONE resume probe", 1, probes.size)
        assertEquals("bytes */${zip.length()}", probes.single())
        // The bytes DID land while the acks were dropped, so the service must
        // report the offset past the chunk the client thinks failed.
        assertEquals((2L * chunk), transport.probeOffset)
        // ... and the client must continue from there, not from its own idea.
        assertTrue(
            "must resume at the server's offset: $ranges",
            ranges.any { it.startsWith("bytes ${2 * chunk}-") },
        )

        val final = client.awaitCompletion(jobId).getOrThrow()
        assertEquals(CloudJobState.DONE, final.state)
        val dest = File(tmp.root, "resumed-result.zip")
        assertTrue(client.downloadResult(jobId, dest).getOrThrow() > 0)
        println("[e2e] resumed job $jobId completed; result ${dest.length()} bytes")
    }

    // ------------------------------------------------------- cancel (DELETE)

    /**
     * `DELETE /jobs/{id}` against the live service — the one public method
     * whose verb is the service's **extension** to A15 §5, so it is the one an
     * A15-only server would answer `405` to. Worth proving against the real
     * thing at least once.
     *
     * Deliberately tolerant of the race it cannot control: the fake/real worker
     * may finish between the last upload byte and the `DELETE`, and the service
     * answers `409` ("already finished; nothing to cancel") in that case. Both
     * outcomes are correct; asserting only one would make this test flaky
     * rather than more rigorous.
     */
    @Test
    fun `cancel issues a real DELETE and the service settles the job`() = runBlocking {
        val client = CloudSubmitClient(UrlConnectionHttpTransport(), config())
        val jobId = client.submit(lscanZip()).getOrThrow()

        val result = client.cancel(jobId)

        result.fold(
            onSuccess = { status ->
                println("[e2e] DELETE /jobs/$jobId -> ${status.state} '${status.message}'")
                // No `cancelled` on the wire: the service settles into failed
                // (or reports the cancelling job's current state).
                assertTrue(
                    "cancelled job should not report done: $status",
                    status.state != CloudJobState.DONE,
                )
            },
            onFailure = { e ->
                println("[e2e] DELETE /jobs/$jobId -> $e (the worker won the race; 409 is correct)")
                assertTrue("expected Protocol(409), got $e", e is CloudError.Protocol)
            },
        )
    }

    // --------------------------------------------------------------- 401

    @Test
    fun `a wrong token is a real 401 from the real service and is not retried`() = runBlocking {
        val counting = CountingTransport(UrlConnectionHttpTransport())
        val client = CloudSubmitClient(counting, config().copy(token = "$token-definitely-wrong"))

        val error = client.submit(lscanZip()).exceptionOrNull()

        println("[e2e] wrong token -> $error after ${counting.count} request(s)")
        assertTrue("expected PermissionDenied, got $error", error is CloudError.PermissionDenied)
        assertEquals("a 401 is a decision the server made; never retry it", 1, counting.count)
    }

    @Test
    fun `an unknown job id is a real 404`() = runBlocking {
        val client = CloudSubmitClient(UrlConnectionHttpTransport(), config())
        // 32 hex chars: the shape `is_job_id` accepts, so this reaches the store.
        val error = client.poll("0123456789abcdef0123456789abcdef").exceptionOrNull()
        println("[e2e] unknown job -> $error")
        assertTrue("expected NotFound, got $error", error is CloudError.NotFound)
    }
}

/** Lets the request through, then throws the response away — a lost ack. */
private class LostAckTransport(
    private val inner: HttpTransport,
    private val rangePrefix: String,
    private var times: Int,
) : HttpTransport {
    val ranges = mutableListOf<String>()
    var probeOffset: Long = -1

    override fun request(req: HttpRequest): HttpResponse {
        req.header("Content-Range")?.let { ranges += it }
        val resp = inner.request(req)
        if (req.header("Content-Range")?.startsWith("bytes */") == true) {
            probeOffset = resp.header("Upload-Offset")?.toLongOrNull() ?: -1
        }
        if (times > 0 && req.header("Content-Range")?.startsWith(rangePrefix) == true) {
            times--
            return HttpResponse.transportFailure()
        }
        return resp
    }
}

private class CountingTransport(private val inner: HttpTransport) : HttpTransport {
    var count = 0
    override fun request(req: HttpRequest): HttpResponse {
        count++
        return inner.request(req)
    }
}
