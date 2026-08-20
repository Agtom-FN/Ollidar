package com.lidarscan.app.share

import android.content.Context
import com.lidarscan.app.debug.CaptureLog
import com.lidarscan.core.cloud.HttpMethod
import com.lidarscan.core.cloud.HttpRequest
import com.lidarscan.core.cloud.HttpTransport
import com.lidarscan.core.cloud.UrlConnectionHttpTransport
import com.lidarscan.core.feedback.DeviceFacts
import com.lidarscan.core.feedback.FeedbackConfig
import com.lidarscan.core.feedback.FeedbackEndpoint
import com.lidarscan.core.feedback.FeedbackResult
import com.lidarscan.core.feedback.FeedbackRoute
import com.lidarscan.core.feedback.FeedbackWording
import com.lidarscan.core.feedback.Multipart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * ROUND 24 item 109(b/c) — **packages the log and sends it, or fails saying
 * where the file is.**
 *
 * ## The order of operations, which is the whole design
 *
 *  1. **Zip** the capture log (both rotations, `[crash]` entries included —
 *     they are the same file, which is exactly why ROUND 22 item 87 put them
 *     there) plus a plain-text device summary.
 *  2. **Copy the zip into `Downloads/LidarScan`** through
 *     [DownloadsExporter]. This happens BEFORE any attempt to send, and it is
 *     what makes the failure message honest: "Saved to Downloads." is a
 *     statement about a file that already exists, not a hope.
 *  3. **Send** — POST to the configured server, or hand the file to the share
 *     sheet.
 *
 * Reversing 2 and 3 would be the obvious optimisation and it would be wrong.
 * The share sheet has no result callback (ROUND 7's own lesson), and a server
 * that is not there fails after a timeout the operator has already walked away
 * from; in both cases the only thing the app can *guarantee* is the file, so
 * the file is guaranteed first.
 *
 * ## Why the cloud credentials, and not new ones
 *
 * There is no confirmed endpoint. Inventing a second URL/token pair for a
 * service that does not exist would be two settings to get wrong instead of
 * one — so this reuses D3's `cloudBaseUrl`/`cloudToken` and adds only a path
 * constant ([FeedbackEndpoint.PATH]). When a real endpoint arrives it is a
 * config value, not a code change; if it needs its own host, the `FeedbackConfig`
 * this is handed simply comes from somewhere else.
 *
 * @param transport injected so the whole send path is testable without a
 *   network — the same seam `CloudSubmitClient` has used since D3.
 */
class FeedbackSender(
    private val context: Context,
    private val captureLog: CaptureLog,
    private val transport: HttpTransport = UrlConnectionHttpTransport(),
    private val clock: () -> Long = System::currentTimeMillis,
) {

    /**
     * Package and deliver.
     *
     * [onProgress] is 0..1 and is called on the caller's dispatcher; the work
     * itself runs on [Dispatchers.IO], because zipping a five-megabyte log and
     * a `MediaStore` copy are both file operations and neither may touch the
     * frame the operator is looking at.
     *
     * The three coarse steps are reported rather than a byte counter: a
     * progress bar that moves in three jumps is honest about a job with three
     * phases, and a smooth one derived from a percentage nobody measures is
     * not.
     */
    suspend fun send(
        config: FeedbackConfig,
        facts: DeviceFacts,
        message: String,
        onProgress: (Float) -> Unit = {},
    ): FeedbackResult = withContext(Dispatchers.IO) {
        val route = config.route
        onProgress(0.1f)

        // ── 1: pack ────────────────────────────────────────────────────────
        val zip = runCatching { pack(facts, message) }.getOrElse { t ->
            captureLog.log(CaptureLog.TAG_EXPORT, "feedback: could not package the log: $t")
            return@withContext FeedbackResult(
                sent = false,
                route = route,
                downloadsPath = null,
                failure = t.toString(),
            )
        }
        onProgress(0.45f)

        // ── 2: land it somewhere the operator can reach, whatever happens ──
        val downloadsPath = DownloadsExporter.copyToDownloads(context, zip)
            .onFailure { captureLog.log(CaptureLog.TAG_EXPORT, "feedback: Downloads copy failed: $it") }
            .getOrNull()
        onProgress(0.7f)

        // ── 3: try to send ─────────────────────────────────────────────────
        val result = when (route) {
            FeedbackRoute.SERVER -> postToServer(config, facts, message, zip, downloadsPath)
            FeedbackRoute.SHARE -> shareSheet(zip, downloadsPath)
        }
        onProgress(1f)
        captureLog.log(
            CaptureLog.TAG_EXPORT,
            "feedback: route=${result.route} sent=${result.sent} " +
                "bytes=${zip.length()} downloads=${result.downloadsPath ?: "-"}" +
                (result.failure?.let { " reason=$it" } ?: ""),
        )
        result
    }

    /**
     * The zip: the live log, its one rotation if there is one, and the device
     * summary.
     *
     * Written into the app's own cache rather than anywhere shared —
     * [DownloadsExporter] is what publishes it, and a temp file in the shared
     * collection is a temp file somebody's photo backup uploads.
     */
    internal fun pack(facts: DeviceFacts, message: String): File {
        val stamped = java.text.SimpleDateFormat("yyyy-MM-dd-HHmm", java.util.Locale.US)
            .format(java.util.Date(clock()))
        val out = File(File(context.cacheDir, "shared").apply { mkdirs() }, "lidarscan-logs-$stamped.zip")
        ZipOutputStream(out.outputStream().buffered()).use { zos ->
            zos.putNextEntry(ZipEntry("device.txt"))
            zos.write(facts.asText().toByteArray(Charsets.UTF_8))
            zos.closeEntry()

            if (message.isNotBlank()) {
                zos.putNextEntry(ZipEntry("feedback.txt"))
                zos.write(message.toByteArray(Charsets.UTF_8))
                zos.closeEntry()
            }

            // `readAll()` is CaptureLog's own reader: the rotation first,
            // then the live file, so the archive reads forward in time. It
            // includes the ROUND 22 `[crash]` entries because they are in the
            // same file — which is the entire reason item 87 put them there.
            zos.putNextEntry(ZipEntry("capture.log"))
            zos.write(captureLog.readAll().toByteArray(Charsets.UTF_8))
            zos.closeEntry()
        }
        return out
    }

    private fun postToServer(
        config: FeedbackConfig,
        facts: DeviceFacts,
        message: String,
        zip: File,
        downloadsPath: String?,
    ): FeedbackResult {
        val url = config.url() ?: return FeedbackResult(
            sent = false,
            route = FeedbackRoute.SERVER,
            downloadsPath = downloadsPath,
            failure = "no server url",
        )
        val boundary = Multipart.boundary(clock())
        val body = Multipart.body(
            boundary = boundary,
            fields = linkedMapOf(
                FeedbackEndpoint.MESSAGE_FIELD to message,
                FeedbackEndpoint.INFO_FIELD to facts.asText(),
            ),
            fileField = FeedbackEndpoint.FILE_FIELD,
            fileName = zip.name,
            fileMime = "application/zip",
            fileBytes = zip.readBytes(),
        )
        val response = transport.request(
            HttpRequest(
                method = HttpMethod.POST,
                url = url,
                headers = mapOf(
                    "Authorization" to "Bearer ${config.token.trim()}",
                    "Content-Type" to Multipart.contentType(boundary),
                ),
                body = body,
            ),
        )
        // `transportOk = false` means no response ever arrived, which is a
        // different fact from a status the server actually returned — see
        // HttpResponse. Both are failures here; only the message differs, and
        // only in the log.
        val ok = response.transportOk && response.status in 200..299
        return FeedbackResult(
            sent = ok,
            route = FeedbackRoute.SERVER,
            downloadsPath = downloadsPath,
            failure = when {
                ok -> null
                !response.transportOk -> "no response from the server"
                else -> "server said ${response.status}"
            },
        )
    }

    /**
     * The share sheet.
     *
     * Reported as `sent = true` on the grounds that the app has done everything
     * it can: the chooser is up and the file is attached. It has no result
     * callback — ROUND 7 named that trap and it has not moved — which is
     * precisely why the zip is already in Downloads by the time this runs. The
     * only failure this can report is "the chooser would not open at all",
     * which is real (no target installed) and worth saying.
     */
    private fun shareSheet(zip: File, downloadsPath: String?): FeedbackResult = runCatching {
        ShareTargets.shareFile(context, zip, "application/zip", FeedbackWording.SHARE_TITLE)
        FeedbackResult(sent = true, route = FeedbackRoute.SHARE, downloadsPath = downloadsPath)
    }.getOrElse { t ->
        FeedbackResult(
            sent = false,
            route = FeedbackRoute.SHARE,
            downloadsPath = downloadsPath,
            failure = t.toString(),
        )
    }
}
