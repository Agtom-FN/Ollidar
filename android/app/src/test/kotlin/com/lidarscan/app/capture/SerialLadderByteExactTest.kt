package com.lidarscan.app.capture

import com.lidarscan.app.usb.D6AutoProbe
import com.lidarscan.app.usb.D6AutoProbeResult
import com.lidarscan.app.usb.Stl27lAutoProbe
import com.lidarscan.app.usb.Stl27lAutoProbeResult
import com.lidarscan.core.engine.D6SignatureScanner
import com.lidarscan.core.model.SensorType
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 31 item 176(b)/(c) — **the whole serial ladder, driven by bytes.**
 *
 * `SerialLidarAutoDetectorTest` proves the ladder's ORDER against rungs that
 * answer by fiat. This suite closes the other half: rungs whose answers come
 * from the **real probe verdicts** (`D6AutoProbe.classify`,
 * `Stl27lAutoProbe.classify`, which share their counters-to-answer mapping
 * with the live `probe()` — see their docs) fed **byte-exact synthetic
 * streams**. Plug a stream in at the top, read a `SensorType` out at the
 * bottom, with the round-31 decision logic in between and nothing faked except
 * the USB read itself.
 *
 * The three streams are the three cases the owner's 2026-08-22 session put in
 * front of the app:
 *
 *  * a COIN-D6 — must still be found, on rung 1, as it always was;
 *  * an **STL-27L**, which is the session that failed — must be found on rung
 *    2 and must NOT be claimed by rung 1;
 *  * an STL-27L **read at the D6's baud**, which is literally what rung 1 sees
 *    when it goes first — must be declined by rung 1.
 *
 * NO HARDWARE. The LD-series bytes are protocol-derived (the same fixtures
 * `Stl27lSignatureScannerTest` and `engine/tests/stl27l_packet_builder.h`
 * build, to the same offsets and the same CRC8), and the mis-clocked stream is
 * an explicit UART model. The owner's retest is the bench validation; item
 * 176(c)'s `[net-debug]` first-64-bytes line is how its result gets read.
 */
class SerialLadderByteExactTest {

    // ── byte-exact streams ──────────────────────────────────────────────────

    private fun d6Frame(samples: Int, startAngleDeg64: Int): ByteArray {
        val fsa = (startAngleDeg64 shl 1) or 0x01
        val lsa = (((startAngleDeg64 + samples * 4) % 23040) shl 1) or 0x01
        val out = ByteArray(D6SignatureScanner.HEADER_BYTES + 3 * samples)
        out[0] = 0xAA.toByte(); out[1] = 0x55.toByte()
        out[2] = 0x00; out[3] = samples.toByte()
        out[4] = (fsa and 0xFF).toByte(); out[5] = ((fsa ushr 8) and 0xFF).toByte()
        out[6] = (lsa and 0xFF).toByte(); out[7] = ((lsa ushr 8) and 0xFF).toByte()
        out[8] = 0x12; out[9] = 0x34
        for (i in 0 until samples) {
            val at = D6SignatureScanner.HEADER_BYTES + 3 * i
            out[at] = ((i * 3) and 0xFC).toByte()
            out[at + 1] = ((i * 5) and 0xFF).toByte()
            out[at + 2] = (10 + i).toByte()
        }
        return out
    }

    private fun d6Stream(frames: Int = 40): ByteArray {
        var out = ByteArray(0)
        for (n in 0 until frames) out += d6Frame(samples = 8, startAngleDeg64 = (n * 512) % 23040)
        return out
    }

    /** One 47-byte LD-series packet at the documented offsets, CRC8 poly 0x4D MSB-first, init 0. */
    private fun stlPacket(startAngleCentiDeg: Int, timestampMs: Int): ByteArray {
        val body = ArrayList<Byte>(46)
        body += 0x54.toByte(); body += 0x2C.toByte()
        fun le16(v: Int) {
            body += (v and 0xFF).toByte(); body += ((v ushr 8) and 0xFF).toByte()
        }
        le16(3_600); le16(startAngleCentiDeg)
        repeat(12) { le16(1_000 + startAngleCentiDeg % 97); body += 0x40.toByte() }
        le16((startAngleCentiDeg + 200) % 36_000); le16(timestampMs)
        val arr = body.toByteArray()
        var crc = 0
        for (b in arr) {
            crc = crc xor (b.toInt() and 0xFF)
            repeat(8) { crc = if (crc and 0x80 != 0) ((crc shl 1) xor 0x4D) and 0xFF else (crc shl 1) and 0xFF }
        }
        return arr + crc.toByte()
    }

    private fun stlStream(packets: Int = 200): ByteArray {
        var out = ByteArray(0)
        for (n in 0 until packets) out += stlPacket((n * 200) % 36_000, n)
        return out
    }

    /** The STL-27L's 921 600 bit train sampled at 230 400 — what rung 1 sees when it goes first. */
    private fun stlAtD6Baud(packets: Int = 900): ByteArray {
        val bits = ArrayList<Int>(packets * 47 * 10)
        for (n in 0 until packets) {
            for (b in stlPacket((n * 200) % 36_000, n)) {
                bits += 0
                for (i in 0 until 8) bits += (b.toInt() ushr i) and 1
                bits += 1
            }
        }
        val sampled = ArrayList<Int>(bits.size / 4)
        var i = 0
        while (i < bits.size) { sampled += bits[i]; i += 4 }
        val out = ByteArray(sampled.size / 10)
        for (k in out.indices) {
            var v = 0
            for (j in 0 until 8) v = v or ((sampled[k * 10 + 1 + j] and 1) shl j)
            out[k] = v.toByte()
        }
        return out
    }

    /** 4 KB reads, the size `D6SerialConnection` actually hands its callback. */
    private fun chunks(stream: ByteArray, size: Int = 4096): List<ByteArray> =
        (stream.indices step size).map { stream.copyOfRange(it, minOf(it + size, stream.size)) }

    // ── rungs backed by the real verdicts ───────────────────────────────────

    /**
     * What a rung would conclude if the port carried [atMyBaud] when opened at
     * MY baud, and nothing recognisable when opened at anyone else's.
     */
    private class ByteStreamStep(
        override val sensor: SensorType,
        private val stream: List<ByteArray>,
    ) : SerialLidarProbeStep {
        override suspend fun probe(devicePath: String): SerialProbeOutcome = when (sensor) {
            SensorType.COIN_D6 -> when (val r = D6AutoProbe.classify(devicePath, stream)) {
                is D6AutoProbeResult.Identified -> SerialProbeOutcome.Identified(r.devicePath, r.evidence)
                is D6AutoProbeResult.NotIdentified -> SerialProbeOutcome.Declined(r.evidence, r.sawPartialMatch)
                else -> SerialProbeOutcome.Unusable("unreachable in this fixture")
            }
            SensorType.STL27L -> when (val r = Stl27lAutoProbe.classify(devicePath, stream)) {
                is Stl27lAutoProbeResult.Identified -> SerialProbeOutcome.Identified(r.devicePath, r.evidence)
                is Stl27lAutoProbeResult.NotIdentified -> SerialProbeOutcome.Declined(r.evidence, r.sawPartialMatch)
                else -> SerialProbeOutcome.Unusable("unreachable in this fixture")
            }
            SensorType.MID360 -> SerialProbeOutcome.Unusable("not a serial sensor")
        }
    }

    /**
     * The production ladder shape — D6 at 230 400 then STL-27L at 921 600 —
     * with each rung shown the bytes IT would actually read at ITS baud.
     */
    private fun ladder(d6Sees: ByteArray, stlSees: ByteArray) = SerialLidarAutoDetector(
        attachedDevicePaths = { listOf(PORT) },
        ladder = listOf(
            ByteStreamStep(SensorType.COIN_D6, chunks(d6Sees)),
            ByteStreamStep(SensorType.STL27L, chunks(stlSees)),
        ),
    )

    // ── the three field cases ───────────────────────────────────────────────

    @Test
    fun `a COIN-D6 on the port is still found on rung one`() {
        // The regression that matters most in this round: every recorded scan
        // in captures/ came through this path and the probe just got stricter.
        val d = ladder(d6Sees = d6Stream(), stlSees = ByteArray(0))
        val found = runBlocking { d.detect() }!!
        assertEquals(SensorType.COIN_D6, found.sensor)
        assertEquals(PORT, found.transportHint)
    }

    @Test
    fun `an STL-27L on the port is found as an STL-27L — the owner's 2026-08-22 session`() {
        // Rung 1 opens at 230400 and sees the mis-clocked bit train; rung 2
        // opens at 921600 and sees clean LD packets. Before round 31 rung 1
        // claimed this port and the capture ran the D6 path.
        val d = ladder(d6Sees = stlAtD6Baud(), stlSees = stlStream())
        val found = runBlocking { d.detect() }!!
        assertEquals(SensorType.STL27L, found.sensor)
        assertEquals("LDROBOT STL-27L · 003", found.label)
        // No IMU, so the phone is the trajectory — the same true sentence the
        // D6's detection carries.
        assertTrue(found.detail!!.contains("phone-tracked"))
    }

    @Test
    fun `rung one refuses an STL-27L read at the D6's baud`() {
        val outcome = runBlocking {
            ByteStreamStep(SensorType.COIN_D6, chunks(stlAtD6Baud())).probe(PORT)
        }
        assertTrue("must decline, got $outcome", outcome is SerialProbeOutcome.Declined)
        // And the evidence line has to make the refusal legible in a field log:
        // it reports the AA 55 count the round-25 probe would have decided on
        // right beside the frame count this one decides on.
        assertTrue(outcome.evidence, outcome.evidence.contains("aa55="))
        assertTrue(outcome.evidence, outcome.evidence.contains("frames="))
    }

    @Test
    fun `rung two refuses a COIN-D6 read at the STL-27L's baud`() {
        // The direction round 25 already proved; kept so the pair is symmetric
        // and so a future CRC change cannot quietly loosen it.
        val outcome = runBlocking {
            ByteStreamStep(SensorType.STL27L, chunks(d6Stream())).probe(PORT)
        }
        assertTrue("must decline, got $outcome", outcome is SerialProbeOutcome.Declined)
    }

    @Test
    fun `an empty port is claimed by neither rung and is not called ambiguous`() {
        val d = ladder(d6Sees = ByteArray(0), stlSees = ByteArray(0))
        assertNull(runBlocking { d.detect() })
        assertNull(d.lastFailureMessage)
    }

    @Test
    fun `the STL-27L probe's CRC gate is what does the work, and the log shows it`() {
        // Header bytes with wrong CRCs: `54 2C` everywhere, not one valid
        // packet. `542c=N packets=0` is exactly the line a field log needs to
        // distinguish "the gate refused coincidences" from "nothing was there".
        val impostor = ByteArray(4096) { i -> if (i % 2 == 0) 0x54 else 0x2C }
        val r = Stl27lAutoProbe.classify(PORT, listOf(impostor))
        assertTrue(r is Stl27lAutoProbeResult.NotIdentified)
        assertTrue(r.evidence, r.evidence.contains("packets=0"))
        assertTrue("the coincidences must actually be present", r.evidence.contains("542c=2048"))
    }

    private companion object {
        const val PORT = "/dev/bus/usb/001/003"
    }
}
