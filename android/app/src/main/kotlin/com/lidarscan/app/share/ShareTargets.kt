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

    /** MIME types for the file kinds this app produces. */
    fun mimeFor(file: File): String = when (file.extension.lowercase()) {
        "zip" -> "application/zip"
        "pdf" -> "application/pdf"
        "dxf" -> "image/vnd.dxf"
        "ply", "las", "pcd" -> "application/octet-stream"
        else -> "application/octet-stream"
    }
}
