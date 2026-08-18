@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.DisplayParams
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
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
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 19 — items 76 and 77 at the ViewModel seam.
 *
 * Item 76's regression target is round 18's finding, verbatim: `displayParams`
 * SYNTHESIZED a fresh block from five controls, so `showTrajectory` was a
 * constant `true` in the live view and every field outside the combine reset
 * to the data-class default. These tests hold the fix: the emission is a
 * `copy()` of the persisted device block, so what Review toggles, the next
 * walk renders.
 *
 * Item 77's contract: the checklist intercepts the FIRST Start press and adds
 * no gate — its own Start continues the press, its dismissal starts nothing,
 * and "don't show again" is one persisted bit.
 */
class CaptureRound19Test {

    @Before fun setUp() { Dispatchers.setMain(Dispatchers.Unconfined) }

    @After fun tearDown() { Dispatchers.resetMain() }

    private class ImmediateD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    private fun tempRoot(): File = File.createTempFile("round19vm", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    private fun newVm(
        storedDisplay: DisplayParams? = null,
        persistedDisplay: AtomicReference<DisplayParams?> = AtomicReference(null),
        checklistDismissed: Boolean = true,
        persistedDismiss: AtomicBoolean = AtomicBoolean(false),
    ) = CaptureViewModel(
        engineBridge = FakeEngineBridge(),
        projectStore = FileProjectStore(tempRoot(), appVersion = "test"),
        autoDetectors = listOf(ImmediateD6Detector()),
        claimSeriesNumber = AtomicInteger(0).let { c -> { c.incrementAndGet() } },
        peekSeriesNumber = { 1 },
        loadDeviceDisplay = { storedDisplay },
        persistDeviceDisplay = { p -> persistedDisplay.set(p) },
        preScanChecklistDismissed = { checklistDismissed },
        persistPreScanChecklistDismissed = { persistedDismiss.set(true) },
    )

    // ── item 76: the persisted base survives the five live controls ─────────

    @Test
    fun `fields outside the five controls come from the persisted block`() = runBlocking {
        val stored = DisplayParams.captureDefaults().copy(
            showTrajectory = false,
            edlEnabled = false,
            clipHeightEnabled = true,
            clipHeightMax = 2.2f,
        )
        val vm = newVm(storedDisplay = stored)
        val p = vm.displayParams.first { !it.showTrajectory }
        assertFalse(p.showTrajectory)
        assertFalse(p.edlEnabled)
        assertTrue(p.clipHeightEnabled)
        assertEquals(2.2f, p.clipHeightMax)
        // ...and the live path toggle finally reads the stored truth.
        assertFalse(vm.showTrajectory.first { !it })
    }

    @Test
    fun `moving a live control does not reset the stored fields`() = runBlocking {
        val stored = DisplayParams.captureDefaults().copy(showTrajectory = false)
        val vm = newVm(storedDisplay = stored)
        vm.displayParams.first { !it.showTrajectory }

        vm.setColorMode(ColorMode.HEIGHT)
        val p = vm.displayParams.first { it.colorMode == ColorMode.HEIGHT }
        // Round 18's bug, inverted: the control moved AND the stored field held.
        assertFalse(p.showTrajectory)
    }

    @Test
    fun `no stored block means the round-8 capture defaults, unchanged`() = runBlocking {
        val vm = newVm(storedDisplay = null)
        val p = vm.displayParams.first()
        assertEquals(DisplayParams.captureDefaults().colorMode, p.colorMode)
        assertTrue(p.showTrajectory) // the data-class default, as before
    }

    // ── item 77: the checklist intercepts, and only ever once ───────────────

    @Test
    fun `the first Start press shows the checklist instead of starting`() = runBlocking {
        val vm = newVm(checklistDismissed = false)
        // Let init read the persisted bit.
        vm.showPreScanChecklist.first { !it }
        vm.startCapture()
        assertTrue(vm.showPreScanChecklist.value)
        // Nothing started: the press was intercepted, not queued.
        assertEquals(CaptureState.IDLE, vm.captureState.value)
    }

    @Test
    fun `dismissing the checklist starts nothing and can mute it for good`() = runBlocking {
        val persisted = AtomicBoolean(false)
        val vm = newVm(checklistDismissed = false, persistedDismiss = persisted)
        vm.showPreScanChecklist.first { !it }
        vm.startCapture()
        assertTrue(vm.showPreScanChecklist.value)

        vm.dismissPreScanChecklist(dontShowAgain = true)
        assertFalse(vm.showPreScanChecklist.value)
        assertEquals(CaptureState.IDLE, vm.captureState.value)
        assertTrue("don't-show-again must persist", persisted.get())

        // Muted: the next press goes straight through the intercept (and then
        // stops at the connection checks this test deliberately never passes).
        vm.startCapture()
        assertFalse(vm.showPreScanChecklist.value)
    }

    @Test
    fun `a dismissed checklist never intercepts — the round-17 flow unchanged`() = runBlocking {
        val vm = newVm(checklistDismissed = true)
        vm.showPreScanChecklist.first { !it }
        vm.startCapture()
        assertFalse(vm.showPreScanChecklist.value)
    }
}
