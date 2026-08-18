package com.lidarscan.app

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 10, owner item 38 — **capture → stop → capture again, in ONE process,
 * against the REAL native engine.**
 *
 * > "when click capture after capture, it still show with the previous
 * > capture. i can't start a new capture unless i close and reopen the app."
 *
 * This is the half no JVM test can reach, because the thing that was wrong is
 * not in Kotlin. On Android there is ONE `scan_engine*` per process
 * (`RealEngineBridge` creates it on first connect and holds it for the app's
 * lifetime) and `Engine`'s `PageStore` is created WITH THE ENGINE, not with
 * the session — `engine.cpp`'s `e->impl_->points = make_unique<PageStore>(...)`
 * sits in `Engine::create`, while a capture is a `start_session`. So capture #2
 * opened on top of capture #1's pages and the operator was shown the previous
 * scan in the live view of the new one.
 *
 * The engine has shipped the fix since ABI 7 — `start_session()` calls
 * `recycle_all()`, with a comment explaining that "a preview + N record cycles
 * on ONE connect all stacked into the same 64 pages" — but that reset is gated
 * on `PageFullPolicy::kEvictOldest`, eviction is opt-in, and **the Android app
 * never opted in**: there was no `nativeSetLivePageEviction` and no
 * `nativeRecycleLivePages` in `ScanEngineNative` at all. Two C-ABI calls that
 * existed and were never bound.
 *
 * The test therefore drives the native layer directly:
 *
 *   create engine -> enable eviction -> session #1 (points appended) -> stop
 *   -> recycle -> session #2 -> assert the window starts EMPTY
 *
 * and asserts both halves of the contract: the live window is empty for
 * capture #2, and both sessions seal a listable project — because a fix that
 * empties the window by breaking the recording would be worse than the bug.
 *
 * No sensor is attached and none is needed: points are appended through the
 * pushbroom seam the D6 driver itself uses, and `Engine::start_session()`
 * opens the recorder before it starts any device.
 */
@RunWith(AndroidJUnit4::class)
class CaptureRound10LiveWindowTest {

    private lateinit var root: File
    private var engineHandle: Long = 0L

    @Before
    fun setUp() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        if (ScanEngineNative.isAvailable) {
            ScanEngineNative.nativeSetTempDir(context.cacheDir.absolutePath)
        }
        root = File(context.getExternalFilesDir(null) ?: context.filesDir, "Round10LiveWindow").also {
            it.deleteRecursively()
            it.mkdirs()
        }
    }

    @After
    fun tearDown() {
        if (engineHandle != 0L) {
            ScanEngineNative.nativeStopSession(engineHandle)
            ScanEngineNative.nativeDestroyEngine(engineHandle)
            engineHandle = 0L
        }
        root.deleteRecursively()
    }

    @Test
    fun twoCapturesInOneProcessBothSealAndTheSecondStartsWithAnEmptyLiveWindow() {
        assumeTrue("needs the native engine (scanengine_jni)", ScanEngineNative.isAvailable)

        val store = FileProjectStore(root, appVersion = BuildConfig.VERSION_NAME)

        engineHandle = ScanEngineNative.nativeCreateEngine("round10-live-window", 2, 32 * 1024, 64, 0)
        assertTrue("scan_engine_create failed: ${ScanEngineNative.nativeLastError()}", engineHandle != 0L)

        // ROUND 10: what `RealEngineBridge.createEngineHandle()` now does. Without
        // it, `Engine::start_session()`'s own between-sessions reset never runs.
        assertEquals(
            "scan_engine_set_live_page_eviction must be bound and accepted",
            ScanEngineNative.ErrorCode.OK,
            ScanEngineNative.nativeSetLivePageEviction(engineHandle, true),
        )

        // --- capture #1 ----------------------------------------------------
        val p1 = store.create("Scan-101-2026-08-18-1200", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        assertEquals(
            ScanEngineNative.ErrorCode.OK,
            ScanEngineNative.nativeStartSession(engineHandle, p1.directory.absolutePath, "quickscan", true, false),
        )
        val pointsInFirst = appendSomePoints(tag = 1)
        assertTrue("capture #1 must actually put points in the live window", pointsInFirst > 0)
        assertEquals(ScanEngineNative.ErrorCode.OK, ScanEngineNative.nativeStopSession(engineHandle))
        assertNotNull("capture #1 must seal", store.updateManifest(p1.id) { it.copy(pointCountEstimate = pointsInFirst) })

        // The pages are STILL THERE after the seal, and that is correct — the
        // Projects thumbnail and the session summary are rendered from them.
        assertTrue(
            "the seal must not empty the window by itself (the thumbnail needs it)",
            ScanEngineNative.nativeTotalPoints(engineHandle) > 0,
        )

        // --- the fix -------------------------------------------------------
        // What the ViewModel now does on the re-arm and on entering Capture.
        assertEquals(
            "scan_engine_recycle_live_pages must be bound and accepted",
            ScanEngineNative.ErrorCode.OK,
            ScanEngineNative.nativeRecycleLivePages(engineHandle),
        )
        assertEquals(
            "the live window must be EMPTY before capture #2 — this is the whole bug",
            0,
            ScanEngineNative.nativePageCount(engineHandle),
        )

        // --- capture #2, same process, same engine handle -------------------
        val p2 = store.create("Scan-102-2026-08-18-1201", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        assertEquals(
            "the SECOND start must succeed on the same handle — no restart, no reconnect",
            ScanEngineNative.ErrorCode.OK,
            ScanEngineNative.nativeStartSession(engineHandle, p2.directory.absolutePath, "quickscan", true, false),
        )
        assertEquals(
            "capture #2 must open on an empty map, not on capture #1's cloud",
            0,
            ScanEngineNative.nativePageCount(engineHandle),
        )

        val pointsInSecond = appendSomePoints(tag = 2)
        assertTrue("capture #2 must record points of its own", pointsInSecond > 0)
        assertEquals(ScanEngineNative.ErrorCode.OK, ScanEngineNative.nativeStopSession(engineHandle))
        assertNotNull("capture #2 must seal", store.updateManifest(p2.id) { it.copy(pointCountEstimate = pointsInSecond) })

        // --- both scans survive --------------------------------------------
        val listed = store.list()
        assertEquals("two captures in one process must leave two listable projects", 2, listed.size)
        for (p in listed) {
            assertTrue("${p.id} must be sealed with points", (p.manifest.pointCountEstimate ?: 0L) > 0L)
            assertTrue(
                "${p.id} must carry a sealed engine container",
                File(p.directory, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).readText().contains("\"sealed\""),
            )
        }
    }

    /**
     * Feed the engine REAL COIN-D6 UART bytes, through the same seam
     * `D6SerialConnection` uses (`nativeAddD6Device` + `nativePushSerialBytes`),
     * so the points in the live window are produced by the production driver
     * and not by a test-only hook. The bytes come from the bundled synthetic
     * capture the replay smoke test already ships
     * (`assets/replay/synth.lscan/streams/lidar.bin`), unwrapped from their
     * `.lscan` chunk framing: 16-byte little-endian header
     * (`payload_len u32`, `type u16`, `flags u16`, `t_mono_ns i64`), payload,
     * `crc32 u32` — `engine/include/scanengine/record/lscan.h`'s format
     * contract.
     *
     * The RAW sensor-frame preview stream is what this populates, which is the
     * right choice here: it needs no pose, no extrinsic and no ARCore, and the
     * question this test asks is about the PageStore's lifetime, not about
     * geometry (which `engine/tests/test_round10_time_offset.cpp` and the
     * chirality suite own).
     */
    private fun appendSomePoints(tag: Int): Long {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val raw = context.assets.open("replay/synth.lscan/streams/lidar.bin").use { it.readBytes() }

        val deviceId = ScanEngineNative.nativeAddD6Device(
            engineHandle,
            "round10-fake-$tag",
            230_400,
            /* sendStartStop = */ false,
            /* writer = */ null,
        )
        assertTrue("scan_engine_add_device failed: ${ScanEngineNative.nativeLastError()}", deviceId >= 0)

        val before = ScanEngineNative.nativeTotalPoints(engineHandle)
        val buffer = java.nio.ByteBuffer.allocateDirect(64 * 1024)
        var off = STREAM_HEADER_BYTES
        var t = 1_000_000_000L * tag
        var chunks = 0
        while (off + CHUNK_HEADER_BYTES + 4 <= raw.size && chunks < 400) {
            val len = readU32(raw, off)
            val type = readU16(raw, off + 4)
            val end = off + CHUNK_HEADER_BYTES + len + 4
            if (len <= 0 || end > raw.size) break
            if (type == CHUNK_TYPE_D6_RAW && len <= buffer.capacity()) {
                buffer.clear()
                buffer.put(raw, off + CHUNK_HEADER_BYTES, len)
                ScanEngineNative.nativePushSerialBytes(engineHandle, deviceId, buffer, len, t)
                t += 20_000_000L
                chunks++
            }
            off = end
        }
        assertTrue("the bundled fixture must yield D6 chunks to push", chunks > 0)

        // The driver decodes on the pushing thread, but the store publishes
        // asynchronously; give it a moment rather than racing it.
        val deadline = System.currentTimeMillis() + 5_000
        while (ScanEngineNative.nativeTotalPoints(engineHandle) == before &&
            System.currentTimeMillis() < deadline
        ) {
            Thread.sleep(20)
        }
        ScanEngineNative.nativeRemoveDevice(engineHandle, deviceId)
        return ScanEngineNative.nativeTotalPoints(engineHandle) - before
    }

    private companion object {
        // engine/include/scanengine/record/lscan.h
        const val STREAM_HEADER_BYTES = 32
        const val CHUNK_HEADER_BYTES = 16
        const val CHUNK_TYPE_D6_RAW = 1

        fun readU16(b: ByteArray, at: Int): Int =
            (b[at].toInt() and 0xFF) or ((b[at + 1].toInt() and 0xFF) shl 8)

        fun readU32(b: ByteArray, at: Int): Int =
            (b[at].toInt() and 0xFF) or ((b[at + 1].toInt() and 0xFF) shl 8) or
                ((b[at + 2].toInt() and 0xFF) shl 16) or ((b[at + 3].toInt() and 0xFF) shl 24)
    }
}
