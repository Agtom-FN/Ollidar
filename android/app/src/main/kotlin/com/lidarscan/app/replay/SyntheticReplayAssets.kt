package com.lidarscan.app.replay

import android.content.Context
import java.io.File

/**
 * B4's acceptance path: "Replay synthetic capture" (a debug-drawer action,
 * see `ui/settings/SettingsScreen.kt`'s debug section) needs the bundled
 * `.lscan` at `assets/replay/synth.lscan/` on a REAL filesystem path —
 * `scanengine::lscan::FileRecordReader::open()` (which
 * `ReplayEngine`/`ReplaySource` use under the hood) calls `std::filesystem`
 * / `fopen` against a directory, not an APK asset stream, so the bundled
 * asset must be extracted to app-private storage first.
 *
 * The bundled tree is the S1 d6synth output also used for
 * `desktop/evidence/synth.lscan` — just `manifest.json` +
 * `streams/lidar.bin` (~396 KB; the other `.lscan` subdirectories are empty
 * for this capture and FileRecordReader tolerates their absence, per
 * `record/lscan.cpp`'s `open()`: only the stream files it finds are opened,
 * a missing one is a warning, not a hard failure — see NOTES.md).
 */
object SyntheticReplayAssets {
    private const val ASSET_ROOT = "replay/synth.lscan"
    private const val EXTRACTED_DIR_NAME = "replay-synth.lscan"

    /**
     * Copies the bundled asset tree into `context.filesDir` on first call
     * (subsequent calls are a no-op if the expected files are already
     * there — this asset never changes at runtime) and returns the
     * extracted directory's absolute path, ready to hand to
     * `ScanEngineNative.nativeReplayStart`.
     */
    fun ensureExtracted(context: Context): File {
        val dest = File(context.filesDir, EXTRACTED_DIR_NAME)
        val manifestOut = File(dest, "manifest.json")
        val lidarOut = File(dest, "streams/lidar.bin")
        if (manifestOut.exists() && lidarOut.exists()) return dest

        dest.deleteRecursively()
        copyAssetTree(context, ASSET_ROOT, dest)
        return dest
    }

    private fun copyAssetTree(context: Context, assetPath: String, outDir: File) {
        val am = context.assets
        // AssetManager.list() on a leaf file returns null on some API levels
        // and an empty array on others — both mean "not a directory" here.
        val children = try {
            am.list(assetPath)
        } catch (e: java.io.IOException) {
            null
        }.orEmpty()
        if (children.isEmpty()) {
            // Leaf file (AssetManager.list() returns empty for a file, not just for
            // an empty directory — the manifest/lidar.bin paths land here).
            outDir.parentFile?.mkdirs()
            am.open(assetPath).use { input ->
                outDir.outputStream().use { output -> input.copyTo(output) }
            }
            return
        }
        outDir.mkdirs()
        for (child in children) {
            copyAssetTree(context, "$assetPath/$child", File(outDir, child))
        }
    }
}
