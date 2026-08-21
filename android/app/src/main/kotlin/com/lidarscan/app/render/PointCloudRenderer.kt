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
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap
import com.lidarscan.core.render.ColormapLut
import com.lidarscan.core.render.FollowCamera
import com.lidarscan.core.render.FollowCameraConfig
import com.lidarscan.core.render.GpuPageBudget
import com.lidarscan.core.render.PointSizeMode
import com.lidarscan.core.render.UpAxis
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.max
import kotlin.math.sqrt

/**
 * How the Filament camera is driven.
 *
 * [AR] is B7's addition (Tech Spec §3.7): the camera follows the live ARCore
 * pose and uses ARCore's own projection, so the cloud sits registered in the
 * camera image drawn underneath by [com.lidarscan.app.ar.ArCameraBackgroundRenderer].
 * [ORBIT] and [FOLLOW] are B4's free-3D modes — §3.7's "Toggle AR view <->
 * free-orbit 3D" is the switch between them.
 *
 * ROUND 8: [FOLLOW] is now a real third-person chase camera driven by
 * [com.lidarscan.core.render.FollowCamera] (behind and above the rig, look-at
 * the rig, framing fitted to *recent* geometry). It used to be a fixed offset
 * from the whole cloud's bounding-box centroid, which followed nothing and zoomed
 * out without bound as the walk grew — see `FollowCamera`'s header for the
 * derivation and for why a *forward*-facing follow is the wrong answer for a D6
 * specifically. [ORBIT] is untouched: it remains the free camera the operator
 * drives by hand through filament-utils' `Manipulator`.
 */
enum class CameraMode { ORBIT, FOLLOW, AR }

/**
 * Loads Filament's native libraries, exactly once per process.
 *
 * **Two calls, not one, and the second was a real crash.** `Filament.init()`
 * loads `libfilament-jni.so` and nothing else; `Manipulator` lives in the
 * separate `filament-utils` artifact and its JNI lives in
 * `libfilament-utils-jni.so`, which only `Utils.init()` loads. B4 called just
 * the first, so the very first `Manipulator.Builder().build()` threw
 * `UnsatisfiedLinkError: No implementation found for
 * com.google.android.filament.utils.Manipulator.nCreateBuilder()` and took the
 * process down.
 *
 * This compiled cleanly, passed every unit test and was invisible to `javap`
 * and `llvm-nm` — the classes and symbols all exist, they were just in an
 * unloaded library. It was found the first time the 3D view was attached on an
 * actual (emulated) device, which is why NOTES.md's "device-deferred" lists
 * were worth taking seriously rather than treating as a formality.
 */
private object FilamentLoader {
    @Volatile private var initialized = false
    fun ensureInitialized() {
        if (initialized) return
        synchronized(this) {
            if (initialized) return
            com.google.android.filament.Filament.init()
            com.google.android.filament.utils.Utils.init()
            initialized = true
        }
    }
}

/** Returned by [PointCloudRenderer.stats] — a plain poll, not a `Flow` (the Capture screen already gets pts/s from `EngineBridge`; this is GPU-side bookkeeping only, useful for debugging). */
data class PointCloudRenderStats(
    val residentPoints: Long = 0,
    val pagesDrawn: Int = 0,
    val boundsValid: Boolean = false,
    /**
     * ROUND 5 AUDIT bugfix: true once a `SCAN_STREAM_SLAM_MAP` page has
     * actually been seen — mirrors [StreamFilter.MAPPED_ONLY]'s own
     * `mappedSeen` fallback rule. The Capture screen's "LIVE MAP · SLAM" vs
     * "RAW" chip used to key off `liveSlam` alone (the *requested* mode), which
     * reads as "LIVE MAP" for the whole stretch `MAPPED_ONLY` is still falling
     * back to drawing raw pages because A6/A8 have not resolved a mapped page
     * yet — telling the operator they are looking at a registered map while
     * they are actually looking at raw, un-extruded sensor-frame points. For a
     * D6 session in particular, that raw fallback view is a flat-looking fan
     * pattern with no pushbroom applied, which is exactly what a field report
     * of "the scan is a flat plane, not 3D" looks like if the operator judged
     * the capture by the live screen rather than the resolved `.lscan`.
     */
    val hasSeenMappedPage: Boolean = false,
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

    /**
     * ROUND 22 item 91 (iv): shared `IndexBuffer`s that have been superseded but
     * are **still referenced by live [GpuPage]s**, awaiting the frame in which
     * nothing points at them any more.
     *
     * [ensureSharedIndexBuffer] used to `destroyIndexBuffer` the old buffer the
     * instant a bigger one was needed — while every already-built GpuPage still
     * held that exact object and handed it to `setGeometryAt` on every frame
     * that page uploaded. That is a use-after-free of a native Filament object
     * from the UI thread: it survives exactly as long as the driver happens not
     * to reuse the memory, which is why it presented as an intermittent crash
     * on the LOD slider (the slider is what makes the renderer ask for a bigger
     * index buffer in the first place) rather than as a reliable one.
     */
    private val retiredIndexBuffers = ArrayList<IndexBuffer>()

    // ── ROUND 16 item 59: THE WALKED PATH, INSIDE THE CLOUD ─────────────────
    //
    // Owner, on 0.9.0: *"i want to see the path of mine showing in the
    // pointcloud too for me to check if the scan is right"*, and the second
    // half of that sentence is the requirement. A cloud alone cannot be checked
    // by eye: a room that has folded at a re-anchor and a room that is simply
    // an odd shape look identical. The walk is the reference — you know where
    // you went — so drawing it in the same frame as the returns turns "does
    // this look right" from a guess into a comparison.
    //
    // It has to be in the SAME frame, not an overlay: the ribbon is built from
    // the same poses the returns were resolved against, so a stitch or a
    // healing transform that moves the cloud moves the path with it. A path
    // drawn from an uncorrected trajectory over a corrected cloud would be a
    // lie that looked like a diagnosis.
    //
    // ROUND 5.3 refused to build this and stated the cost: *"drawing it in the
    // 3D scene would mean a second material, a second geometry upload path and
    // a per-frame rebuild of a line strip"*. Two of those three were right and
    // are paid below; the third is not, because the trail only changes when the
    // operator has walked 15 cm, which at walking pace is about five times a
    // second against a 60 Hz frame. [trailDirty] is what makes it a rebuild on
    // change rather than a rebuild per frame.
    private var trailMaterial: Material? = null
    private var trailMaterialInstance: MaterialInstance? = null
    private var trailEntity = 0
    private var trailVertexBuffer: VertexBuffer? = null
    private var trailIndexBuffer: IndexBuffer? = null
    private var trailCapacity = 0
    private var trailCount = 0

    /** Guards [pendingTrail] — written from the UI thread, read on the GL thread. */
    private val trailLock = Any()
    private var pendingTrailCount = 0
    private var trailDirty = false

    @Volatile
    private var trailVisible = true

    /**
     * ROUND 16 item 59 — the path, in world metres, as `x, y, z` triples plus a
     * packed RGBA per vertex.
     *
     * Takes the colours from the CALLER rather than deriving them here, because
     * what a colour means differs between the two screens and only the caller
     * knows which it is: live, the gradient is "how long ago" and the muted
     * stretches are where ARCore was not tracking; in Review it is the same
     * gradient over the whole sealed walk. One renderer, two meanings, no
     * branch in the renderer.
     *
     * `xyz` must hold `3 * count` floats and `rgba` exactly `count`. Safe from
     * any thread; the upload happens on the next frame.
     */
    fun setTrail(xyz: FloatArray, rgba: IntArray, count: Int) {
        val n = count.coerceAtMost(minOf(xyz.size / 3, rgba.size)).coerceAtLeast(0)
        // Interleaved to the same 16-byte stride the point pages use — three
        // floats then a packed RGBA8 — so the vertex layout below is the one
        // this renderer already knows how to describe, and `trail.mat`'s
        // `requires : [ color ]` is satisfied by the same attribute pair.
        val packed = ByteBuffer.allocateDirect(n * TRAIL_STRIDE_BYTES)
            .order(ByteOrder.nativeOrder())
        for (i in 0 until n) {
            packed.putFloat(xyz[i * 3])
            packed.putFloat(xyz[i * 3 + 1])
            packed.putFloat(xyz[i * 3 + 2])
            packed.putInt(rgba[i])
        }
        packed.flip()
        // ROUND 19 item 75: the trail's newest point IS the operator, and it
        // is already flowing through here at pose rate — no new plumbing.
        if (n > 0) compass.setOperator(xyz[(n - 1) * 3], xyz[(n - 1) * 3 + 2])
        synchronized(trailLock) {
            pendingTrailBytes = packed
            pendingTrailCount = n
            trailDirty = true
        }
    }

    private var pendingTrailBytes: ByteBuffer? = null

    /** ROUND 16 item 59: the view control's toggle. Default on. */
    fun setTrailVisible(visible: Boolean) {
        if (trailVisible == visible) return
        trailVisible = visible
        synchronized(trailLock) { trailDirty = true }
    }

    /**
     * ROUND 25 item 117 — the viewer's camera, replacing filament-utils'
     * `Manipulator`.
     *
     * `@Volatile` because it is written on the touch (main) thread and read on
     * the Choreographer's render thread. It is an immutable value type, so a
     * torn read is impossible: the render thread either sees the old camera or
     * the new one, never half of each. That is the entire reason it is a
     * `data class` of floats rather than six mutable fields.
     */
    @Volatile
    private var orbitCamera: com.lidarscan.core.render.OrbitCamera =
        com.lidarscan.core.render.OrbitCamera.HOME
    private var viewportWidth = 1
    private var viewportHeight = 1

    private var source: PointCloudSource? = null
    private var colorMode: ColorMode = ColorMode.RGB
    private var colormap: Colormap = Colormap.SPECTRUM
    private var pointSizePx: Float = 2.5f
    private var cameraMode: CameraMode = CameraMode.ORBIT

    /** B10: null on the Capture screen (which drives the three simple setters); set on Review. */
    private var displayParams: com.lidarscan.core.render.DisplayParams? = null

    /**
     * B10 / §3.12's "degrade via LOD, never framerate". A **soft** admission
     * ceiling: once this many points are resident on the GPU, further pages are
     * not uploaded this frame. That is coarse-to-fine only in page order, not a
     * real octree LOD — the honest description is "stop before the budget",
     * and the Review screen says so next to the slider rather than implying a
     * decimation that is not happening.
     */
    private var lodPointBudget: Long = Long.MAX_VALUE

    /**
     * ROUND 22 item 100 — the tier whose resident-bytes ceiling this renderer
     * enforces.
     *
     * `STANDARD` by default, which is exactly item 91's 256 MiB constant, so a
     * renderer nobody has told about the device behaves as it did before this
     * item rather than losing its ceiling entirely. `PointCloudView` sets the
     * real one.
     *
     * This is the LAST of three places the ceiling is applied — the controls
     * cannot offer more (`GpuPageBudget.maxSelectableLodPoints`) and
     * `SettingsRepository` clamps on load and on save. It is here anyway
     * because the failure it prevents is a process death, and a memory ceiling
     * enforced only by the code paths that happen to ask for it is a memory
     * ceiling with a hole in it.
     */
    private var deviceTier: com.lidarscan.core.capture.DeviceTier =
        com.lidarscan.core.capture.DeviceTier.STANDARD

    /** ROUND 22 item 100. Safe to call repeatedly; takes effect on the next frame. */
    fun setDeviceTier(tier: com.lidarscan.core.capture.DeviceTier) {
        deviceTier = tier
    }

    /**
     * One engine page, resident on the GPU.
     *
     * ROUND 22 item 91 (ii): [capacity] means **vertices actually allocated in
     * [vertexBuffer]**, which since this round is
     * [GpuPageBudget.allocationVertices] of the page's *count* — not the engine
     * page's capacity, which for Review's `1 << 20` store is 16 MiB per page
     * regardless of how few points it holds. [vertexBuffer] is therefore a
     * `var`: a page that outgrows its buffer is reallocated in place by
     * [growGpuPage], and [capacity] moves with it.
     */
    private class GpuPage(
        val entity: Int,
        var vertexBuffer: VertexBuffer,
        var indexBuffer: IndexBuffer,
        var uploaded: Int,
        var capacity: Int,
    )

    private val gpuPages = LinkedHashMap<Int, GpuPage>()

    /** B3: `pageId -> SCAN_STREAM_*`, so [setStreamFilter] can drop the pages that no longer qualify. */
    private val pageStreams = HashMap<Int, Int>()

    private var streamFilter: StreamFilter = StreamFilter.ALL

    /**
     * ROUND 5 (item 10): the live-view refresh cap, in frames per second. 0 means
     * "every vsync", which is the pre-round-5 behaviour and stays the default.
     *
     * This is a **viewport throttle, not a capture throttle** — the engine keeps
     * decoding and recording at full rate; only the page sync + Filament frame
     * are skipped. That distinction is why it is safe to expose as a live
     * display control: turning the preview down to 10 fps on a hot phone during
     * a long walk costs preview smoothness and nothing in the `.lscan`.
     * §3.12's "degrade via LOD, never framerate" is about *automatic*
     * degradation; this is the operator asking.
     */
    private var maxRefreshHz: Int = 0

    /** Frame-time of the last rendered frame, for [maxRefreshHz]'s interval check. */
    private var lastRenderedFrameNs: Long = 0L

    /**
     * ROUND 5.3 (item 17): the hardware-derived ceiling and the auto-downshift.
     *
     * The governor is pure policy in `:core` (unit-tested); this class only feeds
     * it measured intervals between **rendered** frames and applies its verdict.
     * Recording is never involved: the engine's decode/write threads do not know
     * this exists.
     */
    private var governor: com.lidarscan.core.render.RefreshGovernor? = null

    /** Called (on the UI thread) when the governor eases the live view down a notch. */
    var onRefreshDownshift: ((Int) -> Unit)? = null

    /** True once a `SCAN_STREAM_SLAM_MAP` page has been seen — see [StreamFilter.MAPPED_ONLY]'s fallback. */
    private var mappedPageSeen = false
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

        // ROUND 25 item 117: no `Manipulator.Builder()` here any more. Its
        // framing survives verbatim as `OrbitCamera.HOME` (target 0,0,0, eye
        // 4,3,8, Y up) so the viewer opens on exactly the view it always has —
        // adopting a new camera must not also be a silent change of default
        // framing. The JNI-load ordering note at the top of this file no longer
        // applies to the camera, and is kept because `Utils.init()` is still
        // what loads the rest of filament-utils.

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
        // ROUND 22 item 91 (iv): retired index buffers are held alive only while
        // a page still references one. Nothing does now, and `detach()` must
        // free EVERYTHING regardless — this is the last chance before the
        // Engine itself goes away.
        retiredIndexBuffers.forEach { engine.destroyIndexBuffer(it) }
        retiredIndexBuffers.clear()
        sharedIndexBuffer?.let { engine.destroyIndexBuffer(it) }
        sharedIndexBuffer = null
        sharedIndexBufferCapacity = 0

        destroyTrail()
        trailMaterialInstance?.let { engine.destroyMaterialInstance(it) }
        trailMaterialInstance = null
        trailMaterial?.let { engine.destroyMaterial(it) }
        trailMaterial = null

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

    /**
     * Point the renderer at a different cloud — or at nothing.
     *
     * **ROUND 22 item 91 (v): every state reset below used to happen ONLY on
     * `setSource(null)`.** Handing the renderer a different *non-null* source
     * cleared nothing at all: the previous session's GPU pages stayed in the
     * Filament scene, keyed by page ids that mean something entirely different
     * in the new source's store, so the new cloud drew on top of the old one and
     * the old one's 16 MiB-per-page VertexBuffers were never freed. Two page
     * stores' worth of resident VBOs is the same memory ceiling item 91 is about
     * from the other end.
     *
     * `CaptureViewModel.clearLiveViewport()` documents the workaround it had to
     * invent for this — *"handing it a different non-null source clears nothing.
     * So the flow is null'd and then re-read"*. That dance is now belt and
     * braces rather than the only thing holding the viewport together.
     *
     * The identity check is the whole guard: this is called on **every
     * recomposition** of `PointCloudView`, and a reset per recomposition would
     * destroy and re-upload the entire cloud several times a second. Identity,
     * not equality — two `NativePointCloudSource`s over the same engine handle
     * are the same session, but nothing in that type promises `equals`.
     */
    fun setSource(newSource: PointCloudSource?) {
        if (newSource === source) return
        source = newSource

        // The GPU-side bookkeeping belongs to the source that produced it: page
        // ids, per-page streams, the mapped-page latch and the combined bounds
        // are all keyed to ONE page store.
        gpuPages.values.forEach { destroyGpuPage(it) }
        gpuPages.clear()
        pageStreams.clear()
        mappedPageSeen = false
        haveBounds = false
        // ROUND 22 item 91 (iv): with no pages left, every retired shared index
        // buffer is now unreferenced and can go.
        drainRetiredIndexBuffers()
        // ROUND 11 (item 42): the density grid is one session's coverage of
        // one room. Carrying it into the next capture would open the new
        // scan claiming the operator had already covered a room they have
        // not walked into — the same class of bug ROUND 10 item 38 spent a
        // round on with the page store.
        resetCoverage()
        // ROUND 8: the follow camera's whole state — trail, heading, framing
        // distance — belongs to ONE session's local metric frame. Carrying it
        // into the next capture would ease the camera from the old origin
        // toward the new one across whatever void lies between two unrelated
        // frames. The pose latch goes with it, or the first frame of the new
        // session would consume the last pose of the old one.
        followCamera.reset()
        synchronized(rigPoseLock) { rigPoseValid = false }
        haveRealRigPose = false
        recentGeometryValid = false
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
        // ROUND 5: floor 0.1 px, not 0.5 — the owner's live point-size range is
        // 0.1 – 3.0 px, and a 0.5 floor here would have silently swallowed the
        // bottom of that slider. See DisplayLimits.
        pointSizePx = px.coerceIn(com.lidarscan.core.render.DisplayLimits.POINT_SIZE_MIN_PX, 32f)
        applyDynamicMaterialParams()
    }

    fun setCameraMode(mode: CameraMode) {
        cameraMode = mode
    }

    // --- ROUND 8: the third-person follow camera ------------------------------

    /**
     * The follow solver. Pure `:core` — see
     * [com.lidarscan.core.render.FollowCamera] for the whole derivation; this
     * class only feeds it a pose per frame and hands the answer to
     * `Camera.lookAt`.
     *
     * **[UpAxis.Y_UP] is stated here rather than defaulted**, because the choice
     * is a real one and the codebase has two conventions. The runtime frame is
     * ARCore's: `CaptureArController.publishPose` pushes `camera.pose` verbatim
     * into `scan_engine_push_pose`, A8's pushbroom resolves every return through
     * it, and `PointVertex` therefore carries "the session's local metric frame"
     * = ARCore's world frame, which is **Y-up**. The two other places in this app
     * that already commit agree: `TrajectoryTrail`'s header ("ARCore's world
     * frame is Y-up, so the trail is (x, z)") and this renderer's own ORBIT and
     * pre-ROUND-8 FOLLOW branches, both of which pass `0.0, 1.0, 0.0` as the up
     * vector. The engine's **+z-up** convention appears only in
     * `engine/tests/test_pushbroom.cpp`'s synthetic wall — test-fixture geometry,
     * not a frame any device ever produces — so it is not what the renderer sees.
     *
     * Known inconsistency, deliberately NOT changed here: [applyDynamicMaterialParams]'s
     * HEIGHT auto-range reads `combinedBounds*[2]`, i.e. **z**, as the height
     * axis. Under the Y-up convention established above that is the wrong
     * component, and A14's "the caller refreshes those two fields from the real
     * data range" would then be auto-ranging colour over a horizontal axis. It is
     * flagged rather than fixed because it is a visible colour-mapping behaviour
     * change on the Review screen, which is another workstream's surface this
     * round; it is reported upward instead.
     */
    private val followCamera = FollowCamera(FollowCameraConfig(upAxis = UpAxis.Y_UP))

    /**
     * The minimal seam ROUND 8 adds: the rig's current world position, latched
     * from whatever already has poses.
     *
     * Nothing in the render layer had a rig pose before this. `setArCamera` is
     * the only pose-shaped input and it is only called by [ArOverlayView], which
     * is archived (`AR_OVERLAY_ARCHIVED`), so the follow camera could not have
     * been built on it. The pose *does* exist one layer over, on every ARCore
     * frame, in `CaptureArController.publishPose` — and the Capture screen
     * already holds this renderer, from `PointCloudView`'s existing
     * `onRendererReady` callback (`CaptureScreen`'s `pointCloudRenderer`). So the
     * wiring is one line in a `CaptureArController.addFrameListener` block:
     *
     * ```kotlin
     *   val pose = frame.camera.pose        // getPose(), NOT displayOrientedPose
     *   renderer.setRigPose(pose.tx(), pose.ty(), pose.tz(), frame.timestamp)
     * ```
     *
     * `getPose()` and not `getDisplayOrientedPose()`, for exactly the reason
     * `TrajectoryTrailRecorder.onFrame` gives: the display-oriented pose is
     * rotated for rendering, so a camera driven by it would swing when the phone
     * is turned in the hand. Only the **position** is taken at all — the walk
     * heading is derived from the trajectory inside `FollowCamera`, never from
     * phone yaw, because the phone yaws ±3° per step (ROUND 7's gait model) and
     * the operator glances around, and neither of those should move the camera.
     *
     * No `PointCloudView` parameter for this: a pose arrives 30 times a second
     * and a Compose parameter would mean 30 recompositions a second of the whole
     * viewport. A direct setter on the renderer, called off the AR/GL thread, is
     * the same pattern [setArCamera] already uses — hence the same latch-under-a-
     * lock, consume-on-the-Choreographer-frame discipline.
     *
     * Until that line lands, [FOLLOW] still works: [updateCamera] falls back to a
     * pseudo-pose derived from the newest points (see [recentGeometryValid]),
     * which for a D6 ring is a biased but bounded estimate of where the operator
     * is. That fallback is a safety net, not the design.
     */
    fun setRigPose(x: Float, y: Float, z: Float, tMonoNs: Long) {
        if (!x.isFinite() || !y.isFinite() || !z.isFinite()) return
        synchronized(rigPoseLock) {
            rigPoseX = x.toDouble()
            rigPoseY = y.toDouble()
            rigPoseZ = z.toDouble()
            rigPoseTNs = tMonoNs
            rigPoseValid = true
            rigPoseSeq++
        }
    }

    private val rigPoseLock = Any()
    private var rigPoseX = 0.0
    private var rigPoseY = 0.0
    private var rigPoseZ = 0.0
    private var rigPoseTNs = 0L
    private var rigPoseValid = false

    /**
     * Bumped on every [setRigPose]. The Choreographer frame only feeds
     * [followCamera] when this has moved, so a render running faster than the
     * 30 Hz pose stream does not push the same pose in repeatedly — which would
     * be harmless for the filters (`FollowCamera` drops non-monotonic
     * timestamps) but would waste the work every frame.
     */
    private var rigPoseSeq = 0L
    private var consumedRigPoseSeq = 0L

    /** True once a real pose has ever arrived — after which the points-derived fallback is never used again. */
    private var haveRealRigPose = false

    /**
     * The last [requestToken] actually applied — see [setMaxRefreshHz]'s audit
     * bugfix note for why this exists alongside [maxRefreshHz] itself.
     */
    private var appliedRefreshRequestToken: Int = 0

    /**
     * ROUND 5: caps how often the live view repaints (see [maxRefreshHz]).
     * `0` means uncapped — the Choreographer's own rate, i.e. the panel's.
     *
     * ROUND 5.3: the cap is no longer clamped at 59 — a 120 Hz phone can ask for
     * 90, and the *hardware* ceiling is what bounds the choice (the control's
     * options come from `RefreshGovernor.optionsFor`). Setting it also resets the
     * governor, because an explicit choice outranks an automatic downshift.
     *
     * ROUND 5 AUDIT bugfix: [requestToken] (`CaptureViewModel
     * .refreshRequestToken`) is the "the operator actually asked" signal.
     * `PointCloudView` calls this every recomposition with whatever `hz` the
     * ViewModel currently holds — cheap and correct for the common case, but
     * it meant `hz == maxRefreshHz` (the old, only, guard) was ALSO true every
     * time the operator re-picked the option already showing selected, which
     * is exactly how one recovers from an auto-downshift by hand. Gating on
     * `requestToken` too — which changes on every explicit
     * `CaptureViewModel.setRefreshHz` call regardless of whether the numeric
     * value moved — lets that re-pick through to `governor.request()` while
     * still swallowing the once-per-recomposition repeats that would
     * otherwise fight the governor's own auto-downshift every frame.
     */
    fun setMaxRefreshHz(hz: Int, requestToken: Int = 0) {
        val clamped = if (hz > 0) hz else 0
        if (clamped == maxRefreshHz && requestToken == appliedRefreshRequestToken) return
        maxRefreshHz = clamped
        appliedRefreshRequestToken = requestToken
        governor?.request(maxRefreshHz)
    }

    /**
     * ROUND 5.3 (item 17): the device's real display ceiling, from
     * `Display.getRefreshRate()`. Sets up the governor that eases the live view
     * down when this phone cannot sustain what was asked for.
     */
    fun setDeviceRefreshCeilingHz(ceilingHz: Int) {
        val current = governor
        if (current != null && governorCeilingHz == ceilingHz) return
        governorCeilingHz = ceilingHz
        governor = com.lidarscan.core.render.RefreshGovernor(deviceCeilingHz = ceilingHz).also {
            it.request(maxRefreshHz)
        }
    }

    private var governorCeilingHz: Int = 0

    /** The cap actually in force, after any auto-downshift — for the inline note. */
    fun effectiveRefreshHz(): Int = governor?.effectiveHz ?: maxRefreshHz

    /**
     * B10: bind a whole [DisplayParams] (Tech Spec §3.9's render-settings
     * panel, A14's model) rather than the three individual setters above.
     *
     * Everything `points.mat` declares is bound here by NAME — A14 §4 predicted
     * B10 would treat `DisplayParamsUniforms` as a raw std140 UBO, but B4's
     * renderer is Filament, whose `MaterialInstance` takes named parameters and
     * not a byte blob, so this app is in C1's position rather than the one A14
     * expected. The struct is still the single source of truth for the field
     * names, types and values; only the memory layout is irrelevant here.
     *
     * Three fields do **not** reach the shader, exactly as A14 says they
     * should not: `lodPointBudget` is a CPU-side page-admission decision (see
     * [lodPointBudget]), and the two overlay toggles are drawn by the app.
     * `background` is the Filament `Renderer`'s clear colour, not a material
     * parameter — a shader cannot paint what is behind the points.
     */
    fun setDisplayParams(p: com.lidarscan.core.render.DisplayParams) {
        displayParams = p
        colorMode = p.colorMode
        colormap = p.activeScalar.colormap
        lodPointBudget = p.lodPointBudget.toLong()
        if (!translucent && ::renderer.isInitialized) {
            // Only in the opaque path: the AR overlay's clear colour MUST stay
            // fully transparent black, and honouring a background choice there
            // would paint over the camera feed every frame.
            renderer.clearOptions = Renderer.ClearOptions().apply {
                clear = true
                clearColor = doubleArrayOf(
                    p.background.r / 255.0,
                    p.background.g / 255.0,
                    p.background.b / 255.0,
                    1.0,
                )
            }
        }
        applyDynamicMaterialParams()
    }

    /**
     * The current world→clip matrix as a **ROW-major** 16-double array, and the
     * viewport it applies to — what B11's measure tool projects candidate
     * points with.
     *
     * Filament hands both matrices out COLUMN-major (its own convention, the
     * opposite of the engine's C ABI), so the product is transposed once here
     * rather than at every call site: `pickNearestPoint()` documents row-major
     * and the conversion belongs at the boundary that knows which is which.
     * Null before the first frame has sized the viewport.
     */
    fun viewProjectionRowMajor(): DoubleArray? {
        if (!::camera.isInitialized || viewportWidth <= 1) return null
        val proj = DoubleArray(16)
        val viewM = DoubleArray(16)
        camera.getProjectionMatrix(proj)
        camera.getViewMatrix(viewM)
        // Column-major multiply: C = P * V, C[col*4+row].
        val cm = DoubleArray(16)
        for (col in 0 until 4) {
            for (row in 0 until 4) {
                var s = 0.0
                for (k in 0 until 4) s += proj[k * 4 + row] * viewM[col * 4 + k]
                cm[col * 4 + row] = s
            }
        }
        val rm = DoubleArray(16)
        for (r in 0 until 4) for (c in 0 until 4) rm[r * 4 + c] = cm[c * 4 + r]
        return rm
    }

    fun viewportWidthPx(): Int = viewportWidth
    fun viewportHeightPx(): Int = viewportHeight

    /**
     * B3: which `StreamId`s to draw. See [StreamFilter] for the whole story;
     * the short version is that B4 drew **every** page regardless of stream,
     * which is correct for a D6 record-only session (one point stream) and
     * wrong for a Mid-360 with live SLAM, where `kLidarMid360` (sensor-frame
     * preview) and `kSlamMap` (registered world-frame map) both have pages
     * and drawing both puts two copies of the world on screen at once.
     *
     * Changing the filter drops the GPU pages that no longer belong, so a
     * toggle takes effect on the next frame rather than leaving the old
     * stream's pages resident forever.
     */
    fun setStreamFilter(filter: StreamFilter) {
        if (streamFilter == filter) return
        streamFilter = filter
        dropPagesNotMatchingFilter()
    }

    /** Removes every resident GPU page whose stream the current filter rejects. */
    private fun dropPagesNotMatchingFilter() {
        val stale = gpuPages.filterKeys { pageId ->
            !streamFilter.accepts(pageStreams[pageId] ?: -1, mappedPageSeen)
        }
        if (stale.isEmpty()) return
        stale.forEach { (pageId, page) ->
            scene.removeEntity(page.entity)
            destroyGpuPage(page)
            gpuPages.remove(pageId)
            pageStreams.remove(pageId)
        }
        // ROUND 22 item 91 (iv): pages just went away, so a retired shared
        // index buffer may now be unreferenced.
        drainRetiredIndexBuffers()
        haveBounds = false
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

    // ── ROUND 25 item 117: the viewer's gestures ────────────────────────────
    //
    // Owner: *"Add pan and zoom in out function for lidar scan review."* What
    // was here drove filament-utils' `Manipulator`: one finger orbited, a pinch
    // dollied, and **pan did not exist** — `grabBegin`'s `strafe` flag was never
    // passed `true`, so a two-finger drag fed the FIRST pointer's coordinates
    // into the orbit path and spun the cloud instead of sliding it. On a scan of
    // a corridor, whose geometry sits thirty metres from the origin the
    // Manipulator's target is nailed to (it has no runtime retarget — see
    // [updateCamera]), that is the difference between a viewer and a turntable.
    //
    // The camera is now [com.lidarscan.core.render.OrbitCamera]: pure `:core`
    // arithmetic, unit-tested on a bare JVM, opening on exactly the framing the
    // `Manipulator` did. This class keeps the touch plumbing and the Filament
    // call, which is all it should ever have had.
    //
    // Arbitration is by pointer COUNT and it is deliberate: two fingers pan and
    // pinch AT THE SAME TIME (the `ScaleGestureDetector` reads the spread while
    // this reads the centroid), which is what makes a two-finger gesture feel
    // like one gesture rather than a mode.

    /** Where the last touch was, so a move can be a delta. Touch thread only. */
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var lastPointerCount = 0

    /** Forwarded from the Compose `AndroidView`'s touch listener. */
    fun onTouch(event: MotionEvent): Boolean {
        if (cameraMode != CameraMode.ORBIT) return false
        val count = event.pointerCount
        // The centroid, not pointer 0: with two fingers down, tracking one of
        // them makes the pan lurch whenever the OTHER one is the one that moved.
        var cx = 0f
        var cy = 0f
        for (i in 0 until count) { cx += event.getX(i); cy += event.getY(i) }
        cx /= count.coerceAtLeast(1)
        cy /= count.coerceAtLeast(1)

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN,
            MotionEvent.ACTION_POINTER_UP,
            -> {
                // A finger arriving or leaving moves the centroid by a large
                // jump that is not a drag. Re-anchor instead of integrating it,
                // or lifting one of two fingers throws the camera across the
                // room.
                lastTouchX = cx
                lastTouchY = cy
                lastPointerCount = count
            }

            MotionEvent.ACTION_MOVE -> {
                val dx = cx - lastTouchX
                val dy = cy - lastTouchY
                lastTouchX = cx
                lastTouchY = cy
                if (count != lastPointerCount) {
                    // The pointer count changed between events without an
                    // up/down we saw. Treat this frame as the anchor.
                    lastPointerCount = count
                    return true
                }
                orbitCamera = if (count >= 2) {
                    orbitCamera.pan(dx, dy, viewportHeight, ORBIT_FOV_Y_RAD)
                } else {
                    orbitCamera.orbit(dx, dy, viewportWidth, viewportHeight)
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> lastPointerCount = 0
        }
        return true
    }

    /**
     * Pinch, forwarded from the `ScaleGestureDetector` in `PointCloudView`.
     *
     * [scaleFactor] is the detector's own: >1 when the fingers move apart.
     * Passed through unchanged rather than converted to a delta — the clamping
     * and the sign live in `OrbitCamera.dolly`, where a test can see them.
     */
    fun onScale(scaleFactor: Float) {
        if (cameraMode != CameraMode.ORBIT) return
        orbitCamera = orbitCamera.dolly(scaleFactor)
    }

    /**
     * Double tap: **reset the framing** to the whole cloud.
     *
     * Frames the combined page bounds when there are any, so a scan whose
     * geometry sits forty metres from the session origin is reset to a view OF
     * IT — resetting to the origin would lose the cloud, and this gesture is
     * what an operator reaches for when they have already lost it. With no
     * bounds yet, the home framing, which is the same thing for a cloud at the
     * origin.
     */
    fun resetCameraFraming() {
        if (cameraMode != CameraMode.ORBIT) return
        orbitCamera = if (haveBounds) {
            com.lidarscan.core.render.OrbitCamera.framing(
                combinedBoundsMin[0], combinedBoundsMin[1], combinedBoundsMin[2],
                combinedBoundsMax[0], combinedBoundsMax[1], combinedBoundsMax[2],
                ORBIT_FOV_Y_RAD,
            )
        } else {
            com.lidarscan.core.render.OrbitCamera.HOME
        }
    }

    /** For tests and diagnostics: where the orbit camera currently is. */
    fun orbitCameraState(): com.lidarscan.core.render.OrbitCamera = orbitCamera

    // --- setup helpers -----------------------------------------------------

    private fun loadMaterial() {
        val bytes = context.assets.open("materials/points.filamat").use { it.readBytes() }
        val buffer = ByteBuffer.allocateDirect(bytes.size).order(ByteOrder.nativeOrder())
        buffer.put(bytes).flip()
        material = Material.Builder().payload(buffer, buffer.remaining()).build(engine)
        materialInstance = material!!.createInstance()

        // ROUND 16 item 59. Loaded here rather than lazily on the first trail:
        // a `Material.Builder().build()` on the GL thread mid-walk is a stall
        // the operator would feel, and this file's whole posture is that setup
        // costs are paid at attach.
        val trailBytes = context.assets.open("materials/trail.filamat").use { it.readBytes() }
        val trailBuffer = ByteBuffer.allocateDirect(trailBytes.size).order(ByteOrder.nativeOrder())
        trailBuffer.put(trailBytes).flip()
        trailMaterial = Material.Builder()
            .payload(trailBuffer, trailBuffer.remaining())
            .build(engine)
        trailMaterialInstance = trailMaterial!!.createInstance().apply {
            setParameter("opacity", 1.0f)
            setParameter("intensity", 1.0f)
        }
    }

    private fun buildColormapTexture() {
        val tex = Texture.Builder()
            .width(ColormapLut.SIZE)
            // ROUND 26 (owner item 127): ColormapLut.ROWS, not a literal 3.
            // buildTextureRgba8() below emits one row per Colormap value and
            // item 127 added a fourth (TURBO); a hard-coded height here would
            // have allocated three rows for four rows of bytes, and the extra
            // row would simply never reach the GPU — no exception, just a
            // colormap that renders as whatever CLAMP_TO_EDGE gives back.
            .height(ColormapLut.ROWS)
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

    // ── ROUND 11 (owner item 42): coverage colouring ────────────────────────
    //
    // Density is counted here, on the upload path, and turned into a per-point
    // tint that is written into the GPU's copy of the vertices. Three
    // consequences worth stating, because they are the item's own constraints:
    //
    //  * **Never written into the container.** The engine's PageStore — which
    //    is also the map cache that gets sealed into the `.lscan` — is read and
    //    never touched. The tint exists in a Filament VertexBuffer and dies
    //    with the surface.
    //  * **Deterministic.** `CoverageGrid` is a fixed lattice anchored at the
    //    world origin, so a coordinate always lands in the same cell whatever
    //    order the points arrived in.
    //  * **Honest at the point budget.** The counts come from the points this
    //    renderer actually holds, which is the same set it draws. When the LOD
    //    budget stops admitting new pages, the tint and the drawing decimate
    //    together — so a cell that reads thin IS thin in the map on screen,
    //    which is the question the operator is asking. It is not a claim about
    //    points the renderer never saw.
    private val coverage = com.lidarscan.core.render.CoverageGrid()

    // ROUND 19 item 75 — the same census, as a DIRECTION. Fed beside the grid
    // on the upload path; its origin is the walked trail's newest point (set
    // in [setTrail]), so a sector counts what the D6 painted in that direction
    // from wherever the operator was standing at the time. See
    // core/render/CoverageCompass.kt for what it claims and refuses to claim.
    private val compass = com.lidarscan.core.render.CoverageCompass()

    /** Round-robin cursor for re-tinting already-uploaded pages. */
    private var coverageRefreshCursor = 0
    private var lastCoverageRefreshMs = 0L

    /** Scratch for one tinted slice. Grown, never shrunk; reused every frame. */
    private var tintScratch: java.nio.ByteBuffer? = null

    /**
     * What the shader is told. [com.lidarscan.core.render.ColorMode.COVERAGE]
     * has no shader branch by design — see the KDoc on the enum — so it asks
     * for RGB pass-through and supplies the colour itself.
     */
    private fun shaderColorMode(): Int =
        if (colorMode == com.lidarscan.core.render.ColorMode.COVERAGE) {
            com.lidarscan.core.render.ColorMode.RGB.ordinal
        } else {
            colorMode.ordinal
        }

    /** The share of occupied 25 cm cells still below the "thin" threshold. */
    fun coverageThinFraction(): Float = coverage.thinFraction()

    fun resetCoverage() {
        coverage.clear()
        compass.clear()
        coverageRefreshCursor = 0
        lastCoverageRefreshMs = 0L
    }

    /** ROUND 19 item 75: per-sector coverage for the tile's guidance ring. */
    fun coverageSectors(): FloatArray = compass.sectorCoverage()

    /** ROUND 19 item 75: null until measurable, and null when nothing is thin. */
    fun coverageAdviceLine(): String? = compass.adviceLine()

    /**
     * Count a freshly-uploaded slice into the density grid, then hand back the
     * bytes to upload — tinted when coverage mode is on, and the caller's own
     * slice untouched when it is not (so a capture that never opens coverage
     * mode pays one branch per slice and nothing else).
     *
     * `src` is positioned/limited on the slice, exactly as the upload expects,
     * and this leaves it that way: every read is absolute.
     */
    private fun countAndMaybeTint(src: java.nio.ByteBuffer): java.nio.ByteBuffer {
        val base = src.position()
        val bytes = src.limit() - base
        val points = bytes / POINT_STRIDE_BYTES
        if (points <= 0) return src.slice()
        val le = src.duplicate().order(java.nio.ByteOrder.LITTLE_ENDIAN)
        for (i in 0 until points) {
            val o = base + i * POINT_STRIDE_BYTES
            val x = le.getFloat(o)
            val y = le.getFloat(o + 4)
            val z = le.getFloat(o + 8)
            coverage.add(x, y, z)
            compass.add(x, z)
        }
        if (colorMode != com.lidarscan.core.render.ColorMode.COVERAGE) return src.slice()
        return tintInto(le, base, points)
    }

    /**
     * Copy `points` vertices out of `le` at `base` and rewrite their RGBA from
     * the density grid. The position stays byte-identical; only the colour
     * changes, which is why this can be handed to the same `setBufferAt` the
     * untinted path uses.
     */
    private fun tintInto(le: java.nio.ByteBuffer, base: Int, points: Int): java.nio.ByteBuffer {
        val needed = points * POINT_STRIDE_BYTES
        var scratch = tintScratch
        if (scratch == null || scratch.capacity() < needed) {
            scratch = java.nio.ByteBuffer.allocateDirect(needed)
                .order(java.nio.ByteOrder.LITTLE_ENDIAN)
            tintScratch = scratch
        }
        scratch.clear()
        scratch.limit(needed)
        for (i in 0 until points) {
            val o = base + i * POINT_STRIDE_BYTES
            val x = le.getFloat(o)
            val y = le.getFloat(o + 4)
            val z = le.getFloat(o + 8)
            val d = o + 12
            // The point's own shade, exactly as the grayscale/intensity default
            // draws it (r == g == b for a D6 return; the green channel carries
            // the high-reflectivity flag, so red is the honest one to read).
            val shade = le.get(d).toInt() and 0xFF
            val argb = coverage.tintAt(shade, x, y, z)
            val w = i * POINT_STRIDE_BYTES
            scratch.putFloat(w, x)
            scratch.putFloat(w + 4, y)
            scratch.putFloat(w + 8, z)
            scratch.put(w + 12, ((argb shr 16) and 0xFF).toByte())
            scratch.put(w + 13, ((argb shr 8) and 0xFF).toByte())
            scratch.put(w + 14, (argb and 0xFF).toByte())
            // Alpha is carried through: the pushbroom writes a reduced alpha on
            // points taken during tracking loss (`flagged_alpha`), and coverage
            // must not hide that.
            scratch.put(w + 15, le.get(d + 3))
        }
        scratch.position(0)
        return scratch
    }

    /**
     * Re-tint pages that were uploaded before the grid knew as much as it does
     * now. One page per pass, round-robin, at most every
     * [COVERAGE_REFRESH_MS] — the tint has to converge as the operator revisits
     * a thin area, and a full re-upload of every page on every frame would be
     * exactly the unbounded work the round-5.3 upload budget exists to prevent.
     *
     * Deliberately does NOT touch `gpu.uploaded` or the renderable's geometry
     * count: shrinking the count for a frame to force a re-upload would make the
     * cloud blink.
     */
    private fun refreshCoverageTints(src: PointCloudSource, nowMs: Long) {
        if (colorMode != com.lidarscan.core.render.ColorMode.COVERAGE) return
        if (gpuPages.isEmpty()) return
        if (nowMs - lastCoverageRefreshMs < COVERAGE_REFRESH_MS) return
        lastCoverageRefreshMs = nowMs
        val ids = gpuPages.keys.toList()
        if (coverageRefreshCursor >= ids.size) coverageRefreshCursor = 0
        val pageId = ids[coverageRefreshCursor]
        coverageRefreshCursor++
        val gpu = gpuPages[pageId] ?: return
        if (gpu.uploaded <= 0) return
        val page = src.getPage(pageId) ?: return
        val buf = page.buffer
        val le = buf.duplicate().order(java.nio.ByteOrder.LITTLE_ENDIAN)
        val tinted = tintInto(le, 0, gpu.uploaded)
        gpu.vertexBuffer.setBufferAt(engine, 0, tinted, 0, gpu.uploaded * POINT_STRIDE_BYTES)
    }

    private fun applyDynamicMaterialParams() {
        val mi = materialInstance ?: return
        mi.setParameter("colorMode", shaderColorMode())
        mi.setParameter("colormap", colormap.ordinal)

        val dp = displayParams
        if (dp == null) {
            // B4's original path, kept for the Capture screen, which drives the
            // three simple setters and has no per-project DisplayParams yet at
            // the moment the surface is created. A14 default ranges
            // (profile_defaults(), A14-display.md §5): height 0..3 m (a typical
            // indoor ceiling), intensity already normalised to 0..1.
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
            mi.setParameter("gamma", 1.0f)
            mi.setParameter("invert", 0)
            mi.setParameter("brightness", 1.0f)
            mi.setParameter("pointSizeMode", PointSizeMode.FIXED_PIXELS.ordinal)
            mi.setParameter("pointSizeMinPx", pointSizePx)
            mi.setParameter("pointSizeMaxPx", pointSizePx)
            mi.setParameter("adaptiveReferenceM", 5.0f)
            mi.setParameter("worldSizeM", 0.01f)
            mi.setParameter("clipEnabledMask", 0)
            return
        }

        val s = dp.activeScalar
        // A14 §2's `auto_range` rule, implemented on the side the header says
        // owns it: "the CALLER (the renderer) is expected to refresh those two
        // fields ... from the real data range". For height, the source is the
        // combined page bounds — the same numbers `PageView::bounds_min/max`
        // expose and this renderer already tracks for the follow camera.
        val autoHeight = s.autoRange && dp.colorMode == ColorMode.HEIGHT && haveBounds
        mi.setParameter("valueMin", if (autoHeight) combinedBoundsMin[2] else s.manualMin)
        mi.setParameter("valueMax", if (autoHeight) combinedBoundsMax[2] else s.manualMax)
        mi.setParameter("gamma", s.gamma)
        mi.setParameter("invert", if (s.invert) 1 else 0)
        mi.setParameter("brightness", s.brightness)

        val ps = dp.pointSize
        mi.setParameter("pointSizeMode", ps.mode.ordinal)
        when (ps.mode) {
            PointSizeMode.FIXED_PIXELS -> {
                mi.setParameter("pointSizeMinPx", ps.fixedPx)
                mi.setParameter("pointSizeMaxPx", ps.fixedPx)
            }
            else -> {
                mi.setParameter("pointSizeMinPx", ps.adaptiveMinPx)
                mi.setParameter("pointSizeMaxPx", ps.adaptiveMaxPx)
            }
        }
        mi.setParameter("adaptiveReferenceM", ps.adaptiveReferenceM)
        mi.setParameter("worldSizeM", ps.worldSizeM)

        // bit0 = box clip, bit1 = height clip — A14's own packing, so the
        // shader branch here and `to_uniforms()`'s `clip_enabled_mask` agree.
        var mask = 0
        if (dp.clipBoxEnabled) mask = mask or 1
        if (dp.clipHeightEnabled) mask = mask or 2
        mi.setParameter("clipEnabledMask", mask)
        mi.setParameter("clipHeightMin", dp.clipHeightMin)
        mi.setParameter("clipHeightMax", dp.clipHeightMax)
        mi.setParameter(
            "clipBoxMin",
            dp.clipBoxMin.getOrElse(0) { -10f },
            dp.clipBoxMin.getOrElse(1) { -10f },
            dp.clipBoxMin.getOrElse(2) { -10f },
        )
        mi.setParameter(
            "clipBoxMax",
            dp.clipBoxMax.getOrElse(0) { 10f },
            dp.clipBoxMax.getOrElse(1) { 10f },
            dp.clipBoxMax.getOrElse(2) { 10f },
        )
    }

    /**
     * The identity index buffer every page draws through (Filament requires an
     * index buffer even for `PrimitiveType.POINTS`), grown on demand.
     *
     * **ROUND 22 item 91 (iv): the old buffer is RETIRED, never destroyed here.**
     * Live [GpuPage]s hold the exact `IndexBuffer` object they were built with
     * and pass it to `setGeometryAt` every frame they upload, so destroying it
     * the moment a bigger one is wanted was a use-after-free — see
     * [retiredIndexBuffers]. Retired buffers are freed by
     * [drainRetiredIndexBuffers] on the first frame no page references them,
     * and unconditionally by [detach].
     *
     * The requested size is rounded up to a power of two (floor 65 536 indices
     * = 256 KB) so that growth happens a handful of times rather than once per
     * page: the ladder from 64 k to the engine's 1 << 20 page is five
     * generations totalling ~8 MB of index data if all five are somehow alive
     * at once, against re-growing for all ~700 distinct page sizes a right-sized
     * allocation would otherwise produce.
     */
    private fun ensureSharedIndexBuffer(minCapacity: Int): IndexBuffer {
        sharedIndexBuffer?.let { if (sharedIndexBufferCapacity >= minCapacity) return it }
        val capacity = indexBufferSizeFor(minCapacity)

        val ib = IndexBuffer.Builder()
            .indexCount(capacity)
            .bufferType(IndexBuffer.Builder.IndexType.UINT)
            .build(engine)
        val idx = ByteBuffer.allocateDirect(capacity * 4).order(ByteOrder.nativeOrder())
        val ints = idx.asIntBuffer()
        for (i in 0 until capacity) ints.put(i)
        idx.rewind()
        ib.setBuffer(engine, idx)

        // Retire, do not destroy: pages built against the previous buffer are
        // still drawing through it this very frame.
        sharedIndexBuffer?.let { retiredIndexBuffers.add(it) }
        sharedIndexBuffer = ib
        sharedIndexBufferCapacity = capacity
        drainRetiredIndexBuffers()
        return ib
    }

    /** Rounds an index count up to a power of two, floored at 65 536 — see [ensureSharedIndexBuffer]. */
    private fun indexBufferSizeFor(minCapacity: Int): Int {
        var size = MIN_SHARED_INDEX_COUNT
        while (size < minCapacity) {
            if (size >= Int.MAX_VALUE / 2) return Int.MAX_VALUE
            size *= 2
        }
        return size
    }

    /**
     * ROUND 22 item 91 (iv): destroy every retired shared `IndexBuffer` that no
     * live [GpuPage] still references.
     *
     * A scan rather than a refcount because both collections are tiny — at most
     * a handful of retired generations against a few hundred pages, once per
     * frame — and because a scan cannot go wrong the way a hand-maintained count
     * can when a page is destroyed on one of the four paths that destroy pages
     * (reap, stream-filter drop, source swap, detach).
     */
    private fun drainRetiredIndexBuffers() {
        if (retiredIndexBuffers.isEmpty()) return
        val iterator = retiredIndexBuffers.iterator()
        while (iterator.hasNext()) {
            val ib = iterator.next()
            val referenced = gpuPages.values.any { it.indexBuffer === ib }
            if (!referenced) {
                engine.destroyIndexBuffer(ib)
                iterator.remove()
            }
        }
    }

    // --- ROUND 16 item 59: the trajectory ribbon --------------------------

    private fun destroyTrail() {
        if (trailEntity != 0) {
            scene.removeEntity(trailEntity)
            engine.renderableManager.destroy(trailEntity)
            EntityManager.get().destroy(trailEntity)
            trailEntity = 0
        }
        trailVertexBuffer?.let { engine.destroyVertexBuffer(it) }
        trailVertexBuffer = null
        trailIndexBuffer?.let { engine.destroyIndexBuffer(it) }
        trailIndexBuffer = null
        trailCapacity = 0
        trailCount = 0
    }

    /**
     * Uploads the pending trail, if there is one, and keeps the renderable in
     * step with the toggle. Called once per frame from [FrameCallback], BEFORE
     * `beginFrame`, exactly like [syncPointCloud].
     *
     * The buffers grow by doubling and are never shrunk while the view is
     * alive: a walk only gets longer, the ring is capped at the preset's
     * `trailPoints` (600 by default, i.e. 90 m at the 15 cm spacing), and a
     * destroy/rebuild per growth step would be a stutter every few seconds.
     *
     * `LINE_STRIP`, not `LINES`: the trail is a polyline and a strip is n
     * indices where a line list is 2n-2 of them, but more to the point a strip
     * cannot be given an odd vertex count and silently drop the last segment.
     */
    private fun syncTrail() {
        val bytes: ByteBuffer?
        val count: Int
        synchronized(trailLock) {
            if (!trailDirty) return
            trailDirty = false
            bytes = pendingTrailBytes
            count = pendingTrailCount
        }
        val mi = trailMaterialInstance ?: return

        // Two points is the shortest thing that is a line. One point is a walk
        // that has not started, and drawing a degenerate strip is how a driver
        // gets handed a zero-length primitive.
        if (bytes == null || count < 2 || !trailVisible) {
            if (trailEntity != 0) scene.removeEntity(trailEntity)
            trailCount = 0
            return
        }

        if (trailCapacity < count) {
            destroyTrail()
            var cap = 64
            while (cap < count) cap *= 2
            trailVertexBuffer = VertexBuffer.Builder()
                .bufferCount(1)
                .vertexCount(cap)
                .attribute(
                    VertexBuffer.VertexAttribute.POSITION, 0,
                    VertexBuffer.AttributeType.FLOAT3, 0, TRAIL_STRIDE_BYTES,
                )
                .attribute(
                    VertexBuffer.VertexAttribute.COLOR, 0,
                    VertexBuffer.AttributeType.UBYTE4, 12, TRAIL_STRIDE_BYTES,
                )
                .normalized(VertexBuffer.VertexAttribute.COLOR, true)
                .build(engine)
            val ib = IndexBuffer.Builder()
                .indexCount(cap)
                .bufferType(IndexBuffer.Builder.IndexType.UINT)
                .build(engine)
            val idx = ByteBuffer.allocateDirect(cap * 4).order(ByteOrder.nativeOrder())
            val ints = idx.asIntBuffer()
            for (i in 0 until cap) ints.put(i)
            idx.rewind()
            ib.setBuffer(engine, idx)
            trailIndexBuffer = ib
            trailCapacity = cap
            trailEntity = EntityManager.get().create()
        }

        val vb = trailVertexBuffer ?: return
        val ib = trailIndexBuffer ?: return
        vb.setBufferAt(engine, 0, bytes, 0, count * TRAIL_STRIDE_BYTES)
        trailCount = count

        // Bounds from the payload rather than from a cached box: the path is
        // small (a few hundred vertices) so this is free, and a stale bounding
        // box on a growing strip is how the ribbon disappears at exactly the
        // moment the operator walks somewhere new.
        var minX = Float.MAX_VALUE; var minY = Float.MAX_VALUE; var minZ = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE; var maxY = -Float.MAX_VALUE; var maxZ = -Float.MAX_VALUE
        for (i in 0 until count) {
            val o = i * TRAIL_STRIDE_BYTES
            val x = bytes.getFloat(o)
            val y = bytes.getFloat(o + 4)
            val z = bytes.getFloat(o + 8)
            if (x < minX) minX = x
            if (y < minY) minY = y
            if (z < minZ) minZ = z
            if (x > maxX) maxX = x
            if (y > maxY) maxY = y
            if (z > maxZ) maxZ = z
        }

        val rm = engine.renderableManager
        val existing = rm.getInstance(trailEntity)
        if (existing == 0) {
            RenderableManager.Builder(1)
                .culling(false)
                .castShadows(false)
                .receiveShadows(false)
                .geometry(
                    0, RenderableManager.PrimitiveType.LINE_STRIP, vb, ib, 0, count,
                )
                .material(0, mi)
                .build(engine, trailEntity)
        } else {
            rm.setGeometryAt(
                existing, 0, RenderableManager.PrimitiveType.LINE_STRIP, vb, ib, 0, count,
            )
        }
        val instance = rm.getInstance(trailEntity)
        if (instance != 0) {
            rm.setAxisAlignedBoundingBox(
                instance,
                Box(
                    (minX + maxX) / 2f, (minY + maxY) / 2f, (minZ + maxZ) / 2f,
                    max((maxX - minX) / 2f, 0.05f),
                    max((maxY - minY) / 2f, 0.05f),
                    max((maxZ - minZ) / 2f, 0.05f),
                ),
            )
        }
        scene.addEntity(trailEntity)
    }

    // --- per-frame sync ------------------------------------------------------

    private fun syncPointCloud() {
        val src = source ?: return
        if (!src.isAvailable) return
        val mi = materialInstance ?: return

        val count = src.pageCount()
        var resident = 0L
        var pagesDrawn = 0
        // ROUND 22 item 91 (i): the admission budget is accounted in ALLOCATED
        // BYTES, not in points.
        //
        // `resident` (points) is kept — it is what `PointCloudRenderStats`
        // reports and what the operator sees — but it is no longer what the
        // budget is spent against, because it never was what memory was spent
        // against. The old test charged `page.count` and the allocation twenty
        // lines below built `max(page.capacity, page.count)` vertices: for
        // Review's `1 << 20` page store that is 16 777 216 bytes for a page
        // holding a thousand points, so a 20 M-point budget admitted 20 000
        // pages and asked the driver for 335 GB. See `GpuPageBudget` for the
        // whole table.
        var residentBytes = 0L
        val budgetBytes = GpuPageBudget.budgetBytesFor(lodPointBudget, deviceTier)
        // ROUND 22 item 91 (vi): every page id the source reported this frame.
        // The engine's live store evicts the oldest page when full
        // (`page_store.h`, `PageFullPolicy::kEvictOldest`), so ids disappear on
        // a long walk and their GpuPages have to go with them — see the reap
        // pass after the loop.
        val seenPageIds = HashSet<Int>(count.coerceAtLeast(0) * 2)
        // ROUND 5.3 (item 17, the crash path): per-frame upload work is BOUNDED.
        //
        // Before this, every page whose `count` had grown since the last frame was
        // re-uploaded in the same frame — so a live capture that produced a burst
        // (or a replay that decoded faster than the display drew) queued an
        // unbounded number of `setBufferAt` calls into one Filament frame. That is
        // the shape of the failure the owner asked to be guarded: the driver's
        // command buffer and the staging allocations grow until something gives.
        //
        // The budget below is deliberately in BYTES, not pages: one page can be
        // 64 k points (1 MB) or a few hundred. Work not done this frame is not
        // lost — `gpu.uploaded` still trails `page.count`, so the next frame picks
        // up exactly where this one stopped, which is "coalesce/drop render frames
        // under pressure" rather than "drop points".
        var uploadBudgetBytes = MAX_UPLOAD_BYTES_PER_FRAME
        // ROUND 22 item 91 (iii): and per-frame ALLOCATION is bounded in bytes
        // too, for exactly the same reason one line up. `MAX_NEW_PAGES_PER_FRAME
        // = 24` counted pages, which stopped meaning anything the moment pages
        // stopped being one size: 24 of Review's pages is 402 653 184 bytes of
        // VertexBuffer requested inside a single Choreographer callback.
        var newPageBytesThisFrame = 0L

        for (i in 0 until count) {
            val pageId = src.pageIdAt(i)
            if (pageId < 0) continue
            // Seen = listed by the store, recorded BEFORE any filter/budget
            // skip below: a page the stream filter rejects or the LOD budget
            // refuses is still very much alive, and reaping it would fight the
            // budget for the rest of the session. Recorded before `getPage`
            // too, so a transient read failure does not destroy GPU state.
            seenPageIds.add(pageId)
            val page = src.getPage(pageId) ?: continue
            // B3: pages are single-stream (INT24 §2, "pages are single-stream:
            // StreamId::kSlamMap gets its own pages"), so one check per page
            // is the whole filter. Skipping BEFORE the GPU-page lookup is what
            // keeps a filtered-out stream from ever being uploaded.
            if (page.stream == ScanEngineNative.StreamId.SLAM_MAP && !mappedPageSeen) {
                // The first mapped page ends MAPPED_ONLY's raw fallback. Drop
                // the raw pages that were standing in for it in the same pass,
                // so the switch is one frame and not a slow crossfade of two
                // superimposed clouds.
                mappedPageSeen = true
                if (streamFilter == StreamFilter.MAPPED_ONLY) dropPagesNotMatchingFilter()
            }
            if (!streamFilter.accepts(page.stream, mappedPageSeen)) continue

            var gpu = gpuPages[pageId]
            // ROUND 22 item 91 (ii): what this page would actually cost — its
            // COUNT rounded up to a 4 096-point granule, capped by the engine
            // page's capacity. For the 1 000-point page above that is 65 536
            // bytes, where `max(page.capacity, page.count)` was 16 777 216.
            val allocBytes = GpuPageBudget.allocationBytes(page.count, page.capacity)

            // B10 / §3.12's LOD budget: stop admitting pages once the budget is
            // reached, but never drop a page already on the GPU — a budget that
            // evicted resident pages would make the cloud flicker as page order
            // shifted between frames. Pages already uploaded keep growing.
            //
            // ROUND 22 item 91 (i): in bytes now, against the byte ceiling
            // `GpuPageBudget.budgetBytesFor` derives from the LOD slider.
            if (gpu == null && !GpuPageBudget.admits(residentBytes, allocBytes, budgetBytes)) continue

            pageStreams[pageId] = page.stream
            resident += page.count

            if (gpu == null) {
                // Creating a page is a VertexBuffer allocation plus an entity; a
                // capture that suddenly exposes 200 new pages (a replay seek, a
                // stream filter flip) must not do all of it in one frame.
                //
                // ROUND 22 item 91 (iii): bounded in BYTES, not in pages — 24
                // pages was 1.5 MB of a live 32 k-point store and 384 MiB of
                // Review's. Refused work is not lost: the page is simply built
                // on a later frame, the same contract as the upload budget.
                if (!GpuPageBudget.admitsNewAllocation(
                        newPageBytesThisFrame, allocBytes, MAX_NEW_PAGE_BYTES_PER_FRAME,
                    )
                ) {
                    continue
                }
                newPageBytesThisFrame += allocBytes
                val capacity = GpuPageBudget.allocationVertices(page.count, page.capacity)
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
                    // ROUND 22 item 91 (ii): clamped to the allocation. The
                    // renderable is re-pointed at `gpu.uploaded` a few lines
                    // below anyway; what matters is that the initial count can
                    // never name a vertex the buffer does not have.
                    .geometry(
                        0, RenderableManager.PrimitiveType.POINTS, vb, ib, 0,
                        minOf(page.count, capacity),
                    )
                    .material(0, mi)
                    .build(engine, entity)
                scene.addEntity(entity)

                gpu = GpuPage(entity, vb, ib, uploaded = 0, capacity = capacity)
                gpuPages[pageId] = gpu
            } else if (GpuPageBudget.needsGrowth(gpu.capacity, page.count)) {
                // ROUND 22 item 91 (ii), the other half of right-sizing: a LIVE
                // page climbs from its first 4 096-vertex buffer to the store's
                // capacity one revolution at a time, so a buffer sized to the
                // count it had when it was first seen WILL be outgrown.
                //
                // Growth doubles (`GpuPageBudget.growthVertices`) and is charged
                // against the same per-frame allocation budget as a brand-new
                // page — at its full new size, which is what bounds the
                // re-upload that follows it, since the prefix being re-sent is
                // always smaller than the buffer being paid for. A frame that
                // cannot afford the growth simply does not grow: the upload
                // below is clamped to `gpu.capacity`, so the page stalls at its
                // current size for a frame or two rather than overrunning its
                // buffer.
                val grown = GpuPageBudget.growthVertices(gpu.capacity, page.count, page.capacity)
                val growthBytes = GpuPageBudget.bytesForVertices(grown)
                if (GpuPageBudget.admitsNewAllocation(
                        newPageBytesThisFrame, growthBytes, MAX_NEW_PAGE_BYTES_PER_FRAME,
                    )
                ) {
                    newPageBytesThisFrame += growthBytes
                    growGpuPage(gpu, page, grown)
                }
            }

            // ROUND 22 item 91 (i): what this page ACTUALLY occupies, after any
            // creation or growth above — the number the next page's admission
            // test is measured against.
            residentBytes += GpuPageBudget.bytesForVertices(gpu.capacity)

            // ROUND 22 item 91 (ii): never upload past the allocation. Before
            // right-sizing the buffer was `max(page.capacity, page.count)` so
            // this could not overrun; now it can, and `setBufferAt` past the end
            // of a VertexBuffer is a native heap write.
            val uploadableCount = minOf(page.count, gpu.capacity)
            if (uploadableCount > gpu.uploaded && uploadBudgetBytes > 0) {
                // Clamp this page's slice to what is left of the frame's budget.
                // The remainder is uploaded next frame from the same offset.
                val wantedPoints = uploadableCount - gpu.uploaded
                val affordablePoints = (uploadBudgetBytes / POINT_STRIDE_BYTES).coerceAtLeast(1)
                val newPoints = minOf(wantedPoints, affordablePoints)
                val uploadTo = gpu.uploaded + newPoints
                uploadBudgetBytes -= newPoints * POINT_STRIDE_BYTES

                val newBytes = page.buffer
                newBytes.position(gpu.uploaded * POINT_STRIDE_BYTES)
                newBytes.limit(uploadTo * POINT_STRIDE_BYTES)
                // ROUND 8: measure the slice on its way to the GPU. Absolute
                // reads only, so position/limit are left exactly as the upload
                // below expects them.
                accumulateRecentGeometry(newBytes, gpu.uploaded, uploadTo)
                // ROUND 11 (item 42): counts the slice into the coverage grid
                // and, in coverage mode, hands back a tinted copy. Outside
                // coverage mode this is `newBytes.slice()` and one loop.
                val payload = countAndMaybeTint(newBytes)
                gpu.vertexBuffer.setBufferAt(
                    engine, 0, payload,
                    gpu.uploaded * POINT_STRIDE_BYTES, newPoints * POINT_STRIDE_BYTES,
                )
                gpu.uploaded = uploadTo

                val rm = engine.renderableManager
                val instance = rm.getInstance(gpu.entity)
                if (instance != 0) {
                    // `gpu.uploaded`, not `page.count`: with the round-5.3 upload
                    // budget the two can differ for a frame or two, and drawing
                    // points that have not been uploaded yet renders whatever
                    // uninitialised GPU memory happens to be there.
                    rm.setGeometryAt(
                        instance, 0, RenderableManager.PrimitiveType.POINTS,
                        gpu.vertexBuffer, gpu.indexBuffer, 0, gpu.uploaded,
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

        reapEvictedPages(seenPageIds)
        publishRecentGeometry()
        refreshCoverageTints(src, System.currentTimeMillis())
        lastStats = PointCloudRenderStats(resident, pagesDrawn, haveBounds, mappedPageSeen)
    }

    /**
     * ROUND 22 item 91 (vi): destroy the GPU pages whose engine page no longer
     * exists.
     *
     * The live store runs `PageFullPolicy::kEvictOldest` — *"when full, the
     * OLDEST page is retired to make room, so the view is a moving window over
     * the newest data and a live preview never dead-ends"*
     * (`engine/include/scanengine/cloud/page_store.h`, the fix for the field
     * bug of 2026-08-17). So on any walk long enough to fill the store, page ids
     * stop resolving one at a time, forever, and **nothing here noticed**: the
     * `GpuPage` kept its VertexBuffer, its entity and its place in the Filament
     * scene for the life of the surface, still drawing points the engine has
     * thrown away. A 64-page store cycling through a long capture leaks the
     * whole store's worth of VBOs per cycle — the same memory the LOD budget
     * above is trying to bound, being consumed behind its back.
     *
     * [GpuPageBudget.reap] holds the one rule that matters for safety: an
     * **empty** observation reaps nothing. A frame in which the source reported
     * no pages at all is a store between epochs, a replay seek or a handle that
     * went to 0 for one poll — not an instruction to destroy and re-upload the
     * entire cloud.
     *
     * `haveBounds` is deliberately NOT invalidated. The combined box merely
     * becomes conservative (a superset of the remaining pages) and stays
     * usable, where clearing it would re-fit the camera every time the store
     * evicted a page — a visible jump every few seconds on a long walk.
     */
    private fun reapEvictedPages(seenPageIds: Set<Int>) {
        val doomed = GpuPageBudget.reap(gpuPages.keys, seenPageIds)
        if (doomed.isEmpty()) return
        for (pageId in doomed) {
            gpuPages.remove(pageId)?.let { destroyGpuPage(it) }
            // The stream map is keyed by the same ids and would otherwise grow
            // without bound for the life of the surface.
            pageStreams.remove(pageId)
        }
        drainRetiredIndexBuffers()
    }

    /**
     * ROUND 22 item 91 (ii): reallocate one page's `VertexBuffer` to
     * [vertices] and re-upload what was already on it.
     *
     * Filament vertex buffers cannot be resized, so growth is destroy +
     * rebuild, and the prefix has to be re-sent because the old buffer's
     * contents cannot be read back. The source page still holds every point
     * from index 0, which is what makes this cheap enough to do inline:
     * `page.buffer` is the same memory the incremental upload path reads.
     *
     * The prefix is re-sent in ONE call, outside the round-5.3 per-frame upload
     * budget, and that is deliberate: splitting it across frames would mean
     * setting the renderable's count below what was already on screen, so every
     * growth event would show as the page briefly shrinking. The size is bounded
     * instead by the caller having charged the whole new buffer against
     * [MAX_NEW_PAGE_BYTES_PER_FRAME] — at most one worst-case page's worth of
     * re-upload per frame, and 8 growth events over a page's entire life
     * because [GpuPageBudget.growthVertices] doubles.
     *
     * The re-upload goes through [tintInto] rather than `countAndMaybeTint`:
     * these points have already been counted into the coverage grid and into
     * ROUND 8's recent-geometry accumulator, and counting them twice would
     * inflate both.
     */
    private fun growGpuPage(gpu: GpuPage, page: com.lidarscan.app.engine.NativePointPage, vertices: Int) {
        val previous = minOf(gpu.uploaded, vertices)
        val newVb = VertexBuffer.Builder()
            .bufferCount(1)
            .vertexCount(vertices)
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

        if (previous > 0) {
            val le = page.buffer.duplicate().order(java.nio.ByteOrder.LITTLE_ENDIAN)
            val payload = if (colorMode == com.lidarscan.core.render.ColorMode.COVERAGE) {
                tintInto(le, 0, previous)
            } else {
                val copy = page.buffer.duplicate()
                copy.position(0)
                copy.limit(previous * POINT_STRIDE_BYTES)
                copy.slice()
            }
            newVb.setBufferAt(engine, 0, payload, 0, previous * POINT_STRIDE_BYTES)
        }

        val ib = ensureSharedIndexBuffer(vertices)
        val rm = engine.renderableManager
        val instance = rm.getInstance(gpu.entity)
        if (instance != 0) {
            // Re-point the renderable BEFORE the old buffer dies, or the frame
            // between the two draws through a destroyed native object — the
            // same class of use-after-free as item 91 (iv)'s index buffer.
            rm.setGeometryAt(
                instance, 0, RenderableManager.PrimitiveType.POINTS, newVb, ib, 0, previous,
            )
        }
        engine.destroyVertexBuffer(gpu.vertexBuffer)
        gpu.vertexBuffer = newVb
        gpu.indexBuffer = ib
        gpu.capacity = vertices
        gpu.uploaded = previous
    }

    // --- ROUND 8: what the newest points say about where the operator is ------

    /**
     * Accumulators for the geometry measurement, **carried across frames and
     * reset only when a measurement is published** (see [publishRecentGeometry]).
     *
     * Resetting them per frame — the obvious version, and the first one written
     * here — silently produces nothing at all on the sensor this feature exists
     * for. A COIN-D6 emits 10 Hz × 360 = 3 600 points/s, which at 60 fps is
     * **sixty points per frame**; after the sampling stride that is a handful of
     * samples, permanently below any floor worth calling a measurement, so the
     * follow camera would have run on the nominal radius forever and the bug
     * would have shown up only as "the framing never responds to the room".
     * Accumulating until the floor is reached instead makes the measurement
     * window **auto-scale with the point rate**: ~0.15 s of D6, ~3 ms of a
     * 200 kpt/s Mid-360, both of them a genuinely recent sample of the same size.
     */
    private var recentN = 0
    private var recentSumX = 0.0
    private var recentSumY = 0.0
    private var recentSumZ = 0.0
    private var recentSumSq = 0.0

    /** Centroid of the most recently uploaded points, and their RMS spread about it. Valid only once [recentGeometryValid]. */
    private var recentCentroidX = 0.0
    private var recentCentroidY = 0.0
    private var recentCentroidZ = 0.0
    private var recentRadiusM = 0.0
    private var recentGeometryValid = false

    /**
     * Stride-samples the points being uploaded this frame into a running centroid
     * and second moment.
     *
     * **Why this exists at all.** `FollowCamera` fits its framing distance to how
     * far the *recent* returns reach from the rig, and the renderer is the only
     * layer that ever sees point coordinates. The alternative — the page AABBs
     * this class already tracks — is exactly the whole-cloud bound whose
     * unbounded growth is the bug being fixed; a page is up to 64 k points, i.e.
     * ~18 s of D6 walk at 10 Hz × 360 returns, so even a per-page AABB is far too
     * coarse to call "recent".
     *
     * **Measured about the points' own centroid, which for a D6 is the rig.**
     * `FollowCamera` wants "how far do the recent returns reach from the operator";
     * what is computed here is their spread about their own centre. Those are the
     * same number precisely because the D6 fan is vertical and *across* the walk:
     * one revolution is a ring centred on the rig, so its centroid is the rig.
     * (That identity is also what makes the centroid usable as the fallback
     * pseudo-pose in [feedFollowCamera].) It degrades gracefully where it does not
     * hold — a Mid-360 in a corner biases the centroid toward the open side and
     * inflates the radius a little, which frames slightly wider. Nothing breaks.
     *
     * **RMS spread, not maximum.** For a D6 ring the RMS distance from the
     * centroid *is* the ring radius, and it is robust: a single long return down
     * an open corridor moves the RMS of a few thousand samples by nothing, where
     * a max would throw the camera to the far end of the building for one frame.
     * Computed from sums (`E[|p|²] − |c|²`) so it is one pass, no buffering.
     *
     * **Byte order is not decoration.** `page.buffer` comes from JNI's
     * `NewDirectByteBuffer`, and a JNI-created direct buffer is **BIG_ENDIAN** on
     * the Java side regardless of the platform — the default nobody notices,
     * because the only existing consumer is `VertexBuffer.setBufferAt`, which
     * copies raw bytes and never interprets them. Reading floats without setting
     * the order gives byte-swapped garbage that is still finite and still
     * plausible-looking, which is the worst kind of wrong. Setting the order
     * mutates only this ByteBuffer *view*; the page memory and the upload path
     * are untouched (and `slice()` resets order to BIG_ENDIAN for the copy
     * anyway, which is equally irrelevant to it).
     */
    private fun accumulateRecentGeometry(buffer: ByteBuffer, fromPoint: Int, toPoint: Int) {
        if (toPoint <= fromPoint) return
        buffer.order(ByteOrder.LITTLE_ENDIAN)
        var i = fromPoint
        while (i < toPoint) {
            val base = i * POINT_STRIDE_BYTES
            val x = buffer.getFloat(base).toDouble()
            val y = buffer.getFloat(base + 4).toDouble()
            val z = buffer.getFloat(base + 8).toDouble()
            if (x.isFinite() && y.isFinite() && z.isFinite()) {
                recentN++
                recentSumX += x; recentSumY += y; recentSumZ += z
                recentSumSq += x * x + y * y + z * z
            }
            i += RECENT_GEOMETRY_STRIDE
        }
    }

    /**
     * Turns the accumulators into a centroid + radius once enough samples have
     * gathered, then clears them so the next measurement is of the *next* stretch
     * of walk rather than of the whole capture so far.
     *
     * The floor of [RECENT_GEOMETRY_MIN_SAMPLES] is what keeps the estimate from
     * being defined by the tail of a page: a handful of points is a fan segment,
     * not a room, and its "radius" would swing wildly. Below the floor the last
     * published measurement is held — `FollowCamera` smooths distance with a 1 s
     * time constant anyway, so holding for a few frames is invisible.
     *
     * The clear-on-publish is the half that makes this a *recent*-geometry
     * measurement. Without it the sums would span the entire session and the
     * radius would converge on the size of the whole building, which is the
     * failure this ROUND set out to remove, reintroduced one layer down.
     */
    private fun publishRecentGeometry() {
        if (recentN < RECENT_GEOMETRY_MIN_SAMPLES) return
        val n = recentN.toDouble()
        val cx = recentSumX / n
        val cy = recentSumY / n
        val cz = recentSumZ / n
        // E[|p - c|²] = E[|p|²] - |c|². Clamped at zero: in exact arithmetic it
        // cannot be negative, in float-summed arithmetic over a tight cluster it
        // can be, and sqrt of that is a NaN straight into the camera.
        val variance = (recentSumSq / n - (cx * cx + cy * cy + cz * cz)).coerceAtLeast(0.0)
        recentCentroidX = cx
        recentCentroidY = cy
        recentCentroidZ = cz
        recentRadiusM = sqrt(variance)
        recentGeometryValid = true
        recentN = 0
        recentSumX = 0.0; recentSumY = 0.0; recentSumZ = 0.0; recentSumSq = 0.0
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

    private fun updateCamera(frameTimeNanos: Long) {
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
                // ROUND 25 item 117. The old note here said the orbit target
                // was "anchored at the session's local-frame origin ... not a
                // moving centroid", which was true and was a limitation
                // dressed as a decision: filament-utils' Manipulator has no
                // runtime retarget, its target being fixed at
                // `Builder.build()` time, so the target COULD not move. Item
                // 117 asks for pan, which is precisely "move the target", so
                // the camera is ours now.
                //
                // The half of that note that was a real design point still
                // holds and is now enforced rather than incidental: the target
                // never drifts BY ITSELF as pages arrive — it moves only when
                // the operator's fingers move it (`pan`) or asks for a reset
                // (`resetCameraFraming`). A camera that re-centres under a
                // growing cloud fights the gesture the operator is mid-way
                // through.
                val c = orbitCamera
                camera.lookAt(
                    c.eyeX.toDouble(), c.eyeY.toDouble(), c.eyeZ.toDouble(),
                    c.targetX.toDouble(), c.targetY.toDouble(), c.targetZ.toDouble(),
                    c.upX.toDouble(), c.upY.toDouble(), c.upZ.toDouble(),
                )
                lastEye = floatArrayOf(c.eyeX, c.eyeY, c.eyeZ)
            }
            CameraMode.FOLLOW -> {
                // ROUND 8. Third person: behind and above the rig, along the
                // negative WALK direction, pitched 35° down, looking AT the rig,
                // framing fitted to recent geometry. All of that is
                // `FollowCamera`'s; this branch only chooses what pose to feed it
                // and hands the answer to `Camera.lookAt`.
                //
                // What was here before was not a follow at all: a fixed offset
                // from the whole cloud's bounding-box centroid, with the distance
                // fitted to the whole cloud's span. Both stop tracking the
                // operator the moment the walk is longer than a room, and the
                // second one zooms out without bound. That code is quoted in
                // `FollowCamera`'s header so the failure it had stays written
                // down rather than merely deleted.
                feedFollowCamera(frameTimeNanos)
                val s = followCamera.solution()
                camera.lookAt(
                    s.eyeX, s.eyeY, s.eyeZ,
                    s.targetX, s.targetY, s.targetZ,
                    // Y-up, as stated and justified at [followCamera]'s
                    // declaration — the solver returns the up vector for the
                    // convention it was configured with rather than this branch
                    // re-asserting `0, 1, 0` and hoping the two agree.
                    s.upX, s.upY, s.upZ,
                )
                lastEye = floatArrayOf(s.eyeX.toFloat(), s.eyeY.toFloat(), s.eyeZ.toFloat())
            }
        }
    }

    /**
     * Gives [followCamera] one pose for this frame, from the best source
     * available, and tells it how far the recent returns reach.
     *
     * **Preferred: the real rig pose** latched by [setRigPose]. Consumed only
     * when its sequence number has moved, and with ARCore's own frame timestamp,
     * so the solver's time constants are measured against the pose stream's clock
     * rather than the display's.
     *
     * **Fallback: the centroid of the newest points.** Used only while no real
     * pose has *ever* arrived. It is defensible precisely because of what a D6
     * is: the fan is vertical and across the direction of travel, so one
     * revolution is a ring *around* the operator, and the centroid of the newest
     * returns is therefore an estimate of where the operator is — biased toward
     * whichever wall is nearer and sitting at the ring's mid-height (about
     * 1.3 m in a 2.6 m room) rather than at the phone (about 1.5 m), but bounded,
     * and moving with the walk, which the old whole-cloud centroid was not. The
     * two sources are never mixed: they are different clocks and different biases,
     * and once [haveRealRigPose] latches the fallback is gone for the session.
     *
     * The fallback's timestamp is the Choreographer's `frameTimeNanos`. Both it
     * and ARCore's `Frame.timestamp` are `CLOCK_MONOTONIC` nanoseconds on
     * Android, so they are comparable — but they are still never interleaved,
     * because comparability is not the same as equal latency and the solver only
     * needs *differences* from one consistent source.
     */
    private fun feedFollowCamera(frameTimeNanos: Long) {
        val radius = if (recentGeometryValid) recentRadiusM else null

        var px = 0.0; var py = 0.0; var pz = 0.0; var pt = 0L
        var have = false
        synchronized(rigPoseLock) {
            if (rigPoseValid && rigPoseSeq != consumedRigPoseSeq) {
                px = rigPoseX; py = rigPoseY; pz = rigPoseZ; pt = rigPoseTNs
                consumedRigPoseSeq = rigPoseSeq
                have = true
            } else if (rigPoseValid) {
                // A real pose exists but has not changed since the last frame
                // (the render is outrunning the 30 Hz pose stream). Nothing to
                // feed — the solver holds its last solution, which is the correct
                // behaviour for a camera whose input has not moved.
                haveRealRigPose = true
            }
        }
        if (have) {
            haveRealRigPose = true
            followCamera.update(pt, px, py, pz, radius)
            return
        }
        if (!haveRealRigPose && recentGeometryValid) {
            followCamera.update(
                frameTimeNanos,
                recentCentroidX, recentCentroidY, recentCentroidZ,
                radius,
            )
        }
        // Neither source yet: `FollowCamera` still returns a finite default
        // camera (its documented degenerate contract), so there is nothing to
        // guard against here and no reason for this branch to invent a fallback
        // framing of its own.
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
                camera.setProjection(ORBIT_FOV_Y_DEG, aspect, 0.05, 2000.0, Camera.Fov.VERTICAL)
            }
            view.viewport = Viewport(0, 0, width, height)
            // px per metre at 1m depth = viewportHeightPx / (2 * tan(fovY/2)) — points.mat's comment.
            val fovYRad = ORBIT_FOV_Y_RAD.toDouble()
            val pxPerMeterAt1m = (height / (2.0 * Math.tan(fovYRad / 2.0))).toFloat()
            materialInstance?.setParameter("pxPerMeterAt1m", pxPerMeterAt1m)
        }
    }

    private inner class FrameCallback : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            choreographer.postFrameCallback(this)
            if (!::uiHelper.isInitialized || !uiHelper.isReadyToRender) return
            // ROUND 5: the refresh cap. Checked before syncPointCloud() so a
            // throttled frame also skips the page upload, which is where the
            // work actually is — a cap that still uploaded pages every vsync
            // would save nothing but the composite.
            //
            // ROUND 5.3: the cap in force is the governor's, which starts at what
            // the operator asked for and eases down when this phone cannot hold
            // it. Only frames we actually DRAW are measured — a frame skipped by
            // the cap is not evidence of a slow phone.
            val cap = governor?.effectiveHz ?: maxRefreshHz
            if (cap > 0) {
                val minIntervalNs = 1_000_000_000L / cap
                // A 10 % tolerance, because vsync lands on a 16.7 ms grid: a
                // strict comparison against a 30 Hz interval (33.3 ms) drops
                // every other 16.7 ms frame *and* then the next one, yielding
                // ~20 fps rather than the 30 asked for.
                if (frameTimeNanos - lastRenderedFrameNs < minIntervalNs - minIntervalNs / 10) return
            }
            val previousFrameNs = lastRenderedFrameNs
            lastRenderedFrameNs = frameTimeNanos
            if (previousFrameNs != 0L) {
                governor?.onFrameInterval(frameTimeNanos, frameTimeNanos - previousFrameNs)?.let { eased ->
                    onRefreshDownshift?.invoke(eased)
                }
            }
            syncPointCloud()
            // ROUND 16 item 59: same beat, same thread, before beginFrame.
            syncTrail()
            updateCamera(frameTimeNanos)
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

        /**
         * ROUND 25 item 117 — the ORBIT/FOLLOW projection's vertical field of
         * view, in radians, as the ONE name the pan arithmetic and
         * `setProjection` both read.
         *
         * 45°, unchanged. It is a constant here rather than a literal in three
         * places because `OrbitCamera.pan` derives metres-per-pixel from it: if
         * the projection's FoV were ever changed without this moving with it,
         * pan would drift out from under the operator's fingers by exactly the
         * ratio of the two tangents — a bug that looks like "the pan feels
         * wrong" and reads like nothing at all in a diff.
         */
        const val ORBIT_FOV_Y_RAD = 0.7853982f
        const val ORBIT_FOV_Y_DEG = 45.0

        /** `PointVertex`: float3 position + RGBA8 = 16 bytes. */
        const val POINT_STRIDE_BYTES = 16

        /**
         * ROUND 16 item 59: the trajectory ribbon uses the SAME 16-byte layout.
         * Not a coincidence and not worth collapsing into one name: they are
         * two vertex formats that happen to agree, and the day the path grows a
         * per-vertex width they stop agreeing.
         */
        const val TRAIL_STRIDE_BYTES = 16

        /**
         * ROUND 5.3: the per-frame vertex-upload ceiling — 4 MB, i.e. 262 144
         * points. Comfortably more than a 10 Hz D6 or a 200 kpts/s Mid-360
         * produces between two frames (so a live capture never actually hits it),
         * and small enough that a burst — a replay decoding ahead of the display,
         * a stream-filter flip that admits a backlog — is spread over frames
         * instead of queued into one.
         */
        const val MAX_UPLOAD_BYTES_PER_FRAME = 4 * 1024 * 1024

        /**
         * ROUND 11 (item 42): how often one already-uploaded page is re-tinted
         * from the density grid. 250 ms round-robin means a 30-page cloud fully
         * converges in about 8 s of walking, which is the timescale on which an
         * operator revisits a thin patch anyway — and it costs one page's worth
         * of upload per quarter second instead of the whole cloud per frame.
         */
        const val COVERAGE_REFRESH_MS = 250L

        /**
         * ROUND 22 item 91 (iii): the companion bound to
         * [MAX_UPLOAD_BYTES_PER_FRAME] — new VertexBuffer **allocation** per
         * frame, in bytes.
         *
         * This replaces `MAX_NEW_PAGES_PER_FRAME = 24`, which counted the wrong
         * thing. A count of pages only bounds memory if pages are one size, and
         * they are not: 24 pages is 1.5 MB of a live 32 k-point store
         * ([com.lidarscan.core.render.LivePageStoreSizing]) and **402 653 184
         * bytes** of the `1 << 20` store the Review screen replays through —
         * asked of the driver inside a single Choreographer callback, which is
         * the LOD slider's crash.
         *
         * The value and the reasoning live in
         * [GpuPageBudget.MAX_NEW_PAGE_BYTES_PER_FRAME] because the tests for
         * this arithmetic are in `:core` (this class cannot be unit-tested —
         * Filament is native and the app module has no Robolectric); it is
         * aliased here so the two per-frame budgets read as the pair they are.
         */
        const val MAX_NEW_PAGE_BYTES_PER_FRAME = GpuPageBudget.MAX_NEW_PAGE_BYTES_PER_FRAME

        /**
         * ROUND 22 item 91 (iv): the floor for the shared identity index
         * buffer, in indices — 65 536 × 4 B = 256 KB.
         *
         * Right-sized vertex buffers (item 91 (ii)) come in ~700 distinct sizes
         * rather than one, so an index buffer grown to fit each of them exactly
         * would retire a generation per page. Powers of two from this floor make
         * it five generations at most between an empty scene and the engine's
         * largest page.
         */
        const val MIN_SHARED_INDEX_COUNT = 65536

        /**
         * ROUND 8: take every 16th point when measuring recent geometry.
         *
         * What is wanted is a centroid and an RMS radius, both of which are
         * converged well inside a hundred samples — reading every point would buy
         * significant figures nobody looks at. At the worst case this loop can
         * face (a full [MAX_UPLOAD_BYTES_PER_FRAME] slice, 262 144 points) the
         * stride leaves 16 384 samples of three float reads and five
         * multiply-adds each: tens of microseconds on the UI thread, against the
         * 4 MB of `setBufferAt` happening beside it. Without a stride this would
         * be the only part of the frame that scales with an upload burst.
         *
         * 16 rather than something larger because the stride also sets how fast
         * the measurement can be *produced*: with the cross-frame accumulation
         * above, a D6's 3 600 points/s yields 225 samples/s, so a measurement
         * lands every ~0.15 s — about one and a half D6 revolutions, i.e. at
         * least one complete ring rather than an arc of one. A stride of 64 makes
         * that 0.57 s, which is slow enough that the points-derived fallback pose
         * visibly stutters.
         *
         * A stride is also *phase-uniform* over a revolution (360 returns per
         * turn; 16 divides 360 evenly into 22.5 samples per turn), so it does not
         * systematically prefer one side of the ring the way "the first N points"
         * would.
         */
        const val RECENT_GEOMETRY_STRIDE = 16

        /**
         * Below this many samples the "room" is a fan segment and its radius is
         * noise — see [publishRecentGeometry]. 32 samples give an RMS estimate
         * good to roughly 12 %, which the follow camera's 1 s distance filter
         * then smooths further; the floor is about rejecting an *arc*, not about
         * statistical precision.
         */
        const val RECENT_GEOMETRY_MIN_SAMPLES = 32
    }
}
