package com.lidarscan.app.render

import android.content.Context
import android.view.Choreographer
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceView
import com.google.android.filament.Box
import com.google.android.filament.Camera
import com.google.android.filament.Engine
import com.google.android.filament.EntityManager
import com.google.android.filament.IndexBuffer
import com.google.android.filament.Material
import com.google.android.filament.MaterialInstance
import com.google.android.filament.RenderableManager
import com.google.android.filament.Renderer
import com.google.android.filament.Scene
import com.google.android.filament.SwapChain
import com.google.android.filament.Texture
import com.google.android.filament.TextureSampler
import com.google.android.filament.VertexBuffer
import com.google.android.filament.Viewport
import com.google.android.filament.android.DisplayHelper
import com.google.android.filament.android.UiHelper
import com.google.android.filament.utils.Manipulator
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap
import com.lidarscan.core.render.ColormapLut
import com.lidarscan.core.render.PointSizeMode
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.max

/**
 * How the Filament camera is driven.
 *
 * [AR] is B7's addition (Tech Spec §3.7): the camera follows the live ARCore
 * pose and uses ARCore's own projection, so the cloud sits registered in the
 * camera image drawn underneath by [com.lidarscan.app.ar.ArCameraBackgroundRenderer].
 * [ORBIT] and [FOLLOW] are B4's free-3D modes — §3.7's "Toggle AR view <->
 * free-orbit 3D" is the switch between them.
 */
enum class CameraMode { ORBIT, FOLLOW, AR }

/** `com.google.android.filament.Filament.init()`, called exactly once per process (loads `libfilament-jni.so`). */
private object FilamentLoader {
    @Volatile private var initialized = false
    fun ensureInitialized() {
        if (initialized) return
        synchronized(this) {
            if (initialized) return
            com.google.android.filament.Filament.init()
            initialized = true
        }
    }
}

/** Returned by [PointCloudRenderer.stats] — a plain poll, not a `Flow` (the Capture screen already gets pts/s from `EngineBridge`; this is GPU-side bookkeeping only, useful for debugging). */
data class PointCloudRenderStats(
    val residentPoints: Long = 0,
    val pagesDrawn: Int = 0,
    val boundsValid: Boolean = false,
)

/**
 * B4's live 3D point-cloud renderer: SurfaceView + Filament, driven by
 * [PointCloudSource] (either the live capture engine or B4's replay
 * engine). Mirrors the desktop C1 pipeline this is ported from
 * (`desktop/src/render/PagedCloudRenderer.{h,cpp}`, `desktop/materials/points.mat`)
 * one layer down: paged `VertexBuffer`s uploaded from JNI page reads, one
 * shared identity `IndexBuffer` (Filament requires an index buffer even for
 * `PrimitiveType.POINTS`), A14's `DisplayParamsUniforms` contract bound by
 * name onto a single shared `MaterialInstance`, and a per-frame poll of
 * `source.pageCount()/pageIdAt()/getPage()` — see `PointCloudSource`'s own
 * doc for why polling (not an event stream) is the correct approach here,
 * same reasoning as desktop's `sync()`.
 *
 * Ownership/threading: everything here runs on the UI thread (Filament's
 * Android idiom — `UiHelper` + a `Choreographer.FrameCallback`), touching
 * only Filament objects and this renderer's own GPU-page bookkeeping. The
 * `PointCloudSource` it polls does its own native calls synchronously on
 * this same thread (a handful of `scan_engine_get_point_page`/
 * `page_view()` calls per frame — page reads are non-blocking per
 * `page_store.h`'s "readers take no lock" contract).
 */
class PointCloudRenderer(
    private val context: Context,
    /**
     * B7: in AR mode this `SurfaceView` sits ON TOP of the ARCore camera
     * `GLSurfaceView`, so it must not paint its own background. Set at
     * construction rather than through a setter because `UiHelper.setOpaque`
     * has to be decided before `attachTo()` — the surface's pixel format is
     * fixed at that point.
     */
    private val translucent: Boolean = false,
) {

    private lateinit var engine: Engine
    private lateinit var renderer: Renderer
    private lateinit var scene: Scene
    private lateinit var view: com.google.android.filament.View
    private lateinit var camera: Camera
    private var cameraEntity: Int = 0
    private var swapChain: SwapChain? = null
    private var surfaceView: SurfaceView? = null

    private lateinit var uiHelper: UiHelper
    private lateinit var displayHelper: DisplayHelper
    private val choreographer: Choreographer = Choreographer.getInstance()
    private val frameCallback = FrameCallback()

    private var material: Material? = null
    private var materialInstance: MaterialInstance? = null
    private var colormapTexture: Texture? = null
    private var sharedIndexBuffer: IndexBuffer? = null
    private var sharedIndexBufferCapacity = 0

    private var manipulator: Manipulator? = null
    private var viewportWidth = 1
    private var viewportHeight = 1

    private var source: PointCloudSource? = null
    private var colorMode: ColorMode = ColorMode.RGB
    private var colormap: Colormap = Colormap.SPECTRUM
    private var pointSizePx: Float = 2.5f
    private var cameraMode: CameraMode = CameraMode.ORBIT

    private class GpuPage(
        val entity: Int,
        val vertexBuffer: VertexBuffer,
        val indexBuffer: IndexBuffer,
        var uploaded: Int,
        var capacity: Int,
    )

    private val gpuPages = LinkedHashMap<Int, GpuPage>()
    private var lastStats = PointCloudRenderStats()
    private val combinedBoundsMin = floatArrayOf(0f, 0f, 0f)
    private val combinedBoundsMax = floatArrayOf(0f, 0f, 0f)
    private var haveBounds = false

    fun attach(surfaceView: SurfaceView) {
        this.surfaceView = surfaceView
        FilamentLoader.ensureInitialized()

        engine = Engine.create()
        renderer = engine.createRenderer()
        scene = engine.createScene()
        view = engine.createView()
        cameraEntity = EntityManager.get().create()
        camera = engine.createCamera(cameraEntity)
        camera.setExposure(16.0f, 1.0f / 125.0f, 100.0f)

        view.scene = scene
        view.camera = camera

        loadMaterial()
        buildColormapTexture()
        applyStaticMaterialParams()

        manipulator = Manipulator.Builder()
            .targetPosition(0f, 0f, 0f)
            .orbitHomePosition(4f, 3f, 8f)
            .upVector(0f, 1f, 0f)
            .zoomSpeed(0.02f)
            .build(Manipulator.Mode.ORBIT)

        uiHelper = UiHelper(UiHelper.ContextErrorPolicy.DONT_CHECK)
        uiHelper.renderCallback = SurfaceCallback()
        displayHelper = DisplayHelper(context)
        if (translucent) {
            // Three things have to agree for the camera image below to show
            // through, and missing any one of them yields a black screen with
            // points on it rather than an AR overlay:
            //   1. the surface itself must be translucent (UiHelper, which
            //      also sets the swap-chain's CONFIG_TRANSPARENT flag),
            //   2. the Filament View must BLEND rather than overwrite,
            //   3. the clear colour must be fully TRANSPARENT black — a clear
            //      to opaque black would paint over the camera every frame.
            uiHelper.isOpaque = false
            uiHelper.isMediaOverlay = true
            view.blendMode = com.google.android.filament.View.BlendMode.TRANSLUCENT
            renderer.clearOptions = Renderer.ClearOptions().apply {
                clear = true
                clearColor = doubleArrayOf(0.0, 0.0, 0.0, 0.0)
            }
            // Post-processing resolves into an opaque target on some drivers;
            // off is both correct here and cheaper, and this view draws points
            // with no tone mapping to preserve anyway.
            view.isPostProcessingEnabled = false
        }
        uiHelper.attachTo(surfaceView)

        choreographer.postFrameCallback(frameCallback)
    }

    fun detach() {
        choreographer.removeFrameCallback(frameCallback)
        if (!::uiHelper.isInitialized) return
        uiHelper.detach()

        gpuPages.values.forEach { destroyGpuPage(it) }
        gpuPages.clear()
        sharedIndexBuffer?.let { engine.destroyIndexBuffer(it) }
        sharedIndexBuffer = null

        colormapTexture?.let { engine.destroyTexture(it) }
        materialInstance?.let { engine.destroyMaterialInstance(it) }
        material?.let { engine.destroyMaterial(it) }

        engine.destroyCameraComponent(cameraEntity)
        EntityManager.get().destroy(cameraEntity)
        engine.destroyRenderer(renderer)
        engine.destroyView(view)
        engine.destroyScene(scene)
        engine.destroy()
        surfaceView = null
    }

    fun setSource(newSource: PointCloudSource?) {
        source = newSource
        if (newSource == null) {
            gpuPages.values.forEach { destroyGpuPage(it) }
            gpuPages.clear()
            haveBounds = false
        }
    }

    fun setColorMode(mode: ColorMode) {
        colorMode = mode
        applyDynamicMaterialParams()
    }

    fun setColormap(cm: Colormap) {
        colormap = cm
        applyDynamicMaterialParams()
    }

    fun setPointSizePx(px: Float) {
        pointSizePx = px.coerceIn(0.5f, 32f)
        applyDynamicMaterialParams()
    }

    fun setCameraMode(mode: CameraMode) {
        cameraMode = mode
    }

    /**
     * B7: the live ARCore camera, as two COLUMN-major matrices exactly as
     * ARCore hands them out (`Camera.getProjectionMatrix()` and
     * `Pose.toMatrix()` of the display-oriented pose). Filament's Java API is
     * column-major too, so nothing is transposed on this path — which is the
     * opposite of the engine's row-major C ABI, and the reason both
     * conventions are named wherever a matrix crosses a boundary in this app.
     *
     * Called from the AR/GL thread; the values are latched and consumed on the
     * next Choreographer frame on the UI thread, so the arrays are copied
     * rather than retained.
     */
    fun setArCamera(projectionColumnMajor: FloatArray, modelColumnMajor: FloatArray) {
        if (projectionColumnMajor.size != 16 || modelColumnMajor.size != 16) return
        synchronized(arCameraLock) {
            projectionColumnMajor.copyInto(arProjection)
            modelColumnMajor.copyInto(arModel)
            arCameraValid = true
        }
    }

    fun stats(): PointCloudRenderStats = lastStats

    /** Forwarded from the Compose `AndroidView`'s touch listener — orbit-drag/pinch-zoom via [Manipulator]. */
    fun onTouch(event: MotionEvent): Boolean {
        if (cameraMode != CameraMode.ORBIT) return false
        val m = manipulator ?: return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> m.grabBegin(event.x.toInt(), event.y.toInt(), false)
            MotionEvent.ACTION_MOVE -> m.grabUpdate(event.x.toInt(), event.y.toInt())
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> m.grabEnd()
        }
        return true
    }

    /** Two-finger pinch delta (positive == zoom in), forwarded from a `ScaleGestureDetector` in `PointCloudView`. */
    fun onScale(focusX: Float, focusY: Float, scaleDelta: Float) {
        manipulator?.scroll(focusX.toInt(), focusY.toInt(), -scaleDelta)
    }

    // --- setup helpers -----------------------------------------------------

    private fun loadMaterial() {
        val bytes = context.assets.open("materials/points.filamat").use { it.readBytes() }
        val buffer = ByteBuffer.allocateDirect(bytes.size).order(ByteOrder.nativeOrder())
        buffer.put(bytes).flip()
        material = Material.Builder().payload(buffer, buffer.remaining()).build(engine)
        materialInstance = material!!.createInstance()
    }

    private fun buildColormapTexture() {
        val tex = Texture.Builder()
            .width(ColormapLut.SIZE)
            .height(3)
            .levels(1)
            .sampler(Texture.Sampler.SAMPLER_2D)
            .format(Texture.InternalFormat.RGBA8)
            .build(engine)
        val bytes = ColormapLut.buildTextureRgba8()
        val buffer = ByteBuffer.allocateDirect(bytes.size).order(ByteOrder.nativeOrder())
        buffer.put(bytes).flip()
        val descriptor = Texture.PixelBufferDescriptor(
            buffer, Texture.Format.RGBA, Texture.Type.UBYTE,
        )
        tex.setImage(engine, 0, descriptor)
        colormapTexture = tex
        val sampler = TextureSampler(
            TextureSampler.MinFilter.LINEAR,
            TextureSampler.MagFilter.LINEAR,
            TextureSampler.WrapMode.CLAMP_TO_EDGE,
        )
        materialInstance?.setParameter("colormapLut", tex, sampler)
    }

    /** Fields of DisplayParamsUniforms this renderer treats as fixed (A14 §4 — clipping/gamma left at neutral defaults; B10's future display-params panel is what would surface these). */
    private fun applyStaticMaterialParams() {
        val mi = materialInstance ?: return
        mi.setParameter("gamma", 1.0f)
        mi.setParameter("invert", 0)
        mi.setParameter("brightness", 1.0f)
        mi.setParameter("adaptiveReferenceM", 5.0f)
        mi.setParameter("worldSizeM", 0.01f)
        mi.setParameter("pxPerMeterAt1m", 500f) // recomputed properly in onResized()
        mi.setParameter("clipEnabledMask", 0)
        mi.setParameter("clipHeightMin", -1000f)
        mi.setParameter("clipHeightMax", 1000f)
        mi.setParameter("clipBoxMin", -1000f, -1000f, -1000f)
        mi.setParameter("clipBoxMax", 1000f, 1000f, 1000f)
        applyDynamicMaterialParams()
    }

    private fun applyDynamicMaterialParams() {
        val mi = materialInstance ?: return
        mi.setParameter("colorMode", colorMode.ordinal)
        mi.setParameter("colormap", colormap.ordinal)
        // A14 default ranges (profile_defaults(), A14-display.md §5): height
        // 0..3m (typical indoor ceiling), intensity already-normalized 0..1.
        when (colorMode) {
            ColorMode.HEIGHT -> {
                mi.setParameter("valueMin", 0.0f)
                mi.setParameter("valueMax", 3.0f)
            }
            ColorMode.INTENSITY -> {
                mi.setParameter("valueMin", 0.0f)
                mi.setParameter("valueMax", 1.0f)
            }
            else -> Unit
        }
        mi.setParameter("pointSizeMode", PointSizeMode.FIXED_PIXELS.ordinal)
        mi.setParameter("pointSizeMinPx", pointSizePx)
        mi.setParameter("pointSizeMaxPx", pointSizePx)
    }

    private fun ensureSharedIndexBuffer(minCapacity: Int): IndexBuffer {
        sharedIndexBuffer?.let { if (sharedIndexBufferCapacity >= minCapacity) return it }
        sharedIndexBuffer?.let { engine.destroyIndexBuffer(it) }

        val ib = IndexBuffer.Builder()
            .indexCount(minCapacity)
            .bufferType(IndexBuffer.Builder.IndexType.UINT)
            .build(engine)
        val idx = ByteBuffer.allocateDirect(minCapacity * 4).order(ByteOrder.nativeOrder())
        val ints = idx.asIntBuffer()
        for (i in 0 until minCapacity) ints.put(i)
        idx.rewind()
        ib.setBuffer(engine, idx)

        sharedIndexBuffer = ib
        sharedIndexBufferCapacity = minCapacity
        return ib
    }

    // --- per-frame sync ------------------------------------------------------

    private fun syncPointCloud() {
        val src = source ?: return
        if (!src.isAvailable) return
        val mi = materialInstance ?: return

        val count = src.pageCount()
        var resident = 0L
        var pagesDrawn = 0

        for (i in 0 until count) {
            val pageId = src.pageIdAt(i)
            if (pageId < 0) continue
            val page = src.getPage(pageId) ?: continue
            resident += page.count

            var gpu = gpuPages[pageId]
            if (gpu == null) {
                val capacity = max(page.capacity, page.count)
                val vb = VertexBuffer.Builder()
                    .bufferCount(1)
                    .vertexCount(capacity)
                    .attribute(
                        VertexBuffer.VertexAttribute.POSITION, 0,
                        VertexBuffer.AttributeType.FLOAT3, 0, 16,
                    )
                    .attribute(
                        VertexBuffer.VertexAttribute.COLOR, 0,
                        VertexBuffer.AttributeType.UBYTE4, 12, 16,
                    )
                    .normalized(VertexBuffer.VertexAttribute.COLOR, true)
                    .build(engine)

                val ib = ensureSharedIndexBuffer(capacity)
                val entity = EntityManager.get().create()
                RenderableManager.Builder(1)
                    // Desktop's PagedCloudRenderer culls per-page against real
                    // bounds (see its header: "frustum culling actually
                    // culls"); B4 keeps every page always-visible instead —
                    // a deliberate simplification given the time box, not a
                    // correctness requirement (a mis-set/stale AABB would
                    // just make points vanish early, which is worse than
                    // the modest overdraw cost of skipping culling
                    // entirely at the point counts B4 targets). Documented
                    // as follow-up work in NOTES.md.
                    .culling(false)
                    .castShadows(false)
                    .receiveShadows(false)
                    .geometry(0, RenderableManager.PrimitiveType.POINTS, vb, ib, 0, page.count)
                    .material(0, mi)
                    .build(engine, entity)
                scene.addEntity(entity)

                gpu = GpuPage(entity, vb, ib, uploaded = 0, capacity = capacity)
                gpuPages[pageId] = gpu
            }

            if (page.count > gpu.uploaded) {
                val newPoints = page.count - gpu.uploaded
                val newBytes = page.buffer
                newBytes.position(gpu.uploaded * 16)
                newBytes.limit(page.count * 16)
                gpu.vertexBuffer.setBufferAt(engine, 0, newBytes.slice(), gpu.uploaded * 16, newPoints * 16)
                gpu.uploaded = page.count

                val rm = engine.renderableManager
                val instance = rm.getInstance(gpu.entity)
                if (instance != 0) {
                    rm.setGeometryAt(
                        instance, 0, RenderableManager.PrimitiveType.POINTS,
                        gpu.vertexBuffer, gpu.indexBuffer, 0, page.count,
                    )
                    rm.setAxisAlignedBoundingBox(
                        instance,
                        Box(
                            (page.boundsMinX + page.boundsMaxX) / 2f,
                            (page.boundsMinY + page.boundsMaxY) / 2f,
                            (page.boundsMinZ + page.boundsMaxZ) / 2f,
                            max((page.boundsMaxX - page.boundsMinX) / 2f, 0.05f),
                            max((page.boundsMaxY - page.boundsMinY) / 2f, 0.05f),
                            max((page.boundsMaxZ - page.boundsMinZ) / 2f, 0.05f),
                        ),
                    )
                }
                updateCombinedBounds(page)
            }
            pagesDrawn++
        }

        lastStats = PointCloudRenderStats(resident, pagesDrawn, haveBounds)
    }

    private fun updateCombinedBounds(page: com.lidarscan.app.engine.NativePointPage) {
        if (!haveBounds) {
            combinedBoundsMin[0] = page.boundsMinX; combinedBoundsMin[1] = page.boundsMinY; combinedBoundsMin[2] = page.boundsMinZ
            combinedBoundsMax[0] = page.boundsMaxX; combinedBoundsMax[1] = page.boundsMaxY; combinedBoundsMax[2] = page.boundsMaxZ
            haveBounds = true
        } else {
            combinedBoundsMin[0] = minOf(combinedBoundsMin[0], page.boundsMinX)
            combinedBoundsMin[1] = minOf(combinedBoundsMin[1], page.boundsMinY)
            combinedBoundsMin[2] = minOf(combinedBoundsMin[2], page.boundsMinZ)
            combinedBoundsMax[0] = maxOf(combinedBoundsMax[0], page.boundsMaxX)
            combinedBoundsMax[1] = maxOf(combinedBoundsMax[1], page.boundsMaxY)
            combinedBoundsMax[2] = maxOf(combinedBoundsMax[2], page.boundsMaxZ)
        }
    }

    private fun destroyGpuPage(gpu: GpuPage) {
        scene.removeEntity(gpu.entity)
        engine.destroyEntity(gpu.entity)
        EntityManager.get().destroy(gpu.entity)
        engine.destroyVertexBuffer(gpu.vertexBuffer)
    }

    private fun updateCamera() {
        val centerX = if (haveBounds) (combinedBoundsMin[0] + combinedBoundsMax[0]) / 2f else 0f
        val centerY = if (haveBounds) (combinedBoundsMin[1] + combinedBoundsMax[1]) / 2f else 0f
        val centerZ = if (haveBounds) (combinedBoundsMin[2] + combinedBoundsMax[2]) / 2f else 0f

        when (cameraMode) {
            CameraMode.AR -> {
                val projection = DoubleArray(16)
                val model = FloatArray(16)
                val valid: Boolean
                synchronized(arCameraLock) {
                    valid = arCameraValid
                    if (valid) {
                        for (i in 0 until 16) projection[i] = arProjection[i].toDouble()
                        arModel.copyInto(model)
                    }
                }
                if (!valid) return
                // ARCore's projection already encodes the physical camera's
                // FoV, aspect and principal-point offset — which is exactly
                // what makes the points land on the right pixels. Substituting
                // our own 45° perspective here would look plausible and be
                // wrong by several degrees of FoV.
                camera.setCustomProjection(projection, AR_NEAR_M, AR_FAR_M)
                camera.setModelMatrix(model)
                lastEye = floatArrayOf(model[12], model[13], model[14])
            }
            CameraMode.ORBIT -> {
                // filament-utils' Manipulator has no runtime "retarget"
                // setter (its target is fixed at Builder.build() time) — so
                // orbit is anchored at the session's local-frame origin
                // (0,0,0, PointVertex's own coordinate origin — point_page.h:
                // "session's local metric frame"), not a moving centroid.
                // That is also the more usable behaviour while a capture is
                // growing: a target that silently drifts as new pages arrive
                // would fight the user's own drag/pinch input.
                val m = manipulator ?: return
                val eye = FloatArray(3); val target = FloatArray(3); val up = FloatArray(3)
                m.getLookAt(eye, target, up)
                camera.lookAt(
                    eye[0].toDouble(), eye[1].toDouble(), eye[2].toDouble(),
                    target[0].toDouble(), target[1].toDouble(), target[2].toDouble(),
                    up[0].toDouble(), up[1].toDouble(), up[2].toDouble(),
                )
                lastEye = eye
            }
            CameraMode.FOLLOW -> {
                // Simple chase cam: fixed offset above/behind the combined
                // bounds centroid, updated every frame as the cloud grows —
                // "follow" in the sense of always framing the latest data,
                // not a device-pose-driven AR follow (no ARCore in B4's
                // scope; that lands with B7's ARCore mount-calibration work).
                val span = if (haveBounds) {
                    max(
                        combinedBoundsMax[0] - combinedBoundsMin[0],
                        max(
                            combinedBoundsMax[1] - combinedBoundsMin[1],
                            combinedBoundsMax[2] - combinedBoundsMin[2],
                        ),
                    )
                } else {
                    5f
                }
                val distance = max(span * 1.2f, 3f)
                val eyeY = centerY + distance * 0.5f
                val eyeZ = centerZ + distance
                camera.lookAt(
                    centerX.toDouble(), eyeY.toDouble(), eyeZ.toDouble(),
                    centerX.toDouble(), centerY.toDouble(), centerZ.toDouble(),
                    0.0, 1.0, 0.0,
                )
                lastEye = floatArrayOf(centerX, eyeY, eyeZ)
            }
        }
    }

    private inner class SurfaceCallback : UiHelper.RendererCallback {
        override fun onNativeWindowChanged(surface: Surface) {
            swapChain?.let { engine.destroySwapChain(it) }
            swapChain = engine.createSwapChain(surface)
            surfaceView?.display?.let { displayHelper.attach(renderer, it) }
        }

        override fun onDetachedFromSurface() {
            displayHelper.detach()
            swapChain?.let {
                engine.destroySwapChain(it)
                engine.flushAndWait()
                swapChain = null
            }
        }

        override fun onResized(width: Int, height: Int) {
            viewportWidth = width
            viewportHeight = height
            val aspect = width.toDouble() / height.toDouble().coerceAtLeast(1.0)
            // Not in AR mode: ARCore's own projection owns the camera there,
            // and re-setting a 45° perspective on a resize would silently
            // un-register the overlay until the next AR frame arrived.
            if (cameraMode != CameraMode.AR) {
                camera.setProjection(45.0, aspect, 0.05, 2000.0, Camera.Fov.VERTICAL)
            }
            view.viewport = Viewport(0, 0, width, height)
            manipulator?.setViewport(width, height)
            // px per metre at 1m depth = viewportHeightPx / (2 * tan(fovY/2)) — points.mat's comment.
            val fovYRad = Math.toRadians(45.0)
            val pxPerMeterAt1m = (height / (2.0 * Math.tan(fovYRad / 2.0))).toFloat()
            materialInstance?.setParameter("pxPerMeterAt1m", pxPerMeterAt1m)
        }
    }

    private inner class FrameCallback : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            choreographer.postFrameCallback(this)
            if (!::uiHelper.isInitialized || !uiHelper.isReadyToRender) return
            syncPointCloud()
            updateCamera()
            materialInstance?.setParameter(
                "cameraPos",
                combinedBoundsCameraX(), combinedBoundsCameraY(), combinedBoundsCameraZ(),
            )
            val sc = swapChain ?: return
            if (renderer.beginFrame(sc, frameTimeNanos)) {
                renderer.render(view)
                renderer.endFrame()
            }
        }
    }

    // camera.getPosition isn't exposed pre-computed in older Filament Kotlin
    // bindings uniformly across versions, so the material's `cameraPos` is
    // tracked from the same eye point updateCamera() just computed, cached
    // here rather than re-deriving from the Camera object.
    private var lastEye = floatArrayOf(0f, 0f, 0f)

    // B7: latched ARCore camera matrices (written from the AR/GL thread, read
    // on the Choreographer frame).
    private val arCameraLock = Any()
    private val arProjection = FloatArray(16)
    private val arModel = FloatArray(16)
    private var arCameraValid = false

    private fun combinedBoundsCameraX() = lastEye[0]
    private fun combinedBoundsCameraY() = lastEye[1]
    private fun combinedBoundsCameraZ() = lastEye[2]

    private companion object {
        // Matched to what is asked of ARCore's own projection; both must agree
        // or the depth range differs from the camera image's implied one.
        const val AR_NEAR_M = 0.05
        const val AR_FAR_M = 100.0
    }
}
