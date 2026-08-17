package com.lidarscan.core.engine

import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** AUTO-DETECT (D6 wizard): [D6SignatureScanner] against synthetic byte chunks. */
class D6SignatureScannerTest {

    private fun bytes(vararg values: Int): ByteArray = ByteArray(values.size) { values[it].toByte() }

    @Test
    fun `an empty chunk has no signature`() {
        assertFalse(D6SignatureScanner.containsSignature(null, ByteArray(0), 0))
    }

    @Test
    fun `a chunk with the preamble in the middle is detected`() {
        val chunk = bytes(0x10, 0x20, 0xAA, 0x55, 0x30, 0x40)
        assertTrue(D6SignatureScanner.containsSignature(null, chunk, chunk.size))
    }

    @Test
    fun `a chunk with the preamble at the very start is detected`() {
        val chunk = bytes(0xAA, 0x55, 0x00, 0x00)
        assertTrue(D6SignatureScanner.containsSignature(null, chunk, chunk.size))
    }

    @Test
    fun `a chunk with no preamble is not detected`() {
        val chunk = bytes(0xAA, 0x00, 0x55, 0x11) // AA and 55 present but never adjacent
        assertFalse(D6SignatureScanner.containsSignature(null, chunk, chunk.size))
    }

    @Test
    fun `ASCII-only NMEA-shaped bytes (a GNSS receiver, not a D6) never match`() {
        val chunk = "\$GPGGA,123519,,,,,0,00,,,M,,M,,*".toByteArray(Charsets.US_ASCII)
        assertFalse(D6SignatureScanner.containsSignature(null, chunk, chunk.size))
    }

    @Test
    fun `a preamble split across two reads is caught via the carry byte`() {
        val firstChunk = bytes(0x01, 0x02, 0xAA)
        assertFalse(D6SignatureScanner.containsSignature(null, firstChunk, firstChunk.size))
        val carry = D6SignatureScanner.lastByteOrNull(firstChunk, firstChunk.size)

        val secondChunk = bytes(0x55, 0x03, 0x04)
        assertTrue(D6SignatureScanner.containsSignature(carry, secondChunk, secondChunk.size))
    }

    @Test
    fun `only the first len bytes of a larger reused buffer are considered`() {
        // Simulates a reused ByteArray/ByteBuffer where only [0, len) was
        // actually filled by the latest read; the AA55 sits past `len`.
        val buffer = bytes(0x01, 0x02, 0x00, 0x00, 0xAA, 0x55)
        assertFalse(D6SignatureScanner.containsSignature(null, buffer, 3))
    }

    @Test
    fun `lastByteOrNull is null for an empty chunk`() {
        assertNull(D6SignatureScanner.lastByteOrNull(ByteArray(0), 0))
    }
}
