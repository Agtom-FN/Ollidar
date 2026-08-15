package com.lidarscan.core.cloud

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * The real [HttpTransport], over `java.net.HttpURLConnection`.
 *
 * **Why `HttpURLConnection` and not OkHttp/Retrofit.** Three reasons, in
 * order of weight:
 *
 *  1. **`:core` must stay a plain Kotlin/JVM module.** `core/build.gradle.kts`
 *     says so in its first line, and it is what lets `CloudSubmitE2eTest` run
 *     against a *real* running `cloud/service` in `:core:test` on a bare JVM —
 *     no emulator, no Robolectric, no instrumentation. An Android-only HTTP
 *     stack would push the only end-to-end coverage this client has onto a
 *     device.
 *  2. **No new dependency.** D3 needs a transport, not a networking
 *     framework: the whole surface is "send one request, get one response",
 *     already fixed by [HttpTransport]. Adding a library to `:core` for that
 *     buys nothing the JDK does not already do.
 *  3. **On Android this *is* OkHttp.** Since API 21 the platform's
 *     `HttpURLConnection` is implemented by a bundled fork of OkHttp
 *     (`com.android.okhttp` / `com.android.org.conscrypt` for TLS), so on the
 *     phone we get connection pooling, HTTP/2 where the server offers it and
 *     the platform trust store anyway — through an API that also works on the
 *     desktop JVM.
 *
 * If the app ever needs streaming download progress, request cancellation
 * mid-flight, or interceptors, swapping in an OkHttp-backed [HttpTransport] is
 * a single class — that is the point of the seam.
 *
 * Three details this class must get right, all of them contract-visible:
 *
 *  * **Redirects are NOT followed.** The resume probe's answer is a `308`
 *     (`cloud/service/lidarscan_service/api.py` returns it with no `Location`,
 *     deliberately). A transport that followed 3xx would swallow the one
 *     status the resume path is built on. [HttpURLConnection.setInstanceFollowRedirects]
 *     is forced to `false`.
 *  * **Error bodies are read from [HttpURLConnection.getErrorStream].**
 *     `getInputStream()` throws on any 4xx/5xx, so a naive transport turns
 *     every "413 too large" into an indistinguishable transport failure — and
 *     [CloudSubmitClient] would then *retry* it, which is precisely what the
 *     contract forbids.
 *  * **Request bodies use fixed-length streaming mode.** Without it
 *     `HttpURLConnection` buffers the entire body in memory *again* before
 *     sending, which for an 8 MiB chunk doubles the footprint on a phone.
 *
 * @param connectTimeoutMs TCP/TLS connect budget.
 * @param readTimeoutMs per-read budget once connected. A stall past this is
 *   reported as a transport failure (`transportOk = false`), which is exactly
 *   what [CloudSubmitClient] retries and then resumes from.
 */
class UrlConnectionHttpTransport(
    private val connectTimeoutMs: Int = 15_000,
    private val readTimeoutMs: Int = 60_000,
) : HttpTransport {

    override fun request(req: HttpRequest): HttpResponse {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(req.url).openConnection() as HttpURLConnection).apply {
                instanceFollowRedirects = false
                connectTimeout = connectTimeoutMs
                readTimeout = readTimeoutMs
                requestMethod = req.method.name
                useCaches = false
                for ((name, value) in req.headers) setRequestProperty(name, value)
            }

            if (req.method == HttpMethod.POST || req.method == HttpMethod.PUT) {
                // Always set the body — including the empty one the resume
                // probe sends, which must go out as `Content-Length: 0` and
                // not as a chunked or absent body, or the service's
                // Content-Range parser never sees the probe at all.
                conn.doOutput = true
                conn.setFixedLengthStreamingMode(req.body.size.toLong())
                conn.outputStream.use { out ->
                    var written = 0
                    while (written < req.body.size) {
                        val n = minOf(WRITE_BLOCK_BYTES, req.body.size - written)
                        out.write(req.body, written, n)
                        written += n
                    }
                    out.flush()
                }
            }

            val status = conn.responseCode
            val headers = buildMap {
                for ((name, values) in conn.headerFields) {
                    // headerFields carries the status line under a null key.
                    if (name == null || values.isEmpty()) continue
                    put(name, values.last())
                }
            }
            // 4xx/5xx bodies live on the error stream; a 204/304/308 may have
            // neither stream at all.
            val stream: InputStream? = if (status in 200..399) {
                runCatching { conn.inputStream }.getOrNull()
            } else {
                conn.errorStream
            }
            val body = stream?.use { readAll(it) } ?: HttpRequest.EMPTY_BODY

            HttpResponse(transportOk = true, status = status, headers = headers, body = body)
        } catch (e: IOException) {
            // Refused, DNS failure, TLS failure, timeout, reset mid-body: no
            // HTTP status ever arrived, so this is a transport failure and NOT
            // a status the client may interpret. See HttpResponse.transportOk.
            HttpResponse.transportFailure()
        } catch (e: RuntimeException) {
            // Malformed URL/protocol misuse surfaces as IllegalArgument/IllegalState
            // from HttpURLConnection. Same story from the caller's side.
            HttpResponse.transportFailure()
        } finally {
            conn?.disconnect()
        }
    }

    private fun readAll(input: InputStream): ByteArray {
        val out = ByteArrayOutputStream(READ_BLOCK_BYTES)
        val buf = ByteArray(READ_BLOCK_BYTES)
        while (true) {
            val n = input.read(buf)
            if (n < 0) break
            out.write(buf, 0, n)
        }
        return out.toByteArray()
    }

    private companion object {
        const val WRITE_BLOCK_BYTES = 64 * 1024
        const val READ_BLOCK_BYTES = 64 * 1024
    }
}
