package com.lidarscan.app

import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.hasAnyAncestor
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.junit.Assume
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 38 item 190(d) — **the legend, photographed over a real cloud.**
 *
 * The item asks for *"screenshot verification on the AVD with the real
 * processed cloud … in HEIGHT+Turbo, HEIGHT+grayscale, and INTENSITY (no
 * legend), both themes"*, and every clause of that is a reason this cannot be a
 * composable harness like round 34's `Round34ScanPageShots`. A legend drawn
 * beside a fabricated range over a blank viewport proves that a `Row` lays out;
 * what has to be proven is that the numbers on the bar are the numbers the
 * SHADER is using, over a cloud whose range nobody chose. So this drives the
 * real app: the real Projects list, the real Review screen, the real
 * `PointCloudRenderer`, on the owner's own scan-030 bytes.
 *
 * **The staging is the round-28 hotfix's recipe, done from inside the app.**
 * That hotfix pushed `captures/scan-030.lscan` into
 * `getExternalFilesDir(null)/Projects` with `adb push` and then had to
 * `chown -R <app-uid>:ext_data_rw` it, because files pushed by `adb` are owned
 * by `shell` and the app cannot read them. Copying the same container out of
 * the instrumentation APK's assets — where `Round13ProcessScanTest` already
 * keeps it — writes it **as the app**, so the ownership problem cannot occur
 * and the recipe has no manual step to forget.
 *
 * **Three staged copies rather than one driven through the Display sheet.**
 * The first cut of this drove the sheet — Colour → Height, then the Advanced
 * `Ramp` dropdown for grayscale — and it was a test about `DropdownMenu`
 * semantics wearing a screenshot test's clothes: the grayscale leg needed Lab
 * features on, and "the node whose text is TURBO" matches both the menu item
 * and the row that names the current ramp. The display params are **persisted
 * per project** (`project.json`'s `displayParams`, which is what round 27 item
 * 141's migration is about), so staging three containers that differ only in
 * that block puts the app in the three states directly, through the same
 * `effectiveDisplayParams()` read the operator's own saved scans go through.
 *
 * The grayscale copy carries `"migration": 1` for a reason worth stating: a
 * persisted **height** grayscale is exactly what round 27 item 141 migrates to
 * Turbo on read, so an unstamped one would arrive at the screen as Turbo and
 * the two height shots would be the same picture. Stamping it is what an
 * operator who deliberately chose grey has on disk (round 28 item 153 made the
 * write path stamp), so this is the real state, not a fixture's dodge.
 *
 * **One Review per invocation, and that is not a style choice.** Opening a
 * second scan in Review *in the same app process* takes the process down with a
 * `SIGSEGV` inside Filament's `glBufferSubData` — a native page buffer from the
 * first session being read after the store behind it went away. It is
 * **pre-existing**: it was reproduced on this AVD with every one of round 38's
 * source changes stashed, and it is reported to the owner rather than fixed
 * here, because it is a renderer-lifetime bug and not a legend. So this test
 * takes ONE picture per instrumentation run, named by `-e legendShot <name>`,
 * and the six frames are six runs.
 *
 * **ROUND 39 item 191 fixed that crash** (`Round39ReviewSwapTest` now opens
 * twenty scans in one process). The one-picture-per-run shape is left exactly
 * as it is: it is no longer forced, but it is still what makes each frame a
 * clean-process photograph of a saved project's own persisted display params,
 * and re-walking six screens in one process to save six seconds would trade
 * that away for nothing. The paragraph above stays because it is the record of
 * why the shape was chosen.
 *
 * Off unless asked for: a test that stages three containers and stands still
 * for a photograph has no business in a suite that runs on every push.
 */
@RunWith(AndroidJUnit4::class)
class Round38LegendShots {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    /** Step markers, so logcat says how far the run got when a native layer takes the process. */
    private fun step(m: String) = android.util.Log.i("R38", m)

    private fun has(tag: String): Boolean =
        composeRule.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty()

    /**
     * The framebuffer, via `screencap` as the shell user — round 25's
     * `Round25PopupStyleTest.shoot` pattern, and for its reason: the point of
     * these pictures is the legend ON the cloud, and only the framebuffer has
     * both the `SurfaceView`'s pixels and the Compose chrome over them.
     */
    private fun shoot(name: String) {
        composeRule.waitForIdle()
        Thread.sleep(1_500) // let the renderer land a frame at the new params
        val automation = InstrumentationRegistry.getInstrumentation().uiAutomation
        automation.executeShellCommand("screencap -p /sdcard/Download/$name").use {
            android.os.ParcelFileDescriptor.AutoCloseInputStream(it).use { s -> s.readBytes() }
        }
    }

    /**
     * One staged copy of scan-030 whose `project.json` names [colorMode] and,
     * for the height ones, [heightColormap].
     *
     * The edit is textual on purpose. `project.json` is `kotlinx.serialization`
     * output with defaults omitted, and re-serialising it here would mean this
     * test writing a manifest through a *different* encoder than the app's —
     * which is the one thing a fixture for a persistence-driven screen must not
     * do. Replacing the two values leaves every other byte the owner's.
     */
    private fun stage(
        dirName: String,
        label: String,
        colorMode: String,
        heightColormap: String?,
        createdAt: Long,
    ) {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, "Projects")
        val dest = File(root, dirName)
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
        copyDir("scan-030.lscan", dest)

        val manifest = File(dest, "project.json")
        var json = manifest.readText()
        json = json.replaceFirst(Regex("\"name\"\\s*:\\s*\"[^\"]*\""), "\"name\":\"$label\"")
        json = json.replaceFirst(
            Regex("\"createdAtEpochMillis\"\\s*:\\s*\\d+"),
            "\"createdAtEpochMillis\":$createdAt",
        )
        json = json.replaceFirst(Regex("\"colorMode\"\\s*:\\s*\"[^\"]*\""), "\"colorMode\":\"$colorMode\"")
        if (heightColormap != null) {
            // Drop the height block's own colormap first, then rebuild the
            // block with the wanted one and the round-27 migration stamp beside
            // it, so a deliberate grayscale is not migrated away on read.
            json = json.replaceFirst(
                Regex("\"height\"\\s*:\\s*\\{([^}]*)\\}"),
                "\"migration\":1,\"height\":{\"manualMax\":3.0,\"colormap\":\"$heightColormap\"}",
            )
        }
        manifest.writeText(json)
    }

    private fun stageAll() {
        stage("legend-turbo.lscan", "Legend Turbo", "HEIGHT", "TURBO", 1_755_500_003_000L)
        stage("legend-grey.lscan", "Legend Grey", "HEIGHT", "GRAYSCALE", 1_755_500_002_000L)
        stage("legend-intensity.lscan", "Legend Intensity", "INTENSITY", null, 1_755_500_001_000L)
    }

    private fun awaitProjectsTab() {
        composeRule.waitUntil(timeoutMillis = 60_000) {
            runCatching { has("projectsAvatar") }.getOrDefault(false)
        }
    }

    private fun setTheme(name: String) {
        composeRule.onNodeWithTag("tab_settings").performClick()
        composeRule.waitUntil(timeoutMillis = 20_000) {
            runCatching { has("settingsScreen") }.getOrDefault(false)
        }
        composeRule.onNodeWithTag("settingsThemeRow").performScrollTo().performClick()
        composeRule.waitUntil(timeoutMillis = 10_000) {
            runCatching { has("themeSheet") }.getOrDefault(false)
        }
        composeRule.onNode(hasText(name) and hasAnyAncestor(hasTestTag("themeSheet"))).performClick()
        composeRule.waitForIdle()
    }

    /**
     * Projects → the named scan → Review, waiting for the cloud to actually be
     * there.
     *
     * **Waiting for the cloud, not for the legend.** The first cut of this
     * waited for `heightLegend` and timed out, for a reason that is the feature
     * working: an INTENSITY project has no legend to wait for. The honest
     * signal that a cloud has arrived is the empty state going away —
     * `ReviewViewport` draws `reviewLoadState` exactly when `hasCloud` is false.
     */
    private fun openReview(label: String) {
        composeRule.onNodeWithTag("tab_projects").performClick()
        awaitProjectsTab()
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching { has("projectCard") }.getOrDefault(false)
        }
        step("opening \u0022" + label + "\u0022")
        composeRule.onNodeWithText(label).performScrollTo().performClick()
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching { has("reviewViewport") }.getOrDefault(false)
        }
        composeRule.waitUntil(timeoutMillis = 180_000) {
            runCatching { !has("reviewLoadState") }.getOrDefault(false)
        }
        step("cloud is up")
        composeRule.waitForIdle()
        // Let pages stream in and the auto-range settle before anything is
        // photographed — `refreshAutoHeightRange` re-uploads as the bounds grow,
        // and the legend is polled off what it uploaded.
        Thread.sleep(6_000)
    }

    /**
     * One picture, named by `-e legendShot <name>`:
     *
     * | name | what it shows |
     * |---|---|
     * | `turbo-dark` / `turbo-light` | HEIGHT + Turbo — the legend, the ramp, three labels |
     * | `grey-dark` / `grey-light` | HEIGHT + grayscale — the SAME range on a different ramp |
     * | `intensity-dark` / `intensity-light` | INTENSITY — **no legend**, which is item 190(b) |
     *
     * The INTENSITY frames prove a negative, so they are asserted as well as
     * photographed: "I do not see a bar in this PNG" is a weaker claim than
     * "the node does not exist".
     */
    @Test
    fun theHeightLegendOverTheOwnersCloud() {
        val which = InstrumentationRegistry.getArguments().getString("legendShot")
        Assume.assumeTrue("run with -e legendShot <turbo|grey|intensity>-<dark|light>", which != null)
        val (variant, themeSuffix) = which!!.split("-").let { it[0] to it[1] }
        val theme = if (themeSuffix == "light") "Light" else "Dark"
        val label = when (variant) {
            "turbo" -> "Legend Turbo"
            "grey" -> "Legend Grey"
            "intensity" -> "Legend Intensity"
            else -> throw IllegalArgumentException("unknown legendShot variant: $variant")
        }
        stageAll()
        step("staged")
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            setTheme(theme)
            openReview(label)
            if (variant == "intensity") {
                composeRule.onAllNodesWithTag("heightLegend").assertCountEquals(0)
            } else {
                composeRule.onNodeWithTag("heightLegend").assertExists()
                // The three labels are the legend's whole claim, so their
                // presence is asserted rather than left to the picture.
                composeRule.onNodeWithTag("heightLegendMax", useUnmergedTree = true).assertExists()
                composeRule.onNodeWithTag("heightLegendMid", useUnmergedTree = true).assertExists()
                composeRule.onNodeWithTag("heightLegendMin", useUnmergedTree = true).assertExists()
            }
            shoot("r38-$which.png")
            step("shot r38-$which.png")
        }
    }
}
