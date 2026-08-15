package com.lidarscan.core.cloud

/**
 * A scripted in-memory stand-in for `cloud/service`, behind the [HttpTransport]
 * seam — the Kotlin cousin of `tests/test_jobs.cpp`'s `FakeCloudServer` and of
 * `cloud/service/tests/client_sim.py`'s `SimTransport`.
 *
 * It implements enough of A15 §5 to be *honest about the two failure modes
 * that actually matter* and that a real server cannot be asked to produce on
 * demand:
 *
 *  * [failSendsFor] — the request never leaves the client (dead connection):
 *    the bytes do **not** land and no response arrives.
 *  * [dropAcksFor] — the bytes land and the response is thrown away (a lost
 *    ack). This is the case that makes the resume probe non-trivial: the
 *    server's authoritative offset is *ahead* of where the client thinks it
 *    is, and a client that blindly re-sent from its own offset would either
 *    duplicate bytes or hit a 416.
 *
 * The offset bookkeeping (idempotent re-send below the received offset, `416`
 * on a gap, `308` for the probe, `201` on completion) is copied from
 * `cloud/service/lidarscan_service/api.py` so the fake cannot be more
 * forgiving than the thing it stands in for.
 */
class FakeCloudServer(
    private val token: String = "test-token",
    private val maxUploadBytes: Long = 2L * 1024 * 1024 * 1024,
) : HttpTransport {

    /** Every request the client *attempted*, including ones that never left. */
    val attempted = mutableListOf<HttpRequest>()

    /** Only the requests that actually reached the server. */
    val served = mutableListOf<HttpRequest>()

    private class Job(val id: String, val total: Long) {
        val bytes = ByteArray(total.toInt())
        var received: Long = 0
    }

    private val jobs = LinkedHashMap<String, Job>()
    private var nextId = 1

    /** Scripted `GET /jobs/{id}` answers as (state, progress, message); the last one repeats. */
    var pollScript: MutableList<Triple<String, Float, String>> = mutableListOf()

    /** Served by `GET /jobs/{id}/result` once [pollScript] reports `done`. */
    var resultBytes: ByteArray? = null

    /** Every request fails at the transport level (airplane mode / server gone). */
    var totalOutage: Boolean = false

    private val failSends = LinkedHashMap<String, Int>()
    private val dropAcks = LinkedHashMap<String, Int>()
    private val serverErrors = LinkedHashMap<String, Int>()

    /** The request never leaves the client: bytes do NOT land, no response. */
    fun failSendsFor(contentRangePrefix: String, times: Int) {
        failSends[contentRangePrefix] = times
    }

    /** The bytes land, the response is lost. The server's offset advances anyway. */
    fun dropAcksFor(contentRangePrefix: String, times: Int) {
        dropAcks[contentRangePrefix] = times
    }

    /** Answer with `500` — retryable per the contract, unlike any 4xx. */
    fun serverErrorFor(contentRangePrefix: String, times: Int) {
        serverErrors[contentRangePrefix] = times
    }

    /** The bytes the server currently holds for [jobId] (its authoritative view). */
    fun uploadedBytes(jobId: String): ByteArray =
        jobs.getValue(jobId).let { it.bytes.copyOf(it.received.toInt()) }

    fun receivedBytes(jobId: String): Long = jobs.getValue(jobId).received

    override fun request(req: HttpRequest): HttpResponse {
        attempted += req
        if (totalOutage) return HttpResponse.transportFailure()

        val range = req.header("Content-Range")
        if (take(failSends, range)) return HttpResponse.transportFailure()
        if (take(serverErrors, range)) {
            served += req
            return HttpResponse.ok(500, body = """{"error":"boom"}""".encodeToByteArray())
        }

        served += req
        val response = route(req)
        // The lost-ack case: the server has already mutated its state, and only
        // the response disappears.
        if (take(dropAcks, range)) return HttpResponse.transportFailure()
        return response
    }

    private fun route(req: HttpRequest): HttpResponse {
        if (req.header("Authorization") != "Bearer $token") {
            return HttpResponse.ok(
                401,
                mapOf("WWW-Authenticate" to "Bearer"),
                """{"error":"unauthorized"}""".encodeToByteArray(),
            )
        }
        val path = req.url.substringAfter("://").substringAfter('/').let { "/$it" }

        if (req.method == HttpMethod.POST && path == "/jobs") return createJob(req)
        if (req.method == HttpMethod.PUT && path.endsWith("/upload")) {
            return upload(path.removePrefix("/jobs/").removeSuffix("/upload"), req)
        }
        if (req.method == HttpMethod.GET && path.endsWith("/result")) {
            return result(path.removePrefix("/jobs/").removeSuffix("/result"))
        }
        if (req.method == HttpMethod.GET && path.startsWith("/jobs/")) {
            return status(path.removePrefix("/jobs/"))
        }
        if (req.method == HttpMethod.DELETE && path.startsWith("/jobs/")) {
            return cancel(path.removePrefix("/jobs/"))
        }
        return HttpResponse.ok(404, body = """{"error":"no route"}""".encodeToByteArray())
    }

    private fun createJob(req: HttpRequest): HttpResponse {
        val body = req.body.decodeToString()
        val size = Regex("\"size_bytes\"\\s*:\\s*(\\d+)").find(body)?.groupValues?.get(1)?.toLong()
            ?: return HttpResponse.ok(400, body = """{"error":"size_bytes"}""".encodeToByteArray())
        if (size > maxUploadBytes) {
            return HttpResponse.ok(413, body = """{"error":"too large"}""".encodeToByteArray())
        }
        val id = "job%08d".format(nextId++)
        jobs[id] = Job(id, size)
        return HttpResponse.ok(
            201,
            mapOf("Content-Type" to "application/json"),
            // Extra keys on purpose: the client must tolerate them.
            """{"id":"$id","upload_url":"/jobs/$id/upload","created_at":1.0}""".encodeToByteArray(),
        )
    }

    private fun upload(jobId: String, req: HttpRequest): HttpResponse {
        val job = jobs[jobId] ?: return HttpResponse.ok(404, body = """{"error":"no job"}""".encodeToByteArray())
        val header = req.header("Content-Range")
            ?: return HttpResponse.ok(400, body = """{"error":"Content-Range"}""".encodeToByteArray())

        val probe = Regex("^bytes \\*/(\\d+)$").find(header)
        if (probe != null) {
            // The resume probe. Deliberately answered with a bare 308 + header
            // and no Location, exactly like the service (api.py explains why).
            return HttpResponse.ok(308, mapOf("Upload-Offset" to job.received.toString()))
        }
        val m = Regex("^bytes (\\d+)-(\\d+)/(\\d+)$").find(header)
            ?: return HttpResponse.ok(400, body = """{"error":"malformed Content-Range"}""".encodeToByteArray())
        val start = m.groupValues[1].toLong()
        val end = m.groupValues[2].toLong()
        val total = m.groupValues[3].toLong()
        if (total != job.total) {
            return HttpResponse.ok(400, body = """{"error":"total mismatch"}""".encodeToByteArray())
        }
        if (end < job.received) {
            // Duplicate of bytes we already have: idempotent ack, not a 409.
            return offsetResponse(job)
        }
        if (start > job.received) {
            return HttpResponse.ok(
                416,
                mapOf("Upload-Offset" to job.received.toString()),
                """{"error":"gap"}""".encodeToByteArray(),
            )
        }
        if (req.body.size.toLong() != end - start + 1) {
            return HttpResponse.ok(400, body = """{"error":"length mismatch"}""".encodeToByteArray())
        }
        req.body.copyInto(job.bytes, start.toInt())
        job.received = maxOf(job.received, end + 1)
        return offsetResponse(job)
    }

    private fun offsetResponse(job: Job) = HttpResponse.ok(
        if (job.received >= job.total) 201 else 200,
        mapOf("Upload-Offset" to job.received.toString()),
    )

    private fun status(jobId: String): HttpResponse {
        val job = jobs[jobId] ?: return HttpResponse.ok(404, body = """{"error":"no job"}""".encodeToByteArray())
        val (state, progress, message) = nextPollAnswer(job)
        // `internal_state`, `received_bytes`, `size_bytes` and `exit_code` are
        // exactly the extra keys the real service adds and the client must drop.
        val body = """
            {"id":"${job.id}","state":"$state","progress":$progress,"message":"$message",
             "size_bytes":${job.total},"received_bytes":${job.received},
             "internal_state":"$state","exit_code":null,"created_at":1.0,"updated_at":2.0}
        """.trimIndent().encodeToByteArray()
        return HttpResponse.ok(200, mapOf("Content-Type" to "application/json"), body)
    }

    private fun nextPollAnswer(job: Job): Triple<String, Float, String> {
        if (pollScript.isEmpty()) {
            return if (job.received >= job.total) {
                Triple("done", 1f, "complete")
            } else {
                Triple("uploading", job.received.toFloat() / job.total, "uploading")
            }
        }
        return if (pollScript.size > 1) pollScript.removeAt(0) else pollScript[0]
    }

    private fun result(jobId: String): HttpResponse {
        val job = jobs[jobId] ?: return HttpResponse.ok(404, body = """{"error":"no job"}""".encodeToByteArray())
        val bytes = resultBytes
        val state = nextPollAnswerPeek(job).first
        if (bytes == null || state != "done") {
            return HttpResponse.ok(404, body = """{"error":"not ready"}""".encodeToByteArray())
        }
        return HttpResponse.ok(200, mapOf("Content-Type" to "application/zip"), bytes)
    }

    private fun nextPollAnswerPeek(job: Job): Triple<String, Float, String> =
        pollScript.firstOrNull() ?: if (job.received >= job.total) {
            Triple("done", 1f, "complete")
        } else {
            Triple("uploading", 0f, "uploading")
        }

    private fun cancel(jobId: String): HttpResponse {
        val job = jobs[jobId] ?: return HttpResponse.ok(404, body = """{"error":"no job"}""".encodeToByteArray())
        pollScript = mutableListOf(Triple("failed", 0f, "cancelled by client"))
        return HttpResponse.ok(
            200,
            mapOf("Content-Type" to "application/json"),
            """{"id":"${job.id}","state":"failed","progress":0.0,"message":"cancelled by client"}"""
                .encodeToByteArray(),
        )
    }

    private fun take(table: MutableMap<String, Int>, range: String?): Boolean {
        if (range == null) return false
        for ((prefix, remaining) in table.entries.toList()) {
            if (range.startsWith(prefix) && remaining > 0) {
                table[prefix] = remaining - 1
                return true
            }
        }
        return false
    }
}
