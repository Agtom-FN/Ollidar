package com.lidarscan.app

/**
 * ROUND 8 — a COIN-D6 packet builder for the instrumentation tests.
 *
 * This is the Kotlin twin of `engine/tests/packet_builder.h`, and it exists for
 * one reason: the emulator tests need the app to receive bytes that the real
 * `d6::Parser` accepts, and there is no way to reach the C++ builder from an
 * instrumentation test. It is a deliberate second implementation of the same
 * on-wire format, which is also a cross-check — if the two ever disagree the
 * emulator tests stop parsing, loudly.
 *
 * The checksum is the vendor SDK's byte-stream state machine
 * (`Lidar_Data_Processing::waitPackage`), replayed exactly as the C++ builder
 * replays it — NOT a call into the engine's own `d6::checksum()`, for the same
 * reason that file gives: a builder that shares the implementation it is
 * feeding proves nothing about the format.
 *
 * On-wire layout (S1 REPORT.md §1):
 *
 * ```
 *   0  0xAA  0x55        PH
 *   2  (M<<1)|start      CT: scan frequency and the start-packet flag
 *   3  LSN                number of 3-byte samples that follow
 *   4  FSA (LE, u16)      first sample angle, deg*64 << 1 | check bit
 *   6  LSA (LE, u16)      last  sample angle, same encoding
 *   8  CS  (LE, u16)      checksum
 *  10  samples...         3 bytes each
 * ```
 */
object D6SyntheticPackets {

    /** deg -> the on-wire 16-bit FSA/LSA field, check bit set. */
    private fun encodeAngle(deg: Double): Int {
        var d64 = Math.round(deg * 64.0).toInt()
        while (d64 < 0) d64 += 360 * 64
        while (d64 >= 360 * 64) d64 -= 360 * 64
        return ((d64 and 0xFFFF) shl 1 or 0x0001) and 0xFFFF
    }

    /**
     * One 3-byte sample, encoded so the documented formulas invert it:
     * `Distance = Si_H*64 + (Si_2nd>>2)`, `Intensity = (Si_2nd&3)*64 + (Si_L>>2)`.
     */
    private fun encodeSample(distanceMm: Int, intensity: Int, out: ByteArray, at: Int) {
        val d = distanceMm and 0x3FFF
        out[at + 2] = (d shr 6).toByte()
        out[at + 1] = (((d and 0x3F) shl 2) or ((intensity shr 6) and 0x03)).toByte()
        out[at] = (((intensity and 0x3F) shl 2)).toByte()
    }

    private fun vendorChecksum(pkt: ByteArray, lsn: Int): Int {
        var cs = 0x55AA
        val sampleNumAndCt = (pkt[2].toInt() and 0xFF) or ((pkt[3].toInt() and 0xFF) shl 8)
        val fsa = (pkt[4].toInt() and 0xFF) or ((pkt[5].toInt() and 0xFF) shl 8)
        val lsa = (pkt[6].toInt() and 0xFF) or ((pkt[7].toInt() and 0xFF) shl 8)
        cs = cs xor fsa
        var latch = 0
        for (i in 0 until lsn * 3) {
            val b = pkt[10 + i].toInt() and 0xFF
            when (i % 3) {
                0 -> {
                    latch = b
                    cs = cs xor b
                }
                1 -> latch = b
                else -> {
                    latch = (latch + b * 0x100) and 0xFFFF
                    cs = cs xor latch
                }
            }
        }
        cs = cs xor sampleNumAndCt
        cs = cs xor lsa
        return cs and 0xFFFF
    }

    fun packet(
        firstAngleDeg: Double,
        lastAngleDeg: Double,
        distancesMm: IntArray,
        intensity: Int = 140,
        startPacket: Boolean = false,
        scanFreq: Int = 10,
    ): ByteArray {
        val lsn = distancesMm.size
        val pkt = ByteArray(10 + lsn * 3)
        pkt[0] = 0xAA.toByte()
        pkt[1] = 0x55.toByte()
        pkt[2] = (((scanFreq shl 1) or if (startPacket) 1 else 0) and 0xFF).toByte()
        pkt[3] = lsn.toByte()
        val fsa = encodeAngle(firstAngleDeg)
        val lsa = encodeAngle(lastAngleDeg)
        pkt[4] = (fsa and 0xFF).toByte()
        pkt[5] = (fsa shr 8).toByte()
        pkt[6] = (lsa and 0xFF).toByte()
        pkt[7] = (lsa shr 8).toByte()
        for (i in distancesMm.indices) encodeSample(distancesMm[i], intensity, pkt, 10 + i * 3)
        val cs = vendorChecksum(pkt, lsn)
        pkt[8] = (cs and 0xFF).toByte()
        pkt[9] = (cs shr 8).toByte()
        return pkt
    }

    /**
     * A full revolution at a constant range: a start packet followed by
     * [packets] point packets of [perPacket] samples, spread over 360 degrees.
     *
     * A constant range is exactly what this test wants. It makes the RAW cloud a
     * perfect flat disc in the sensor's own scan plane (`z == 0` for every
     * point, by construction) and the RESOLVED cloud a cylinder swept along the
     * walk — so the two are trivially distinguishable by their z extent, which
     * is the property owner item 27 is about.
     */
    fun revolution(
        packets: Int = 18,
        perPacket: Int = 20,
        distanceMm: Int = 2000,
    ): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        out.write(packet(0.0, 0.0, intArrayOf(distanceMm), startPacket = true))
        val step = 360.0 / (packets * perPacket)
        for (k in 0 until packets) {
            val a0 = step * (k * perPacket)
            out.write(
                packet(
                    firstAngleDeg = a0,
                    lastAngleDeg = a0 + step * (perPacket - 1),
                    distancesMm = IntArray(perPacket) { distanceMm },
                ),
            )
        }
        return out.toByteArray()
    }
}
