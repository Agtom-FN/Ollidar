package com.lidarscan.app.ui.calib

import android.os.Build
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.google.ar.core.Frame
import com.google.ar.core.TrackingState
import com.google.ar.core.exceptions.NotYetAvailableException
import com.lidarscan.app.ar.ArAvailability
import com.lidarscan.app.ar.CaptureArController
import com.lidarscan.app.ar.toLumaImage
import com.lidarscan.app.engine.NativeMountCalibResult
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.core.calib.BoardSegmentation
import com.lidarscan.core.calib.BoardSegmenter
import com.lidarscan.core.calib.BracketNominals
import com.lidarscan.core.calib.CalibrationGate
import com.lidarscan.core.calib.CheckerboardDetection
import com.lidarscan.core.calib.CheckerboardSpec
import com.lidarscan.core.calib.DiversityWheel
import com.lidarscan.core.calib.FileMountCalibrationStore
import com.lidarscan.core.calib.GateReadout
import com.lidarscan.core.calib.LidarProfile
import com.lidarscan.core.calib.LiveObservation
import com.lidarscan.core.calib.Mat4
import com.lidarscan.core.calib.MountCalibration
import com.lidarscan.core.calib.PinholeIntrinsics
import com.lidarscan.core.calib.PoseCheck
import com.lidarscan.core.calib.PoseCheckState
import com.lidarscan.core.calib.PoseCheckThresholds
import com.lidarscan.core.calib.PoseChecker
import com.lidarscan.core.calib.PosePlan
import com.lidarscan.core.calib.PrescribedPose
import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.SaddleCheckerboardDetector
import com.lidarscan.core.calib.ShutterTimer
import com.lidarscan.core.calib.TargetPlaneEstimator
import com.lidarscan.core.calib.TargetPlaneObservation
import com.lidarscan.core.calib.Vec3
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.ProjectStore
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/** WIZARD.md's five screens, as a state machine. */
enum class WizardStep {
    /** Screen 1 — the target, the three rules, and the blocking preconditions. */
    PREPARE,

    /** Screen 2 — pose-by-pose capture with the live chips and the automatic shutter. */
    CAPTURE,

    /** Screen 3 — the 8-second clock sweep. */
    CLOCK_SWEEP,

    /** Screen 4 — solve, then the split-half gate verdict. */
    VERDICT,

    /** Screen 5 — verify live in the AR overlay. */
    VERIFY,
}

/** One captured pose, kept so the wizard can report and (on a partial reject) drop it. */
data class CapturedPose(
    val prescribed: PrescribedPose,
    val plane: TargetPlaneObservation,
    val segmentation: BoardSegmentation,
    val cameraRollDeg: Double,
    val tMonoNs: Long,
)

data class WizardUiState(
    val step: WizardStep = WizardStep.PREPARE,
    val sensor: SensorType = SensorType.COIN_D6,
    val profile: LidarProfile = LidarProfile.D6,
    val spec: CheckerboardSpec = CheckerboardSpec(),
    val prescribed: List<PrescribedPose> = emptyList(),
    val captured: List<CapturedPose> = emptyList(),
    val checks: PoseCheckState = PoseCheckState(emptySet(), null),
    val ringProgress: Float = 0f,
    val diversity: Triple<Double, Double, Double> = Triple(0.0, 0.0, 0.0),
    val weakestAxis: DiversityWheel.Axis = DiversityWheel.Axis.ROLL,
    val arAvailability: ArAvailability = ArAvailability.CHECKING,
    val engineConnected: Boolean = false,
    val lidarReturnsOnBoard: Int = 0,
    val detectionFound: Boolean = false,
    val lastDetectionRmsPx: Double = 0.0,
    val solving: Boolean = false,
    val result: NativeMountCalibResult? = null,
    val readout: GateReadout? = null,
    val diagnosis: String? = null,
    val saved: MountCalibration? = null,
    val message: String? = null,
    val clockSweepSeconds: Float = 0f,
) {
    val currentPose: PrescribedPose?
        get() = prescribed.getOrNull(captured.size)

    val poseCount: Int get() = captured.size
    val poseTarget: Int get() = prescribed.size
    val canSolve: Boolean get() = captured.size >= MIN_OBSERVATIONS

    companion object {
        /** A8 §4.3: fewer than 3 is refused outright; 5 is the Zhang–Pless floor below which the solve is marked degenerate. */
        const val MIN_OBSERVATIONS = 5
    }
}

/**
 * Drives the mount-calibration wizard (S6 `WIZARD.md`, Tech Spec §3.3/§3.5).
 *
 * ### The two halves of one observation
 *
 * Each captured pose contributes ONE `scan_mount_calib_add_observation` call,
 * carrying:
 *
 *  * the **camera** half — the board plane `(n, d)` in the camera frame, from
 *    the checkerboard detection through a homography
 *    ([TargetPlaneEstimator]); and
 *  * the **lidar** half — the returns segmented onto the board, in the sensor
 *    frame, read live from the engine's `PageStore` through the same page
 *    reads B4's renderer uses.
 *
 * The lidar points are in the SENSOR frame because the pushbroom assembler is
 * *off* during the wizard: with no mount extrinsic yet there is nothing to
 * assemble world points with, and `D6Driver::on_point()` publishes
 * `x = -d·sinθ, y = d·cosθ, z = 0` — the sensor frame the solver's residual
 * is written against (A8 §3.1, derived in full in
 * `engine/include/scanengine/drivers/d6/d6_fan.h`). ROUND 9 item 34 corrected
 * the sign of that x term; the driver and the pushbroom now share one
 * implementation, so a wizard solve and a resolved cloud cannot disagree
 * about which way the fan sweeps.
 *
 * ### Honesty about the detector
 *
 * Detection is [SaddleCheckerboardDetector], a real implementation in `:core`
 * with its own tests — not a stub, and not a fake success. Its verified
 * envelope is synthetic imagery only; it has never seen a real camera frame,
 * which is stated in its own doc and in android/NOTES.md. The wizard is
 * built so that matters as little as possible: a pose is only captured when
 * all five live checks pass, and the session is only accepted when the
 * engine's split-half gate says so — and that gate observes the actual
 * capture, which is exactly the S6 argument for using it instead of the
 * solver's covariance.
 */
class MountCalibrationViewModel(
    private val arController: CaptureArController,
    private val engineBridge: EngineBridge,
    private val projectStore: ProjectStore,
    private val calibrationStore: FileMountCalibrationStore,
    private val projectId: String,
    private val appVersion: String,
    private val engineHandleProvider: () -> Long,
    private val pointCloudSourceProvider: () -> PointCloudSource?,
) : ViewModel() {

    private val _uiState = MutableStateFlow(WizardUiState())
    val uiState: StateFlow<WizardUiState> = _uiState.asStateFlow()

    private val detector = SaddleCheckerboardDetector()
    private val wheel = DiversityWheel()
    private var thresholds = PoseCheckThresholds.forProfile(LidarProfile.D6)
    private var shutter = ShutterTimer(thresholds.dwellMillis)
    private val detecting = AtomicBoolean(false)

    /**
     * Detection runs off the GL thread: a full detection pass is tens of
     * milliseconds, and doing it inline would stall `Session.update()` and
     * therefore the pose stream feeding the engine. One frame at a time —
     * [detecting] drops frames arriving while a pass is in flight rather than
     * queueing them, because a stale detection is worthless for a live check.
     */
    private val detectorExecutor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "lidarscan-checkerboard").apply { priority = Thread.NORM_PRIORITY - 1 }
    }

    private var calibHandle: Long = 0L
    private var bracketId: String = BracketNominals.DEFAULT_BRACKET_ID
    private var sensorSerial: String? = null

    init {
        viewModelScope.launch(Dispatchers.IO) {
            val project = projectStore.open(projectId)
            val sensor = project?.manifest?.sensor ?: SensorType.COIN_D6
            val profile = LidarProfile.of(sensor)
            thresholds = PoseCheckThresholds.forProfile(profile)
            shutter = ShutterTimer(thresholds.dwellMillis)
            _uiState.value = _uiState.value.copy(
                sensor = sensor,
                profile = profile,
                prescribed = PosePlan.poses(PosePlan.recommendedCount(profile)),
            )
        }

        viewModelScope.launch {
            arController.status.collect { status ->
                _uiState.value = _uiState.value.copy(arAvailability = status.availability)
            }
        }
        viewModelScope.launch {
            engineBridge.connectionState.collect { state ->
                _uiState.value = _uiState.value.copy(
                    engineConnected = state == com.lidarscan.core.engine.ConnectionState.CONNECTED,
                )
            }
        }

        arController.addFrameListener(::onArFrame)
    }

    fun setSquareSize(metres: Double) {
        if (metres <= 0.0) return
        val spec = _uiState.value.spec
        _uiState.value = _uiState.value.copy(spec = spec.copy(squareSizeM = metres))
    }

    fun setBracketId(id: String) {
        bracketId = id.ifBlank { BracketNominals.DEFAULT_BRACKET_ID }
    }

    fun goTo(step: WizardStep) {
        if (step == WizardStep.CAPTURE) ensureSolverHandle()
        _uiState.value = _uiState.value.copy(step = step, message = null)
    }

    fun restartCapture() {
        destroySolverHandle()
        shutter.reset()
        _uiState.value = _uiState.value.copy(
            step = WizardStep.CAPTURE,
            captured = emptyList(),
            result = null,
            readout = null,
            diagnosis = null,
            saved = null,
            message = null,
        )
        ensureSolverHandle()
    }

    /**
     * Drops the poses with the worst lidar residuals and asks for
     * replacements — WIZARD.md screen 4's third diagnosis ("high residuals on
     * 1–2 poses -> drop those poses, re-solve, and ask for two replacements").
     * The observations themselves cannot be removed from the engine's solver
     * handle (the C ABI is add-only), so the handle is rebuilt from the kept
     * poses, which is equivalent and needs no ABI change.
     */
    fun dropWorstPosesAndRetry(count: Int = 2) {
        val kept = _uiState.value.captured
            .sortedBy { it.segmentation.residualRmsM }
            .dropLast(count.coerceAtMost(_uiState.value.captured.size))
        destroySolverHandle()
        ensureSolverHandle()
        kept.forEach { addObservationToSolver(it) }
        _uiState.value = _uiState.value.copy(
            step = WizardStep.CAPTURE,
            captured = kept,
            result = null,
            readout = null,
            diagnosis = null,
            message = "Dropped the $count worst poses — capture $count more",
        )
    }

    // --- live loop ----------------------------------------------------------

    /** Called on the ARCore/GL thread for every frame. Cheap work only; detection is dispatched. */
    private fun onArFrame(frame: Frame) {
        val state = _uiState.value
        if (state.step != WizardStep.CAPTURE && state.step != WizardStep.CLOCK_SWEEP) return
        if (!detecting.compareAndSet(false, true)) return

        val image = try {
            frame.acquireCameraImage()
        } catch (e: NotYetAvailableException) {
            detecting.set(false)
            return
        } catch (e: Exception) {
            detecting.set(false)
            return
        }

        val luma = try {
            image.toLumaImage()
        } finally {
            image.close()
        }

        val intrinsics = frame.camera.imageIntrinsics
        val focal = intrinsics.focalLength
        val principal = intrinsics.principalPoint
        val dimensions = intrinsics.imageDimensions
        val pinhole = PinholeIntrinsics(
            fx = focal[0].toDouble(), fy = focal[1].toDouble(),
            cx = principal[0].toDouble(), cy = principal[1].toDouble(),
            width = dimensions[0], height = dimensions[1],
        )
        val pose = frame.camera.pose
        val orientation = Quat(
            pose.qx().toDouble(), pose.qy().toDouble(), pose.qz().toDouble(), pose.qw().toDouble(),
        )
        val tracking = frame.camera.trackingState == TrackingState.TRACKING
        val timestamp = frame.timestamp

        if (luma == null) {
            detecting.set(false)
            return
        }
        detectorExecutor.execute {
            try {
                processDetection(luma, pinhole, orientation, tracking, timestamp)
            } finally {
                detecting.set(false)
            }
        }
    }

    private fun processDetection(
        luma: com.lidarscan.core.calib.LumaImage,
        intrinsics: PinholeIntrinsics,
        orientation: Quat,
        tracking: Boolean,
        tMonoNs: Long,
    ) {
        val state = _uiState.value
        val prescribed = state.currentPose ?: return
        val detection: CheckerboardDetection? = detector.detect(luma, state.spec)
        val plane = detection?.let { TargetPlaneEstimator.estimate(it, intrinsics) }

        // Lidar side: read the live pages and segment against the plane the
        // camera just measured, transformed into the lidar frame with the
        // bracket's CAD nominal (the bootstrap — see BoardSegmenter's header).
        val segmentation = plane?.let { segmentBoard(it) }

        val motion = arController.motion.estimateAt(tMonoNs)
        val roll = Math.toDegrees(orientation.rollRadians())
        val observation = LiveObservation(
            detection = detection,
            plane = plane,
            imageWidth = luma.width,
            imageHeight = luma.height,
            cameraRollDeg = roll,
            prescribedRollDeg = prescribed.rollDeg,
            lidarReturnsOnBoard = segmentation?.count ?: 0,
            angularRateRadPerS = motion.angularRateRadPerS.toDouble(),
            linearSpeedMPerS = motion.linearSpeedMPerS.toDouble(),
            trackingOk = tracking,
        )
        val checks = PoseChecker.evaluate(observation, thresholds)
        val progress = shutter.update(checks.allGreen, System.currentTimeMillis())

        _uiState.value = _uiState.value.copy(
            checks = checks,
            ringProgress = progress.ring,
            detectionFound = detection != null,
            lastDetectionRmsPx = plane?.reprojectionRmsPx ?: 0.0,
            lidarReturnsOnBoard = observation.lidarReturnsOnBoard,
        )

        if (progress.fire && plane != null && segmentation != null) {
            capturePose(prescribed, plane, segmentation, roll, tMonoNs)
        }
    }

    private fun segmentBoard(plane: TargetPlaneObservation): BoardSegmentation? {
        val source = pointCloudSourceProvider() ?: return null
        if (!source.isAvailable) return null
        val profile = _uiState.value.profile
        val predicted = BoardSegmenter.predictInLidarFrame(
            plane,
            BracketNominals.cadNominal(_uiState.value.sensor),
        )

        // Read the most recent page only: at the wizard's ranges a single
        // 1M-point page is far more data than one board needs, and walking
        // every page would re-scan the whole session for each live frame.
        val pageCount = source.pageCount()
        if (pageCount <= 0) return null
        val pageId = source.pageIdAt(pageCount - 1)
        val page = source.getPage(pageId) ?: return null

        val points = ArrayList<Vec3>(minOf(page.count, MAX_POINTS_PER_SEGMENTATION))
        val buffer = page.buffer.duplicate().order(java.nio.ByteOrder.nativeOrder())
        val stride = 16
        val first = maxOf(0, page.count - MAX_POINTS_PER_SEGMENTATION)
        for (i in first until page.count) {
            val base = i * stride
            if (base + 12 > buffer.limit()) break
            points.add(
                Vec3(
                    buffer.getFloat(base).toDouble(),
                    buffer.getFloat(base + 4).toDouble(),
                    buffer.getFloat(base + 8).toDouble(),
                ),
            )
        }
        if (points.isEmpty()) return null
        return BoardSegmenter.segment(points, predicted, profile)
    }

    private fun capturePose(
        prescribed: PrescribedPose,
        plane: TargetPlaneObservation,
        segmentation: BoardSegmentation,
        rollDeg: Double,
        tMonoNs: Long,
    ) {
        val captured = CapturedPose(prescribed, plane, segmentation, rollDeg, tMonoNs)
        if (!addObservationToSolver(captured)) return

        wheel.add(prescribed.azimuthDeg, prescribed.elevationDeg, rollDeg)
        shutter.reset()
        val list = _uiState.value.captured + captured
        _uiState.value = _uiState.value.copy(
            captured = list,
            diversity = wheel.axisCoverage(),
            weakestAxis = wheel.weakestAxis(),
            ringProgress = 0f,
            message = null,
        )
    }

    private fun addObservationToSolver(pose: CapturedPose): Boolean {
        ensureSolverHandle()
        val handle = calibHandle
        if (handle == 0L) return false
        val err = ScanEngineNative.nativeMountCalibAddObservation(
            handle = handle,
            nx = pose.plane.normal.x, ny = pose.plane.normal.y, nz = pose.plane.normal.z,
            d = pose.plane.d,
            xyz = pose.segmentation.toFloatArray(),
            sigmaM = _uiState.value.profile.sigmaM,
        )
        if (err != ScanEngineNative.ErrorCode.OK) {
            _uiState.value = _uiState.value.copy(
                message = "The solver refused that pose: ${ScanEngineNative.nativeErrorStr(err)}",
            )
            return false
        }
        return true
    }

    // --- solve + persist ----------------------------------------------------

    fun solve() {
        val state = _uiState.value
        if (!state.canSolve) {
            _uiState.value = state.copy(message = "Capture at least ${WizardUiState.MIN_OBSERVATIONS} poses first")
            return
        }
        _uiState.value = state.copy(solving = true, step = WizardStep.VERDICT)
        viewModelScope.launch(Dispatchers.Default) {
            val handle = calibHandle
            val result = if (handle == 0L) {
                null
            } else {
                ScanEngineNative.nativeMountCalibSolve(
                    handle,
                    BracketNominals.cadNominal(state.sensor).m,
                )
            }
            if (result == null) {
                _uiState.value = _uiState.value.copy(
                    solving = false,
                    message = "The solve failed: ${ScanEngineNative.nativeLastError()}",
                )
                return@launch
            }
            val gate = CalibrationGate.fromEngine(result.gate)
            _uiState.value = _uiState.value.copy(
                solving = false,
                result = result,
                readout = GateReadout.of(gate, result.splitHalfPx),
                diagnosis = if (gate == CalibrationGate.REJECT) diagnose(result) else null,
            )
        }
    }

    /**
     * WIZARD.md screen 4: "On reject, diagnose rather than just fail — the
     * wizard knows which it was."
     */
    private fun diagnose(result: NativeMountCalibResult): String {
        val state = _uiState.value
        val (_, _, roll) = state.diversity
        val medianReturns = state.captured.map { it.segmentation.count }.sorted()
            .getOrNull(state.captured.size / 2) ?: 0
        return when {
            result.degenerate ->
                "Too few usable poses, or they were all too similar. Capture more, and vary the angle between them."
            roll < 0.5 ->
                "The poses were not varied enough in roll. Try again, and tilt the phone more between shots."
            medianReturns < state.profile.minReturnsPerPose * 2 ->
                "The lidar saw very little of the board. Move the board further from the wall, and stand a little closer."
            else ->
                "One or two poses look much worse than the rest — drop them and capture replacements."
        }
    }

    /** Screen 4's accept: store per bracket AND in the project's manifest (WIZARD.md §3). */
    fun accept() {
        val state = _uiState.value
        val result = state.result ?: return
        val gate = CalibrationGate.fromEngine(result.gate)
        if (gate == CalibrationGate.REJECT) {
            _uiState.value = state.copy(message = "A rejected calibration cannot be saved — redo the capture")
            return
        }
        val calibration = MountCalibration(
            id = UUID.randomUUID().toString(),
            sensor = state.sensor,
            bracketId = bracketId,
            sensorSerial = sensorSerial,
            cameraFromLidar = result.cameraFromLidar.copyOf(),
            splitHalfPx = result.splitHalfPx,
            gate = gate,
            gateRangeM = result.gateRangeM,
            poseCount = state.captured.size,
            squareSizeM = state.spec.squareSizeM,
            boardCols = state.spec.cols,
            boardRows = state.spec.rows,
            // The clock sweep (WIZARD.md screen 3) needs
            // `scan_clock_sweep_estimate`, which A11 §8.2 lists as a C-ABI
            // seam and which does not exist at SCAN_ABI_VERSION 3 — so no
            // offset is claimed rather than a zero being written as if it
            // had been measured. See android/NOTES.md.
            clockOffsetNs = null,
            rmsResidualM = result.rmsResidualM,
            sigmaRotDeg = result.sigmaRotDeg,
            sigmaTransMm = result.sigmaTransMm,
            conditionNumber = result.conditionNumber,
            createdAtEpochMillis = System.currentTimeMillis(),
            appVersion = appVersion,
            phoneModel = "${Build.MANUFACTURER} ${Build.MODEL}",
        )

        viewModelScope.launch(Dispatchers.IO) {
            calibrationStore.save(calibration)
            val project = projectStore.open(projectId)
            if (project != null) {
                projectStore.updateManifest(projectId) { manifest ->
                    manifest.copy(
                        mountCalibrationId = calibration.id,
                        mountCalibration = calibration,
                    )
                }
            }
            _uiState.value = _uiState.value.copy(
                saved = calibration,
                step = WizardStep.VERIFY,
                message = null,
            )
        }
    }

    /** Screen 5's confirmation. "Looks off" is a rejection of the saved calibration, not a no-op. */
    fun confirmVerification(looksAligned: Boolean) {
        if (looksAligned) {
            _uiState.value = _uiState.value.copy(message = "Mount calibration saved for this bracket")
            return
        }
        val saved = _uiState.value.saved
        viewModelScope.launch(Dispatchers.IO) {
            if (saved != null) {
                calibrationStore.delete(saved.id)
                projectStore.updateManifest(projectId) { manifest ->
                    manifest.copy(mountCalibrationId = null, mountCalibration = null)
                }
            }
            _uiState.value = _uiState.value.copy(
                saved = null,
                message = "Discarded — check the bracket and the phone seating, then run the wizard again",
            )
            restartCapture()
        }
    }

    fun onClockSweepProgress(seconds: Float) {
        _uiState.value = _uiState.value.copy(clockSweepSeconds = seconds)
    }

    /**
     * Applies a stored calibration to a live engine session:
     * `scan_engine_set_mount_extrinsics` + `pushbroom_enable`. Called at
     * capture start, not here — exposed as a function so both the wizard's
     * verify step and the Capture screen use one implementation.
     */
    fun applyToEngine(calibration: MountCalibration): Boolean {
        val handle = engineHandleProvider()
        if (handle == 0L) return false
        val matrix = Mat4(calibration.cameraFromLidar.copyOf())
        if (!matrix.isRigid(1e-4)) {
            _uiState.value = _uiState.value.copy(
                message = "The stored extrinsic is not a rigid transform — refusing to apply it",
            )
            return false
        }
        val err = ScanEngineNative.nativeSetMountExtrinsics(handle, matrix.m)
        if (err != ScanEngineNative.ErrorCode.OK) {
            _uiState.value = _uiState.value.copy(
                message = "The engine rejected the extrinsic: ${ScanEngineNative.nativeErrorStr(err)}",
            )
            return false
        }
        return ScanEngineNative.nativePushbroomEnable(handle, true) == ScanEngineNative.ErrorCode.OK
    }

    private fun ensureSolverHandle() {
        if (calibHandle != 0L) return
        if (!ScanEngineNative.isAvailable) {
            _uiState.value = _uiState.value.copy(
                message = "The native engine is not loaded — calibration needs the real engine, not the simulator",
            )
            return
        }
        calibHandle = ScanEngineNative.nativeMountCalibCreate()
    }

    private fun destroySolverHandle() {
        if (calibHandle != 0L) {
            ScanEngineNative.nativeMountCalibDestroy(calibHandle)
            calibHandle = 0L
        }
    }

    override fun onCleared() {
        arController.removeFrameListener(::onArFrame)
        destroySolverHandle()
        detectorExecutor.shutdown()
        super.onCleared()
    }

    private companion object {
        /**
         * Points read per live segmentation pass. A D6 revolution is ~4k
         * points and a Mid-360 second is ~200k; the board occupies a small
         * fraction of either, and scanning more than this per FRAME would cost
         * more than the detection does.
         */
        const val MAX_POINTS_PER_SEGMENTATION = 20_000
    }

    /** The check chips, in the order WIZARD.md's table lists them. */
    val checkOrder: List<PoseCheck> get() = PoseCheck.entries
}
