package com.lidarscan.core.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 31 item 176(b) — **[D6FrameScanner] against the stream that actually
 * broke it.**
 *
 * On 2026-08-22 the owner plugged the project's first real LDROBOT STL-27L
 * into the Pixel and the app called it a COIN-D6. Round 25's report had
 * already named the hole — *"the D6 probe accepts any adjacent `AA 55`"* — and
 * this suite is the fence around it. Two claims, both falsifiable here on a
 * bare JVM:
 *
 *  1. a real COIN-D6 frame chain is still identified, quickly, including when
 *     the USB stack splits it across reads (the regression risk of making the
 *     probe stricter is that the *working* sensor stops being found), and
 *  2. streams that the round-25 probe said yes to — noise containing `AA 55`,
 *     and an STL-27L's own bytes sampled at the D6's baud — are refused.
 *
 * Claim 2's fixtures are byte-exact and deterministic: the noise comes from a
 * seeded xorshift, and the mis-clocked STL-27L is produced by an explicit UART
 * model over byte-exact LD-series packets. No hardware, no randomness, no
 * `Math.random()`.
 *
 * WHAT THIS CANNOT PROVE: that a real STL-27L, on a real CH340, at the wrong
 * divisor, produces bytes shaped like [misClockedStl27l]'s. That is a model.
 * What it does prove is that the probe now needs D6 *structure* rather than a
 * two-byte coincidence, which is the property the field failure violated —
 * and item 176(c)'s `[net-debug]` first-64-bytes line exists so the owner's
 * retest can replace the model with the real thing.
 */
class D6FrameScannerTest {

    // ── COIN-D6 fixtures ────────────────────────────────────────────────────
    //
    // Built at the offsets in engine/include/scanengine/drivers/d6/d6_parser.h:
    //   0 AA | 1 55 | 2 M&T | 3 LSN | 4..5 FSA | 6..7 LSA | 8..9 CS | 10.. samples
    // FSA/LSA bit 0 is the constant check bit. The checksum is NOT computed —
    // the scanner deliberately does not read it (see D6SignatureScanner's
    // "what is deliberately NOT checked"), so a fixture that computed one
    // would be asserting something no code consumes.

    private fun d6Frame(samples: Int = 8, startAngleDeg64: Int = 0, startPacket: Boolean = false): ByteArray {
        require(samples in 1..255)
        val fsa = (startAngleDeg64 shl 1) or 0x01
        val lsa = (((startAngleDeg64 + samples * 4) % 23040) shl 1) or 0x01
        val out = ByteArray(D6SignatureScanner.HEADER_BYTES + 3 * samples)
        out[0] = 0xAA.toByte()
        out[1] = 0x55.toByte()
        out[2] = (if (startPacket) 0x01 else 0x00).toByte()
        out[3] = samples.toByte()
        out[4] = (fsa and 0xFF).toByte()
        out[5] = ((fsa ushr 8) and 0xFF).toByte()
        out[6] = (lsa and 0xFF).toByte()
        out[7] = ((lsa ushr 8) and 0xFF).toByte()
        out[8] = 0x12
        out[9] = 0x34
        for (i in 0 until samples) {
            val at = D6SignatureScanner.HEADER_BYTES + 3 * i
            out[at] = ((i * 3) and 0xFC).toByte()
            out[at + 1] = ((i * 5) and 0xFF).toByte()
            out[at + 2] = (10 + i).toByte()
        }
        return out
    }

    private fun d6Stream(frames: Int, samples: Int = 8): ByteArray {
        var out = ByteArray(0)
        for (n in 0 until frames) out += d6Frame(samples = samples, startAngleDeg64 = (n * 512) % 23040)
        return out
    }

    // ── the impostor fixtures ───────────────────────────────────────────────

    /**
     * Deterministic xorshift32 — the honest "framing garbage" model, and
     * reproducible byte for byte.
     *
     * The default seed is not arbitrary and the reason is itself the finding:
     * one probe window of noise contains an `AA 55` pair only about **41 %** of
     * the time, so most seeds produce a window the round-25 probe would have
     * *declined* — which would make the comparison below prove nothing. `0x1039`
     * is a seed whose window contains four of them, i.e. a perfectly ordinary
     * two-in-five window, and it is the one this suite pins.
     */
    private fun noise(bytes: Int, seed: Int = 0x1039): ByteArray {
        var x = seed
        return ByteArray(bytes) {
            x = x xor (x shl 13)
            x = x xor (x ushr 17)
            x = x xor (x shl 5)
            (x ushr 8).toByte()
        }
    }

    /** One byte-exact 47-byte LD-series packet, CRC8 poly 0x4D, written out at the documented offsets. */
    private fun stlPacket(startAngleCentiDeg: Int, timestampMs: Int): ByteArray {
        val body = ArrayList<Byte>(46)
        body += 0x54.toByte()
        body += 0x2C.toByte()
        fun le16(v: Int) {
            body += (v and 0xFF).toByte()
            body += ((v ushr 8) and 0xFF).toByte()
        }
        le16(3_600)
        le16(startAngleCentiDeg)
        repeat(12) {
            le16(1_000 + startAngleCentiDeg % 97)
            body += 0x40.toByte()
        }
        le16((startAngleCentiDeg + 200) % 36_000)
        le16(timestampMs)
        val arr = body.toByteArray()
        var crc = 0
        for (b in arr) {
            crc = crc xor (b.toInt() and 0xFF)
            repeat(8) { crc = if (crc and 0x80 != 0) ((crc shl 1) xor 0x4D) and 0xFF else (crc shl 1) and 0xFF }
        }
        return arr + crc.toByte()
    }

    /**
     * **The owner's stream.** An STL-27L talking 921 600 8N1 into a receiver
     * clocked at 230 400 — the exact situation the D6 rung is in when it runs
     * first against an STL-27L.
     *
     * Modelled rather than hand-waved: each transmitted byte becomes its ten
     * UART bits (start 0, eight data LSB-first, stop 1), the receiver samples
     * that bit train at one quarter of the rate, and every ten sampled bits are
     * reassembled as a byte with the framing thrown away. That is why the
     * result is dense in alternating-bit runs — which is precisely what `0xAA`
     * and `0x55` are, and precisely why the round-25 probe fell for it.
     */
    private fun misClockedStl27l(packets: Int, oversample: Int = 4): ByteArray {
        val bits = ArrayList<Int>(packets * 47 * 10)
        for (n in 0 until packets) {
            for (b in stlPacket(startAngleCentiDeg = (n * 200) % 36_000, timestampMs = n)) {
                bits += 0 // start
                for (i in 0 until 8) bits += (b.toInt() ushr i) and 1
                bits += 1 // stop
            }
        }
        val sampled = ArrayList<Int>(bits.size / oversample)
        var i = 0
        while (i < bits.size) {
            sampled += bits[i]
            i += oversample
        }
        val out = ByteArray(sampled.size / 10)
        for (k in out.indices) {
            var v = 0
            for (i in 0 until 8) v = v or ((sampled[k * 10 + 1 + i] and 1) shl i)
            out[k] = v.toByte()
        }
        return out
    }

    private fun scan(stream: ByteArray, chunk: Int = 4096): D6FrameScanner {
        val s = D6FrameScanner()
        var at = 0
        while (at < stream.size) {
            val n = minOf(chunk, stream.size - at)
            s.feed(stream.copyOfRange(at, at + n), n)
            at += n
        }
        return s
    }

    // ── the working sensor must keep working ────────────────────────────────

    @Test
    fun `a COIN-D6 frame chain identifies`() {
        val s = scan(d6Stream(frames = 12))
        assertTrue(s.evidence(), s.identified)
        assertTrue("chained pairs: ${s.evidence()}", s.chainedPairs >= 1)
    }

    @Test
    fun `four chained frames are enough — a real D6 clears the bar in a fraction of the window`() {
        // 5 frames so the 4th has a successor to chain against; 8 samples each
        // is 34 bytes a frame, ~170 bytes, which a 230400 link delivers in ~7ms
        // against a 1500 ms probe window.
        val s = scan(d6Stream(frames = 5))
        assertTrue(s.evidence(), s.identified)
        assertTrue("frames=${s.frames}", s.frames >= D6SignatureScanner.FRAMES_TO_IDENTIFY)
    }

    @Test
    fun `frames split across USB reads still count — the round-25 one-byte carry could not do this`() {
        // A D6 frame is up to 775 bytes; feeding 7 at a time guarantees every
        // frame in the stream straddles several chunks.
        val s = scan(d6Stream(frames = 12), chunk = 7)
        assertTrue(s.evidence(), s.identified)
    }

    @Test
    fun `the largest legal frame is still recognised`() {
        val s = scan(d6Frame(samples = 255) + d6Frame(samples = 255) + d6Frame(samples = 255) +
            d6Frame(samples = 255) + d6Frame(samples = 255))
        assertTrue(s.evidence(), s.identified)
    }

    // ── the impostors must be refused ───────────────────────────────────────

    @Test
    fun `a whole probe window of noise is refused — and the round-25 probe would have taken it`() {
        // 34 500 bytes is 1.5 s at 230 400 8N1: one D6 probe window exactly.
        val stream = noise(34_500)
        val s = scan(stream)

        assertFalse(s.evidence(), s.identified)
        // The point of the round, stated as an assertion: the OLD test says yes
        // to this same buffer. That is not a hypothetical — it is 41 % of all
        // noise windows, and it is how the owner's STL-27L became a D6.
        assertTrue(
            "this fixture must actually contain the round-25 signal, else it proves nothing",
            D6SignatureScanner.containsSignature(null, stream, stream.size),
        )
        assertTrue("aa55 pairs seen: ${s.preamblePairs}", s.preamblePairs > 0)
    }

    @Test
    fun `an STL-27L read at the D6's baud is refused`() {
        val stream = misClockedStl27l(packets = 900)
        val s = scan(stream)
        assertFalse("the owner's 2026-08-22 failure: ${s.evidence()}", s.identified)
    }

    @Test
    fun `an STL-27L read at its OWN baud is refused too`() {
        // The trivial direction, but worth pinning: clean LD-series bytes hold
        // no AA 55 at all, so this must decline with zero evidence of any kind.
        var stream = ByteArray(0)
        repeat(400) { stream += stlPacket(startAngleCentiDeg = (it * 200) % 36_000, timestampMs = it) }
        val s = scan(stream)
        assertFalse(s.evidence(), s.identified)
        assertEquals(0, s.frames)
    }

    @Test
    fun `a stream of nothing but AA 55 never identifies`() {
        val stream = ByteArray(8_192) { if (it % 2 == 0) 0xAA.toByte() else 0x55.toByte() }
        val s = scan(stream)
        assertFalse(s.evidence(), s.identified)
        // Every even offset is a preamble, so this is the pathological input
        // for the old probe and a no-op for the new one.
        assertTrue(s.preamblePairs > 1_000)
    }

    @Test
    fun `a well-formed header with no body and no successor is not a frame`() {
        // Exactly one header's worth of bytes: the scanner must not count a
        // frame it cannot see the end of, or a truncated read would identify.
        val header = d6Frame(samples = 8).copyOfRange(0, D6SignatureScanner.HEADER_BYTES)
        val s = scan(header)
        assertEquals(0, s.frames)
        assertFalse(s.identified)
    }

    @Test
    fun `LSN zero is refused — the most likely header-shaped value inside a run of zeros`() {
        val frame = d6Frame(samples = 8)
        frame[3] = 0
        val s = scan(ByteArray(64) + frame + ByteArray(64))
        assertEquals(0, s.frames)
    }

    @Test
    fun `the angle check bit is required, exactly as the vendor parser requires it`() {
        var stream = ByteArray(0)
        repeat(12) {
            val f = d6Frame(samples = 8, startAngleDeg64 = it * 128)
            f[4] = (f[4].toInt() and 0xFE).toByte() // clear FSA bit 0
            stream += f
        }
        val s = scan(stream)
        assertEquals(0, s.frames)
        assertFalse(s.identified)
    }

    @Test
    fun `a spinning-up D6, whose packets are separated by FE FF filler, is still found`() {
        // The one regression a stricter probe must not be allowed to have.
        // `d6::kSpeedAdjA`/`kSpeedAdjB` filler is emitted before the rotation
        // stabilises — which is exactly when the probe runs, because the probe
        // runs the moment the device is plugged in. Nothing chains here.
        var stream = ByteArray(0)
        repeat(20) {
            stream += d6Frame(samples = 8, startAngleDeg64 = (it * 512) % 23040)
            stream += ByteArray(6) { i -> if (i % 2 == 0) 0xFE.toByte() else 0xFF.toByte() }
        }
        val s = scan(stream)
        assertEquals("nothing may chain in this fixture, or it proves the wrong thing", 0, s.chainedPairs)
        assertTrue(s.evidence(), s.identified)
    }

    @Test
    fun `the unchained bar is high enough that noise cannot reach it`() {
        // The fallback above must not become the way garbage gets in. A whole
        // window of noise produces ~0.13 expected well-formed headers; twelve
        // of them is a Poisson tail around 10^-20.
        val s = scan(noise(34_500))
        assertTrue("noise frames=${s.frames}", s.frames < D6SignatureScanner.FRAMES_WITHOUT_CHAIN)
        val mis = scan(misClockedStl27l(packets = 900))
        assertTrue("mis-clocked frames=${mis.frames}", mis.frames < D6SignatureScanner.FRAMES_WITHOUT_CHAIN)
    }

    @Test
    fun `the evidence line names both bars so a declining probe can be diagnosed from a log`() {
        val s = scan(d6Stream(frames = 6))
        val e = s.evidence()
        assertTrue(e, e.contains("bytes="))
        assertTrue(e, e.contains("aa55="))
        assertTrue(e, e.contains("frames="))
        assertTrue(e, e.contains("chained="))
    }
}
