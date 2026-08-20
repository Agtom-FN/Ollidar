package com.lidarscan.core.net

import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * ROUND 25 item 118, **owner amendment** — the same rate limiter
 * `ArSessionGate.noteRefusal` has had since ROUND 22 item 89, lifted into
 * `:core` and given a category key.
 *
 * ## Why it is the round-22 shape and not a new one
 *
 * Matching it is the point. `capture.log` is 512 KB live plus 512 KB rotated
 * (see `CaptureLog.MAX_BYTES`), and the whole value of that file is that the
 * evidence of a failure is still in it when someone goes looking. A wizard
 * poll runs once a second for as long as the Mid-360 screen is open, and a
 * full sweep is a twenty-line block; an operator who leaves the wizard up
 * while they walk to the van would rotate the log out of existence and destroy
 * exactly the evidence this amendment exists to leave.
 *
 * So: **at most one line per second per category**, and every suppressed line
 * is counted and reported on the next one that gets through, in the same
 * `(+N more since the last line)` text `ArSessionGate` uses. A person who has
 * read one of those lines can read the other. A suppressed count is what
 * distinguishes "one sweep during a hand-off" from "sixty a minute for four
 * minutes", and that distinction is the reason this is a counter and not a
 * plain drop.
 *
 * ## Why categories
 *
 * `ArSessionGate` had one stream of refusals and one window. This has several
 * — the wizard poll, the sensor auto-detect run, the discovery listener — and
 * they must not starve one another: a wizard polling at 1 Hz would otherwise
 * suppress every auto-detect sweep forever, which would silently delete the
 * category with the most to say. Each category gets its own window and its own
 * suppressed count, and they are completely independent.
 *
 * The on-demand Settings sweep deliberately does **not** go through this at
 * all. A person pressing a button is entitled to a line.
 *
 * ## Thread safety
 *
 * Sweeps arrive from a wizard coroutine on `Dispatchers.IO`, from the
 * auto-detect race, and from the discovery socket's own thread. Per-category
 * state is a `ConcurrentHashMap` of two atomics, and admission is a
 * compare-and-set on the window's timestamp — the loser of a race counts
 * itself as suppressed rather than logging, exactly as `ArSessionGate` does,
 * so the count stays honest under contention and two threads can never both
 * win one window.
 */
class ConnectionDebugRateLimiter(
    /** One line per category per this many milliseconds. */
    val intervalMillis: Long = DEFAULT_INTERVAL_MILLIS,
    /** Injectable clock, so the window can be tested without sleeping. */
    private val clockMillis: () -> Long = { System.currentTimeMillis() },
) {

    /**
     * Permission to emit one line in a category, carrying how many were
     * dropped since the previous one.
     *
     * A value type rather than a bare `Boolean` because a multi-line block
     * needs the suppressed count on its FIRST line (the verdict), not appended
     * to the bottom of twenty lines of detail where nobody reads it — see
     * [ConnectionSweepFormat.format]'s `extraVerdictSuffix`.
     */
    data class Admission(val category: String, val suppressed: Int) {
        /** `" (+4 more since the last line)"`, or empty. The round-22 text, verbatim. */
        val suffix: String
            get() = if (suppressed > 0) " (+$suppressed more since the last line)" else ""
    }

    private class Window {
        val lastAtMillis = AtomicLong(Long.MIN_VALUE)
        val suppressed = AtomicInteger(0)
    }

    private val windows = ConcurrentHashMap<String, Window>()

    /**
     * May [category] emit a line right now?
     *
     * Returns null when the window is still open — in which case the call has
     * already been counted as suppressed and the caller must simply not log.
     * Returns an [Admission] otherwise, and resets that category's suppressed
     * count to zero, having handed it to the caller.
     */
    fun admit(category: String): Admission? {
        val window = windows.computeIfAbsent(category) { Window() }
        val now = clockMillis()
        val last = window.lastAtMillis.get()
        if (last != Long.MIN_VALUE && now - last < intervalMillis) {
            window.suppressed.incrementAndGet()
            return null
        }
        // One winner per window. The loser counts itself rather than logging,
        // so two threads racing the same open window produce one line and an
        // accurate count instead of two lines.
        if (!window.lastAtMillis.compareAndSet(last, now)) {
            window.suppressed.incrementAndGet()
            return null
        }
        return Admission(category, window.suppressed.getAndSet(0))
    }

    /**
     * [admit] for a single-line caller: returns [line] with the suppressed
     * suffix already appended, or null when suppressed.
     *
     * This is the `ArSessionGate` call shape exactly, and it is what the
     * discovery-side one-liners use.
     */
    fun line(category: String, line: String): String? =
        admit(category)?.let { line + it.suffix }

    /**
     * Reopens [category]'s window immediately, discarding nothing.
     *
     * `ArSessionGate.claim` does the same thing on a fresh claim, for the same
     * reason: a new story is entitled to speak at once rather than waiting out
     * the previous one's window. The wizard being re-entered, or the operator
     * plugging something in, is such a story.
     */
    fun reopen(category: String) {
        windows[category]?.lastAtMillis?.set(Long.MIN_VALUE)
    }

    /** Reopens every category. Used when developer mode is switched on. */
    fun reopenAll() {
        windows.values.forEach { it.lastAtMillis.set(Long.MIN_VALUE) }
    }

    /** How many lines [category] has dropped since its last admitted one. For tests and for the debug screen. */
    fun suppressedSoFar(category: String): Int = windows[category]?.suppressed?.get() ?: 0

    companion object {
        /**
         * One second, the same number `ArSessionGate.LOG_INTERVAL_MS` uses.
         * The wizard polls at 1 Hz, so this admits roughly every other poll in
         * the worst case and every poll in the common one — which is the
         * intent: the periodic log is a heartbeat of the diagnosis, not a
         * transcript of it.
         */
        const val DEFAULT_INTERVAL_MILLIS = 1_000L

        /** The wizard's ~1 s diagnostic poll. */
        const val CATEGORY_WIZARD_POLL = "wizard-poll"

        /** One sensor auto-detect race (the Capture tab's entry probe). */
        const val CATEGORY_AUTO_DETECT = "auto-detect"

        /** UDP 56201 listener lifecycle and the datagrams it hears. */
        const val CATEGORY_DISCOVERY = "discovery"

        /** The serial D6 → STL-27L ladder's per-rung outcomes. */
        const val CATEGORY_SERIAL_PROBE = "serial-probe"
    }
}
