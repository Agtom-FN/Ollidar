package com.lidarscan.app

import android.app.Application
import com.lidarscan.app.debug.CaptureLog
import com.lidarscan.app.di.AppContainer

class LidarScanApplication : Application() {

    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
        installCrashRecorder(container.captureLog)
    }

    /**
     * ROUND 22 item 87 — **the recorder the last eight process deaths did not
     * have.**
     *
     * The owner's 2026-08-20 session died eight times and
     * `filesDir/logs/capture.log` — the log ROUND 6 built for exactly this
     * question, and which carries `[session]`, `[ar]`, `[seal]` and `[net]`
     * lines right up to the last millisecond — has **nothing** about any of
     * them. Not because the log failed, but because nothing ever asked it: an
     * uncaught exception goes to the default handler, and the default handler
     * writes to logcat and dies. On a phone in a flat, at night, with no cable,
     * logcat is gone before anyone can read it. Every one of items 88–92 was
     * found by reading code, which worked and is not a plan.
     *
     * ### The three deliberate properties
     *
     *  * **Same channel as the narrative.** The stack goes through
     *    [CaptureLog.log] under `[crash]`, so it lands in the same file, in
     *    order, directly beneath the `[ar]`/`[session]` lines that describe
     *    what the app was doing. A crash reporter in a separate file is a
     *    second thing to export and a timestamp to match up by hand — which is
     *    precisely the failure ROUND 10's dated export filename was about.
     *    It also lands in the open capture's own `debug/capture-debug.log`
     *    when developer mode has one open, because [CaptureLog.log] mirrors
     *    there: a `.lscan` the owner sends then carries its own ending.
     *  * **Synchronous flush.** [CaptureLog.log] appends and closes per line
     *    (`appendText`), so by the time this returns the bytes are with the
     *    filesystem. A queued/async writer is a writer that loses the crash it
     *    exists to describe — the process is about to be killed.
     *  * **Delegates to the previous handler.** This records and then hands the
     *    same throwable to whatever handler was installed before it (on
     *    Android that is `RuntimeInit`'s killer, plus anything Play Services or
     *    a crash SDK chained in). The system crash dialog still appears, the
     *    ANR flow is untouched, and the process still dies. A recorder that
     *    swallows the platform's handling is a second bug wearing the first
     *    one's clothes: the app would limp on with a dead thread and no dialog,
     *    and the owner would report *that* instead.
     *
     * ### What is recorded
     *
     * The thread's name (a GL-thread death and a main-thread death are
     * different bugs and the round-16 session race is specifically a
     * render-thread one), the full stack including causes, the app version
     * (the log outlives several installs), and the heap numbers — because item
     * 91's renderer defect kills the process by allocating ~839 MB of vertex
     * buffers, and "free 12 MB of a 512 MB heap" at the moment of death is the
     * difference between reading that as an OOM and reading it as a logic bug.
     * Native VBO allocation does not come out of the Java heap, so these
     * numbers exonerate as often as they accuse; both answers are useful and
     * neither was available before.
     *
     * Everything is wrapped: a crash recorder that can itself throw turns one
     * fatal exception into two and loses the first one's stack.
     */
    private fun installCrashRecorder(log: CaptureLog) {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching { log.log(CaptureLog.TAG_CRASH, crashReport(thread, error)) }
            // Whatever was there before still runs. On Android that is the
            // platform killer; if a crash SDK is ever added it chains here too.
            previous?.uncaughtException(thread, error)
        }
    }

    private fun crashReport(thread: Thread, error: Throwable): String {
        val runtime = Runtime.getRuntime()
        val mb = 1024L * 1024L
        val stack = runCatching {
            java.io.StringWriter().also { w ->
                error.printStackTrace(java.io.PrintWriter(w))
            }.toString().trim()
        }.getOrElse { "<stack unavailable: ${it.javaClass.simpleName}>" }
        return buildString {
            append("UNCAUGHT on thread \"").append(thread.name).append("\" (id=").append(thread.id)
            append(") — v").append(BuildConfig.VERSION_NAME)
            append(" (code ").append(BuildConfig.VERSION_CODE).append(")")
            append(" heapFree=").append((runtime.freeMemory() / mb)).append("MB")
            append(" heapTotal=").append((runtime.totalMemory() / mb)).append("MB")
            append(" heapMax=").append((runtime.maxMemory() / mb)).append("MB")
            append('\n').append(stack)
        }
    }
}
