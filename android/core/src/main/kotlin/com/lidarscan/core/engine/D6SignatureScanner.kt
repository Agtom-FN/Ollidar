package com.lidarscan.core.engine

/**
 * AUTO-DETECT (D6 wizard): does a stream of freshly-read serial bytes look
 * like it came from a COIN-D6?
 *
 * A CH340 adapter attaching does not by itself mean a D6 is on the other
 * end — `android/NOTES.md`'s "Field verdict" section on GNSS-over-USB-serial
 * records exactly this ambiguity: a Unicore UM982 GNSS eval board is the
 * same VID/PID class (`0x1A86`/`0x7523`) and is a real device this app's
 * users attach.
 *
 * ## ROUND 31 item 176(b) — why the old `AA 55` test had to go
 *
 * Round 25 shipped this file as "did I see the two bytes `AA 55` adjacent",
 * and item 119's own report wrote down the weakness in full: *"the D6 probe
 * accepts any adjacent `AA 55` — about 1 chance in 65 536 per byte offset,
 * across roughly 34 kB of a 1.5 s window. A misidentification there is
 * genuinely possible."*
 *
 * It is worse than "possible". A 1.5 s window at 230 400 8N1 is ~34 500 bytes,
 * so against uniform noise the expected number of `AA 55` pairs is ~0.53 and
 * the chance of seeing **at least one** is about **41 %**. That is not a
 * probe; it is a coin weighted slightly against the operator. And the traffic
 * it is judging is not noise — it is an STL-27L's 921 600 stream sampled at a
 * quarter of its bit rate, which produces long alternating-bit runs, the very
 * pattern `AA` and `55` are.
 *
 * On 2026-08-22 the owner plugged in the project's first real STL-27L. The app
 * called it a COIN-D6. This is why.
 *
 * ## What a match means now
 *
 * The probe reads the D6's actual frame layout (`engine/include/scanengine/
 * drivers/d6/d6_parser.h`, mirrored below) and requires:
 *
 *  * **[FRAMES_TO_IDENTIFY] complete, well-formed frames** across the window,
 *    and
 *  * at least one **chained pair** — a well-formed frame whose successor
 *    begins at exactly `10 + 3 * LSN` bytes later and is also well-formed,
 *
 * or, when nothing chains, [FRAMES_WITHOUT_CHAIN] well-formed frames on their
 * own. See that constant for why the second form exists: it is the guard
 * against this round's only real regression risk, a spinning-up D6 whose
 * packets are separated by `FE`/`FF` filler and therefore chain nowhere.
 *
 * Well-formed means `AA 55`, `LSN >= 1`, and the constant check bit set in
 * **both** angle low bytes (the vendor driver's own cheap false-header filter,
 * `d6::Config::require_angle_check_bit`). That is 16 bits of preamble plus two
 * check bits plus a non-zero length: ~1 false header per 262 000 byte offsets,
 * so ~0.13 expected per window. Four of them is ~1 in 10^5; requiring one of
 * the four to be *chained* — two correctly-linked well-formed headers, ~1 in
 * 10^11 per offset, ~5 in 10^7 per window — closes the case outright.
 *
 * Against the round-25 probe that is a factor of ~10^5 on the exact failure
 * the owner hit, and it costs a real COIN-D6 nothing: the D6 emits ~100
 * packets/s, so four chained frames arrive in about 40 ms of a 1500 ms window.
 *
 * ## What is deliberately NOT checked: the checksum
 *
 * The D6's 16-bit XOR checksum has **two surviving readings** of an
 * untranslatable spec figure (`d6::ChecksumVariant::kVendorSdk` vs
 * `kSpecLiteral`; see `d6_parser.h` and the S1 REPORT). The engine ships both
 * and decides between them at runtime from live counters. Duplicating a
 * *contested* checksum into Kotlin, and then gating the one detection path
 * with field history on it, would be trading a proven weakness for an
 * unproven one — a D6 that failed the Kotlin copy would simply stop being
 * detected, in the field, with no way to tell why.
 *
 * Structure is enough here and structure is not in dispute. The checksum is
 * checked where it can be judged against real counters: in the engine.
 *
 * ## Streaming, because a D6 frame straddles USB reads
 *
 * A frame is `10 + 3 * LSN` bytes — up to 775 — and a USB read hands over
 * 4 KB at arbitrary boundaries, so a one-byte carry (which is all the round-25
 * scanner had, and all it needed for a two-byte preamble) cannot see a frame
 * split across two chunks. [D6FrameScanner] therefore keeps a residual buffer
 * and is fed chunk by chunk.
 *
 * Pure byte-array logic, no Android/USB dependency, so it is trivially
 * JVM-tested — the orchestration (open the port, read for a bounded window,
 * feed this per chunk) is `com.lidarscan.app.usb.D6AutoProbe`, `:app`.
 */
object D6SignatureScanner {

    /** First header byte on the wire (`d6::kPh0`). */
    const val PREAMBLE_0: Byte = 0xAA.toByte()

    /** Second header byte on the wire (`d6::kPh1`). */
    const val PREAMBLE_1: Byte = 0x55.toByte()

    /** `PH .. CS` inclusive, before the samples begin (`d6::kHeaderBytes`). */
    const val HEADER_BYTES: Int = 10

    /** Bytes per measurement sample (`d6::kSampleBytes`). */
    const val SAMPLE_BYTES: Int = 3

    /** FSA/LSA low-byte bit 0, constant 1 on real traffic (`d6::kAngleCheckBit`). */
    const val ANGLE_CHECK_BIT: Int = 0x01

    /** Largest frame the protocol allows: `LSN` is a byte, so 255 samples. */
    const val MAX_FRAME_BYTES: Int = HEADER_BYTES + 255 * SAMPLE_BYTES

    /**
     * How many well-formed frames make an identification **when one of them is
     * chained**.
     *
     * **Four**, the same bar [Stl27lSignatureScanner]'s caller uses and the
     * same bar the engine's own `discovery::Stl27lSniffer` uses, so the two
     * serial probes are judged by one number rather than two that have to be
     * separately defended. A real D6 clears it in ~40 ms.
     */
    const val FRAMES_TO_IDENTIFY: Int = 4

    /**
     * How many well-formed frames identify a D6 **with no chained pair at
     * all** — the escape hatch that keeps the strict probe from being able to
     * reject the sensor it is supposed to find.
     *
     * The chain requirement assumes packets sit back to back in the stream.
     * They normally do, but `d6::kSpeedAdjA` / `kSpeedAdjB` (`FE` / `FF`)
     * filler is emitted "before the rotation stabilises", and the probe runs
     * the moment a device is plugged in — which is exactly when a spinning
     * lidar has not stabilised. A window full of filler-separated packets
     * would have zero chained pairs and, without this, a COIN-D6 that
     * auto-detect refused. That is the one regression a stricter probe must
     * not be allowed to have.
     *
     * **Twelve** unchained well-formed frames is not a weaker bar, it is a
     * differently-shaped one: at ~1 false header per 262 000 byte offsets the
     * expected count across a whole window is ~0.13, so twelve is a Poisson
     * tail around 10^-20. A real D6 at ~100 packets/s reaches it in ~120 ms of
     * a 1500 ms window.
     */
    const val FRAMES_WITHOUT_CHAIN: Int = 12

    /**
     * Is there a well-formed D6 frame HEADER at [at]?
     *
     * Header only — this says nothing about whether the frame's samples are
     * present. [D6FrameScanner] is what pairs it with a length check.
     */
    fun isWellFormedHeaderAt(bytes: ByteArray, at: Int, len: Int): Boolean {
        if (at < 0 || at + HEADER_BYTES > len) return false
        if (bytes[at] != PREAMBLE_0 || bytes[at + 1] != PREAMBLE_1) return false
        // LSN = 0 is malformed: the vendor parser rejects it (a packet with no
        // samples has nothing to carry) and it is the single most likely value
        // for a byte inside a run of zeros.
        if ((bytes[at + 3].toInt() and 0xFF) == 0) return false
        if ((bytes[at + 4].toInt() and ANGLE_CHECK_BIT) == 0) return false
        if ((bytes[at + 6].toInt() and ANGLE_CHECK_BIT) == 0) return false
        return true
    }

    /** Total frame length implied by the `LSN` of the header at [at]. Caller must have checked the header. */
    fun frameLengthAt(bytes: ByteArray, at: Int): Int =
        HEADER_BYTES + SAMPLE_BYTES * (bytes[at + 3].toInt() and 0xFF)

    /**
     * ROUND 25's test, kept **only** so the round-25 suite still describes what
     * it described and so the STL-27L cross-check
     * (`Stl27lSignatureScannerTest`: "a D6 chunk is not an STL-27L") keeps its
     * counterpart.
     *
     * It is NOT what [D6FrameScanner] — and therefore not what the app's
     * auto-detect — decides on any more, for the reason in the class doc: on
     * its own it says yes to 41 % of pure noise. Do not reach for it in new
     * code.
     */
    fun containsSignature(carry: Byte?, chunk: ByteArray, len: Int): Boolean {
        if (len <= 0) return false
        if (carry == PREAMBLE_0 && chunk[0] == PREAMBLE_1) return true
        var i = 0
        while (i < len - 1) {
            if (chunk[i] == PREAMBLE_0 && chunk[i + 1] == PREAMBLE_1) return true
            i++
        }
        return false
    }

    /** The byte to remember as [containsSignature]'s next `carry`, i.e. the chunk's last byte. */
    fun lastByteOrNull(chunk: ByteArray, len: Int): Byte? = if (len > 0) chunk[len - 1] else null
}

/**
 * ROUND 31 item 176(b) — the COIN-D6 half of serial auto-detect, as a
 * **streaming** frame counter.
 *
 * Fed one USB read at a time; keeps whatever tail it could not yet judge.
 * Single-writer by construction (the reader thread of one
 * `com.lidarscan.app.usb.D6SerialConnection`), and its counters are volatile
 * so the probe coroutine can read the evidence after its window closes.
 *
 * The counters are the diagnostic the owner's next field report needs: a probe
 * that declines can say *why* — "no headers at all" is a different fault from
 * "37 headers, none chained", and the second one means this scanner is looking
 * at something D6-shaped that it is nonetheless refusing.
 */
class D6FrameScanner {

    private var buf = ByteArray(INITIAL_CAPACITY)
    private var size = 0

    /** First index of [buf] whose `AA 55` pair has not been counted yet. Keeps the tally from double-counting a retained residual. */
    private var preambleScanFrom = 0

    @Volatile
    var bytesSeen: Long = 0L
        private set

    /** Complete, well-formed frames counted so far. */
    @Volatile
    var frames: Int = 0
        private set

    /** Frames whose immediate successor is also a well-formed header — the structural gate. */
    @Volatile
    var chainedPairs: Int = 0
        private set

    /**
     * Bare `AA 55` pairs seen. Counted **only** as evidence for the log: this
     * is exactly the number round 25's probe would have decided on, so a field
     * log can show the difference between the two probes on the same bytes.
     */
    @Volatile
    var preamblePairs: Int = 0
        private set

    /**
     * The verdict: four frames **with a chain**, or twelve without one.
     *
     * See [D6SignatureScanner.FRAMES_TO_IDENTIFY] for the ordinary bar and
     * [D6SignatureScanner.FRAMES_WITHOUT_CHAIN] for why the second clause
     * exists — it is there so a spinning-up D6, whose packets are separated by
     * `FE`/`FF` filler and therefore chain nowhere, is still found.
     */
    val identified: Boolean
        get() = (frames >= D6SignatureScanner.FRAMES_TO_IDENTIFY && chainedPairs >= 1) ||
            frames >= D6SignatureScanner.FRAMES_WITHOUT_CHAIN

    fun feed(chunk: ByteArray, len: Int) {
        if (len <= 0) return
        bytesSeen += len
        append(chunk, len)
        countPreambles()
        scan()
    }

    /**
     * One line for `[net-debug]`, and for the round-31 report the owner is
     * asked to send back if the retest still fails.
     */
    fun evidence(): String =
        "bytes=$bytesSeen aa55=$preamblePairs frames=$frames chained=$chainedPairs " +
            "(need frames>=${D6SignatureScanner.FRAMES_TO_IDENTIFY} with chained>=1, " +
            "or frames>=${D6SignatureScanner.FRAMES_WITHOUT_CHAIN} without)"

    private fun append(chunk: ByteArray, len: Int) {
        if (size + len > buf.size) {
            var cap = buf.size
            while (cap < size + len) cap *= 2
            buf = buf.copyOf(cap)
        }
        System.arraycopy(chunk, 0, buf, size, len)
        size += len
    }

    /**
     * The round-25 signal, over the same residual buffer. Counted before
     * [scan] consumes anything, and only over the region [scan] has not yet
     * retired, so a pair is never counted twice.
     */
    private fun countPreambles() {
        var i = preambleScanFrom.coerceAtLeast(0)
        while (i < size - 1) {
            if (buf[i] == D6SignatureScanner.PREAMBLE_0 && buf[i + 1] == D6SignatureScanner.PREAMBLE_1) {
                preamblePairs++
            }
            i++
        }
        preambleScanFrom = (size - 1).coerceAtLeast(0)
    }

    private fun scan() {
        var i = 0
        while (i + D6SignatureScanner.HEADER_BYTES <= size) {
            if (!D6SignatureScanner.isWellFormedHeaderAt(buf, i, size)) {
                i++
                continue
            }
            val frameLen = D6SignatureScanner.frameLengthAt(buf, i)
            // The chain is judged by looking at where the NEXT header must
            // start, so a frame is not retired until those bytes have arrived.
            // Breaking here (rather than counting the frame and losing the
            // chain) is why a genuine device's chain count is not a function of
            // where the USB stack happened to split its reads.
            if (i + frameLen + D6SignatureScanner.HEADER_BYTES > size) break
            frames++
            val next = i + frameLen
            if (D6SignatureScanner.isWellFormedHeaderAt(buf, next, size)) chainedPairs++
            i = next
        }
        retire(i)
    }

    /**
     * Drop the bytes [scan] is finished with, keeping the tail it still has to
     * judge — and never letting that tail exceed one maximum frame plus one
     * header, so a stream that is all `AA 55` and no frames cannot grow this
     * buffer without bound.
     */
    private fun retire(from: Int) {
        var keepFrom = from
        val maxResidual = D6SignatureScanner.MAX_FRAME_BYTES + D6SignatureScanner.HEADER_BYTES
        if (size - keepFrom > maxResidual) keepFrom = size - maxResidual
        if (keepFrom <= 0) return
        val remaining = size - keepFrom
        System.arraycopy(buf, keepFrom, buf, 0, remaining)
        size = remaining
        preambleScanFrom = (preambleScanFrom - keepFrom).coerceIn(0, remaining)
    }

    private companion object {
        /** One USB read plus one maximum frame — the common case never reallocates. */
        const val INITIAL_CAPACITY = 4096 + D6SignatureScanner.MAX_FRAME_BYTES
    }
}
