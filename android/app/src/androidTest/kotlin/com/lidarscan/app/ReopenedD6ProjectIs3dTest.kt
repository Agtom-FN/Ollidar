package com.lidarscan.app

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.lidarscan.app.engine.ProjectProbe
import com.lidarscan.app.engine.RealEngineBridge
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.processing.ProcessingRepository
import com.lidarscan.app.render.ProcessingCloudSource
import com.lidarscan.app.render.samplePoints
import com.lidarscan.app.usb.D6SerialConnection
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.EngineTarget
import com.lidarscan.core.model.SensorType
import java.io.File
import java.util.EnumSet
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
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
 * ROUND 8, owner item 27 — **"when i check the recording, it still show a 2D
 * scan. i need a 3d mapping."** — proved on a device, end to end, against the
 * real native engine.
 *
 * ### What this test is for
 *
 * `engine/tests/test_round8_d6_reopen.cpp` already proves the geometry: a
 * synthetic walk, recorded and reopened, comes back as a straight wall to
 * 0.05 cm. What it cannot prove is that the ANDROID path reaches it — that
 * `RealEngineBridge` really records poses into the `.lscan` it seals, that the
 * sealed container really carries a `kPoseAr` stream, and that
 * `ProcessingRepository`, which is what the Review screen drives, really turns
 * that directory back into a 3D cloud. Every one of those is app-side wiring
 * that a C++ test cannot see, and the ROUND 7 field bugs were all app-side
 * wiring.
 *
 * ### The stimulus, and why a constant range is the right one
 *
 * The fake serial port hands the reader a full D6 revolution at a constant
 * 2.0 m ([D6SyntheticPackets.revolution]), while a straight-line pose trail is
 * pushed through `scan_engine_push_pose`. That makes the two clouds trivially
 * distinguishable, which is the whole point:
 *
 *  * the RAW sensor-frame cloud is a flat disc — a COIN-D6's returns lie in its
 *    own scan plane by construction, so **every one of them has z == 0 to the
 *    bit**. (That is not a theory: 2,027 of the 4,040 points in the owner's own
 *    exported `preview.f32` are exactly z == 0. See
 *    `core/render/PreviewSanity.kt`.)
 *  * the RESOLVED cloud is that disc swept along the walk — a cylinder, with
 *    real extent on every axis and essentially no exact zeros.
 *
 * So "is the reopened project 3D?" becomes a measurement rather than a
 * judgement, and the failing case is the exact shape of the field report.
 */
@RunWith(AndroidJUnit4::class)
class ReopenedD6ProjectIs3dTest {

    private lateinit var root: File
    private var bridge: RealEngineBridge? = null
    private var processing: ProcessingRepository? = null

    /**
     * A `UsbSerialPort` that is a byte source rather than a device. Same shape
     * as `D6TransportReArmTest`'s, duplicated deliberately: that test's port
     * hands back zeros (it is testing the transport's state machine, not the
     * parser) and this one has to hand back packets the real `d6::Parser`
     * accepts, so sharing one class would mean one of the two carrying a
     * parameter it does not use.
     */
    private class RevolutionPort(private val chunk: ByteArray) : UsbSerialPort {
        val reads = AtomicLong(0)

        @Volatile
        var open = true

        override fun read(dest: ByteArray, timeout: Int): Int {
            // ~10 Hz, the D6's own revolution rate. Pacing matters here in a way
            // it does not in the transport test: the pushbroom resolves a return
            // against the poses BRACKETING it, so bytes arriving faster than
            // poses would be dropped for want of a pose and the test would
            // measure the wrong thing.
            Thread.sleep(100)
            if (!open) return 0
            val n = minOf(dest.size, chunk.size)
            System.arraycopy(chunk, 0, dest, 0, n)
            reads.incrementAndGet()
            return n
        }

        override fun read(dest: ByteArray, dstOffset: Int, timeout: Int): Int = read(dest, timeout)
        override fun write(src: ByteArray, timeout: Int) = Unit
        override fun write(src: ByteArray, srcOffset: Int, timeout: Int) = Unit
        override fun close() {
            open = false
        }
        override fun isOpen(): Boolean = open

        override fun getDriver(): UsbSerialDriver = throw UnsupportedOperationException()
        override fun getDevice(): android.hardware.usb.UsbDevice = throw UnsupportedOperationException()
        override fun getPortNumber(): Int = 0
        override fun getWriteEndpoint(): android.hardware.usb.UsbEndpoint? = null
        override fun getReadEndpoint(): android.hardware.usb.UsbEndpoint? = null
        override fun getSerial(): String? = "fake-d6-round8"
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
        root = File(context.getExternalFilesDir(null) ?: context.filesDir, "Round8ReopenTest").also {
            it.deleteRecursively()
            it.mkdirs()
        }
    }

    @After
    fun tearDown() {
        runBlocking { runCatching { bridge?.disconnect() } }
        bridge = null
        processing?.clearCloud()
        processing = null
        root.deleteRecursively()
    }

    /**
     * The owner's rig: D6 flat on the back of the phone with the scan fan
     * vertical and across the direction of travel. lidar +x -> world +y,
     * +y -> world +z, +z -> world +x. Row-major, which is the only layout
     * `scan_engine_set_mount_extrinsics` accepts.
     */
    private fun mountMatrix() = doubleArrayOf(
        0.0, 0.0, 1.0, 0.0,
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    )

    /** Records a synthetic walk into a fresh `.lscan` and seals it. Returns the directory. */
    private fun recordWalk(name: String): File = runBlocking {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val port = RevolutionPort(D6SyntheticPackets.revolution())
        val path = "/dev/round8-fake-d6"
        val connection = D6SerialConnection(path, port)
        val registry = D6UsbConnectionRegistry(context).also { it.register(connection) }

        val engine = RealEngineBridge(registry).also { bridge = it }
        assertTrue(
            "connect must succeed against the real engine",
            engine.connect(EngineTarget(SensorType.COIN_D6, transportHint = path)).isSuccess,
        )
        val handle = engine.engineHandleOrZero()
        assertTrue("the bridge must expose a live scan_engine*", handle != 0L)

        val dir = File(root, name).also { it.mkdirs() }
        assertTrue(engine.startCapture(dir.absolutePath, false, "quickscan").isSuccess)
        assertEquals(CaptureState.RECORDING, engine.captureState.value)

        // The pushbroom is what turns 2D returns into 3D geometry, and it needs
        // the extrinsic before it will run at all (the engine refuses to enable
        // it otherwise, on purpose — docs/A8-pushbroom.md §4).
        assertEquals(
            ScanEngineNative.ErrorCode.OK,
            ScanEngineNative.nativeSetMountExtrinsics(handle, mountMatrix()),
        )
        assertEquals(ScanEngineNative.ErrorCode.OK, ScanEngineNative.nativePushbroomEnable(handle, true))

        // A straight-line walk at 1 m/s, poses at 30 Hz on the engine's own
        // clock (CLOCK_BOOTTIME — `SystemClock.elapsedRealtimeNanos`, which is
        // exactly what `D6SerialConnection` stamps its chunks with, so the two
        // streams are in one time domain with no conversion).
        val t0 = android.os.SystemClock.elapsedRealtimeNanos()
        val walkSeconds = 3.0
        val poseHz = 30
        val poseCount = (walkSeconds * poseHz).toInt()
        var pushed = 0
        for (i in 0 until poseCount) {
            val t = i.toDouble() / poseHz
            val err = ScanEngineNative.nativePushPose(
                handle,
                t0 + (t * 1e9).toLong(),
                t, 0.0, 1.35,           // walking along +x at 1 m/s, phone at 1.35 m
                0.0, 0.0, 0.0, 1.0,     // identity attitude — the walk is the geometry here
                0.01f, 0.5f,
                ScanEngineNative.PoseQuality.GOOD,
                false,
                -1f,
            )
            if (err == ScanEngineNative.ErrorCode.OK) pushed++
            // Real time, so the D6 reader thread's chunks interleave with the
            // poses the way they do in the field. Without this the poses would
            // all land before any bytes and the assembler would resolve nothing.
            Thread.sleep((1000L / poseHz))
        }
        assertTrue("poses must be accepted by the engine, got $pushed of $poseCount", pushed > poseCount / 2)
        assertTrue("the reader thread must have delivered revolutions", port.reads.get() > 5)

        ScanEngineNative.nativePushbroomFlush(handle)
        assertTrue(engine.stopCapture().isSuccess)
        engine.disconnect()
        bridge = null
        dir
    }

    // ========================================================================
    // THE PROOF
    // ========================================================================
    @Test
    fun aReopenedD6ProjectIsTheResolved3dCloudAndNotRawSlices() {
        assumeTrue("needs the native engine (scanengine_jni)", ScanEngineNative.isAvailable)
        val dir = recordWalk("walk.lscan")

        val repo = ProcessingRepository(CoroutineScope(SupervisorJob() + Dispatchers.Default))
            .also { processing = it }

        // --- what the sealed container turned out to hold ---------------------
        val probe = repo.probeProject(dir)
        assertTrue("the sealed .lscan must be readable", probe.opened)
        assertTrue("the bytes must identify it as a D6 project", probe.isD6)
        // THE assertion this round exists for. Before ROUND 8 `Engine::push_pose`
        // never touched the recorder, so this was false for every capture this
        // app had ever made — and a D6 capture without its trajectory can never
        // be rebuilt into 3D by anything.
        assertTrue("the sealed project must carry its trajectory (kPoseAr)", probe.hasPoses)
        assertTrue("the resolved cloud must be cached in the container", probe.hasRecordedMap)
        assertTrue("the manifest must carry the mount extrinsic", probe.hasMount)
        assertTrue("a project with poses and a cached map can be shown in 3D", probe.canShow3d)
        assertFalse(probe.predatesTrajectoryStorage)

        // --- reopening it, exactly as the Review screen does -------------------
        val loaded = repo.openRecordedCloud("round8-walk", dir)
        assertTrue("reopening must produce points, got $loaded", loaded > 500)
        assertTrue(
            "a reopened project must count as having a processed cloud (Export/Colorize gate on it)",
            repo.hasProcessedCloud("round8-walk"),
        )

        val sample = ProcessingCloudSource { repo.handleOrZero() }.samplePoints(20_000)
        assertTrue("the reopened cloud must be readable through the render path", sample.size > 200)

        // --- and it is THREE-dimensional ---------------------------------------
        var minX = Double.MAX_VALUE
        var maxX = -Double.MAX_VALUE
        var minY = Double.MAX_VALUE
        var maxY = -Double.MAX_VALUE
        var minZ = Double.MAX_VALUE
        var maxZ = -Double.MAX_VALUE
        var exactZeroZ = 0
        for (p in sample) {
            // Vec3 is Float; widened here because the extents are compared
            // against metres and the format strings below are double-typed.
            val x = p.x.toDouble()
            val y = p.y.toDouble()
            val z = p.z.toDouble()
            minX = minOf(minX, x); maxX = maxOf(maxX, x)
            minY = minOf(minY, y); maxY = maxOf(maxY, y)
            minZ = minOf(minZ, z); maxZ = maxOf(maxZ, z)
            // The comparison stays in FLOAT: "exactly zero" means the bit
            // pattern the D6's own scan plane produces, and widening first
            // would be comparing a converted value rather than the recorded one.
            if (p.z == 0.0f) exactZeroZ++
        }
        val spanX = maxX - minX
        val spanY = maxY - minY
        val spanZ = maxZ - minZ
        val msg = "reopened extents: x=%.2f y=%.2f z=%.2f m over %d points, %d at exactly z=0"
            .format(spanX, spanY, spanZ, sample.size, exactZeroZ)

        // The walk. A single fan revolution cannot span this, however it is
        // oriented — the D6 sees 12 m and the fan is a plane.
        assertTrue("$msg — the cloud must span the walk", spanX > 0.8)
        // The fan, swept: a 2.0 m constant range through a vertical fan gives
        // roughly +/-2 m of vertical extent.
        assertTrue("$msg — the cloud must have vertical extent", spanZ > 1.0)
        assertTrue("$msg — the cloud must have lateral extent", spanY > 1.0)
        // The literal "this is not a 2D scan" assertion, written against the
        // exact symptom measured in the owner's export: 50 % of its preview
        // points sat at exactly z == 0. A resolved cloud has essentially none.
        assertTrue("$msg — this is the raw sensor-frame fan, not the resolved map", exactZeroZ * 20 < sample.size)
    }

    // ========================================================================
    // THE CONTROL: a pre-0.5.0 project is diagnosed, not silently blanked
    // ========================================================================
    @Test
    fun aProjectWithoutATrajectoryIsIdentifiedAsPreTrajectoryStorage() {
        assumeTrue("needs the native engine (scanengine_jni)", ScanEngineNative.isAvailable)
        val dir = recordWalk("nopose.lscan")

        // Turn the sealed container into exactly what every pre-0.5.0 capture
        // is: raw returns, no trajectory, no cached cloud. If this case ever
        // starts reporting canShow3d, something has begun inventing a
        // trajectory that was never recorded.
        assertTrue(File(dir, "streams/poses_ar.bin").delete())
        assertTrue(File(dir, "streams/map.bin").delete())

        val repo = ProcessingRepository(CoroutineScope(SupervisorJob() + Dispatchers.Default))
            .also { processing = it }
        val probe = repo.probeProject(dir)

        assertTrue(probe.opened)
        assertTrue(probe.isD6)
        assertFalse("there is no trajectory in this container", probe.hasPoses)
        assertFalse("and no cached cloud either", probe.hasRecordedMap)
        assertFalse("so it cannot be shown in 3D", probe.canShow3d)
        assertTrue(
            "and Review must be able to say so honestly rather than showing an empty box",
            probe.predatesTrajectoryStorage,
        )
        assertEquals(0L, repo.openRecordedCloud("round8-nopose", dir))
        assertEquals(ProjectProbe.NONE.opened, false)  // the absent-project sentinel stays absent
    }
}
