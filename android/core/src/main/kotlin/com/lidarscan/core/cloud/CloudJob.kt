package com.lidarscan.core.cloud

/**
 * Everything [CloudSubmitClient] needs to reach one cloud job service — a
 * Kotlin mirror of `scanengine::jobs::CloudSubmitConfig`.
 *
 * [baseUrl] and [token] have **no defaults on purpose**. The engine's C++
 * struct ships a placeholder host; here the app is the thing a user points at
 * their own server, and a default that silently works against nothing is a
 * worse failure than a compile error. The caller (settings screen / project
 * store) supplies both.
 */
data class CloudSubmitConfig(
    /** e.g. `https://cloud.example.com` or `http://127.0.0.1:8080`. A trailing `/` is trimmed. */
    val baseUrl: String,
    /** The single tenant's bearer token (Tech Spec §3.8 MVP: token auth, no accounts). */
    val token: String,
    /** Bytes per `Content-Range` PUT. 8 MiB matches the engine's default. */
    val chunkBytes: Int = 8 * 1024 * 1024,
    /**
     * Tech Spec §3.8's hard upload-size cap, checked against the **local
     * file's size before `POST /jobs` is even sent**. Same number as the
     * service's own `LIDARSCAN_MAX_UPLOAD_BYTES` default, so a file that would
     * be refused is refused without moving a byte over a phone's mobile data.
     */
    val maxUploadBytes: Long = 2L * 1024 * 1024 * 1024,
    /** Retries per request before the resume probe. */
    val maxRetries: Int = 5,
    val backoffInitialMs: Long = 500,
    val backoffMultiplier: Double = 2.0,
    val backoffMaxMs: Long = 30_000,
    val pollIntervalMs: Long = 2_000,
    /** 0 = unbounded; bounded only by the caller's cancel flag / coroutine scope. */
    val maxPollAttempts: Int = 0,
) {
    /** [baseUrl] with any trailing slashes removed, so `base + "/jobs"` is well-formed. */
    val normalizedBaseUrl: String get() = baseUrl.trimEnd('/')

    init {
        require(chunkBytes > 0) { "chunkBytes must be > 0" }
        require(maxRetries >= 0) { "maxRetries must be >= 0" }
        require(backoffMultiplier >= 1.0) { "backoffMultiplier must be >= 1.0" }
    }
}

/**
 * The five state strings A15 §5 puts on the wire, plus [UNKNOWN] for anything
 * a newer server invents.
 *
 * There is deliberately **no `CANCELLED`**: the service (README, "state is one
 * of …") settles a cancelled job into `failed` with the word in `message`,
 * matching the engine's own job model (A15 §2). A client that invented a sixth
 * state here would disagree with both.
 */
enum class CloudJobState {
    UNKNOWN, QUEUED, UPLOADING, PROCESSING, DONE, FAILED;

    val isTerminal: Boolean get() = this == DONE || this == FAILED

    companion object {
        fun fromWire(s: String?): CloudJobState = when (s) {
            "queued" -> QUEUED
            "uploading" -> UPLOADING
            "processing" -> PROCESSING
            "done" -> DONE
            "failed" -> FAILED
            else -> UNKNOWN
        }
    }
}

/**
 * A `GET /jobs/{id}` snapshot, narrowed to the four fields A15 §5 makes
 * contractual. The service also returns `internal_state`, `received_bytes`,
 * `size_bytes` and `exit_code`; those are additive and this client ignores
 * them (see the `ignoreUnknownKeys` note in [CloudSubmitClient]).
 */
data class CloudJobStatus(
    val id: String,
    val state: CloudJobState,
    /** 0..1. */
    val progress: Float,
    val message: String,
)

/**
 * Every way a cloud submit can fail, as a sealed type so the UI can branch on
 * the *kind* rather than on a string.
 *
 * These are `Exception`s so they fit Kotlin's [Result], which every public
 * method on [CloudSubmitClient] returns. The split mirrors the `ScanError`
 * values the engine's client sets, because the two clients must be
 * indistinguishable to a server.
 */
sealed class CloudError(message: String, cause: Throwable? = null) : Exception(message, cause) {
    /** `401` — the token was rejected. Never retried. */
    class PermissionDenied(message: String) : CloudError(message)

    /**
     * The upload is over the cap: either the local pre-flight check against
     * [CloudSubmitConfig.maxUploadBytes], or the server's own `413`. Never
     * retried — a size does not become acceptable by asking again.
     */
    class TooLarge(message: String) : CloudError(message)

    /** `404` — no such job, or a result that is not ready yet (A15 §5 conflates the two). */
    class NotFound(message: String) : CloudError(message)

    /**
     * No HTTP response at all, after retries — and, for an upload, after the
     * single resume probe also failed to answer.
     */
    class Network(message: String, cause: Throwable? = null) : CloudError(message, cause)

    /** The job reached `failed` on the server. [status] carries the server's own message. */
    class JobFailed(val status: CloudJobStatus) :
        CloudError("cloud job ${status.id} failed: ${status.message.ifEmpty { "no message" }}")

    /** The caller's cancel flag went true, or `DELETE /jobs/{id}` was issued. */
    class Cancelled(message: String) : CloudError(message)

    /** Local filesystem trouble: the zip cannot be read, the destination cannot be written. */
    class Io(message: String, cause: Throwable? = null) : CloudError(message, cause)

    /**
     * The server answered, but not with anything the contract allows — an
     * unexpected status, a malformed body, a resume probe with no
     * `Upload-Offset`, an offset past EOF. Distinct from [Network] because
     * retrying will not help and the operator needs to see it.
     */
    class Protocol(message: String) : CloudError(message)

    /** Timed out waiting for a terminal state ([CloudSubmitConfig.maxPollAttempts]). */
    class Timeout(message: String) : CloudError(message)
}
