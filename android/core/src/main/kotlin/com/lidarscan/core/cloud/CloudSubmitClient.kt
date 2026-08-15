package com.lidarscan.core.cloud

import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/**
 * The app half of Tech Spec §3.8's **Cloud** processing mode:
 *
 * ```
 *  .lscan.zip --(resumable chunked upload)--> job service --> Linux worker
 *             <--(poll state/progress, download result)------
 * ```
 *
 * This is a **line-for-line port of `engine/src/jobs/cloud_submit.cpp`**, the
 * client that already ships on desktop, against the REST contract in
 * `engine/docs/A15-jobs.md` §5 and implemented by `cloud/service`. Porting
 * rather than re-designing is the whole point: the service's own test suite
 * (`cloud/service/tests/client_sim.py`) is a *third* port of the same
 * algorithm precisely so that any drift between the three fails a build. The
 * behaviours that must not drift are:
 *
 *  * **request order** — `POST /jobs`, then one `PUT` per chunk with
 *    `Content-Range: bytes <start>-<end>/<total>`, then `GET /jobs/{id}`,
 *    then `GET /jobs/{id}/result`;
 *  * **the retry rule** — only a transport failure (no response at all) or a
 *    `5xx` is retried, with exponential backoff. A real status the server
 *    *did* return (401/413/400/416) is never retried; that decision belongs to
 *    [submit]/[poll]/[downloadResult] directly;
 *  * **exactly ONE resume probe** — when a chunk's retries are exhausted, the
 *    client sends `Content-Range: bytes *&#47;<total>` with an empty body, reads
 *    the `Upload-Offset` off the `308`, and continues from the *server's*
 *    authoritative offset. This is what makes "the ack was lost but the bytes
 *    landed" resumable instead of a duplicate-or-fail coin flip;
 *  * **the size cap is checked before any request** — the local file's length
 *    is compared against [CloudSubmitConfig.maxUploadBytes] before `POST /jobs`
 *    is sent, so an over-cap file costs zero bytes of a phone's mobile data.
 *
 * The client owns no socket: everything goes through [HttpTransport], which is
 * why the disconnect/resume path can be tested as a pure function. Use
 * [UrlConnectionHttpTransport] in production.
 *
 * **Threading.** Every public method is a `suspend fun`. The blocking
 * transport call is moved to [ioDispatcher]; the backoff and poll waits use
 * `delay`, so they are both structurally cancellable (cancel the coroutine)
 * and cooperatively cancellable (the `cancelled` lambda, checked between
 * chunks, between polls, and every 20 ms of a wait — the same granularity as
 * the engine's `post::CancelToken`).
 *
 * @param ioDispatcher where the blocking [HttpTransport.request] runs. Tests
 *   that drive a fake transport override it with the test dispatcher so
 *   `runTest`'s virtual clock skips the backoff waits entirely.
 */
class CloudSubmitClient(
    private val transport: HttpTransport,
    private val config: CloudSubmitConfig,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
) {

    // The service returns strictly more than A15 §5 specifies —
    // `internal_state`, `received_bytes`, `size_bytes`, `exit_code`,
    // `created_at`, `updated_at` — and the README says in as many words that
    // the engine client ignores them. `ignoreUnknownKeys` is what makes that
    // true here, and what keeps this client working when the service grows a
    // field. The engine's C++ gets the same property by hand-rolling a
    // "find this key" reader instead of a real parser.
    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
        isLenient = true
    }

    private val base: String get() = config.normalizedBaseUrl

    private fun authHeaders(): Map<String, String> = mapOf("Authorization" to "Bearer ${config.token}")

    // ------------------------------------------------------------- submit

    /**
     * Creates a cloud job and uploads [zipFile] in `Content-Range` chunks.
     *
     * @param zipFile the local `.lscan.zip`. Its length is the `size_bytes`
     *   declared at `POST /jobs` and the `<total>` in every `Content-Range`.
     * @param progress called with 0..1 after each accepted chunk and after a
     *   resume repositions the offset; always ends at exactly `1.0` on success.
     * @param cancelled polled before each chunk and every 20 ms of a backoff
     *   wait. Returning `true` fails the call with [CloudError.Cancelled];
     *   note that this abandons the upload locally — call [cancel] as well if
     *   the server should stop working on an already-uploaded job.
     * @return the cloud job id, or a [CloudError].
     */
    suspend fun submit(
        zipFile: File,
        progress: ((Float) -> Unit)? = null,
        cancelled: (() -> Boolean)? = null,
    ): Result<String> = runCatchingCloud { submitOrThrow(zipFile, progress, cancelled) }

    private suspend fun submitOrThrow(
        zipFile: File,
        progress: ((Float) -> Unit)?,
        cancelled: (() -> Boolean)?,
    ): String {
        if (!zipFile.isFile) {
            throw CloudError.Io("cloud/submit: cannot stat '${zipFile.path}'")
        }
        val total = zipFile.length()

        // Tech Spec §3.8's cap, BEFORE the first request. `submitRejectsOverCap`
        // asserts zero requests reach the transport when this trips.
        if (total > config.maxUploadBytes) {
            throw CloudError.TooLarge(
                "cloud/submit: $total bytes exceeds the ${config.maxUploadBytes}-byte cap (Tech Spec §3.8)"
            )
        }
        if (total == 0L) {
            // The service refuses size_bytes <= 0 with a 400; saying so here
            // is a better diagnosis than relaying "400 bad request".
            throw CloudError.Io("cloud/submit: '${zipFile.path}' is empty")
        }

        // --- POST /jobs ------------------------------------------------------
        val createBody = json.encodeToString(
            CreateJobRequest.serializer(),
            CreateJobRequest(kind = "lscan", sizeBytes = total),
        ).encodeToByteArray()

        val createResp = sendWithRetry(
            HttpRequest(
                method = HttpMethod.POST,
                url = "$base/jobs",
                headers = authHeaders() + ("Content-Type" to "application/json"),
                body = createBody,
            ),
            cancelled,
        )
        if (!createResp.transportOk) {
            throw CloudError.Network("cloud/submit: POST /jobs: no response (disconnected)")
        }
        when (createResp.status) {
            401 -> throw CloudError.PermissionDenied("cloud/submit: POST /jobs: token rejected")
            413 -> throw CloudError.TooLarge("cloud/submit: POST /jobs: server rejected upload size")
            201 -> Unit
            else -> throw CloudError.Protocol(
                "cloud/submit: POST /jobs: unexpected status ${createResp.status}"
            )
        }
        val created = try {
            json.decodeFromString(CreateJobResponse.serializer(), createResp.body.decodeToString())
        } catch (e: Exception) {
            throw CloudError.Protocol("cloud/submit: POST /jobs: malformed response body: ${e.message}")
        }
        val jobId = created.id
        // A15 §5 says `upload_url` is a path (`/jobs/<id>/upload`) and the C++
        // client concatenates it onto base_url. `LIDARSCAN_URL_PREFIX` only
        // changes that path's prefix, so concatenation stays right — but a
        // future service behind a different host could answer with an absolute
        // URL, and honouring one costs nothing.
        val uploadUrl = if (created.uploadUrl.startsWith("http://") || created.uploadUrl.startsWith("https://")) {
            created.uploadUrl
        } else {
            base + created.uploadUrl
        }

        // --- chunked upload --------------------------------------------------
        var offset = 0L
        RandomAccessFile(zipFile, "r").use { raf ->
            while (offset < total) {
                if (cancelled?.invoke() == true) {
                    throw CloudError.Cancelled("cloud/submit: cancelled mid-upload")
                }
                val n = minOf(config.chunkBytes.toLong(), total - offset).toInt()
                val chunk = ByteArray(n)
                try {
                    raf.seek(offset)
                    raf.readFully(chunk)
                } catch (e: IOException) {
                    throw CloudError.Io("cloud/submit: short read staging chunk at offset $offset", e)
                }

                val resp = sendWithRetry(
                    HttpRequest(
                        method = HttpMethod.PUT,
                        url = uploadUrl,
                        headers = authHeaders() +
                            ("Content-Range" to "bytes $offset-${offset + n - 1}/$total"),
                        body = chunk,
                    ),
                    cancelled,
                )

                if (!resp.transportOk) {
                    // Retries exhausted on this chunk. ONE resume probe, then
                    // continue from whatever the server says it actually has.
                    offset = resumeOffset(uploadUrl, total, cancelled)
                    progress?.invoke(offset.toFloat() / total.toFloat())
                    continue
                }

                when (resp.status) {
                    401 -> throw CloudError.PermissionDenied("cloud/submit: PUT upload: token rejected")
                    413 -> throw CloudError.TooLarge("cloud/submit: PUT upload: server rejected upload size")
                    404 -> throw CloudError.NotFound("cloud/submit: PUT upload: job $jobId is gone")
                    200, 201 -> Unit
                    else -> throw CloudError.Protocol(
                        "cloud/submit: PUT upload: unexpected status ${resp.status}"
                    )
                }

                offset += n
                progress?.invoke(offset.toFloat() / total.toFloat())
            }
        }

        progress?.invoke(1f)
        return jobId
    }

    /**
     * The single resume probe: `Content-Range: bytes *&#47;<total>`, empty body,
     * answered with `308` + `Upload-Offset`. Returns the server's
     * authoritative received-byte count.
     */
    private suspend fun resumeOffset(
        uploadUrl: String,
        total: Long,
        cancelled: (() -> Boolean)?,
    ): Long {
        val probe = sendWithRetry(
            HttpRequest(
                method = HttpMethod.PUT,
                url = uploadUrl,
                headers = authHeaders() + ("Content-Range" to "bytes */$total"),
            ),
            cancelled,
        )
        if (!probe.transportOk) {
            throw CloudError.Network("cloud/submit: upload disconnected and the resume probe also failed")
        }
        if (probe.status == 401) {
            throw CloudError.PermissionDenied("cloud/submit: resume probe: token rejected")
        }
        val offsetHeader = probe.header("Upload-Offset")
        if ((probe.status != 308 && probe.status != 200) || offsetHeader == null) {
            throw CloudError.Protocol(
                "cloud/submit: resume probe returned no Upload-Offset (status ${probe.status})"
            )
        }
        val offset = offsetHeader.trim().toLongOrNull()
            ?: throw CloudError.Protocol("cloud/submit: resume probe Upload-Offset '$offsetHeader' is not a number")
        if (offset > total) {
            throw CloudError.Protocol("cloud/submit: resume probe offset $offset exceeds file size $total")
        }
        return offset
    }

    // --------------------------------------------------------------- poll

    /** `GET /jobs/{id}` — one snapshot of the job's state/progress/message. */
    suspend fun poll(jobId: String): Result<CloudJobStatus> = runCatchingCloud { pollOrThrow(jobId) }

    private suspend fun pollOrThrow(jobId: String): CloudJobStatus {
        val resp = sendWithRetry(
            HttpRequest(HttpMethod.GET, "$base/jobs/$jobId", authHeaders()),
            cancelled = null,
        )
        if (!resp.transportOk) {
            throw CloudError.Network("cloud/poll: GET /jobs/$jobId: no response")
        }
        when (resp.status) {
            401 -> throw CloudError.PermissionDenied("cloud/poll: GET /jobs/$jobId: token rejected")
            404 -> throw CloudError.NotFound("cloud/poll: GET /jobs/$jobId: not found")
            200 -> Unit
            else -> throw CloudError.Protocol("cloud/poll: GET /jobs/$jobId: unexpected status ${resp.status}")
        }
        val body = try {
            json.decodeFromString(JobStatusBody.serializer(), resp.body.decodeToString())
        } catch (e: Exception) {
            throw CloudError.Protocol("cloud/poll: GET /jobs/$jobId: malformed response body: ${e.message}")
        }
        return CloudJobStatus(
            id = body.id.ifEmpty { jobId },
            state = CloudJobState.fromWire(body.state),
            progress = body.progress.coerceIn(0f, 1f),
            message = body.message,
        )
    }

    /**
     * Polls until the job reaches `done` or `failed`.
     *
     * `done` is [Result.success]; `failed` is [Result.failure] with a
     * [CloudError.JobFailed] carrying the final status, because a caller that
     * forgets to check `state` after a "successful" wait would ship a silent
     * bug. A cancelled job arrives here as `failed` with "cancelled" in the
     * message — the service has no `cancelled` wire state (README).
     *
     * @param pollIntervalMs defaults to [CloudSubmitConfig.pollIntervalMs].
     * @param onStatus called with every snapshot, for a progress UI.
     * @param cancelled polled before each request and during each wait.
     */
    suspend fun awaitCompletion(
        jobId: String,
        pollIntervalMs: Long = config.pollIntervalMs,
        onStatus: ((CloudJobStatus) -> Unit)? = null,
        cancelled: (() -> Boolean)? = null,
    ): Result<CloudJobStatus> = runCatchingCloud {
        awaitOrThrow(jobId, pollIntervalMs, onStatus, cancelled)
    }

    private suspend fun awaitOrThrow(
        jobId: String,
        pollIntervalMs: Long,
        onStatus: ((CloudJobStatus) -> Unit)?,
        cancelled: (() -> Boolean)?,
    ): CloudJobStatus {
        var attempts = 0
        while (true) {
            if (cancelled?.invoke() == true) {
                throw CloudError.Cancelled("cloud/await: cancelled while polling")
            }
            val status = pollOrThrow(jobId)
            onStatus?.invoke(status)
            if (status.state == CloudJobState.DONE) return status
            if (status.state == CloudJobState.FAILED) throw CloudError.JobFailed(status)

            attempts++
            if (config.maxPollAttempts != 0 && attempts >= config.maxPollAttempts) {
                throw CloudError.Timeout("cloud/await: exceeded maxPollAttempts (${config.maxPollAttempts})")
            }
            awaitCancellable(pollIntervalMs, cancelled)
        }
    }

    // ----------------------------------------------------------- download

    /**
     * `GET /jobs/{id}/result` — writes the result bundle to [destFile],
     * byte-exact, and returns its length.
     *
     * A `404` is [CloudError.NotFound]; per A15 §5 that also means "not ready
     * yet", which is why callers should reach [awaitCompletion] first.
     */
    suspend fun downloadResult(jobId: String, destFile: File): Result<Long> = runCatchingCloud {
        val resp = sendWithRetry(
            HttpRequest(HttpMethod.GET, "$base/jobs/$jobId/result", authHeaders()),
            cancelled = null,
        )
        if (!resp.transportOk) {
            throw CloudError.Network("cloud/download: GET /jobs/$jobId/result: no response")
        }
        when (resp.status) {
            401 -> throw CloudError.PermissionDenied("cloud/download: token rejected")
            404 -> throw CloudError.NotFound("cloud/download: result not ready for job $jobId")
            200 -> Unit
            else -> throw CloudError.Protocol("cloud/download: unexpected status ${resp.status}")
        }
        try {
            withContext(ioDispatcher) {
                destFile.parentFile?.mkdirs()
                destFile.writeBytes(resp.body)
            }
        } catch (e: IOException) {
            throw CloudError.Io("cloud/download: cannot write '${destFile.path}'", e)
        }
        resp.body.size.toLong()
    }

    // ------------------------------------------------------------- cancel

    /**
     * `DELETE /jobs/{id}` — ask the server to stop.
     *
     * **This verb is the service's documented EXTENSION to A15 §5, not part of
     * the engine contract.** `cloud/service/README.md` says so explicitly ("`DELETE`
     * is an extension — A15 §5 defines no cancel verb, and one worker with a
     * hard job timeout needs one"), and `engine/src/jobs/cloud_submit.cpp` has
     * no equivalent. A server that implements only A15 §5 will answer `405`
     * (surfaced as [CloudError.Protocol]) and the caller must treat cancelling
     * as a best-effort nicety, not a guarantee. The local `cancelled` lambda
     * passed to [submit]/[awaitCompletion] is the part that always works.
     *
     * The returned status is the job's post-cancel snapshot; the service
     * settles a cancelled job into `failed` with "cancelled" in the message
     * rather than inventing a `cancelled` wire state. `409` (already finished)
     * is surfaced as [CloudError.Protocol].
     */
    suspend fun cancel(jobId: String): Result<CloudJobStatus> = runCatchingCloud {
        val resp = sendWithRetry(
            HttpRequest(HttpMethod.DELETE, "$base/jobs/$jobId", authHeaders()),
            cancelled = null,
        )
        if (!resp.transportOk) {
            throw CloudError.Network("cloud/cancel: DELETE /jobs/$jobId: no response")
        }
        when (resp.status) {
            401 -> throw CloudError.PermissionDenied("cloud/cancel: token rejected")
            404 -> throw CloudError.NotFound("cloud/cancel: no such job $jobId")
            200 -> Unit
            else -> throw CloudError.Protocol("cloud/cancel: unexpected status ${resp.status}")
        }
        val body = try {
            json.decodeFromString(JobStatusBody.serializer(), resp.body.decodeToString())
        } catch (e: Exception) {
            throw CloudError.Protocol("cloud/cancel: malformed response body: ${e.message}")
        }
        CloudJobStatus(
            id = body.id.ifEmpty { jobId },
            state = CloudJobState.fromWire(body.state),
            progress = body.progress.coerceIn(0f, 1f),
            message = body.message,
        )
    }

    // ------------------------------------------------------------ plumbing

    /**
     * Retries a **transport-level failure (no response at all) or a 5xx** up to
     * [CloudSubmitConfig.maxRetries] times with exponential backoff, and
     * nothing else. A real status the transport DID deliver (401, 413, 400,
     * 416) is returned to the caller untouched — retrying a decision the
     * server already made is the bug this rule exists to prevent.
     */
    private suspend fun sendWithRetry(req: HttpRequest, cancelled: (() -> Boolean)?): HttpResponse {
        var backoff = config.backoffInitialMs
        var resp = HttpResponse.transportFailure()
        for (attempt in 0..config.maxRetries) {
            if (cancelled?.invoke() == true) throw CloudError.Cancelled("cloud: cancelled")
            resp = withContext(ioDispatcher) { transport.request(req) }
            val retryable = !resp.transportOk || resp.status >= 500
            if (!retryable || attempt == config.maxRetries) return resp
            awaitCancellable(backoff, cancelled)
            backoff = minOf(config.backoffMaxMs, (backoff * config.backoffMultiplier).toLong())
        }
        return resp
    }

    /**
     * Waits [totalMs], checking [cancelled] every 20 ms so a cancellation
     * during a 30-second backoff is prompt instead of blocking for the whole
     * interval — the granularity discipline the engine's own long-running
     * seams use.
     */
    private suspend fun awaitCancellable(totalMs: Long, cancelled: (() -> Boolean)?) {
        var waited = 0L
        while (waited < totalMs) {
            if (cancelled?.invoke() == true) throw CloudError.Cancelled("cloud: cancelled while waiting")
            val step = minOf(CANCEL_POLL_SLICE_MS, totalMs - waited)
            delay(step)
            waited += step
        }
        if (cancelled?.invoke() == true) throw CloudError.Cancelled("cloud: cancelled while waiting")
    }

    /**
     * Turns the internally-thrown [CloudError] into a [Result].
     * `CancellationException` is deliberately NOT caught: structured
     * concurrency must keep working, so a cancelled coroutine propagates
     * rather than being flattened into a failed `Result`.
     */
    private suspend inline fun <T> runCatchingCloud(block: () -> T): Result<T> = try {
        Result.success(block())
    } catch (e: CloudError) {
        Result.failure(e)
    }

    private companion object {
        const val CANCEL_POLL_SLICE_MS = 20L
    }
}

// --- wire bodies -------------------------------------------------------------
// Snake_case on the wire (A15 §5), camelCase in Kotlin. Every field the
// service adds beyond these is dropped by `ignoreUnknownKeys`.

@Serializable
private data class CreateJobRequest(
    val kind: String,
    @SerialName("size_bytes") val sizeBytes: Long,
)

@Serializable
private data class CreateJobResponse(
    val id: String,
    @SerialName("upload_url") val uploadUrl: String,
)

@Serializable
private data class JobStatusBody(
    val id: String = "",
    val state: String = "",
    val progress: Float = 0f,
    val message: String = "",
)
