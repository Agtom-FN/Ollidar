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

    // ── ROUND 17 item 66: the per-capture debug log ─────────────────────────
    //
    // The owner asked, in developer mode, for a verbose log written INTO the
    // bundle. The reason is the one this class was built for, one level
    // sharper: `filesDir/logs/capture.log` is a rolling app-lifetime log that
    // has to be exported separately and then matched up by timestamp against a
    // scan that may be one of five taken that evening. A log that travels with
    // the scan needs no matching up — the `.lscan` a person sends is already
    // the whole story.
    //
    // Deliberately NOT a stream. See `debug/README` written beside it: this is
    // not chunked, not CRC'd, not in the manifest, and not part of the replay
    // guarantee. `record/replay` walks `streams/`, so a byte-identical replay
    // is unaffected by anything here — which is the property that lets the log
    // be as verbose as it likes.
    //
    // Two files here as well, for the same reason, and a hard cap: a capture
    // that runs for an hour with the verbose sink on must not be able to fill
    // the phone.

    private val lock2 = Any()
    private var captureSink: File? = null
    private var captureSinkRotated: File? = null

    /**
     * ROUND 17 item 66 — Developer Mode's answer, mirrored here so the decision
     * lives at ONE place and no call site has to carry a preference around.
     * Written from the Capture route's settings collector, read on the capture
     * thread at Start.
     */
    @Volatile
    var developerCaptureDebug: Boolean = false

    /**
     * Points the verbose sink at `<proj>.lscan/debug/capture-debug.log`.
     * Silently does nothing when [enabled] is false, which is how the developer
     * toggle reaches every call site without any call site knowing about it.
     */
    fun beginCaptureDebug(projectDir: File, header: String) {
        synchronized(lock2) {
            captureSink = null
            captureSinkRotated = null
            if (!developerCaptureDebug) return
            runCatching {
                val d = File(projectDir, DEBUG_DIR).apply { mkdirs() }
                File(d, "README.txt").writeText(README)
                val f = File(d, DEBUG_FILE)
                f.writeText("")
                captureSink = f
                captureSinkRotated = File(d, "$DEBUG_FILE.1")
            }
            debug("capture", header)
        }
    }

    /** Closes the sink. Idempotent; safe to call for a capture that never opened one. */
    fun endCaptureDebug(footer: String) {
        synchronized(lock2) {
            if (captureSink != null) debug("capture", footer)
            captureSink = null
            captureSinkRotated = null
        }
    }

    /** True while a capture debug log is open — for the HUD's developer strip. */
    val captureDebugPath: String? get() = synchronized(lock2) { captureSink?.absolutePath }

    /**
     * Verbose, capture-only, bundle-local. Goes to the open sink and to logcat
     * and to NOTHING else — in particular not to `capture.log`, whose value is
     * that it stays readable.
     */
    fun debug(tag: String, message: String) {
        val line = "${timestamps.format(Date())} [$tag] $message"
        runCatching {
            synchronized(lock2) {
                val target = captureSink ?: return@synchronized
                if (target.isFile && target.length() > DEBUG_MAX_BYTES) {
                    captureSinkRotated?.let { r ->
                        r.delete()
                        if (!target.renameTo(r)) target.delete()
                    }
                }
                target.appendText(line + "\n")
            }
        }
    }

    /**
     * Appends one line under [tag]. Also mirrors to `logcat` so a bench session
     * with a cable attached sees it in both places.
     */
    fun log(tag: String, message: String) {
        val line = "${timestamps.format(Date())} [$tag] $message"
        _lastLine.value = line
        Log.i(LOGCAT_TAG, line)
        // ROUND 17 item 66: every [ar]/[session]/[seal]/[net] line also lands
        // in the open capture's own log, so the bundle carries the narrative
        // and not just its own bytes. A no-op when no sink is open, which is
        // the case for every capture that did not ask for one.
        debug(tag, message)
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
    fun exportTo(cacheRoot: File, atEpochMillis: Long = System.currentTimeMillis()): File? = runCatching {
        val out = File(cacheRoot, exportFileName(atEpochMillis))
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
        private const val LOGCAT_TAG = "LidarScanCapture"

        /** 512 KB live + 512 KB rotated. Thousands of lines; nothing a phone notices. */
        const val MAX_BYTES = 512L * 1024L

        private const val MAX_EXPORT_CHARS = 1_000_000

        /**
         * ROUND 10 (owner item 40) — **`lidarscan-capture-log-YYYY-MM-DD-HHMM.txt`,
         * in the device's local time.**
         *
         * The owner's words: *"the capture log please save with date and time
         * in the file name."* The export name was a bare constant, so every
         * export landed on the same `Downloads/LidarScan/lidarscan-capture-log.txt`
         * and MediaStore de-duplicated it into `… (1).txt`, `… (2).txt`. The
         * evidence of that is quoted in the repository itself: `MountTrim.kt`
         * cites a real field artifact called `lidarscan-capture-log (1).txt`.
         * Nobody can tell those apart, and pairing one with the scan it
         * describes means opening it and reading timestamps.
         *
         * LOCAL time, not UTC, and to the MINUTE: this name exists to be read
         * next to a scan named `Scan-020-2026-08-18-1106`, which
         * `ScanAutoName` also formats in local time to the minute. Two
         * different clocks in two filenames that are meant to be compared by
         * eye is how you get a support thread instead of an answer.
         *
         * Seconds are deliberately absent for the same reason — the scan names
         * do not carry them — and two exports inside one minute are the one
         * case where MediaStore's `(1)` suffix is the correct behaviour rather
         * than a silent collision.
         */
        fun exportFileName(epochMillis: Long): String {
            val stamp = java.text.SimpleDateFormat("yyyy-MM-dd-HHmm", java.util.Locale.US)
                .format(java.util.Date(epochMillis))
            return "lidarscan-capture-log-$stamp.txt"
        }

        // Tags, so the log greps cleanly and every call site spells them the
        // same way. These are the capture-survival path end to end.
        // ROUND 17 item 66.
        const val DEBUG_DIR = "debug"
        const val DEBUG_FILE = "capture-debug.log"

        /**
         * 5 MB per file, two files. A verbose capture writes a few hundred
         * bytes a second, so this is hours before the first rotation and it is
         * a hard ceiling either way — a developer toggle must not be a way to
         * fill a phone.
         */
        const val DEBUG_MAX_BYTES = 5L * 1024L * 1024L

        val README: String =
            """
            This directory is a DEVELOPER DIAGNOSTIC and is not part of the scan.

            capture-debug.log is a verbose, human-readable transcript of one
            capture: session lifecycle, pose acceptance, re-anchor decisions,
            watchdog transitions, operator cues and preset changes.

            It is NOT a recorded stream. It is not chunked, not CRC'd, not
            listed in manifest.json, and NOT part of the replay guarantee:
            re-resolving this container reads only streams/, so this file has
            no effect on the geometry and deleting it changes nothing.

            Written only while Developer Mode is on (Settings -> tap the version
            footer seven times). Capped at 5 MB per file, two files.
            """.trimIndent()

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
