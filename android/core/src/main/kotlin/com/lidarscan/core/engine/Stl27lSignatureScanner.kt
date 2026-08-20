package com.lidarscan.core.engine

/**
 * ROUND 25 item 119 — AUTO-DETECT: does a chunk of freshly-read serial bytes
 * look like it came from an **LDROBOT STL-27L**?
 *
 * The sibling of [D6SignatureScanner], and deliberately a stricter one.
 *
 * ## Why a two-byte header is NOT enough here, and that is the whole point
 *
 * [D6SignatureScanner] can get away with "did I see `AA 55` adjacent" because
 * of what it is *distinguishing from*: `android/NOTES.md`'s GNSS-over-USB gap
 * note says the realistic impostor on a CH340 port is a Unicore UM982 sending
 * printable ASCII, and ASCII cannot contain `AA 55`.
 *
 * The STL-27L probe does not get that luxury. Its sync is `54 2C` — two bytes
 * that are **both printable ASCII** (`'T'` and `','`), so a CSV-ish or NMEA-ish
 * text stream can produce the pair outright, and any binary stream produces it
 * about once every 65 536 byte offsets. At 921 600 baud a 1.5 s probe window is
 * ~138 000 bytes, so "I saw `54 2C`" is expected to be true roughly **twice per
 * probe against pure noise**. Declaring a match on that would not be a probe;
 * it would be a coin flip that says yes.
 *
 * So a match here means: header `0x54`, ver_len `0x2C`, **and** the packet's
 * own CRC8 over its preceding 46 bytes verifies. That is 8 more bits of
 * evidence — roughly one false accept per 16.7 million byte offsets, i.e.
 * ~0.008 expected across a whole probe window — and it is the strongest check
 * available without decoding geometry.
 *
 * The consequence is stated plainly rather than hidden: a packet whose 47
 * bytes are **not all present in this chunk** cannot be verified and therefore
 * does not count. That is fine in practice — the STL-27L emits ~1800 packets/s
 * and a USB-serial read returns hundreds of bytes, so complete packets land
 * inside chunks constantly — but a caller must not read "0 packets in this
 * chunk" as "not an STL-27L" after a single short read. See
 * `com.lidarscan.app.usb.Stl27lAutoProbe`, which accumulates across the window.
 *
 * ## Wire format (protocol-derived, UNVERIFIED)
 *
 * Fixed 47 bytes, little-endian, from the public LD-series references
 * (LD06 / LD19 / STL-27L share the framing byte for byte):
 *
 * ```
 *   off  size  field
 *    0    1    header  = 0x54
 *    1    1    ver_len = 0x2C   (low 5 bits = 12 measurement points)
 *    2    2    speed        [deg/s]
 *    4    2    start_angle  [0.01 deg]
 *    6   36    12 x { u16 distance_mm ; u8 intensity }
 *   42    2    end_angle    [0.01 deg]
 *   44    2    timestamp    [ms, wraps at 30000]
 *   46    1    crc8         over bytes [0, 46)
 * ```
 *
 * **No STL-27L hardware exists on this project.** Every byte offset above, and
 * the CRC parameters below, are protocol-derived and not observed. The tests
 * beside this file prove the scanner against synthetic packets built to that
 * same specification — they cannot prove the specification.
 *
 * ## Why the CRC is duplicated from C++
 *
 * `engine/src/drivers/stl27l/stl27l_parser.cpp` already implements this CRC,
 * and `engine/tests/stl27l_packet_builder.h` implements it a second time
 * bitwise as a cross-check. This is a **third** implementation, in Kotlin, for
 * one boring reason: the auto-detect probe runs in the app before any device
 * has been handed to the engine, on the JVM side of the JNI boundary, and the
 * two languages cannot share the routine. Duplication across a language
 * boundary is not the kind that rots quietly — a disagreement shows up as a
 * probe that never identifies a device the engine then parses happily, which
 * is a loud failure, and `Stl27lSignatureScannerTest` pins the same catalogue
 * check value the C++ tests pin.
 *
 * ## There is no native probe to bind, and that is on purpose
 *
 * The engine does have a real STL-27L probe in C++
 * (`discovery::ProbeSerialStl27l` / `discovery::Stl27lSniffer`, covered by its
 * own tests) — but **its C-ABI mirror was deliberately deferred**. Adding
 * `scan_probe_stl27l()` would have been a new exported symbol and therefore
 * ABI 13, and the entire discipline of item 119 is that the ABI stays at 12.
 * So this Kotlin scanner is not a stopgap for a binding somebody forgot: it is
 * the app's probe, the only one, and the same arrangement the D6 has always
 * had (`D6AutoProbe` has never called `scan_probe_d6` either). Nobody should
 * go hunting for a native call to replace it with.
 *
 * Pure byte-array logic, no Android/USB dependency, exactly like
 * [D6SignatureScanner]: the orchestration (open the port at 921 600, read for
 * a bounded window, feed this per chunk) lives in `:app`.
 */
object Stl27lSignatureScanner {

    /** First byte of every LD-series packet. Also ASCII `'T'` — see the class doc. */
    const val HEADER_BYTE: Byte = 0x54

    /** Version 1 (bits 5-7) with 12 measurement points (bits 0-4). Also ASCII `','`. */
    const val VER_LEN_BYTE: Byte = 0x2C

    /** Header .. crc8 inclusive. Fixed; the LD protocol has no variable-length frame. */
    const val PACKET_BYTES: Int = 47

    /** Index of the crc8 byte inside a packet; the CRC covers `[0, CRC_OFFSET)`. */
    const val CRC_OFFSET: Int = 46

    /**
     * CRC8 parameters: width=8, **poly 0x4D**, init 0x00, MSB-first (no input
     * or output reflection), no final XOR. The LDROBOT SDK ships this as a
     * 256-entry `CrcTable[]` used as `crc = CrcTable[crc ^ byte]`; that table
     * is exactly "XOR the byte in, then shift the register eight times through
     * 0x4D", which is what [crc8] does directly. A pasted 256-entry table is a
     * 256-line assertion nobody can check; a four-line shift loop is one
     * somebody can.
     */
    const val CRC_POLY: Int = 0x4D

    /**
     * CRC8 over `count` bytes of [bytes] starting at [from].
     *
     * Bitwise rather than table-driven on purpose: the table would have to be
     * trusted, this can be read. It is called at most a few hundred times per
     * probe window over 46 bytes each, which is nothing next to the USB read
     * that produced the bytes.
     */
    fun crc8(bytes: ByteArray, from: Int, count: Int): Byte {
        var crc = 0
        for (i in from until from + count) {
            crc = crc xor (bytes[i].toInt() and 0xFF)
            repeat(8) {
                crc = if (crc and 0x80 != 0) ((crc shl 1) xor CRC_POLY) and 0xFF else (crc shl 1) and 0xFF
            }
        }
        return crc.toByte()
    }

    /**
     * How many **complete, CRC-valid** STL-27L packets are wholly contained in
     * this chunk.
     *
     * Counting rather than a bare boolean because one accepted packet is
     * weaker evidence than it looks (see the class doc), and the caller is the
     * right place to decide how many are enough —
     * `com.lidarscan.app.usb.Stl27lAutoProbe` wants four, mirroring the
     * engine's own `discovery::Stl27lSniffer::kPacketsToIdentify`.
     *
     * After an accepted packet the scan resumes **past** it rather than at the
     * next byte, so a genuine back-to-back stream counts 1 packet per 47 bytes
     * instead of double-counting a `54 2C` that happens to sit inside a
     * distance field.
     *
     * @param carry the last byte of the previous chunk (or null for the first
     *   chunk / after a reset). Only one byte, matching [D6SignatureScanner]'s
     *   mechanism, and it buys exactly one thing: a packet whose `0x54` was the
     *   final byte of the previous read is still verifiable, because its
     *   remaining 46 bytes are all in this chunk. A packet split any *earlier*
     *   than that is simply not counted here — the probe sees the next one.
     * @param chunk the newly-read bytes; only `[0, len)` is read, so callers
     *   reusing a larger buffer across reads need not slice.
     */
    fun validPacketCount(carry: Byte?, chunk: ByteArray, len: Int): Int {
        if (len <= 0) return 0
        var count = 0

        // The carry case: 0x54 was the last byte of the previous chunk, so the
        // packet is [carry] + chunk[0 .. 45]. The CRC covers the carry byte
        // plus chunk[0 .. 44] and is compared against chunk[45].
        if (carry == HEADER_BYTE && chunk[0] == VER_LEN_BYTE && len >= PACKET_BYTES - 1) {
            var crc = crc8(byteArrayOf(carry), 0, 1).toInt() and 0xFF
            // Continue the register across the chunk's first 45 bytes.
            crc = continueCrc8(crc, chunk, 0, CRC_OFFSET - 1)
            if (crc.toByte() == chunk[CRC_OFFSET - 1]) count++
        }

        var i = 0
        while (i + PACKET_BYTES <= len) {
            if (chunk[i] == HEADER_BYTE &&
                chunk[i + 1] == VER_LEN_BYTE &&
                crc8(chunk, i, CRC_OFFSET) == chunk[i + CRC_OFFSET]
            ) {
                count++
                i += PACKET_BYTES
            } else {
                i++
            }
        }
        return count
    }

    /**
     * True when at least one complete, CRC-valid packet is in this chunk.
     *
     * The same shape as [D6SignatureScanner.containsSignature] so the two
     * probes read alike — but read [validPacketCount]'s doc before using it as
     * a one-shot verdict, because for this protocol one packet is deliberately
     * not the bar the probe uses.
     */
    fun containsSignature(carry: Byte?, chunk: ByteArray, len: Int): Boolean =
        validPacketCount(carry, chunk, len) > 0

    /** The byte to remember as [validPacketCount]'s next `carry`, i.e. the chunk's last byte. */
    fun lastByteOrNull(chunk: ByteArray, len: Int): Byte? = if (len > 0) chunk[len - 1] else null

    /** Feeds more bytes into an already-running CRC register. */
    private fun continueCrc8(start: Int, bytes: ByteArray, from: Int, count: Int): Int {
        var crc = start
        for (i in from until from + count) {
            crc = crc xor (bytes[i].toInt() and 0xFF)
            repeat(8) {
                crc = if (crc and 0x80 != 0) ((crc shl 1) xor CRC_POLY) and 0xFF else (crc shl 1) and 0xFF
            }
        }
        return crc
    }
}
