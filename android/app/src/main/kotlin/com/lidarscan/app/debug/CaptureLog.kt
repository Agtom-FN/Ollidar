package com.lidarscan.app.debug

import android.content.Context
import android.util.Log
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * ROUND 6, owner item 20 — **the evidence the last field failure did not leave.**
 *
 * > "the capture not saved to the phone, it just gone and the app project not
 * > see any saved"
 *
 * That report arrived with no stack trace, no logcat and no way to reproduce it
 * off the device, because everything the capture path knew went to `Log.*` and
 * `logcat` on a phone in the field is gone the moment the buffer wraps. The root
 * cause was found by reading code, which worked this once and is not a plan.
 *
 * So: a **persistent, app-internal, rolling text log** of the events that decide
 * whether a capture survives — project created, session started/stopped,
 * pushbroom enabled, seal written, seal *verified*, and every failure on any of
 * those paths. It lives in `filesDir/logs/`, its path is shown on the Settings
 * screen, and it can be exported/shared from there.
 *
 * ### Deliberate properties
 *
 * * **Internal storage, not external.** `filesDir` needs no permission, is
 *   backed up with the app, and cannot be cleared by a file manager the way the
 *   external `Android/data` tree can. The projects themselves stay on external
 *   storage — the log is small and must outlive them.
 * * **Bounded, two files.** Writes go to `capture.log` until it passes
 *   [MAX_BYTES], then it is rotated onto `capture.log.1` (replacing the previous
 *   one) and a fresh file starts. Two files means the most recent
 *   [MAX_BYTES]…2×[MAX_BYTES] of history always survives, with no unbounded
 *   growth and no directory scan.
 * * **Synchronous and best-effort.** A line is a few dozen bytes appended to a
 *   file; queueing it would mean the log misses the crash it exists to describe.
 *   Every write is wrapped: a logger that can throw is a logger that turns a
 *   recoverable capture bug into a crash.
 * * **Never contains a location fix, an address or a token.** Callers pass
 *   ids, counts, file paths and error text. This is a diagnostic, not telemetry,
 *   and nothing here leaves the phone unless the owner exports it.
 */
class CaptureLog(context: Context) {

    private val appContext = context.applicationContext
    private val dir = File(appContext.filesDir, "logs")
    private val file = File(dir, FILE_NAME)
    private val rotated = File(dir, "$FILE_NAME.1")
    private val lock = Any()

    private val timestamps = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)

    private val _lastLine = MutableStateFlow<String?>(null)

    /** The most recent line, so a screen can show that logging is genuinely happening. */
    val lastLine: StateFlow<String?> = _lastLine.asStateFlow()

    /** Absolute path of the live log file — printed on the Settings screen. */
    val path: String get() = file.absolutePath

    /** Bytes currently held across the live file and its one rotation. */
    fun sizeBytes(): Long = runCatching {
        (if (file.isFile) file.length() else 0L) + (if (rotated.isFile) rotated.length() else 0L)
    }.getOrDefault(0L)

    init {
        runCatching { dir.mkdirs() }
    }

    /**
     * Appends one line under [tag]. Also mirrors to `logcat` so a bench session
     * with a cable attached sees it in both places.
     */
    fun log(tag: String, message: String) {
        val line = "${timestamps.format(Date())} [$tag] $message"
        _lastLine.value = line
        Log.i(LOGCAT_TAG, line)
        runCatching {
            synchronized(lock) {
                if (file.isFile && file.length() > MAX_BYTES) {
                    rotated.delete()
                    if (!file.renameTo(rotated)) file.delete()
                }
                dir.mkdirs()
                file.appendText(line + "\n")
            }
        }
    }

    /** Convenience for the failure half — same file, a tag that greps. */
    fun logFailure(tag: String, message: String, error: Throwable? = null) {
        val suffix = error?.let { " :: ${it.javaClass.simpleName}: ${it.message}" }.orEmpty()
        log("$tag!", message + suffix)
    }

    /**
     * The whole retained log, oldest first (rotation then live), for the
     * Settings screen's export. Capped so a share intent cannot be handed
     * something unbounded.
     */
    fun readAll(): String = runCatching {
        synchronized(lock) {
            buildString {
                if (rotated.isFile) append(rotated.readText())
                if (file.isFile) append(file.readText())
            }
        }
    }.getOrElse { "" }.takeLast(MAX_EXPORT_CHARS)

    /**
     * Writes the retained log to a shareable file in the app's own cache and
     * returns it, or null. `cacheDir/shared/` is what `ShareTargets`'
     * `FileProvider` root already exposes.
     */
    fun exportTo(cacheRoot: File): File? = runCatching {
        val out = File(cacheRoot, EXPORT_NAME)
        out.parentFile?.mkdirs()
        out.writeText(readAll())
        out
    }.getOrNull()

    fun clear() {
        runCatching {
            synchronized(lock) {
                file.delete()
                rotated.delete()
            }
        }
        _lastLine.value = null
    }

    companion object {
        private const val FILE_NAME = "capture.log"
        private const val EXPORT_NAME = "lidarscan-capture-log.txt"
        private const val LOGCAT_TAG = "LidarScanCapture"

        /** 512 KB live + 512 KB rotated. Thousands of lines; nothing a phone notices. */
        const val MAX_BYTES = 512L * 1024L

        private const val MAX_EXPORT_CHARS = 1_000_000

        // Tags, so the log greps cleanly and every call site spells them the
        // same way. These are the capture-survival path end to end.
        const val TAG_PROJECT = "project"
        const val TAG_SESSION = "session"
        const val TAG_SEAL = "seal"
        const val TAG_AR = "ar"
        const val TAG_PUSHBROOM = "pushbroom"
        const val TAG_STORE = "store"

        /**
         * ROUND 7: every user-triggered file operation, with its destination.
         * The owner's exported scan-008 bundle "went nowhere" and the log had
         * nothing to say about it, which is the half of the failure that made it
         * unreportable.
         */
        const val TAG_EXPORT = "export"
    }
}
