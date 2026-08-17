@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

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
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 9, owner item 33 — **a scan that never happened must leave nothing
 * behind.**
 *
 * > "Entering Capture = a new-scan context; leaving WITHOUT ever starting a
 * > recording must leave NO project behind (no dir, no list entry). record+stop
 * > keeps + redirects (as shipped). Also prune 0-point legacy strays."
 *
 * Three different ways a stray could be created, one test each, all asserted
 * against the **same store the Projects tab lists with** (`store.list()`) *and*
 * against the directory on disk — because "not in the list" and "not on the
 * phone" have been different things before in this codebase (ROUND 6's
 * `manifest.json` collision) and the owner's complaint is about both.
 */
class CaptureRound9FlowTest {

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

    /**
     * Connects like the fake engine, then **refuses to record** — the shape of a
     * `scan_engine_start()` that returns `SCAN_ERR_*` on a real phone (a sensor
     * that enumerated over USB and then failed to spin up, which is the exact
     * failure the owner hit in the field).
     */
    private class RefusingEngineBridge : EngineBridge {
        private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
        override val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()
        private val _captureState = MutableStateFlow(CaptureState.IDLE)
        override val captureState: StateFlow<CaptureState> = _captureState.asStateFlow()
        private val _events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 64)
        override val events: SharedFlow<EngineEvent> = _events.asSharedFlow()
        override val deviceHealth: StateFlow<DeviceHealth?> = MutableStateFlow(null).asStateFlow()

        override suspend fun connect(target: EngineTarget): Result<Unit> {
            _connectionState.value = ConnectionState.CONNECTED
            return Result.success(Unit)
        }

        override suspend fun disconnect() {
            _connectionState.value = ConnectionState.DISCONNECTED
        }

        override suspend fun startCapture(projectDirectory: String, liveSlam: Boolean, profile: String) =
            Result.failure<Unit>(IllegalStateException("SCAN_ERR_DEVICE: the sensor never spun up"))

        override suspend fun pauseCapture() = Result.success(Unit)
        override suspend fun resumeCapture() = Result.success(Unit)
        override suspend fun stopCapture(): Result<Unit> {
            _captureState.value = CaptureState.IDLE
            return Result.success(Unit)
        }
    }

    /**
     * Records — and produces **nothing**. No `CaptureStats` event ever, so the
     * session's point count is a hard zero rather than "whatever the fake engine
     * had ticked by the time Stop landed". This is `scan-009`: a session that
     * started, ran, and received no sensor packets at all.
     */
    private class NoDataEngineBridge : EngineBridge {
        private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
        override val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()
        private val _captureState = MutableStateFlow(CaptureState.IDLE)
        override val captureState: StateFlow<CaptureState> = _captureState.asStateFlow()
        private val _events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 64)
        override val events: SharedFlow<EngineEvent> = _events.asSharedFlow()
        override val deviceHealth: StateFlow<DeviceHealth?> = MutableStateFlow(null).asStateFlow()

        override suspend fun connect(target: EngineTarget): Result<Unit> {
            _connectionState.value = ConnectionState.CONNECTED
            return Result.success(Unit)
        }

        override suspend fun disconnect() {
            _connectionState.value = ConnectionState.DISCONNECTED
        }

        override suspend fun startCapture(projectDirectory: String, liveSlam: Boolean, profile: String): Result<Unit> {
            _captureState.value = CaptureState.RECORDING
            return Result.success(Unit)
        }

        override suspend fun pauseCapture() = Result.success(Unit)
        override suspend fun resumeCapture() = Result.success(Unit)
        override suspend fun stopCapture(): Result<Unit> {
            _captureState.value = CaptureState.IDLE
            return Result.success(Unit)
        }
    }

    private fun tempRoot(): File = File.createTempFile("round9", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    /** Every `.lscan` directory actually on disk, listed or not. */
    private fun lscanDirsOnDisk(root: File): List<String> =
        root.listFiles { f: File -> f.isDirectory && f.name.endsWith(".lscan") }
            ?.map { it.name }
            .orEmpty()

    private fun viewModel(
        root: File,
        bridge: EngineBridge,
        keepEmptyScans: Boolean = false,
        series: AtomicInteger = AtomicInteger(0),
        store: FileProjectStore = FileProjectStore(root, appVersion = "0.5.0"),
    ) = CaptureViewModel(
        engineBridge = bridge,
        projectStore = store,
        autoDetectors = listOf(ImmediateD6Detector()),
        claimSeriesNumber = { series.incrementAndGet() },
        peekSeriesNumber = { series.get() + 1 },
        keepEmptyScans = { keepEmptyScans },
    )

    // =====================================================================
    // (a) ENTERING — and leaving — the Capture tab creates nothing
    // =====================================================================

    /**
     * The tab is a *new-scan context*, not a new scan. Constructing the
     * ViewModel runs everything screen entry runs — auto-detect, auto-connect,
     * the live preview, the auto-name — and none of it may touch the store.
     *
     * This is the regression test for a bug that (per the ROUND 9 audit) does
     * NOT exist today and must not be re-introduced: ROUND 5 item 9 moved
     * project creation from screen entry to Start, and `onCleared` deletes
     * nothing because there is nothing to delete. The assertion is cheap; the
     * class of bug it catches cost the owner a screenful of `scan-0xx`
     * directories.
     */
    @Test
    fun `entering the capture tab and leaving without recording creates nothing`(): Unit = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.5.0")
        val vm = viewModel(root, FakeEngineBridge(), store = store)

        // Screen entry, in full: detection resolved, connected, previewing.
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        assertTrue("the tab must open as a NEW SCAN", vm.uiState.value is CaptureUiState.NewScan)

        // …and now the operator walks to another tab without ever pressing
        // Start. Nothing was ever started, so there is nothing to seal and
        // nothing to roll back.
        assertEquals("no project may reach the list", emptyList<String>(), store.list().map { it.id })
        assertEquals("and none may exist on disk either", emptyList<String>(), lscanDirsOnDisk(root))
    }

    // =====================================================================
    // (b) A START THE ENGINE REFUSES leaves nothing behind
    // =====================================================================

    /**
     * **The real "leaves a project behind" bug.**
     *
     * `startCapture()` creates the `.lscan` *before* calling the engine, and
     * ROUND 6's own comment on the failure branch admitted the consequence: "the
     * project directory exists at this point but nothing will ever be written
     * into it". Every refused Start therefore left a permanent empty project in
     * the list. It is now rolled back — while the operator still gets the red
     * banner, which is the half ROUND 6 got right.
     */
    @Test
    fun `a Start the engine refuses leaves no project on disk and none in the list`(): Unit = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.5.0")
        val vm = viewModel(root, RefusingEngineBridge(), store = store)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture()
        val error = withTimeout(5_000) { vm.saveError.first { it != null } }!!

        assertTrue("the operator must still be told: $error", error.contains("did not start"))
        assertEquals("a refused Start must leave NO list entry", emptyList<String>(), store.list().map { it.id })
        assertEquals("…and NO directory", emptyList<String>(), lscanDirsOnDisk(root))
        assertTrue(
            "the tab must go back to being a new-scan context — leaving it Loaded would make the next " +
                "Start re-record into a project that no longer exists",
            vm.uiState.value is CaptureUiState.NewScan,
        )
    }

    // =====================================================================
    // (c) A 0-POINT STOP is pruned by default, kept when the setting says so
    // =====================================================================

    /**
     * `scan-009`, and the twelve like it: Start, record nothing, Stop. The scan
     * is not kept, the shell is not sent to it, and the banner says what
     * happened.
     */
    @Test
    fun `a stop with zero points prunes the project and does not navigate to it`(): Unit = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.5.0")
        val vm = viewModel(root, NoDataEngineBridge(), keepEmptyScans = false, store = store)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        val sealed = java.util.Collections.synchronizedList(mutableListOf<String>())
        val collector = kotlinx.coroutines.CoroutineScope(Dispatchers.Unconfined).launch {
            vm.sealedProjectId.collect { sealed += it }
        }

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        // The project DOES exist while the session is live — the prune is a
        // decision made at Stop, not a reason never to create the .lscan.
        assertEquals(1, lscanDirsOnDisk(root).size)
        assertEquals(0L, vm.stats.value.pointsCaptured)

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }

        assertEquals("an empty scan must not reach the list", emptyList<String>(), store.list().map { it.id })
        assertEquals("…and its directory must be gone", emptyList<String>(), lscanDirsOnDisk(root))
        assertTrue(
            "navigating to a project that was just deleted is worse than not navigating: $sealed",
            sealed.isEmpty(),
        )
        // The seal's own banner, not the 2-second no-data watchdog's — the two
        // share `noDataAlert` and only the seal's says what happened to the
        // directory, so this waits for that one specifically.
        val alert = withTimeout(5_000) { vm.noDataAlert.first { it?.contains("removed") == true } }!!
        assertTrue("the banner must say the scan recorded nothing: $alert", alert.contains("NO POINTS"))
        assertTrue(
            "…and it must not claim a saved path for a directory that no longer exists",
            vm.lastSavedProject.value == null,
        )
        collector.cancel()
    }

    /**
     * The other arm of the switch, and the reason it exists: while chasing a
     * sensor that produces nothing, the empty `.lscan` **is** the evidence —
     * manifest, mount trim, timestamps of an attempt that failed. ROUND 7's
     * behaviour, still reachable.
     */
    @Test
    fun `a stop with zero points keeps the project when keepEmptyScans is on`(): Unit = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.5.0")
        val vm = viewModel(root, NoDataEngineBridge(), keepEmptyScans = true, store = store)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }

        assertEquals("the evidence must survive when the operator asked for it", 1, store.list().size)
        assertEquals(1, lscanDirsOnDisk(root).size)
        val kept = store.list().single()
        assertTrue("…and it is genuinely an empty scan", kept.manifest.isEmptyScan)
        val alert = withTimeout(5_000) {
            vm.noDataAlert.first { it?.contains("saved so the evidence") == true }
        }!!
        assertTrue("the ROUND 7 wording is what applies here: $alert", alert.contains("NO POINTS"))
    }

    // =====================================================================
    // (d) A REAL SCAN is kept and redirects, exactly as shipped
    // =====================================================================

    /**
     * "record+stop keeps + redirects (as shipped)" — the half of item 33 that is
     * a promise not to break anything. A capture with points in it is sealed,
     * listed, and published to the shell for the ROUND 8 item 31 redirect.
     */
    @Test
    fun `a stop with points keeps the project and still publishes the sealed id`(): Unit = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.5.0")
        val vm = viewModel(root, FakeEngineBridge(), keepEmptyScans = false, store = store)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        val sealed = java.util.Collections.synchronizedList(mutableListOf<String>())
        val collector = kotlinx.coroutines.CoroutineScope(Dispatchers.Unconfined).launch {
            vm.sealedProjectId.collect { sealed += it }
        }

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        withTimeout(5_000) { vm.stats.first { it.pointsCaptured > 0L } }
        val recordedId = (vm.uiState.value as CaptureUiState.Loaded).project.id

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { while (sealed.isEmpty()) kotlinx.coroutines.delay(20) }

        assertEquals("a scan with points in it is kept", listOf(recordedId), store.list().map { it.id })
        assertEquals("…and the shell is still sent to it", listOf(recordedId), sealed.toList())
        val kept = store.list().single()
        assertTrue("…with its point count sealed into the manifest", (kept.manifest.pointCountEstimate ?: 0L) > 0L)
        assertTrue("…so it is NOT an empty scan", !kept.manifest.isEmptyScan)
        assertNotNull("…and the saved path is offered", vm.lastSavedProject.value)
        collector.cancel()
    }
}
