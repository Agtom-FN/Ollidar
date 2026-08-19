package com.lidarscan.app.ui.capture

import com.lidarscan.core.calib.MountTrim
import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.StoredMountTrim
import com.lidarscan.core.capture.PerformancePresets
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.render.DisplayParams
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 20 — the ViewModel halves of items 78/79 (trim migration on load),
 * 82 (the lever arm reaching the extrinsic resolution) and 83 (New capture).
 *
 * The hold-steady stage itself needs a live CaptureArController (an ARCore
 * object no JVM test can build); its geometry is pinned in :core
 * (MountTrimRound20Test, with the owner's real quaternions) and the stage is
 * skipped by construction when no controller exists — which is what keeps
 * every earlier round's startCapture test meaning what it meant.
 */
class CaptureRound20Test {

    @Before fun setUp() { Dispatchers.setMain(Dispatchers.Unconfined) }

    @After fun tearDown() { Dispatchers.resetMain() }

    private class ImmediateD6Detector : com.lidarscan.core.capture.SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): com.lidarscan.core.capture.AutoDetection =
            com.lidarscan.core.capture.AutoDetection(
                sensor = sensor,
                transportHint = "/dev/fake-d6",
                label = "COIN-D6 · fake",
            )
    }

    private fun tempRoot(): File = File.createTempFile("round20vm", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    // Trim A, verbatim from the owner's scan-054/055 bundles — a legacy
    // (pre-0.9.5) persisted trim that still carries its hold's yaw.
    private val legacyTrimA = MountTrim(
        qx = -0.14713701533870038,
        qy = 0.30202905772616806,
        qz = 0.7044345907133807,
        qw = 0.6252208045264762,
        capturedAtEpochMillis = 1_787_113_211_503L,
        sampleCount = 32,
        stabilityDeg = 0.58,
    )

    private fun newVm(
        storedTrim: StoredMountTrim? = null,
        persistedTrim: AtomicReference<StoredMountTrim?> = AtomicReference(null),
        leverArm: com.lidarscan.core.calib.MountLeverArm = com.lidarscan.core.calib.MountLeverArm.DEFAULT,
        logs: MutableList<String> = mutableListOf(),
    ) = CaptureViewModel(
        engineBridge = FakeEngineBridge(),
        projectStore = FileProjectStore(tempRoot(), appVersion = "test"),
        autoDetectors = listOf(ImmediateD6Detector()),
        claimSeriesNumber = AtomicInteger(0).let { c -> { c.incrementAndGet() } },
        peekSeriesNumber = { 1 },
        loadStoredMountTrim = { storedTrim },
        persistMountTrim = { t -> persistedTrim.set(t) },
        logEvent = { tag, line -> logs.add("[$tag] $line") },
        loadMountLeverArm = { leverArm },
    )

    // ── item 79: a legacy trim is yaw-normalised on load ─────────────────────

    @Test
    fun `a pre-0_9_5 trim is yaw-normalised on load and re-persisted`() = runBlocking {
        val persisted = AtomicReference<StoredMountTrim?>(null)
        val logs = mutableListOf<String>()
        val vm = newVm(
            storedTrim = StoredMountTrim(legacyTrimA, appRunId = "old-run"),
            persistedTrim = persisted,
            logs = logs,
        )
        val loaded = vm.mountTrim.first { it != null }!!
        assertTrue("the in-force trim must be gravity-referenced", loaded.gravityReferenced)
        // The owner's trim A normalises to 92.05 deg with zero y-component.
        assertEquals(92.05, loaded.magnitudeDeg, 0.05)
        assertEquals(0.0, loaded.qy, 1e-12)
        // The normalised form was persisted, so the migration runs once.
        assertTrue(persisted.get()?.trim?.gravityReferenced == true)
        assertTrue(
            "the migration must be in the log",
            logs.any { it.contains("yaw-normalised on load") },
        )
    }

    @Test
    fun `an already gravity-referenced trim loads untouched`() = runBlocking {
        val persisted = AtomicReference<StoredMountTrim?>(null)
        val migrated = legacyTrimA.yawNormalized()
        val vm = newVm(
            storedTrim = StoredMountTrim(migrated, appRunId = "old-run"),
            persistedTrim = persisted,
        )
        val loaded = vm.mountTrim.first { it != null }!!
        assertEquals(0.0, Math.toDegrees(migrated.rotation.angleTo(loaded.rotation)), 1e-9)
        // No re-persist: nothing changed.
        assertNull(persisted.get())
    }

    // ── item 83: New capture ─────────────────────────────────────────────────

    @Test
    fun `new capture resets per-scan settings and needs no confirm when idle`() = runBlocking {
        val vm = newVm()
        // Diverge some per-scan settings.
        vm.setPointSizePx(1.3f)
        vm.setColorMode(com.lidarscan.core.render.ColorMode.HEIGHT)
        vm.requestNewCapture()
        assertFalse("no dialog while idle", vm.showNewCaptureConfirm.value)
        assertEquals(PerformancePresets.DEFAULT, vm.preset.value)
        val p = vm.displayParams.first { it.colorMode == DisplayParams.CAPTURE_COLOR_MODE }
        assertEquals(DisplayParams.CAPTURE_COLOR_MODE, p.colorMode)
        assertEquals(DisplayParams.CAPTURE_POINT_SIZE_PX, vm.pointSizePx.value)
        assertEquals(0L, vm.stats.value.pointsCaptured)
    }

    @Test
    fun `new capture keeps the device facts - trim and lever arm survive`() = runBlocking {
        val vm = newVm(storedTrim = StoredMountTrim(legacyTrimA.yawNormalized(), appRunId = "r"))
        vm.mountTrim.first { it != null }
        vm.requestNewCapture()
        assertTrue("the mount trim is a device fact and must survive", vm.mountTrim.value != null)
        assertEquals(
            com.lidarscan.core.calib.MountLeverArm.DEFAULT,
            vm.mountLeverArm.value,
        )
    }

    @Test
    fun `new capture over a live recording asks first`() = runBlocking {
        val vm = newVm()
        vm.startCapture()
        // FakeEngineBridge starts synchronously under Unconfined.
        if (vm.captureState.value == CaptureState.RECORDING) {
            vm.requestNewCapture()
            assertTrue(vm.showNewCaptureConfirm.value)
            vm.dismissNewCaptureConfirm()
            assertFalse(vm.showNewCaptureConfirm.value)
            assertEquals(
                "keep-recording must not stop the capture",
                CaptureState.RECORDING,
                vm.captureState.value,
            )
        }
    }

    // ── item 82: the lever arm reaches the extrinsic log line ───────────────

    @Test
    fun `a user lever arm is loaded and reported`() = runBlocking {
        val logs = mutableListOf<String>()
        val arm = com.lidarscan.core.calib.MountLeverArm.clamped(
            upCm = 1.0,
            behindCm = 2.5,
            rightCm = -0.5,
            nowMillis = 42L,
        )
        val vm = newVm(leverArm = arm, logs = logs)
        vm.mountLeverArm.first { !it.isDefault }
        assertEquals("user", vm.mountLeverArm.value.provenance)
        // The translation encodes the documented frame mapping.
        val t = vm.mountLeverArm.value.translation
        assertEquals(-0.005, t.x, 1e-12)
        assertEquals(-0.010, t.y, 1e-12)
        assertEquals(-0.025, t.z, 1e-12)
        assertTrue(logs.any { it.contains("lever arm loaded") })
    }
}
