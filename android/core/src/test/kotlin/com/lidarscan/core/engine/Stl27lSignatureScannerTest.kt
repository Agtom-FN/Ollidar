package com.lidarscan.core.engine

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 25 item 119 — [Stl27lSignatureScanner] against synthetic LD-series
 * packets.
 *
 * Every packet here is assembled **byte by byte at the documented offsets**
 * and its CRC is computed by [independentCrc8] — a second implementation
 * written out in this file — rather than by calling
 * [Stl27lSignatureScanner.crc8]. That is the same discipline
 * `engine/tests/stl27l_packet_builder.h` follows on the C++ side, and for the
 * same reason: a fixture that computes its checksum with the routine under
 * test proves the routine agrees with itself, which is not a fact anybody
 * needed.
 *
 * What these tests CANNOT do is prove the protocol. No STL-27L hardware exists
 * on this project; the offsets and the CRC parameters are protocol-derived
 * from the public LD-series references. If the real device disagrees, it will
 * disagree with the fixtures below and with the engine's C++ parser at the
 * same time — which is the loud failure we want, not a silent one.
 */
class Stl27lSignatureScannerTest {

    // --- an independent CRC8, poly 0x4D, MSB-first, init 0x00, no final XOR ---
    private fun independentCrc8(bytes: ByteArray): Byte {
        var crc = 0
        for (b in bytes) {
            crc = crc xor (b.toInt() and 0xFF)
            for (bit in 0 until 8) {
                crc = if (crc and 0x80 != 0) ((crc shl 1) xor 0x4D) and 0xFF else (crc shl 1) and 0xFF
            }
        }
        return crc.toByte()
    }

    /**
     * One 47-byte packet at the documented offsets:
     * `0 header | 1 ver_len | 2..3 speed | 4..5 start_angle |
     *  6..41 12 x (u16 distance, u8 intensity) | 42..43 end_angle |
     *  44..45 timestamp | 46 crc8`.
     */
    private fun packet(
        startAngleCentiDeg: Int = 0,
        endAngleCentiDeg: Int = 200,
        timestampMs: Int = 0,
        distanceMm: Int = 1_000,
        intensity: Int = 0x40,
        header: Int = 0x54,
        verLen: Int = 0x2C,
        corruptCrc: Boolean = false,
    ): ByteArray {
        val body = ArrayList<Byte>(47)
        body += header.toByte()
        body += verLen.toByte()
        fun le16(v: Int) {
            body += (v and 0xFF).toByte()
            body += ((v ushr 8) and 0xFF).toByte()
        }
        le16(3_600) // speed, deg/s == 10 Hz
        le16(startAngleCentiDeg)
        repeat(12) {
            le16(distanceMm)
            body += intensity.toByte()
        }
        le16(endAngleCentiDeg)
        le16(timestampMs)
        val bytes = body.toByteArray()
        check(bytes.size == 46) { "body must be 46 bytes, was ${bytes.size}" }
        val crc = independentCrc8(bytes)
        return bytes + if (corruptCrc) (crc.toInt() xor 0xFF).toByte() else crc
    }

    private fun bytes(vararg values: Int): ByteArray = ByteArray(values.size) { values[it].toByte() }

    // --- the CRC itself ------------------------------------------------------

    /**
     * The LDROBOT SDK's published `CrcTable[]` begins
     * `00 4d 9a d7 79 34 e3 ae f2 bf 68 25 8b c6 11 5c`, and
     * `table[i] == crc8([i])` because the table IS the register after one byte
     * from init 0x00. The engine's `tests/test_stl27l.cpp` pins the same
     * prefix; this is the Kotlin half of that agreement.
     */
    @Test
    fun `crc8 of a single byte reproduces the vendor table's first sixteen entries`() {
        val expected = bytes(
            0x00, 0x4D, 0x9A, 0xD7, 0x79, 0x34, 0xE3, 0xAE,
            0xF2, 0xBF, 0x68, 0x25, 0x8B, 0xC6, 0x11, 0x5C,
        )
        val actual = ByteArray(16) { i -> Stl27lSignatureScanner.crc8(bytes(i), 0, 1) }
        assertArrayEquals(expected, actual)
    }

    @Test
    fun `crc8 agrees with the independent bitwise implementation over a whole packet`() {
        val p = packet()
        assertEquals(independentCrc8(p.copyOfRange(0, 46)), Stl27lSignatureScanner.crc8(p, 0, 46))
    }

    // --- framing -------------------------------------------------------------

    @Test
    fun `an empty chunk has no signature`() {
        assertFalse(Stl27lSignatureScanner.containsSignature(null, ByteArray(0), 0))
        assertEquals(0, Stl27lSignatureScanner.validPacketCount(null, ByteArray(0), 0))
    }

    @Test
    fun `a byte-exact packet at the start of the chunk is detected`() {
        val chunk = packet()
        assertTrue(Stl27lSignatureScanner.containsSignature(null, chunk, chunk.size))
    }

    @Test
    fun `a packet preceded by garbage is still found`() {
        val chunk = bytes(0x01, 0x02, 0x54, 0x03) + packet()
        assertEquals(1, Stl27lSignatureScanner.validPacketCount(null, chunk, chunk.size))
    }

    @Test
    fun `back-to-back packets are counted once each, not once per byte offset`() {
        val chunk = packet(startAngleCentiDeg = 0) +
            packet(startAngleCentiDeg = 200, endAngleCentiDeg = 400) +
            packet(startAngleCentiDeg = 400, endAngleCentiDeg = 600)
        assertEquals(3, Stl27lSignatureScanner.validPacketCount(null, chunk, chunk.size))
    }

    // --- the CRC gate, which is the entire point of this scanner --------------

    @Test
    fun `a header pair with a bad CRC is rejected`() {
        val chunk = packet(corruptCrc = true)
        assertFalse(Stl27lSignatureScanner.containsSignature(null, chunk, chunk.size))
    }

    @Test
    fun `a bare 54 2C header with too few bytes behind it is not a match`() {
        // 0x54 0x2C is two printable ASCII characters ('T' and ','); on its own
        // it is the weakest possible evidence, and the scanner refuses to act
        // on it until it can check the CRC.
        val truncated = packet().copyOfRange(0, 46)
        assertFalse(Stl27lSignatureScanner.containsSignature(null, truncated, truncated.size))

        val justTheHeader = bytes(0x54, 0x2C)
        assertFalse(Stl27lSignatureScanner.containsSignature(null, justTheHeader, justTheHeader.size))
    }

    @Test
    fun `a wrong ver_len byte is rejected even with a correct CRC`() {
        val chunk = packet(verLen = 0x2B)
        assertFalse(Stl27lSignatureScanner.containsSignature(null, chunk, chunk.size))
    }

    @Test
    fun `only the first len bytes of a larger reused buffer are considered`() {
        val buffer = ByteArray(16) + packet()
        assertFalse(Stl27lSignatureScanner.containsSignature(null, buffer, 16))
        assertTrue(Stl27lSignatureScanner.containsSignature(null, buffer, buffer.size))
    }

    // --- the split across two USB reads --------------------------------------

    @Test
    fun `a packet whose header byte ended the previous read is caught via the carry`() {
        val p = packet()
        val firstChunk = bytes(0x11, 0x22) + p.copyOfRange(0, 1) // ends on 0x54
        assertFalse(Stl27lSignatureScanner.containsSignature(null, firstChunk, firstChunk.size))

        val carry = Stl27lSignatureScanner.lastByteOrNull(firstChunk, firstChunk.size)
        assertEquals(0x54.toByte(), carry)

        val secondChunk = p.copyOfRange(1, p.size) // 46 bytes, starting at ver_len
        assertTrue(Stl27lSignatureScanner.containsSignature(carry, secondChunk, secondChunk.size))
    }

    @Test
    fun `a carried header whose packet fails the CRC is still rejected`() {
        val p = packet(corruptCrc = true)
        val carry = p[0]
        val secondChunk = p.copyOfRange(1, p.size)
        assertFalse(Stl27lSignatureScanner.containsSignature(carry, secondChunk, secondChunk.size))
    }

    @Test
    fun `lastByteOrNull is null for an empty chunk`() {
        assertNull(Stl27lSignatureScanner.lastByteOrNull(ByteArray(0), 0))
    }

    // --- the two probes must never claim each other's hardware ----------------

    @Test
    fun `a COIN-D6 stream is not mistaken for an STL-27L`() {
        // A plausible D6 chunk: the AA 55 preamble and a run of binary payload,
        // long enough (256 bytes) that the STL scanner's 47-byte window really
        // sweeps it rather than running off the end after two offsets.
        val d6Ints = (0 until 32).flatMap { n ->
            listOf(0xAA, 0x55, n, 0x00, (n * 7) and 0xFF, 0x00, 0x00, 0x00)
        }
        val d6Chunk = ByteArray(d6Ints.size) { d6Ints[it].toByte() }
        assertFalse(Stl27lSignatureScanner.containsSignature(null, d6Chunk, d6Chunk.size))
        assertTrue(D6SignatureScanner.containsSignature(null, d6Chunk, d6Chunk.size))
    }

    @Test
    fun `an STL-27L stream is not mistaken for a COIN-D6`() {
        val stlChunk = packet(startAngleCentiDeg = 0) +
            packet(startAngleCentiDeg = 200, endAngleCentiDeg = 400)
        assertTrue(Stl27lSignatureScanner.containsSignature(null, stlChunk, stlChunk.size))
        assertFalse(D6SignatureScanner.containsSignature(null, stlChunk, stlChunk.size))
    }

    @Test
    fun `ASCII-only NMEA-shaped bytes (a GNSS receiver) never match`() {
        // Contains 'T' (0x54) followed by ',' (0x2C) on purpose — the exact
        // coincidence a header-only probe would have fallen for — and is long
        // enough that the CRC, not the end of the buffer, is what rejects it.
        val line = "\$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n"
        val chunk = line.repeat(8).toByteArray(Charsets.US_ASCII)
        assertTrue("the 54 2C coincidence must actually be present", chunk.size > 47)
        assertFalse(Stl27lSignatureScanner.containsSignature(null, chunk, chunk.size))
    }
}
