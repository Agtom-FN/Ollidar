package com.lidarscan.app

import android.provider.MediaStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.app.share.DownloadsExporter
import java.io.File
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 7, owner field item — **"I exported scan-008 and the file is nowhere."**
 *
 * The bundle was written to `<project>.lscan/exports/`, i.e. under
 * `Android/data/`, which the system Files app has refused to browse since
 * Android 11. It existed and was unreachable, and then a share sheet — a
 * hand-off with no result callback — was the only route out. Two silent exits
 * on one button.
 *
 * `DownloadsExporter` is the fix, and it is exactly the kind of code that
 * "compiles fine" and does nothing on a real phone (a wrong `RELATIVE_PATH`, a
 * missing `IS_PENDING` clear, a permission the app does not hold). So it is
 * tested where it runs: against a real `MediaStore` on the device, reading the
 * bytes back out through the provider rather than trusting the insert.
 */
@RunWith(AndroidJUnit4::class)
class DownloadsExporterTest {

    private lateinit var staging: File
    private val written = mutableListOf<String>()

    @Before
    fun setUp() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        staging = File(context.cacheDir, "downloadsExporterTest").also {
            it.deleteRecursively()
            it.mkdirs()
        }
    }

    @After
    fun tearDown() {
        // Leave no test artifacts in the user's Downloads.
        val resolver = InstrumentationRegistry.getInstrumentation().targetContext.contentResolver
        for (name in written) {
            runCatching {
                resolver.delete(
                    MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY),
                    "${MediaStore.Downloads.DISPLAY_NAME} = ?",
                    arrayOf(name),
                )
            }
        }
        staging.deleteRecursively()
    }

    private fun readBackBytes(displayName: String): ByteArray? {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val collection = MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        context.contentResolver.query(
            collection,
            arrayOf(MediaStore.Downloads._ID, MediaStore.Downloads.RELATIVE_PATH),
            "${MediaStore.Downloads.DISPLAY_NAME} = ?",
            arrayOf(displayName),
            null,
        )?.use { cursor ->
            if (!cursor.moveToFirst()) return null
            val id = cursor.getLong(0)
            val relative = cursor.getString(1)
            // The folder is part of the contract: everything this app writes
            // lands in one place a phone with 200 scans can still navigate.
            assertTrue(
                "expected Downloads/${DownloadsExporter.SUBDIRECTORY}/, got $relative",
                relative.contains(DownloadsExporter.SUBDIRECTORY),
            )
            val uri = android.content.ContentUris.withAppendedId(collection, id)
            return context.contentResolver.openInputStream(uri)?.use { it.readBytes() }
        }
        return null
    }

    @Test
    fun anExportedBundleLandsInDownloadsAndIsReadableByteForByte() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        // Big enough to cross the copy buffer more than once — a truncating
        // copy is exactly the bug a 12-byte fixture would hide.
        val payload = ByteArray(400_000) { (it % 251).toByte() }
        val source = File(staging, "Scan-008-2026-08-17-2253.lscan.zip").also { it.writeBytes(payload) }
        val name = "round7-test-${System.currentTimeMillis()}.lscan.zip"
        written += name

        val result = DownloadsExporter.copyToDownloads(context, source, fileName = name)
        assertTrue("copy failed: ${result.exceptionOrNull()}", result.isSuccess)

        // The string the UI shows and the log records must name a place a human
        // can actually open, not an app-private path.
        assertEquals("Downloads/${DownloadsExporter.SUBDIRECTORY}/$name", result.getOrThrow())

        val readBack = readBackBytes(name)
        assertTrue("the file must be visible in Downloads after the copy", readBack != null)
        assertEquals("size must match", payload.size, readBack!!.size)
        assertTrue("bytes must match", payload.contentEquals(readBack))

        // And the in-app copy is KEPT — it is what the engine wrote and what a
        // later cloud submit reads.
        assertTrue(source.isFile)
    }

    @Test
    fun aMissingSourceFailsLoudlyRatherThanReportingSuccess() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val missing = File(staging, "never-written.zip")
        val result = DownloadsExporter.copyToDownloads(context, missing)
        assertTrue("a missing source must not report success", result.isFailure)
        // The caller's contract is to show this; an empty message would defeat it.
        assertTrue(result.exceptionOrNull()?.message.orEmpty().isNotBlank())
    }

    @Test
    fun exportingTheSameProjectTwiceDoesNotOverwriteTheFirstBundle() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val name = "round7-dup-${System.currentTimeMillis()}.lscan.zip"
        written += name
        val first = File(staging, "a.zip").also { it.writeBytes(ByteArray(1024) { 1 }) }
        val second = File(staging, "b.zip").also { it.writeBytes(ByteArray(2048) { 2 }) }

        assertTrue(DownloadsExporter.copyToDownloads(context, first, fileName = name).isSuccess)
        assertTrue(DownloadsExporter.copyToDownloads(context, second, fileName = name).isSuccess)

        // MediaStore de-duplicates ("name (1).zip"), which is what a user
        // expects from re-exporting; the first bundle must still be intact.
        val readBack = readBackBytes(name)
        assertEquals(1024, readBack?.size)
    }
}
