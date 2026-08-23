package com.lidarscan.app

import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.core.app.ActivityScenario
import androidx.test.espresso.Espresso
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assume
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 39 item 191 — **open a scan, go back, open another one.**
 *
 * This is the regression guard for the crash round 38 found and did not cause:
 * a `SIGSEGV`/`SEGV_MAPERR` in `__memcpy_aarch64_simd` under `glBufferSubData`
 * on Filament's `FEngine::loop` thread, about a third of a second into the
 * second scan's Review, copying ~1.25 MB — 78 389 vertices at 16 bytes, which
 * is *the first scan's whole cloud*. The fault was on the SOURCE address: a
 * native page buffer read after the store behind it was freed.
 *
 * **Why the test needs two DIFFERENT containers.** The processing engine's
 * `PageStore` is process-wide (`ProcessingRepository` holds one handle for the
 * life of the app — see its header, *"one instance per process … it is not per
 * project"*), and `open_recorded_cloud` starts by `clear_cloud()`ing it. So
 * opening a second scan does not build a second store beside the first: it
 * **frees the first one's pages** and refills the same store, on
 * `Dispatchers.IO`, while the renderer is enumerating and uploading pages out
 * of it on the choreographer thread. Staging the same container twice would
 * still exercise the free — but it could not tell whether the cloud on screen
 * afterwards is the scan that was opened or the one before it, and *that* is
 * the failure the crash is only the loud version of. On a phone the freed page
 * usually stays mapped, and then nothing crashes and one scan is painted with
 * another's bytes.
 *
 * So: `scan-030` (78 389 points, the crash's own numbers) and `scan-042`
 * (47 508 points, a different room), both staged out of the instrumentation
 * APK's assets **as the app** — round 38's recipe, which is round 28's hotfix
 * with the `chown` step removed by never letting `adb` own the files.
 *
 * **What proves "its own bytes" without a human looking at a picture.** Both
 * copies are staged in HEIGHT + Turbo, so both draw round 38's legend, and the
 * legend's three numbers are not a second opinion about anything: they are
 * `PointCloudRenderer.appliedValueRange()`, *the range the shader was actually
 * given*, formatted. Two different rooms have different height extents, so the
 * label triple is a fingerprint of the bytes that reached the GPU. The test
 * asserts the triple is stable per scan across every cycle and different
 * between the two — which is exactly what "scan B is drawing scan A's cloud"
 * would break, crash or no crash.
 *
 * `-e swapCycles N` runs the long proof (20 open→back→open cycles); without it
 * the class still runs the three-open guard, which is the crash path and is
 * short enough to live in the suite that runs on every push. `-e swapShots 1`
 * additionally photographs both scans after a swap.
 */
@RunWith(AndroidJUnit4::class)
class Round39ReviewSwapTest {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    private fun step(m: String) = android.util.Log.i("R39", m)

    private fun has(tag: String): Boolean =
        composeRule.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty()

    private data class Scan(val dir: String, val label: String, val asset: String, val createdAt: Long)

    private val scanA = Scan("swap-a.lscan", "Swap Room A", "scan-030.lscan", 1_755_600_002_000L)
    private val scanB = Scan("swap-b.lscan", "Swap Room B", "scan-042.lscan", 1_755_600_001_000L)

    /**
     * One staged copy, forced into HEIGHT + Turbo so the legend is drawn.
     *
     * Textual edit of `project.json` for round 38's reason: it is
     * `kotlinx.serialization` output with defaults omitted, and re-encoding it
     * here would mean a fixture writing the app's manifest through a different
     * encoder than the app's own.
     */
    private fun stage(scan: Scan) {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, "Projects")
        val dest = File(root, scan.dir)
        dest.deleteRecursively()
        dest.mkdirs()
        val am = InstrumentationRegistry.getInstrumentation().context.assets
        fun copyDir(assetPath: String, into: File) {
            val entries = am.list(assetPath) ?: return
            if (entries.isEmpty()) {
                into.parentFile?.mkdirs()
                am.open(assetPath).use { input -> into.outputStream().use { input.copyTo(it) } }
                return
            }
            into.mkdirs()
            for (e in entries) copyDir("$assetPath/$e", File(into, e))
        }
        copyDir(scan.asset, dest)

        val manifest = File(dest, "project.json")
        var json = manifest.readText()
        json = json.replaceFirst(Regex("\"name\"\\s*:\\s*\"[^\"]*\""), "\"name\":\"${scan.label}\"")
        json = json.replaceFirst(
            Regex("\"createdAtEpochMillis\"\\s*:\\s*\\d+"),
            "\"createdAtEpochMillis\":${scan.createdAt}",
        )
        json = json.replaceFirst(Regex("\"colorMode\"\\s*:\\s*\"[^\"]*\""), "\"colorMode\":\"HEIGHT\"")
        json = json.replaceFirst(
            Regex("\"height\"\\s*:\\s*\\{([^}]*)\\}"),
            "\"height\":{\"manualMax\":3.0,\"colormap\":\"TURBO\"}",
        )
        manifest.writeText(json)
    }

    private fun awaitProjectsList() {
        composeRule.waitUntil(timeoutMillis = 60_000) {
            runCatching { has("projectCard") }.getOrDefault(false)
        }
    }

    /** The legend's three labels, top-first — the shader's range, printed. */
    private fun legendLabels(): List<String> = listOf("heightLegendMax", "heightLegendMid", "heightLegendMin")
        .map { tag ->
            val node = composeRule.onAllNodesWithTag(tag, useUnmergedTree = true)
                .fetchSemanticsNodes()
                .firstOrNull()
            runCatching { node?.config?.get(SemanticsProperties.Text)?.joinToString("") }
                .getOrNull() ?: ""
        }

    /**
     * Poll until the legend stops moving. The auto-range grows as pages stream
     * in (`refreshAutoHeightRange` re-uploads as the combined box grows), so a
     * triple read at the first frame is a fingerprint of half a cloud. Two
     * identical reads 600 ms apart is the cloud having landed.
     */
    private fun settledLegend(): List<String> {
        var last = legendLabels()
        val deadline = System.currentTimeMillis() + 30_000
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(600)
            val now = legendLabels()
            if (now == last && now.none { it.isBlank() }) return now
            last = now
        }
        return last
    }

    /** Projects → the named scan → Review, waited out to a settled cloud. */
    private fun openReview(scan: Scan): List<String> {
        awaitProjectsList()
        step("opening ${scan.label}")
        composeRule.onNodeWithText(scan.label).performScrollTo().performClick()
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching { has("reviewViewport") }.getOrDefault(false)
        }
        composeRule.waitUntil(timeoutMillis = 180_000) {
            runCatching { !has("reviewLoadState") }.getOrDefault(false)
        }
        composeRule.waitUntil(timeoutMillis = 60_000) {
            runCatching { has("heightLegend") }.getOrDefault(false)
        }
        val labels = settledLegend()
        step("${scan.label} up: $labels")
        return labels
    }

    /** The system Back, which is the gesture the crash report names. */
    private fun back() {
        Espresso.pressBack()
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching { !has("reviewViewport") && has("projectCard") }.getOrDefault(false)
        }
        step("back on the list")
    }

    private fun shoot(name: String) {
        composeRule.waitForIdle()
        Thread.sleep(1_200)
        val automation = InstrumentationRegistry.getInstrumentation().uiAutomation
        automation.executeShellCommand("screencap -p /sdcard/Download/$name").use {
            android.os.ParcelFileDescriptor.AutoCloseInputStream(it).use { s -> s.readBytes() }
        }
    }

    /**
     * The guard: open → back → open → back → open, in one process, across two
     * different scans. Before round 39's fix the SECOND open took the process
     * down, so this test could not reach its first assertion.
     */
    @Test
    fun aSecondScanOpensInTheSameProcessAndDrawsItsOwnCloud() {
        stage(scanA)
        stage(scanB)
        ActivityScenario.launch(MainActivity::class.java).use {
            composeRule.onNodeWithTag("tab_projects").performClick()
            val a1 = openReview(scanA)
            back()
            val b1 = openReview(scanB)
            back()
            val a2 = openReview(scanA)

            assertTrue("scan A's legend should carry numbers, got $a1", a1.none { it.isBlank() })
            assertNotEquals("two different rooms must not share a height range", a1, b1)
            assertEquals("scan A must draw the same cloud the second time it is opened", a1, a2)
        }
    }

    /**
     * ROUND 39 item 191 — **what the fix costs, measured on the device rather
     * than argued about.**
     *
     * The fix uploads from a copy: one bulk `put` of one direct buffer into
     * another, out of the engine's page memory and into a pooled buffer of the
     * renderer's own, on the render thread. The number that matters is that
     * copy against a 16.7 ms frame, at the largest size the app can ever ask
     * for — scan-030's whole cloud in one `setBufferAt`, which is exactly what
     * the crash's 1.25 MB `memcpy` was.
     *
     * Both arms are timed because both are the real thing: `acquire` out of a
     * warm pool (every frame after the first of its size class) and a cold
     * `allocateDirect` (the first). `-e swapMeasure 1`.
     */
    @Test
    fun theCostOfCopyingBeforeUpload() {
        Assume.assumeTrue(
            "run with -e swapMeasure 1",
            InstrumentationRegistry.getArguments().getString("swapMeasure") != null,
        )
        val bytes = 78_389 * 16 // scan-030's cloud, the crash's own memcpy length
        val src = java.nio.ByteBuffer.allocateDirect(bytes).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        val pool = com.lidarscan.app.render.UploadBufferPool()

        fun timed(reps: Int, body: () -> Unit): Double {
            repeat(5) { body() } // warm
            val t0 = System.nanoTime()
            repeat(reps) { body() }
            return (System.nanoTime() - t0) / 1e6 / reps
        }

        val pooled = timed(100) {
            val dst = pool.acquire(bytes)
            src.position(0)
            src.limit(bytes)
            dst.put(src)
            dst.position(0)
            pool.release(dst)
        }
        val cold = timed(20) {
            val dst = java.nio.ByteBuffer.allocateDirect(bytes)
            src.position(0)
            src.limit(bytes)
            dst.put(src)
        }
        step("copy of $bytes B: pooled ${"%.3f".format(pooled)} ms, fresh alloc ${"%.3f".format(cold)} ms")
        // Not a benchmark gate — a floor under "this is not a frame-killer".
        // The whole cloud is uploaded ONCE per scan opened; a live capture's
        // per-frame slice is kilobytes.
        assertTrue("a 1.25 MB copy should not cost a frame", pooled < 16.0)
    }

    /**
     * The long proof, `-e swapCycles 20`: N open→back→open cycles alternating
     * between the two scans, every one of them asserting the legend against the
     * scan that was actually opened.
     */
    @Test
    fun manyOpenBackOpenCycles() {
        val n = InstrumentationRegistry.getArguments().getString("swapCycles")?.toIntOrNull()
        Assume.assumeTrue("run with -e swapCycles <n>", n != null)
        val shots = InstrumentationRegistry.getArguments().getString("swapShots") != null
        stage(scanA)
        stage(scanB)
        var fingerprintA: List<String>? = null
        var fingerprintB: List<String>? = null
        ActivityScenario.launch(MainActivity::class.java).use {
            composeRule.onNodeWithTag("tab_projects").performClick()
            for (i in 1..n!!) {
                val scan = if (i % 2 == 1) scanA else scanB
                val labels = openReview(scan)
                if (shots && i <= 2) shoot("r39-cycle$i-${scan.dir.substringBefore('.')}.png")
                if (scan === scanA) {
                    if (fingerprintA == null) fingerprintA = labels
                    assertEquals("cycle $i: scan A drew someone else's cloud", fingerprintA, labels)
                } else {
                    if (fingerprintB == null) fingerprintB = labels
                    assertEquals("cycle $i: scan B drew someone else's cloud", fingerprintB, labels)
                }
                step("cycle $i/$n survived (${scan.label})")
                back()
            }
        }
        assertNotEquals("the two staged scans must be distinguishable", fingerprintA, fingerprintB)
    }
}
