package com.lidarscan.app.usb

import com.lidarscan.core.engine.SerialLidarBaud
import com.lidarscan.core.engine.SerialModemLines
import com.lidarscan.core.engine.SilentLineFallback
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.random.Random

/**
 * ROUND 32 item 178 — **the silent line, byte-exact, through the real ladder.**
 *
 * There is still no STL-27L on this machine, so everything here is synthetic —
 * but it is synthetic *input* to the production decision, not a re-implementation
 * of it. [Stl27lAutoProbe.runLadder] is the function the phone runs; the only
 * thing these tests replace is the USB reader that feeds it, which is round 31's
 * own standard for this file.
 *
 * The fixtures are the owner's 2026-08-22 numbers: a 921 600 window that
 * delivers sixteen bytes of nothing while the sensor is visibly spinning.
 */
class Stl27lSilentLineTest {

    // ── fixtures ───────────────────────────────────────────────────────────

    /** One 47-byte LD-series packet, CRC8 poly 0x4D MSB-first, init 0 — as `SerialLadderByteExactTest` builds it. */
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

    private fun stlStream(packets: Int = 60): List<ByteArray> =
        (0 until packets).map { stlPacket((it * 200) % 36_000, it) }

    /**
     * The owner's line: sixteen bytes, mostly `00`, no `54 2C` anywhere.
     *
     * Deterministic — a seeded generator rather than `Random()` — because a
     * probe test whose input changes per run cannot pin a threshold.
     */
    private fun silentWindow(bytes: Int = 16): List<ByteArray> {
        val rng = Random(178)
        return listOf(ByteArray(bytes) { if (rng.nextInt(8) == 0) rng.nextInt(256).toByte() else 0 })
    }

    /** A LOUD window of the wrong protocol: something is streaming, it is just not an STL-27L. */
    private fun noisyWindow(bytes: Int = 34_500): List<ByteArray> {
        val rng = Random(31)
        return listOf(ByteArray(bytes) { rng.nextInt(256).toByte() })
    }

    private fun ladder(windows: Map<Int, List<ByteArray>>): Pair<Stl27lAutoProbeResult, List<Int>> {
        val tried = mutableListOf<Int>()
        val result = runBlocking {
            Stl27lAutoProbe.runLadder("/dev/bus/usb/001/002") { baud ->
                tried += baud
                Stl27lAutoProbe.countWindow(baud, windows[baud] ?: emptyList())
            }
        }
        return result to tried
    }

    // ── the case the round exists for ─────────────────────────────────────

    /**
     * Silent at the datasheet's rate, alive at the family's. The session must
     * adopt 230 400 **and carry it**, because the engine derives per-point
     * timing from the number it is told.
     */
    @Test
    fun `a silent 921600 falls back and the adopted rate travels with the verdict`() {
        val (result, tried) = ladder(
            mapOf(
                SerialLidarBaud.STL27L to silentWindow(),
                230_400 to stlStream(),
            ),
        )
        val identified = result as Stl27lAutoProbeResult.Identified
        assertEquals(230_400, identified.baud)
        assertEquals(listOf(SerialLidarBaud.STL27L, 230_400), tried)
        assertTrue(identified.evidence, identified.evidence.contains("adopted 230400"))
        assertTrue(identified.evidence, identified.evidence.contains("921600:bytes=16"))
    }

    /** 460 800 is reached only when 230 400 has also been silent. */
    @Test
    fun `the ladder walks to the second fallback rate when the first is silent too`() {
        val (result, tried) = ladder(
            mapOf(
                SerialLidarBaud.STL27L to silentWindow(),
                230_400 to silentWindow(20),
                460_800 to stlStream(),
            ),
        )
        assertEquals(460_800, (result as Stl27lAutoProbeResult.Identified).baud)
        assertEquals(listOf(SerialLidarBaud.STL27L, 230_400, 460_800), tried)
    }

    /** A healthy sensor at its documented rate never sees the fallback ladder at all. */
    @Test
    fun `a normal STL-27L identifies at 921600 and no other rate is opened`() {
        val (result, tried) = ladder(mapOf(SerialLidarBaud.STL27L to stlStream()))
        val identified = result as Stl27lAutoProbeResult.Identified
        assertEquals(SerialLidarBaud.STL27L, identified.baud)
        assertEquals("re-clocking a working link is exactly what this must not do", listOf(921_600), tried)
        // …and its evidence line is round 31's, unchanged in shape.
        assertTrue(identified.evidence, identified.evidence.startsWith("bytes="))
        assertFalse(identified.evidence.contains("adopted"))
    }

    /**
     * **The COIN-D6 path pays nothing.** A loud window carrying the wrong
     * protocol has answered the question — something is streaming and it is not
     * this — so the port is left alone. This is the guard that keeps item
     * 178(b) off the one path every recorded scan came through.
     */
    @Test
    fun `a loud line carrying the wrong protocol is declined without re-clocking`() {
        val (result, tried) = ladder(mapOf(SerialLidarBaud.STL27L to noisyWindow()))
        assertTrue(result is Stl27lAutoProbeResult.NotIdentified)
        assertEquals(listOf(921_600), tried)
    }

    /** Silent everywhere: nothing is claimed, and the log says which rates were asked. */
    @Test
    fun `a line that is silent at every rate is declined with all three counted`() {
        val (result, tried) = ladder(
            mapOf(
                SerialLidarBaud.STL27L to silentWindow(),
                230_400 to silentWindow(22),
                460_800 to silentWindow(30),
            ),
        )
        val declined = result as Stl27lAutoProbeResult.NotIdentified
        assertEquals(listOf(921_600, 230_400, 460_800), tried)
        assertFalse(declined.sawPartialMatch)
        for (baud in listOf(921_600, 230_400, 460_800)) {
            assertTrue(declined.evidence, declined.evidence.contains("$baud:bytes="))
        }
        assertTrue(declined.evidence, declined.evidence.contains("no fallback rate answered"))
    }

    /** A rate that will not open does not end the ladder — the next one is still asked. */
    @Test
    fun `a fallback rate that cannot be opened is skipped, not fatal`() {
        val tried = mutableListOf<Int>()
        val result = runBlocking {
            Stl27lAutoProbe.runLadder("/dev/bus/usb/001/002") { baud ->
                tried += baud
                when (baud) {
                    SerialLidarBaud.STL27L -> Stl27lAutoProbe.countWindow(baud, silentWindow())
                    230_400 -> Stl27lAutoProbe.WindowResult.Failed("Could not claim interface 0")
                    else -> Stl27lAutoProbe.countWindow(baud, stlStream())
                }
            }
        }
        assertEquals(460_800, (result as Stl27lAutoProbeResult.Identified).baud)
        assertEquals(listOf(921_600, 230_400, 460_800), tried)
    }

    /** A port that will not open **at all** is an error, exactly as before. */
    @Test
    fun `a port that refuses the first open is still an error`() {
        val result = runBlocking {
            Stl27lAutoProbe.runLadder("/dev/bus/usb/001/002") {
                Stl27lAutoProbe.WindowResult.Failed("USB permission denied")
            }
        }
        assertEquals("USB permission denied", (result as Stl27lAutoProbeResult.Error).message)
    }

    /**
     * ROUND 31's ambiguity input is unchanged: only the datasheet rate's
     * partial match counts. A fallback rate finding fragments must not make the
     * port look "ambiguous" — it makes it look like an STL-27L.
     */
    @Test
    fun `only the datasheet rate contributes to the ambiguity verdict`() {
        // Three valid packets at 921 600: under the bar, and a real partial.
        val (partial, _) = ladder(mapOf(SerialLidarBaud.STL27L to stlStream(3)))
        assertTrue((partial as Stl27lAutoProbeResult.NotIdentified).sawPartialMatch)

        // Silent at 921 600, three packets at 230 400: not a partial match at
        // the rate ambiguity is judged on.
        val (fallbackPartial, _) = ladder(
            mapOf(
                SerialLidarBaud.STL27L to silentWindow(),
                230_400 to stlStream(3),
                460_800 to silentWindow(18),
            ),
        )
        assertFalse((fallbackPartial as Stl27lAutoProbeResult.NotIdentified).sawPartialMatch)
    }

    // ── item 178(a): the two setter calls ─────────────────────────────────

    /**
     * The fix, at the wire.
     *
     * `D6UsbConnectionRegistry.open` ended with `port.setDTR(false)` and never
     * mentioned RTS, so every port this app has opened has had both control
     * lines low — invisible on the COIN-D6, and the whole failure on a CH340
     * dev-kit board that gates the sensor's output on them.
     */
    @Test
    fun `the STL-27L open asserts both modem lines and the D6 open asserts neither`() {
        val stlPort = RecordingPort()
        assertTrue(D6UsbConnectionRegistry.applyModemLines(stlPort, SerialModemLines.STL27L))
        assertEquals(listOf("dtr=true", "rts=true"), stlPort.calls)

        val d6Port = RecordingPort()
        assertTrue(D6UsbConnectionRegistry.applyModemLines(d6Port, SerialModemLines.COIN_D6))
        assertEquals(
            "the D6's state is what a hundred field captures were taken with",
            listOf("dtr=false", "rts=false"),
            d6Port.calls,
        )
    }

    /**
     * A driver that refuses a control line must not cost us the port.
     *
     * Neither line is required by a device that ignores them; some CDC-ACM and
     * FTDI paths throw on a `setRTS` they cannot perform. Losing a working port
     * in order to insist on a signal the device does not read would be a worse
     * bug than the one this round fixes.
     */
    @Test
    fun `a driver that refuses the lines reports false and does not throw`() {
        val port = RecordingPort(failOnRts = true)
        assertFalse(D6UsbConnectionRegistry.applyModemLines(port, SerialModemLines.STL27L))
        // DTR still went out — the failure is recorded, not rolled back.
        assertEquals(listOf("dtr=true"), port.calls)
    }

    /**
     * The bare minimum of `UsbSerialPort` that [D6UsbConnectionRegistry.applyModemLines]
     * touches. Everything else throws, which is the point: a seam that needed
     * more of the USB stack than this would not be a seam.
     */
    private class RecordingPort(private val failOnRts: Boolean = false) :
        com.hoho.android.usbserial.driver.UsbSerialPort by unreachable() {

        val calls = mutableListOf<String>()

        override fun setDTR(value: Boolean) {
            calls += "dtr=$value"
        }

        override fun setRTS(value: Boolean) {
            if (failOnRts) throw java.io.IOException("setRTS unsupported")
            calls += "rts=$value"
        }

        companion object {
            fun unreachable(): com.hoho.android.usbserial.driver.UsbSerialPort =
                java.lang.reflect.Proxy.newProxyInstance(
                    com.hoho.android.usbserial.driver.UsbSerialPort::class.java.classLoader,
                    arrayOf(com.hoho.android.usbserial.driver.UsbSerialPort::class.java),
                ) { _, method, _ ->
                    throw UnsupportedOperationException("${method.name} is not part of this seam")
                } as com.hoho.android.usbserial.driver.UsbSerialPort
        }
    }
}
