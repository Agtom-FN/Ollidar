package com.lidarscan.core.cloud

/**
 * The pluggable HTTP seam [CloudSubmitClient] is written against — a Kotlin
 * mirror of the engine's `scanengine::jobs::HttpTransport`
 * (`engine/include/scanengine/jobs/http_transport.h`).
 *
 * The shape is copied deliberately, not adapted: **one [request] call is ONE
 * HTTP round trip**. Resumable upload is built on top of this by
 * [CloudSubmitClient] issuing one call per chunk with a `Content-Range`
 * header, *not* by the interface streaming internally. That is what keeps a
 * fake transport a pure function of (method, url, headers, body) -> response
 * with no long-lived connection state, which is in turn what makes the
 * disconnect/resume path testable at all — see `CloudSubmitClientTest`.
 *
 * `:core` is a plain Kotlin/JVM module with no Android dependency (see
 * `core/build.gradle.kts`), so nothing in this package may import
 * `android.*`. That is not incidental: it is what lets the end-to-end test
 * in `CloudSubmitE2eTest` talk to a real running service from `:core:test`
 * on a bare JVM, with no emulator.
 */
interface HttpTransport {
    fun request(req: HttpRequest): HttpResponse
}

/**
 * The verbs this contract uses. `DELETE` is here for the cancel extension the
 * service documents (`cloud/service/README.md`); the engine's C++ enum has
 * only GET/POST/PUT because A15 §5 defines no cancel verb.
 */
enum class HttpMethod { GET, POST, PUT, DELETE }

/**
 * One request. [body] is the body of *this* request only — for an upload
 * chunk that is the chunk's bytes, never the whole file, and it is empty for
 * `GET`/`DELETE` and for the `Content-Range: bytes *&#47;<total>` resume probe.
 */
data class HttpRequest(
    val method: HttpMethod,
    val url: String,
    val headers: Map<String, String> = emptyMap(),
    val body: ByteArray = EMPTY_BODY,
) {
    // data class + ByteArray: the generated equals/hashCode would compare the
    // array by identity, which is a trap for anyone writing a test that
    // matches on a request. Compare contents instead.
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is HttpRequest) return false
        return method == other.method &&
            url == other.url &&
            headers == other.headers &&
            body.contentEquals(other.body)
    }

    override fun hashCode(): Int {
        var result = method.hashCode()
        result = 31 * result + url.hashCode()
        result = 31 * result + headers.hashCode()
        result = 31 * result + body.contentHashCode()
        return result
    }

    /** Case-insensitive header lookup, for tests and transports that need it. */
    fun header(name: String): String? =
        headers.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value

    companion object {
        internal val EMPTY_BODY = ByteArray(0)
    }
}

/**
 * One response.
 *
 * [transportOk] `false` means **no response ever arrived** — a refused
 * connection, a DNS failure, a timeout, an aborted socket. [status],
 * [headers] and [body] are meaningless in that case. This is the single flag
 * [CloudSubmitClient]'s retry/resume logic keys off: it retries a transport
 * failure or a 5xx, and never retries a real status the server *did* return
 * (401/413/400/416). Collapsing "no answer" into some synthetic status code
 * would destroy exactly that distinction, which is why the engine models it
 * as a separate boolean and this port does too.
 */
data class HttpResponse(
    val transportOk: Boolean,
    val status: Int = 0,
    val headers: Map<String, String> = emptyMap(),
    val body: ByteArray = HttpRequest.EMPTY_BODY,
) {
    /**
     * Case-insensitive header lookup. HTTP header names are case-insensitive
     * (RFC 9110 §5.1) and real servers disagree about casing — uvicorn emits
     * `upload-offset`, the contract writes `Upload-Offset`. The client must
     * not care, so the lookup is here rather than in each call site.
     */
    fun header(name: String): String? =
        headers.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is HttpResponse) return false
        return transportOk == other.transportOk &&
            status == other.status &&
            headers == other.headers &&
            body.contentEquals(other.body)
    }

    override fun hashCode(): Int {
        var result = transportOk.hashCode()
        result = 31 * result + status
        result = 31 * result + headers.hashCode()
        result = 31 * result + body.contentHashCode()
        return result
    }

    companion object {
        /** The "no response at all" response. */
        fun transportFailure(): HttpResponse = HttpResponse(transportOk = false)

        fun ok(
            status: Int,
            headers: Map<String, String> = emptyMap(),
            body: ByteArray = HttpRequest.EMPTY_BODY,
        ): HttpResponse = HttpResponse(transportOk = true, status = status, headers = headers, body = body)
    }
}
