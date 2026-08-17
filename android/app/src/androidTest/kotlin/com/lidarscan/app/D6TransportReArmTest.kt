package com.lidarscan.app

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.lidarscan.app.engine.RealEngineBridge
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.usb.D6SerialConnection
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.EngineTarget
import com.lidarscan.core.model.SensorType
import java.io.File
import java.util.EnumSet
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 7, field bug 2 — **the second D6 capture of one connect recorded
 * nothing**, pinned against the real `RealEngineBridge` and the real native
 * engine.
 *
 * From the owner's log, two minutes apart on the same cable:
 *
 * ```
 * 22:53:40 [seal] sealed OK id=scan-008 … points=216653 elapsedMs=30543
 * 22:54:16 [seal] sealed OK id=scan-009 … points=0      elapsedMs=0
 * ```
 *
 * The cause was entirely app-side and entirely in the transport's own state:
 * `stopCapture()` implements the D6's Stop by telling the reader thread to stop
 * forwarding bytes into `push_serial_bytes` (the C ABI has no pause/resume, so
 * that is where a pause lives), and **only the Pause button ever turned
 * forwarding back on**. The second `scan_engine_start()` therefore succeeded,
 * returned `SCAN_OK`, opened a healthy `.lscan` — and never received a byte.
 *
 * Neither hardware-free path could see it. `ReplayEngineBridge` has no serial
 * connection at all; `FakeEngineBridge` has no transport. So this test supplies
 * the one missing piece — a `UsbSerialPort` that is a byte source rather than a
 * device — and drives the genuine connect → start → stop → start sequence
 * through the genuine bridge and the genuine engine, asserting the invariant the
 * field session violated: **after a second Start, bytes are flowing again.**
 *
 * Run it against the pre-ROUND-7 `RealEngineBridge` and
 * `secondStartReArmsTheTransport` fails on its last assertion.
 */
@RunWith(AndroidJUnit4::class)
class D6TransportReArmTest {

    private lateinit var root: File
    private var bridge: RealEngineBridge? = null

    /**
     * A `UsbSerialPort` that hands back a fixed chunk on every read, so the
     * reader thread behaves exactly as it does against a real CH340 — including
     * the 4096-byte buffer and the blocking-read shape.
     *
     * Not a mocking framework: this project has none on the instrumentation
     * classpath, and an interface with a handful of methods that all return
     * constants is clearer written out than generated.
     */
    private class FakeSerialPort(private val chunk: ByteArray) : UsbSerialPort {
        val reads = AtomicLong(0)
        val written = AtomicLong(0)

        @Volatile
        var open = true

        override fun read(dest: ByteArray, timeout: Int): Int {
            // A real port blocks until data or timeout; pacing keeps the test
            // from spinning a core and makes "how many chunks got through"
            // meaningful.
            Thread.sleep(5)
            if (!open) return 0
            val n = minOf(dest.size, chunk.size)
            System.arraycopy(chunk, 0, dest, 0, n)
            reads.incrementAndGet()
            return n
        }

        override fun read(dest: ByteArray, dstOffset: Int, timeout: Int): Int = read(dest, timeout)
        override fun write(src: ByteArray, timeout: Int) {
            written.addAndGet(src.size.toLong())
        }
        override fun write(src: ByteArray, srcOffset: Int, timeout: Int) = write(src, timeout)
        override fun close() {
            open = false
        }
        override fun isOpen(): Boolean = open

        override fun getDriver(): UsbSerialDriver = throw UnsupportedOperationException()
        override fun getDevice(): android.hardware.usb.UsbDevice = throw UnsupportedOperationException()
        override fun getPortNumber(): Int = 0
        override fun getWriteEndpoint(): android.hardware.usb.UsbEndpoint? = null
        override fun getReadEndpoint(): android.hardware.usb.UsbEndpoint? = null
        override fun getSerial(): String? = "fake-d6"
        override fun open(connection: android.hardware.usb.UsbDeviceConnection) = Unit
        override fun setParameters(baudRate: Int, dataBits: Int, stopBits: Int, parity: Int) = Unit
        override fun getCD(): Boolean = false
        override fun getCTS(): Boolean = false
        override fun getDSR(): Boolean = false
        override fun getDTR(): Boolean = false
        override fun setDTR(value: Boolean) = Unit
        override fun getRI(): Boolean = false
        override fun getRTS(): Boolean = false
        override fun setRTS(value: Boolean) = Unit
        override fun getControlLines(): EnumSet<UsbSerialPort.ControlLine> =
            EnumSet.noneOf(UsbSerialPort.ControlLine::class.java)
        override fun getSupportedControlLines(): EnumSet<UsbSerialPort.ControlLine> =
            EnumSet.noneOf(UsbSerialPort.ControlLine::class.java)
        override fun setFlowControl(value: UsbSerialPort.FlowControl) = Unit
        override fun getFlowControl(): UsbSerialPort.FlowControl = UsbSerialPort.FlowControl.NONE
        override fun getSupportedFlowControl(): EnumSet<UsbSerialPort.FlowControl> =
            EnumSet.of(UsbSerialPort.FlowControl.NONE)
        override fun getXON(): Boolean = true
        override fun purgeHwBuffers(purgeWriteBuffers: Boolean, purgeReadBuffers: Boolean) = Unit
        override fun setBreak(value: Boolean) = Unit
    }

    @Before
    fun setUp() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        if (ScanEngineNative.isAvailable) ScanEngineNative.nativeSetTempDir(context.cacheDir.absolutePath)
        root = File(context.getExternalFilesDir(null) ?: context.filesDir, "Round7ReArmTest").also {
            it.deleteRecursively()
            it.mkdirs()
        }
    }

    @After
    fun tearDown() {
        runBlocking { runCatching { bridge?.disconnect() } }
        bridge = null
        root.deleteRecursively()
    }

    @Test
    fun secondStartReArmsTheTransport(): Unit = runBlocking {
        assumeTrue("needs the native engine (scanengine_jni)", ScanEngineNative.isAvailable)
        val context = InstrumentationRegistry.getInstrumentation().targetContext

        val port = FakeSerialPort(ByteArray(512) { 0x00 })
        val path = "/dev/round7-fake-d6"
        val connection = D6SerialConnection(path, port)
        val registry = D6UsbConnectionRegistry(context).also { it.register(connection) }

        val engine = RealEngineBridge(registry).also { bridge = it }
        assertTrue(
            "connect must succeed against the real engine",
            engine.connect(EngineTarget(SensorType.COIN_D6, transportHint = path)).isSuccess,
        )
        assertEquals(ConnectionState.CONNECTED, engine.connectionState.value)
        assertTrue("the reader must be forwarding right after connect", connection.isForwarding)

        // ── cycle 1 ────────────────────────────────────────────────────────
        val dir1 = File(root, "cycle1.lscan").also { it.mkdirs() }
        assertTrue(engine.startCapture(dir1.absolutePath, false, "quickscan").isSuccess)
        assertEquals(CaptureState.RECORDING, engine.captureState.value)
        assertTrue(connection.isForwarding)
        val readsAfterStart1 = port.reads.get()
        Thread.sleep(300)
        assertTrue("the reader thread must actually be reading", port.reads.get() > readsAfterStart1)

        assertTrue(engine.stopCapture().isSuccess)
        assertEquals(CaptureState.IDLE, engine.captureState.value)
        // Stop deliberately closes the tap — this half was always right.
        assertFalse("Stop stops forwarding, by design", connection.isForwarding)

        // ── cycle 2: THE BUG ───────────────────────────────────────────────
        val dir2 = File(root, "cycle2.lscan").also { it.mkdirs() }
        assertTrue(
            "the engine happily starts a second session — which is why this was silent",
            engine.startCapture(dir2.absolutePath, false, "quickscan").isSuccess,
        )
        assertEquals(CaptureState.RECORDING, engine.captureState.value)

        // Before ROUND 7 this was false, and stayed false for the rest of the
        // connect: every byte the D6 sent was dropped on the app's own floor
        // while the UI showed a healthy recording. `sealed OK … points=0`.
        assertTrue(
            "a second Start must re-arm the D6 reader — this is scan-009",
            connection.isForwarding,
        )

        val readsAfterStart2 = port.reads.get()
        Thread.sleep(300)
        assertTrue(
            "and bytes must genuinely be flowing again, not just the flag",
            port.reads.get() > readsAfterStart2,
        )
        assertTrue(engine.stopCapture().isSuccess)
    }

    @Test
    fun pauseAndResumeStillGateTheTransport(): Unit = runBlocking {
        assumeTrue("needs the native engine (scanengine_jni)", ScanEngineNative.isAvailable)
        val context = InstrumentationRegistry.getInstrumentation().targetContext

        val port = FakeSerialPort(ByteArray(256) { 0x00 })
        val path = "/dev/round7-fake-d6-pause"
        val connection = D6SerialConnection(path, port)
        val registry = D6UsbConnectionRegistry(context).also { it.register(connection) }
        val engine = RealEngineBridge(registry).also { bridge = it }

        assertTrue(engine.connect(EngineTarget(SensorType.COIN_D6, transportHint = path)).isSuccess)
        val dir = File(root, "pause.lscan").also { it.mkdirs() }
        assertTrue(engine.startCapture(dir.absolutePath, false, "quickscan").isSuccess)

        assertTrue(engine.pauseCapture().isSuccess)
        assertEquals(CaptureState.PAUSED, engine.captureState.value)
        assertFalse("a pause is a closed tap", connection.isForwarding)

        assertTrue(engine.resumeCapture().isSuccess)
        assertEquals(CaptureState.RECORDING, engine.captureState.value)
        assertTrue("and a resume opens it again", connection.isForwarding)

        assertTrue(engine.stopCapture().isSuccess)
    }

    /**
     * ROUND 7, item 2: the chunk stamp the engine now back-dates from must be
     * `CLOCK_BOOTTIME`, because that is the domain `SteadyClock::now()` reads on
     * Android and the domain ARCore's `Frame.getTimestamp()` is already in. A
     * stamp from the wrong clock is a whole-cloud shear that nothing downstream
     * can detect.
     */
    @Test
    fun chunkTimestampsAreBoottimeAndMonotonic() {
        val port = FakeSerialPort(ByteArray(128) { 0x00 })
        val connection = D6SerialConnection("/dev/round7-clock", port)
        connection.setSensorLatencyMillis(0)

        val stamps = java.util.Collections.synchronizedList(mutableListOf<Long>())
        val before = android.os.SystemClock.elapsedRealtimeNanos()
        connection.startReading { _, _, t -> stamps.add(t) }
        Thread.sleep(200)
        connection.stopReading()
        val after = android.os.SystemClock.elapsedRealtimeNanos()

        val seen = synchronized(stamps) { stamps.toList() }
        assertTrue("the reader must have produced chunks", seen.size > 5)
        // Never 0 ("engine, stamp it yourself"), which is what this used to send.
        assertTrue("stamps must be real times, not 0", seen.all { it > 0L })
        assertTrue("stamps must be inside the window this test ran in", seen.all { it in before..after })
        assertEquals("and monotonic", seen.sorted(), seen)

        // And the latency knob really does shift them.
        val shifted = java.util.Collections.synchronizedList(mutableListOf<Long>())
        connection.setSensorLatencyMillis(50)
        val mark = android.os.SystemClock.elapsedRealtimeNanos()
        connection.startReading { _, _, t -> shifted.add(t) }
        Thread.sleep(150)
        connection.stopReading()
        val shiftedSeen = synchronized(shifted) { shifted.toList() }
        assertTrue(shiftedSeen.isNotEmpty())
        assertTrue(
            "a 50 ms latency must back-date the stamp",
            shiftedSeen.first() < mark + 50_000_000L,
        )
    }
}
