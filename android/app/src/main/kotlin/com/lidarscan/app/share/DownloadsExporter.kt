package com.lidarscan.app.share

import android.content.ContentValues
import android.content.Context
import android.os.Environment
import android.provider.MediaStore
import java.io.File

/**
 * ROUND 7, owner field item — **"I exported scan-008 and the file is nowhere."**
 *
 * ## Why it was nowhere
 *
 * Every export this app produced went to
 * `<project>.lscan/exports/<name>` — which lives under
 * `/storage/emulated/0/Android/data/com.lidarscan.app.debug/files/…`. That path
 * is app-specific external storage, and **since Android 11 the system Files app
 * refuses to browse `Android/data/` at all.** The bytes were on the phone,
 * exactly where the app said, and there was no way for a human to reach them
 * with a file manager. The `.lscan.zip` path then opened a share sheet, which
 * is a hand-off with no result callback: dismiss it, or pick a target that
 * silently fails, and the app has already forgotten it happened.
 *
 * That is the same failure shape as ROUND 6's vanished captures and ROUND 7's
 * `points=0` seal: **a user-triggered operation that ends in neither a visible
 * success with a path nor a visible failure.** The rule this file exists to
 * enforce is that there is no third outcome.
 *
 * ## What it does
 *
 * Copies a finished export into the shared **Downloads** collection through
 * `MediaStore`, under `Downloads/LidarScan/`, and returns the human-readable
 * destination so the caller can put it on screen and in `capture.log`.
 *
 * * `MediaStore`, not `File(Environment.getExternalStoragePublicDirectory(...))`
 *   — the latter needs `WRITE_EXTERNAL_STORAGE`, which is not granted on API 29+
 *   and which this app deliberately does not request. `MediaStore.Downloads`
 *   needs **no permission at all** for a file the app itself inserts, which is
 *   why the whole thing works on a scoped-storage phone with nothing in the
 *   manifest. `minSdk` here is 29, so there is no legacy branch to write.
 * * `IS_PENDING = 1` while copying, cleared at the end: a multi-gigabyte bundle
 *   must not be visible to a file manager (or a backup agent) half-written.
 * * The name is de-duplicated by the platform, not by us — inserting
 *   `Scan-008.lscan.zip` twice yields `Scan-008 (1).lscan.zip`, which is what a
 *   user expects and what re-exporting a project should do.
 * * The in-app copy under `exports/` is **kept**. It is what the engine's job
 *   wrote, it is what a subsequent cloud submit reads, and deleting it to save
 *   space would trade a known-good artifact for a duplicate.
 *
 * Failure is a `Result` carrying the real exception, never a swallowed null:
 * the caller's contract is to show it.
 */
object DownloadsExporter {

    /** Everything this app writes lands in one folder, so a phone with 200 scans is still navigable. */
    const val SUBDIRECTORY = "LidarScan"

    /** What the UI and the log call the destination. Not a filesystem path — MediaStore owns that. */
    fun displayPathFor(fileName: String): String = "Downloads/$SUBDIRECTORY/$fileName"

    /**
     * Copies [source] into `Downloads/LidarScan/[fileName]`.
     *
     * @return the display path on success (e.g. `Downloads/LidarScan/Scan-008.lscan.zip`).
     */
    fun copyToDownloads(
        context: Context,
        source: File,
        fileName: String = source.name,
        mimeType: String = ShareTargets.mimeFor(source),
    ): Result<String> = runCatching {
        require(source.isFile) { "${source.absolutePath} is not a file" }
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, fileName)
            put(MediaStore.Downloads.MIME_TYPE, mimeType)
            put(MediaStore.Downloads.RELATIVE_PATH, "${Environment.DIRECTORY_DOWNLOADS}/$SUBDIRECTORY")
            put(MediaStore.Downloads.IS_PENDING, 1)
            put(MediaStore.Downloads.SIZE, source.length())
        }
        val collection = MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        val uri = resolver.insert(collection, values)
            ?: error("MediaStore refused to create Downloads/$SUBDIRECTORY/$fileName")

        try {
            resolver.openOutputStream(uri).use { out ->
                checkNotNull(out) { "MediaStore returned no output stream for $fileName" }
                source.inputStream().use { input -> input.copyTo(out, DEFAULT_BUFFER_SIZE) }
            }
        } catch (t: Throwable) {
            // A half-written pending entry is invisible to the user but real to
            // the provider; leaving it behind would accumulate ghosts.
            runCatching { resolver.delete(uri, null, null) }
            throw t
        }

        resolver.update(uri, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
        displayPathFor(fileName)
    }

    private const val DEFAULT_BUFFER_SIZE = 128 * 1024
}
