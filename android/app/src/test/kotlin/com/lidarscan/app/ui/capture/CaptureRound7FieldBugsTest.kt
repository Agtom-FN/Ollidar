@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.calib.MountTrim
import com.lidarscan.core.calib.StoredMountTrim
import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineEvent
import com.lidarscan.core.engine.EngineTarget
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 7 — the two bugs the owner's own capture log proves, driven through the
 * real [CaptureViewModel].
 *
 * ```
 * 22:53:04 [ar]        mount re-zero captured: magnitude=132.44deg spread=0.47deg
 * 22:53:09 [pushbroom] extrinsic applied: source=nominal trim=132.81deg     <- scan-008
 * 22:53:40 [seal]      sealed OK id=scan-008 … points=216653 elapsedMs=30543
 * 22:54:06 [pushbroom] extrinsic applied: source=nominal trim=none          <- scan-009 (BUG 1)
 * 22:54:16 [seal]      sealed OK id=scan-009 … points=0    elapsedMs=0      <- (BUG 2)
 * ```
 *
 * Neither had a test that could have caught it: the trim lived in a ViewModel
 * field that nothing outlived, and the no-data path only exists on real USB.
 * Both are now assertions.
 */
class CaptureRound7FieldBugsTest {

    @Before
    fun setUp() {
        Dispatchers.setMain(Dispatchers.Unconfined)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    private class ImmediateD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    private fun tempRoot(): File = File.createTempFile("round7", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    private fun trimOf(magnitudeDeg: Double, capturedAtMillis: Long): MountTrim {
        val q = com.lidarscan.core.calib.Quat.fromAxisAngle(
            com.lidarscan.core.calib.Vec3(0.0, 1.0, 0.0),
            Math.toRadians(magnitudeDeg),
        )
        return MountTrim(
            qx = q.x, qy = q.y, qz = q.z, qw = q.w,
            sensor = SensorType.COIN_D6,
            capturedAtEpochMillis = capturedAtMillis,
            sampleCount = 37,
            spreadDeg = 0.47,
        )
    }

    // =====================================================================
    // FIELD BUG 1 — the trim must survive the screen it was taken on
    // =====================================================================

    /**
     * The exact field sequence, minus the hardware: a trim is set, a capture
     * runs on it, the operator walks to Projects and back (which destroys the
     * Capture tab's `NavBackStackEntry` and with it this ViewModel), and the
     * NEXT capture must still run on that trim.
     *
     * "A new ViewModel over the same persistent store" is precisely what
     * navigating away and back is, so that is what the test does.
     */
    @Test
    fun `a mount trim survives the capture ViewModel being rebuilt`(): Unit = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.4.0")
        val series = AtomicInteger(0)
        // The app's DataStore, standing in as one mutable cell — which is all
        // SettingsRepository is from this ViewModel's point of view.
        var persisted: StoredMountTrim? = null
        val runId = "run-1"

        fun newViewModel(logs: MutableList<String>) = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            logEvent = { tag, message -> synchronized(logs) { logs += "[$tag] $message" } },
            loadStoredMountTrim = { persisted },
            persistMountTrim = { stored -> persisted = stored },
            appRunId = runId,
        )

        // --- session A: the operator re-zeros the mount ---------------------
        val logsA = mutableListOf<String>()
        val vmA = newViewModel(logsA)
        withTimeout(5_000) { vmA.autoConnectState!!.first { it.isPreviewing } }
        // `setMountReference` needs a live ARCore controller, which no JVM test
        // has; the persistence seam under test is the store, so the trim is put
        // in through the same door the sampler's success path uses.
        persisted = StoredMountTrim(trimOf(132.81, System.currentTimeMillis()), runId)

        // --- the trip to the Projects tab -----------------------------------
        val logsB = mutableListOf<String>()
        val vmB = newViewModel(logsB)
        withTimeout(5_000) { vmB.mountTrim.first { it != null } }

        // THE assertion. 0.3.0 read `null` here and logged `trim=none`.
        val restored = vmB.mountTrim.value
        assertNotNull("the trim must survive a new ViewModel", restored)
        assertEquals(132.81, restored!!.magnitudeDeg, 1e-6)

        val provenance = vmB.mountTrimProvenance.value
        assertFalse("same app run, so not a restart warning", provenance.fromPreviousRun)
        assertTrue(provenance.logSuffix.contains("trim=132.81deg"))
        assertTrue(
            "the restore must be in the capture log, so the next field report has it",
            synchronized(logsB) { logsB.any { it.contains("mount trim restored") } },
        )

        // And clearing it really does clear it, everywhere.
        vmB.clearMountReference()
        withTimeout(5_000) { vmB.mountTrim.first { it == null } }
        assertNull("Clear must reach the persistent store, not just the flow", persisted)

        val logsC = mutableListOf<String>()
        val vmC = newViewModel(logsC)
        withTimeout(2_000) {
            vmC.autoConnectState!!.first { it.isPreviewing }
        }
        assertNull(vmC.mountTrim.value)
        assertEquals("trim=none", vmC.mountTrimProvenance.value.logSuffix)
    }

    /** A trim from a previous app run is still applied, and the panel says where it came from. */
    @Test
    fun `a trim restored across an app restart is applied and announced`(): Unit = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.4.0")
        val stored = StoredMountTrim(
            trimOf(132.81, System.currentTimeMillis() - 20 * 60_000L),
            appRunId = "the-previous-run",
        )
        val notes = mutableListOf<String>()
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            claimSeriesNumber = { 1 },
            peekSeriesNumber = { 2 },
            logEvent = { _, message -> synchronized(notes) { notes += message } },
            loadStoredMountTrim = { stored },
            appRunId = "this-run",
        )
        withTimeout(5_000) { vm.mountTrim.first { it != null } }

        assertNotNull("a restart must not silently discard the mount reference", vm.mountTrim.value)
        val p = vm.mountTrimProvenance.value
        assertTrue(p.fromPreviousRun)
        assertTrue(p.label.contains("restored from your last session"))
        // Announced once, in the panel, rather than left for the operator to
        // discover from a crooked scan.
        withTimeout(5_000) { vm.mountTrimNote.first { it != null } }
        assertTrue(vm.mountTrimNote.value!!.contains("restored"))
    }

    // =====================================================================
    // FIELD BUG 2 — a capture receiving nothing must say so WHILE it runs
    // =====================================================================

    /**
     * A bridge that starts and stops cleanly and delivers **no data at all** —
     * which is exactly what the real `RealEngineBridge` did on the second Start
     * of a connect, because its first Stop latched the D6 reader's byte
     * forwarding off and only the Pause button ever turned it back on.
     *
     * The transport bug itself is fixed at the source (`RealEngineBridge`
     * re-arms the connection on every start). This proves the *second* half:
     * that whatever the cause, a silent zero-point session is now impossible to
     * ship without the screen and the log both saying so.
     */
    private class SilentBridge(
        private val bytesIn: Long = 0L,
        private val packetsOk: Long = 0L,
        private val packetsBad: Long = 0L,
    ) : EngineBridge {
        private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
        override val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()
        private val _captureState = MutableStateFlow(CaptureState.IDLE)
        override val captureState: StateFlow<CaptureState> = _captureState.asStateFlow()
        private val _events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 16)
        override val events: SharedFlow<EngineEvent> = _events.asSharedFlow()
        private val _health = MutableStateFlow<DeviceHealth?>(null)
        override val deviceHealth: StateFlow<DeviceHealth?> = _health.asStateFlow()

        override suspend fun connect(target: EngineTarget): Result<Unit> {
            _connectionState.value = ConnectionState.CONNECTED
            return Result.success(Unit)
        }

        override suspend fun disconnect() {
            _connectionState.value = ConnectionState.DISCONNECTED
        }

        override suspend fun startCapture(
            projectDirectory: String,
            liveSlam: Boolean,
            profile: String,
        ): Result<Unit> {
            // Note what this returns: SUCCESS. The engine session really did
            // start. That is why the field log says `sealed OK`.
            _captureState.value = CaptureState.RECORDING
            _health.value = DeviceHealth(
                id = 0, kind = 1, state = 3, lastError = 0,
                bytesIn = bytesIn, packetsOk = packetsOk, packetsBad = packetsBad,
                pointsOut = 0, drops = 0, pointsPerSec = 0.0, rotationHz = 0.0,
                checksumPassRate = 0.0, tLastDataNs = 0L,
            )
            return Result.success(Unit)
        }

        override suspend fun pauseCapture(): Result<Unit> {
            _captureState.value = CaptureState.PAUSED
            return Result.success(Unit)
        }

        override suspend fun resumeCapture(): Result<Unit> {
            _captureState.value = CaptureState.RECORDING
            return Result.success(Unit)
        }

        override suspend fun stopCapture(): Result<Unit> {
            _captureState.value = CaptureState.IDLE
            return Result.success(Unit)
        }
    }

    private suspend fun runSilentCapture(
        bridge: SilentBridge,
        logs: MutableList<String>,
    ): CaptureViewModel {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.4.0")
        val vm = CaptureViewModel(
            engineBridge = bridge,
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { 1 },
            peekSeriesNumber = { 2 },
            logEvent = { tag, message -> synchronized(logs) { logs += "[$tag] $message" } },
        )
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        return vm
    }

    @Test
    fun `a capture receiving no bytes shouts within the grace window, not after the seal`(): Unit = runBlocking {
        val logs = mutableListOf<String>()
        val vm = runSilentCapture(SilentBridge(bytesIn = 0L), logs)

        // Nothing yet — a scanner is allowed a moment to spin up.
        assertNull("must not cry wolf during spin-up", vm.noDataAlert.value)

        val alert = withTimeout(6_000) { vm.noDataAlert.first { it != null } }!!
        // DURING the capture, and specific about which half of the chain failed.
        assertEquals(CaptureState.RECORDING, vm.captureState.value)
        assertTrue(alert, alert.contains("NO DATA"))
        assertTrue(alert, alert.contains("0 bytes"))
        assertTrue(alert, alert.contains("USB-C"))

        assertTrue(
            "the capture log must carry the numbers, not just the screen",
            synchronized(logs) { logs.any { it.contains("NO DATA after") && it.contains("bytesIn=0") } },
        )

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        // The seal runs in NonCancellable after the state flips, so wait for the
        // verdict itself rather than for the state.
        withTimeout(5_000) {
            while (synchronized(logs) { logs.none { it.contains("[seal] sealed") } }) {
                kotlinx.coroutines.delay(25)
            }
        }

        // And the seal is honest about it. The field log's whole record of this
        // failure was `sealed OK … points=0 elapsedMs=0`.
        val sealLine = synchronized(logs) { logs.last { it.contains("sealed OK") } }
        assertTrue(sealLine, sealLine.contains("NO-DATA=true"))
        // ROUND 10: WAIT for the banner rather than sampling it the instant the
        // `sealed OK` line appears. The two are not simultaneous and never
        // were — between them the seal decides whether to prune, which is a
        // `withContext(Dispatchers.IO)` hop — so the original assertion was a
        // race that happened to win. The claim being made is unchanged ("still
        // saying so AFTER the stop", i.e. the alert is not cleared by sealing);
        // only the sampling is no longer a coin flip.
        val afterStop = withTimeout(5_000) { vm.noDataAlert.first { it != null } }
        assertNotNull("a saved-but-empty scan must still be saying so after the stop", afterStop)
        assertTrue(afterStop!!.contains("RECORDED NO POINTS"))
    }

    @Test
    fun `bytes arriving with no valid packets is reported as a different failure`(): Unit = runBlocking {
        val logs = mutableListOf<String>()
        val vm = runSilentCapture(SilentBridge(bytesIn = 98_304L, packetsOk = 0L, packetsBad = 41L), logs)
        val alert = withTimeout(6_000) { vm.noDataAlert.first { it != null } }!!
        // The cable is fine and the data is not the sensor's — a different
        // action for the operator, so a different sentence.
        assertTrue(alert, alert.contains("98304 bytes"))
        assertTrue(alert, alert.contains("41 rejected"))
        assertFalse(alert, alert.contains("0 bytes"))
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    @Test
    fun `a healthy capture never raises the no-data alert`(): Unit = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.4.0")
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(), // ticks 20 k points every 500 ms
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { 1 },
            peekSeriesNumber = { 2 },
        )
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        // Well past the grace window.
        kotlinx.coroutines.delay(3_500)
        assertNull("a scan that is producing points must stay quiet", vm.noDataAlert.value)
        assertTrue(vm.stats.value.pointsCaptured > 0L)
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        assertNull(vm.noDataAlert.value)
    }
}
