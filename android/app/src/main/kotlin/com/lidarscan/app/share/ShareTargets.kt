package com.lidarscan.app.share

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import java.io.File

/**
 * B6/B11 — hands a file to the system share sheet (Tech Spec §3.8's "Project
 * exported as `.lscan.zip` via share sheet / USB / network share", and §3.6's
 * DXF/PDF export on Android).
 *
 * Everything goes through the app's [FileProvider] (`res/xml/file_paths.xml`).
 * A bare `file://` Uri has thrown `FileUriExposedException` since API 24, and
 * the alternative that actually works — copying into MediaStore — would
 * duplicate multi-gigabyte bundles for no benefit. The `FLAG_GRANT_READ_URI_PERMISSION`
 * grant is per-Uri and lasts for the receiving activity, which is the narrowest
 * thing that works.
 */
object ShareTargets {

    fun uriFor(context: Context, file: File) =
        FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)

    fun shareFile(context: Context, file: File, mimeType: String, title: String) {
        val uri = uriFor(context, file)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = mimeType
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_TITLE, file.name)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val chooser = Intent.createChooser(send, title).apply {
            // A chooser launched from a non-activity context (a ViewModel's
            // application context) needs its own task.
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(chooser)
    }

    /**
     * ROUND 23 item 104c — **N files, ONE share sheet.**
     *
     * Group Share exports each selected scan in turn (see
     * `com.lidarscan.core.projects.BatchExport`) and then hands the whole set
     * over at once. Opening one sheet per scan would be three sheets stacked on
     * top of each other, of which the operator sees the last: the same "a
     * hand-off with no result callback" trap ROUND 7 named, multiplied.
     *
     * `ACTION_SEND_MULTIPLE` with `EXTRA_STREAM` as an **`ArrayList<Uri>`** —
     * `putParcelableArrayListExtra` will not take any other `List`, and a plain
     * array silently produces a sheet with nothing attached. Everything else is
     * [shareFile]'s contract, unchanged: FileProvider Uris, a per-Uri read
     * grant, and a chooser with its own task because the caller is a ViewModel
     * holding an application context.
     *
     * The files are already in `Downloads/LidarScan/` by the time this is
     * called. The sheet is the extra, never the delivery.
     */
    fun shareFiles(context: Context, files: List<File>, title: String) {
        if (files.isEmpty()) return
        if (files.size == 1) {
            shareFile(context, files.first(), mimeFor(files.first()), title)
            return
        }
        val uris = ArrayList<android.net.Uri>(files.size)
        files.forEach { uris.add(uriFor(context, it)) }
        val send = Intent(Intent.ACTION_SEND_MULTIPLE).apply {
            type = commonMime(files)
            putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris)
            putExtra(Intent.EXTRA_TITLE, title)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val chooser = Intent.createChooser(send, title).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(chooser)
    }

    /**
     * The one `type` an `ACTION_SEND_MULTIPLE` of mixed files can honestly
     * carry.
     *
     * Pure, and separated from [shareFiles] so it is unit-testable without an
     * emulator — the intent itself is not, and this is the half that decides
     * which targets appear in the sheet. Three PLY files keep
     * `application/octet-stream`; a PLY and a PNG fall back to the full
     * wildcard; two PNGs collapse to the image family, which keeps the
     * image-capable targets that `application/octet-stream` would have thrown
     * away (the ROUND 15 item 56 lesson, applied to a set).
     *
     * (Kotlin block comments nest, so the literal MIME wildcards are spelled
     * out in words here rather than written with a slash-star.)
     */
    fun commonMime(files: List<File>): String {
        if (files.isEmpty()) return "*/*"
        val mimes = files.map { mimeFor(it) }.distinct()
        if (mimes.size == 1) return mimes.first()
        val families = mimes.map { it.substringBefore('/') }.distinct()
        return if (families.size == 1) "${families.first()}/*" else "*/*"
    }

    /** MIME types for the file kinds this app produces. */
    fun mimeFor(file: File): String = when (file.extension.lowercase()) {
        "zip" -> "application/zip"
        "pdf" -> "application/pdf"
        "dxf" -> "image/vnd.dxf"
        // ROUND 15 (item 56): the floor plan's rendered preview. Without this
        // the share sheet offers a PNG as application/octet-stream and every
        // image-capable target disappears from it.
        "png" -> "image/png"
        "jpg", "jpeg" -> "image/jpeg"
        "ply", "las", "pcd" -> "application/octet-stream"
        else -> "application/octet-stream"
    }
}
