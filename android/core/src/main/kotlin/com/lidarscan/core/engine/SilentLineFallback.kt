package com.lidarscan.core.engine

/**
 * ROUND 32 item 178(b) — **when the line is silent, ask the other bauds.**
 *
 * The owner's 0.9.16 retest produced the one probe outcome round 31 did not
 * have an answer for: the STL-27L rung opened the port at 921 600, read for its
 * window, and counted **sixteen bytes**. Round 31 made the probes honest about
 * *declining*; a decline on sixteen bytes and a decline on thirty thousand
 * bytes of the wrong protocol print the same word and mean completely different
 * things. One is "this is not an STL-27L". The other is "nothing is talking to
 * me", and at that point the baud is a hypothesis rather than a fact.
 *
 * It is a live hypothesis. The LD-series is not one rate: the LD06 and LD19 in
 * the same family run **230 400**, several STL-27L clone and dev-kit batches
 * ship at the family default rather than at the datasheet's 921 600, and a
 * `first1=00` on a mis-clocked line is exactly what a UART sampling a faster or
 * slower stream produces. So a silent window is followed by the two other rates
 * the family is known to use, the byte and packet counts from each go in the
 * `[net-debug]` block, and if one of them yields CRC-valid packets **that is
 * the rate the session runs at**.
 *
 * ## What this deliberately does not do
 *
 * It does not run on a **loud** window. A port that delivered thirty thousand
 * bytes and no valid packet has answered the question — something is streaming
 * and it is not this protocol — and re-clocking a working link two more times
 * to ask again would be churn on the COIN-D6 path for nothing. The gate is
 * silence, and silence is defined as a number ([SILENT_LINE_BYTES]) rather than
 * as zero, because a mis-clocked UART emits a trickle of framing noise rather
 * than nothing at all — the owner's line produced 42 bytes in two seconds and
 * would have failed a `== 0` test.
 *
 * All of the decisions live here, in `:core`, so they can be driven from
 * synthetic streams on a bare JVM. There is still no STL-27L on this machine.
 */
object SilentLineFallback {

    /**
     * Below this many bytes in a probe window, the line counts as **silent**
     * and the other rates are worth asking.
     *
     * 256. The owner's evidence is the calibration: 42 bytes in a 2 s window
     * and 16 bytes in the 750 ms probe window, against a healthy STL-27L's
     * ~92 000 bytes per second. Anything a real device sends clears this in
     * under three milliseconds, and a mis-clocked line's framing trickle stays
     * far below it — the two populations are three orders of magnitude apart,
     * so the threshold's exact value is not delicate.
     */
    const val SILENT_LINE_BYTES: Long = 256

    /**
     * The rates to try after a silent window at 921 600, in order.
     *
     * 230 400 first because it is the LD-series family default and therefore
     * the likeliest, and it is the rate this app's CH340 path already has a
     * hundred field captures at. 460 800 second: it is the geometric middle of
     * the two, it appears on some LD firmware builds, and trying it costs one
     * more 750 ms window on a port that by definition is not doing anything.
     */
    val FALLBACK_BAUDS: List<Int> = listOf(230_400, 460_800)

    /**
     * ROUND 32 item 178(c) — below this many bytes **in a whole capture**, the
     * no-data banner stops blaming the baud and asks whether the sensor is
     * turning.
     *
     * A capture window is seconds rather than milliseconds, so this is a
     * looser bar than [SILENT_LINE_BYTES] and it is calibrated on the same
     * evidence: the owner's two failed scans delivered 552 bytes in 25 s and
     * 666 bytes in 5 s. A real STL-27L delivers ~92 000 bytes per second and a
     * COIN-D6 ~24 000, so anything genuinely streaming is four orders of
     * magnitude above this after one second. Nothing that is actually sending
     * data can be mistaken for a silent line.
     */
    const val QUIET_LINE_BYTES: Long = 4_096

    /** True when a probe window at [SerialLidarBaud.STL27L] justifies asking the other rates. */
    fun isSilent(bytesSeen: Long): Boolean = bytesSeen < SILENT_LINE_BYTES

    /**
     * One rate's result. [packets] is the CRC-valid count — the only field that
     * decides anything; [bytes] and [headers] are for the log, and they are the
     * two numbers that separate "silent", "loud but wrong" and "the CRC refused
     * coincidences" from each other.
     */
    data class Attempt(
        val baud: Int,
        val bytes: Long,
        val headers: Int,
        val packets: Int,
    )

    /**
     * The rate to run the session at, or null when none of them answered.
     *
     * First past the bar, in the order they were tried — not "the best one".
     * A device speaks one rate; two rates producing CRC-valid LD packets from
     * the same physical line is not a thing that happens, and if it somehow did,
     * preferring the earlier attempt keeps the family default over the exotic
     * one.
     */
    fun chooseBaud(attempts: List<Attempt>, packetsToIdentify: Int): Int? =
        attempts.firstOrNull { it.packets >= packetsToIdentify }?.baud

    /**
     * The `[net-debug]` line: every rate tried, with what it produced.
     *
     * `921600:bytes=16,542c=0,packets=0 230400:bytes=34104,542c=712,packets=709`
     * — one line that says both "the fast rate is dead" and "the slow one is
     * the sensor", which is the whole diagnosis. Round 31's lesson, applied to
     * one more axis: the counters a probe decided on belong in the log next to
     * the verdict, not behind it.
     */
    fun evidenceLine(attempts: List<Attempt>): String =
        attempts.joinToString(" ") { "${it.baud}:bytes=${it.bytes},542c=${it.headers},packets=${it.packets}" }

    /**
     * The `[session]` line for a session that ended up somewhere other than the
     * datasheet's rate. Null at the standard rate, because a log line that says
     * "everything is normal" is noise.
     */
    fun nonStandardBaudLine(baud: Int): String? =
        if (baud == SerialLidarBaud.STL27L) null else "STL-27L at $baud (non-standard)"
}
