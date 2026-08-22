package com.lidarscan.app.ui.projects

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.unit.Dp
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.app.render.samplePoints
import com.lidarscan.app.render.streamsPresent
import com.lidarscan.core.render.PreviewSanity
import com.lidarscan.app.ui.theme.sensorBadgeColor
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.HeightRamp
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.core.store.Project
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.util.Collections
import kotlin.math.max
import kotlin.math.min
import kotlin.random.Random

/**
 * Project-card cloud thumbnails.
 *
 * **Why a Canvas scatter and not an offscreen Filament render.** Filament is
 * wired into this app through `SurfaceView` + `UiHelper` + a `Choreographer`
 * frame loop (`PointCloudRenderer` — see NOTES.md's B4 section); there is no
 * offscreen/readback path today, and adding one means a second `Engine`,
 * a render target, a pixel readback and a bitmap cache per visible row of a
 * `LazyColumn`. That is a large amount of GPU machinery for a 108 dp tile, and
 * the tile does not need any of Filament's actual capabilities — no camera
 * control, no material, no depth. A `Canvas` draws the same information at a
 * fraction of the cost and cannot leak a GL context out of a scrolling list.
 *
 * **It still draws the project's own data.** A capture writes a bounded XYZ
 * sample of what it actually recorded to `<project>/processed/preview.f32`
 * when the session stops ([writeProjectPreview], called from
 * `CaptureViewModel.stopCapture`). [ProjectPreviewCache] reads that back on an
 * IO dispatcher and the tile projects it. Only when a project has no such file
 * — nothing captured yet, or a project that predates this — does the tile fall
 * back to a **seeded** placeholder derived from the project id, so a given
 * project always looks like itself but never pretends to be data it does not
 * have (the placeholder is drawn dimmer and without the trajectory head, which
 * is the visual tell).
 */

/** Points are stored as XYZ float triples; this cap is what a 108 dp tile can usefully show. */
private const val PREVIEW_MAX_POINTS = 4_000
private const val PREVIEW_MAGIC = 0x4C53_5056 // "LSPV"
private const val PREVIEW_FILE = "preview.f32"

/** An XYZ sample of a project's cloud, already normalised into a unit box. */
class ProjectPreview(
    /** Interleaved x,y,z in [0,1] after normalisation, y = height. */
    val points: FloatArray,
    val count: Int,
)

private fun previewFile(projectDirectory: File) = File(File(projectDirectory, "processed"), PREVIEW_FILE)

/**
 * Writes a bounded XYZ sample of [source] beside the project's processed
 * results. Called once per capture stop — cheap (a strided walk of pages the
 * renderer already holds) and idempotent (it overwrites).
 *
 * Returns false and leaves any existing preview alone when the source has
 * nothing to sample, so a stop that recorded nothing does not blank a good
 * thumbnail from an earlier session.
 */
fun writeProjectPreview(projectDirectory: File, source: PointCloudSource?): Boolean {
    if (source == null) return false

    // --- ROUND 8: sample the RESOLVED map, not the raw fan on top of it ------
    //
    // This is the fix for the second half of owner item 27, and it was found by
    // measuring the owner's own exported capture rather than by reading code:
    // `captures/scan-015-pixel-0.4.0.lscan/processed/preview.f32` has exactly
    // 2,027 of its 4,040 points at z == 0.0f — 50.2 %, one raw sensor-frame
    // return for every resolved one. See
    // [com.lidarscan.core.render.PreviewSanity] for the whole autopsy.
    //
    // A D6 capture with the pushbroom running holds two point streams in one
    // PageStore. The live renderer has filtered between them since B3
    // (`StreamFilter`); this writer never did, so the tile a user judges a scan
    // by was the 3D room with a flat 2D disc drawn through it. At 108 dp that
    // reads as "the scan is 2D", which is precisely what the owner reported.
    //
    // The policy mirrors `StreamFilter.MAPPED_ONLY` exactly, including its
    // fallback: prefer the mapped stream, fall back to raw when there is no
    // mapped page at all (a Record-only session genuinely has nothing else,
    // and a blank tile would be worse than an honest sensor-frame one).
    val streams = source.streamsPresent()
    val mapped = ScanEngineNative.StreamId.SLAM_MAP
    val sample = if (streams.contains(mapped)) {
        source.samplePoints(PREVIEW_MAX_POINTS) { it == mapped }
    } else {
        source.samplePoints(PREVIEW_MAX_POINTS)
    }
    if (sample.isEmpty()) return false

    val xyz = FloatArray(sample.size * 3)
    for (i in sample.indices) {
        xyz[i * 3] = sample[i].x.toFloat()
        xyz[i * 3 + 1] = sample[i].y.toFloat()
        xyz[i * 3 + 2] = sample[i].z.toFloat()
    }
    // The second line of defence. A preview is written once per capture and
    // read on every list scroll for the life of the project, so a bad one is
    // effectively permanent; refusing to write it costs a tile, and the
    // existing behaviour for "no preview file" is a seeded placeholder that is
    // drawn dimmer and without a trajectory head — i.e. it already says "this
    // is not your data" visually. Better that than a lie.
    when (val verdict = PreviewSanity.check(xyz, sample.size)) {
        is PreviewSanity.Verdict.Ok -> Unit
        is PreviewSanity.Verdict.Rejected -> {
            android.util.Log.w(
                "CloudThumbnail",
                "refusing to write a preview for ${projectDirectory.name}: ${verdict.reason}",
            )
            return false
        }
    }

    val out = previewFile(projectDirectory)
    out.parentFile?.mkdirs()
    return runCatching {
        DataOutputStream(out.outputStream().buffered()).use { s ->
            s.writeInt(PREVIEW_MAGIC)
            s.writeInt(sample.size)
            for (i in sample.indices) {
                s.writeFloat(xyz[i * 3])
                s.writeFloat(xyz[i * 3 + 1])
                s.writeFloat(xyz[i * 3 + 2])
            }
        }
        true
    }.getOrDefault(false)
}

/**
 * Reads previews off disk once per project and keeps them in memory.
 *
 * Bounded on purpose: a synchronized LRU of [CAPACITY] entries, each at most
 * `PREVIEW_MAX_POINTS * 3` floats (48 KB), so a long project list cannot grow
 * this without limit. `MISSING` is memoised too — a project with no capture
 * would otherwise stat the filesystem on every recomposition.
 */
object ProjectPreviewCache {
    private const val CAPACITY = 24
    private val MISSING = ProjectPreview(FloatArray(0), 0)

    private val cache: MutableMap<String, ProjectPreview> = Collections.synchronizedMap(
        object : LinkedHashMap<String, ProjectPreview>(CAPACITY, 0.75f, true) {
            override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, ProjectPreview>) = size > CAPACITY
        },
    )

    /** Drops one project's memoised preview so the next read picks up a freshly written file. */
    fun invalidate(projectId: String) {
        cache.remove(projectId)
    }

    suspend fun load(project: Project): ProjectPreview? {
        cache[project.id]?.let { return it.takeIf { p -> p.count > 0 } }
        val loaded = withContext(Dispatchers.IO) { readPreview(previewFile(project.directory)) } ?: MISSING
        cache[project.id] = loaded
        return loaded.takeIf { it.count > 0 }
    }

    private fun readPreview(file: File): ProjectPreview? {
        if (!file.isFile) return null
        return runCatching {
            DataInputStream(file.inputStream().buffered()).use { s ->
                if (s.readInt() != PREVIEW_MAGIC) return@use null
                val n = s.readInt()
                if (n <= 0 || n > PREVIEW_MAX_POINTS * 4) return@use null
                val raw = FloatArray(n * 3)
                for (i in raw.indices) raw[i] = s.readFloat()
                // ROUND 8: the same verdict on the way IN, because previews
                // written by 0.4.0 and earlier are already on people's phones —
                // the owner's own scan-015 among them — and this app cannot go
                // back and rewrite them. A tile that would draw the raw
                // sensor-frame fan falls back to the placeholder, which is
                // visibly not-your-data (dimmer, no trajectory head) rather
                // than a flat disc that looks like a finished scan.
                if (PreviewSanity.check(raw, n) !is PreviewSanity.Verdict.Ok) return@use null
                normalise(raw, n)
            }
        }.getOrNull()
    }

    /**
     * Fits the sample into a unit box, keeping the horizontal aspect ratio so a
     * corridor still looks like a corridor. Height (`z` in the engine's frame)
     * becomes the tile's colour ramp axis and is normalised independently.
     */
    private fun normalise(raw: FloatArray, n: Int): ProjectPreview? {
        var minX = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var minY = Float.MAX_VALUE
        var maxY = -Float.MAX_VALUE
        var minZ = Float.MAX_VALUE
        var maxZ = -Float.MAX_VALUE
        for (i in 0 until n) {
            val x = raw[i * 3]
            val y = raw[i * 3 + 1]
            val z = raw[i * 3 + 2]
            if (!x.isFinite() || !y.isFinite() || !z.isFinite()) continue
            minX = min(minX, x); maxX = max(maxX, x)
            minY = min(minY, y); maxY = max(maxY, y)
            minZ = min(minZ, z); maxZ = max(maxZ, z)
        }
        if (minX > maxX) return null
        val spanXy = max(max(maxX - minX, maxY - minY), 1e-3f)
        val spanZ = max(maxZ - minZ, 1e-3f)
        val cx = (minX + maxX) / 2f
        val cy = (minY + maxY) / 2f
        val out = FloatArray(n * 3)
        var kept = 0
        for (i in 0 until n) {
            val x = raw[i * 3]
            val y = raw[i * 3 + 1]
            val z = raw[i * 3 + 2]
            if (!x.isFinite() || !y.isFinite() || !z.isFinite()) continue
            out[kept * 3] = ((x - cx) / spanXy + 0.5f).coerceIn(0f, 1f)
            out[kept * 3 + 1] = ((y - cy) / spanXy + 0.5f).coerceIn(0f, 1f)
            out[kept * 3 + 2] = ((z - minZ) / spanZ).coerceIn(0f, 1f)
            kept++
        }
        return if (kept == 0) null else ProjectPreview(out, kept)
    }
}

/**
 * The tile. Draws the project's real sampled cloud when one exists, and a
 * seeded placeholder when it does not — never a spinner, because a list that
 * flickers while scrolling is worse than a tile that is honest about being a
 * placeholder.
 *
 * ROUND 28 item 162 — **it is back in the list row**, 56 dp at the leading
 * edge (§D.5, finding P1c). Round 25 item 114 deleted it from the list on the
 * argument that a 108 dp preview above every row means four scans fill a
 * screen; that argument was against the 108 dp CARD, and it took the single
 * strongest differentiator between 66 otherwise identical rows with it. A
 * 56 dp tile inside a 72 dp row costs no height at all.
 *
 * @param cornerRadius the tile's radius. §D.5 asks for 8-12 on the row, and
 *   the gallery keeps the larger one because there it is the card's picture
 *   rather than a row's leading glyph. (It was a bare `14.dp`, which is off
 *   the 4 dp grid in both places.)
 */
@Composable
fun ProjectThumbnail(
    project: Project,
    modifier: Modifier = Modifier,
    cornerRadius: Dp = ScanDims.S3,
) {
    // ROUND 28 item 162 (P1j): an empty scan draws a trough TILE and nothing
    // else. It has no preview file, so the code below would fall through to
    // the seeded placeholder — and a scan that recorded nothing must not be
    // given an invented cloud to look at. The flat tile is the honest picture
    // of "there is nothing in here", and it is what §D.5's mockup draws.
    val empty = project.manifest.isEmptyScan
    var preview by remember(project.id) { mutableStateOf<ProjectPreview?>(null) }
    LaunchedEffect(project.id, project.manifest.pointCountEstimate) {
        preview = if (empty) null else ProjectPreviewCache.load(project)
    }

    val seed = remember(project.id) { project.id.hashCode().toLong() }
    val placeholder = remember(seed) { seededScatter(seed) }
    // ROUND 25 item 119: exhaustive, see `sensorBadgeColor`.
    val sensorTint = sensorBadgeColor(project.manifest.sensor)
    // ROUND 28 item 154: the tile reads the SAME lookup table the shader does,
    // rather than lerping between two sensor-identity colours. See [HeightRamp].
    val ramp = HeightRamp.Turbo

    Canvas(
        modifier = modifier
            .clip(RoundedCornerShape(cornerRadius))
            .background(if (empty) ScanColors.trough else ScanColors.viewport)
            .fillMaxSize(),
    ) {
        if (empty) return@Canvas
        val real = preview
        if (real != null) {
            drawRealCloud(real, sensorTint, ramp)
        } else {
            drawPlaceholder(placeholder, sensorTint, ramp)
        }
    }
}

/**
 * Projects the normalised sample with a light isometric tilt: x to the right,
 * the horizontal `y` axis raked upward, and height pushing the point up the
 * tile. Colour rides [HeightRamp] — the same Turbo lookup table `points.mat`
 * samples, so a thumbnail and the live viewport agree on what "high" looks
 * like. ROUND 28 item 154: it used to lerp teal-to-sand, which agreed with
 * nothing.
 */
private fun DrawScope.drawRealCloud(preview: ProjectPreview, tint: Color, ramp: HeightRamp) {
    val w = size.width
    val h = size.height
    // ── ROUND 28 item 162, the owner's call: ONE PIXEL. ────────────────────
    //
    // This was `min(w, h) * 0.011f`, which is a radius proportional to the
    // tile — 1.2 px on the old 108 dp card and 0.6 px on the new 56 dp row, so
    // the same cloud got fatter as the tile got bigger and vanished as it got
    // smaller. Neither is what a point cloud looks like. The owner's decision
    // is that the tile draws the REAL cloud, dense, at point size 1 px in
    // every tile it appears in — the density is the information, and fat dots
    // merge 4,000 samples into a blob.
    //
    // A literal, not a `dp`: this is a size in the drawing surface's own
    // pixels, and converting a dp here would make the point bigger on a denser
    // screen, which is the opposite of the intent.
    val r = 1f
    for (i in 0 until preview.count) {
        val nx = preview.points[i * 3]
        val ny = preview.points[i * 3 + 1]
        val nz = preview.points[i * 3 + 2]
        val px = (0.08f + nx * 0.84f) * w + (ny - 0.5f) * w * 0.16f
        val py = (0.94f - ny * 0.30f - nz * 0.62f) * h
        if (px < -r || px > w + r || py < -r || py > h + r) continue
        drawCircle(
            color = ramp.at(nz).copy(alpha = 0.55f + 0.35f * nz),
            radius = r,
            center = Offset(px, py),
        )
    }
    drawTrajectoryHint(tint, alpha = 0.0f)
}

/**
 * The seeded placeholder: a scatter plus the ember trajectory sweep the mockup
 * draws. Deterministic from the project id — the same project always draws the
 * same tile — and visibly quieter than a real cloud.
 */
private class SeededScatter(val xs: FloatArray, val ys: FloatArray, val zs: FloatArray)

private fun seededScatter(seed: Long): SeededScatter {
    val rnd = Random(seed)
    val n = 90
    val xs = FloatArray(n)
    val ys = FloatArray(n)
    val zs = FloatArray(n)
    for (i in 0 until n) {
        xs[i] = rnd.nextFloat()
        // Biased toward the band the trajectory sweeps through, so the tile
        // reads as "a scan along a path" rather than uniform noise.
        val t = rnd.nextFloat()
        ys[i] = (t * t * 0.75f + rnd.nextFloat() * 0.25f).coerceIn(0f, 1f)
        zs[i] = rnd.nextFloat()
    }
    return SeededScatter(xs, ys, zs)
}

private fun DrawScope.drawPlaceholder(scatter: SeededScatter, tint: Color, ramp: HeightRamp) {
    val w = size.width
    val h = size.height
    val r = min(w, h) * 0.013f
    for (i in scatter.xs.indices) {
        val nz = scatter.zs[i]
        drawCircle(
            color = ramp.at(nz).copy(alpha = 0.30f + 0.25f * nz),
            radius = r,
            center = Offset(scatter.xs[i] * w, (0.16f + scatter.ys[i] * 0.72f) * h),
        )
    }
    drawTrajectoryHint(tint, alpha = 1f)
}

/**
 * The ember trajectory sweep with its head dot. Drawn only on the placeholder
 * (alpha 0 elsewhere): a real preview stores points, not poses, and drawing an
 * invented path over real data would be the one dishonest pixel on the screen.
 */
private fun DrawScope.drawTrajectoryHint(tint: Color, alpha: Float) {
    if (alpha <= 0f) return
    val w = size.width
    val h = size.height
    val path = Path().apply {
        moveTo(w * 0.14f, h * 0.74f)
        cubicTo(w * 0.32f, h * 0.56f, w * 0.42f, h * 0.86f, w * 0.58f, h * 0.62f)
        cubicTo(w * 0.68f, h * 0.47f, w * 0.68f, h * 0.42f, w * 0.80f, h * 0.44f)
    }
    drawPath(
        path = path,
        color = Ember.copy(alpha = 0.85f * alpha),
        style = Stroke(width = max(2f, min(w, h) * 0.022f), cap = StrokeCap.Round),
    )
    drawCircle(
        color = Ember.copy(alpha = 0.30f * alpha),
        radius = min(w, h) * 0.075f,
        center = Offset(w * 0.80f, h * 0.44f),
    )
    drawCircle(
        color = Ember.copy(alpha = alpha),
        radius = min(w, h) * 0.042f,
        center = Offset(w * 0.80f, h * 0.44f),
    )
    // `tint` is the sensor identity; a hair of it under the head keeps a D6 and
    // a Mid-360 tile distinguishable at a glance in a long list.
    drawCircle(
        color = tint.copy(alpha = 0.22f * alpha),
        radius = min(w, h) * 0.115f,
        center = Offset(w * 0.80f, h * 0.44f),
    )
}
