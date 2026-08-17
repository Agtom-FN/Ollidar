package com.lidarscan.core.engine

/**
 * AUTO-DETECT (D6 wizard): does a chunk of freshly-read serial bytes look
 * like it came from a COIN-D6, without decoding anything?
 *
 * A CH340 adapter attaching does not by itself mean a D6 is on the other
 * end — `android/NOTES.md`'s "Field verdict" section on GNSS-over-USB-serial
 * records exactly this ambiguity: a Unicore UM982 GNSS eval board is the
 * same VID/PID class (`0x1A86`/`0x7523`) and is a real device this app's
 * users attach. Auto-connecting to *any* CH340 device on attach would just
 * as happily try to hand a UM982's NMEA text stream to the D6 driver.
 *
 * The D6's binary frames all start with the two-byte preamble `AA 55`
 * (`D6SerialConnection`'s write-side doc calls out "D6 start/stop bytes,
 * `AA 55 F0 0F`" for the same marker on the write path). A UM982 in its
 * default NMEA mode sends nothing but printable ASCII (`$GPGGA...`), which
 * cannot contain that byte pair. Seeing `AA 55` in a short read window is
 * therefore a cheap, honest signature check — not a full packet decode,
 * just "this is binary D6-shaped traffic, not GNSS text."
 *
 * Pure byte-array logic, no Android/USB dependency, so it is trivially
 * JVM-tested — the orchestration (open the port, read for a bounded window,
 * call this per chunk) is `com.lidarscan.app.usb.D6AutoProbe`, `:app`.
 */
object D6SignatureScanner {

    private const val PREAMBLE_0: Byte = 0xAA.toByte()
    private const val PREAMBLE_1: Byte = 0x55.toByte()

    /**
     * @param carry the last byte of the previous chunk fed to this scanner
     *   (or null for the first chunk / after a reset) — needed so a
     *   preamble split exactly across two USB reads (byte 0xAA at the end
     *   of one chunk, 0x55 at the start of the next) is still caught.
     * @param chunk the newly-read bytes. Only indices `[0, len)` are read —
     *   callers reusing a larger buffer across reads do not need to slice.
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
