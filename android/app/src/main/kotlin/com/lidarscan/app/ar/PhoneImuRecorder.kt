package com.lidarscan.app.ar

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.capture.CameraFromImuExtrinsics
import com.lidarscan.core.capture.ImuClockDomain
import com.lidarscan.core.capture.ImuRateMeter
import com.lidarscan.core.capture.ImuStreamMerger
import com.lidarscan.core.capture.PhoneImuStatus
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * ROUND 9, owner item 35 — **the first `SensorManager` in this app.**
 *
 * The owner: *"lidar data and the imu position data need sync the frequency."*
 * ARCore hands out poses at ~30 Hz (150 poses over 4.999 s on the owner's real
 * `scan-017`, median interval 33.33 ms); the COIN-D6 samples at 4000 Hz. Every
 * D6 return is therefore resolved against an INTERPOLATED pose, and until this
 * round that interpolation was a plain LERP/SLERP — exact for a dolly, wrong
 * for a walking human whose 2 Hz gait puts real angular motion inside every
 * 33 ms gap. The engine now densifies those poses by integrating the phone's
 * gyro between each bracketing pair; measured engine-side on a synthetic wall
 * with 1.5 deg of 12 Hz jitter, plane-fit RMS went **0.739 cm -> 0.021 cm**.
 *
 * This class is the source of that gyro. Before it there was no
 * `SensorManager`, no `SensorEventListener` and no `Sensor` reference anywhere
 * in the app — a deliberate absence, documented in
 * [com.lidarscan.core.capture.RigMotionTracker]'s KDoc ("the phone's own IMU is
 * not a stream this app pushes into the engine at all"), now updated.
 *
 * ## Shape
 *
 *  * **Its own [HandlerThread].** Not the main looper: at 400 Hz that is 400
 *    main-thread callbacks a second competing with Compose, and not the GL
 *    thread either, because that one is already carrying `Session.update()` +
 *    [CaptureArController.publishPose] at 30 Hz and must not be made to also
 *    carry this. Both sensors are registered on the SAME handler, which is what
 *    makes [ImuStreamMerger] safe without a lock.
 *  * **Fastest rate the device allows**, `SENSOR_DELAY_FASTEST` with a zero
 *    report latency (no batching — batched delivery keeps the timestamps but
 *    adds latency the densifier has no use for). `targetSdk` is 36 and Android
 *    caps delivery at 200 Hz without `HIGH_SAMPLING_RATE_SENSORS`; that
 *    permission is declared in the manifest (install-time, no prompt), which
 *    lifts the cap to the hardware's real 400+ Hz. Whether it was actually
 *    granted is MEASURED, not assumed — see [PhoneImuStatus.measuredHz].
 *  * **`TYPE_GYROSCOPE`, the calibrated one**, not `TYPE_GYROSCOPE_UNCALIBRATED`.
 *    The engine's densifier estimates residual bias itself over each short
 *    ARCore interval, so handing it the raw uncalibrated stream would ask it to
 *    re-derive a factory/runtime calibration the platform already applies.
 *  * **Merge:** hold the latest accel, emit on each gyro. The reasoning is in
 *    [ImuStreamMerger]'s KDoc — the consumer is a gyro integrator, so the gyro
 *    sets the cadence and no timestamp is ever invented.
 *  * **Timestamps pass through UNCHANGED.** `SensorEvent.timestamp` is already
 *    the engine's `CLOCK_BOOTTIME` domain. See the probe below for the part of
 *    that sentence that is not guaranteed.
 *
 * ## The clock-domain probe
 *
 * The app treats `CLOCK_BOOTTIME` as the engine clock with no conversion
 * anywhere. `SensorEvent.timestamp` is BOOTTIME on virtually all devices but
 * the platform does **not** guarantee it. That is the same class of assumption
 * [ArCameraCharacteristicsProbe] guards for the camera via
 * `SENSOR_INFO_TIMESTAMP_SOURCE`, and this follows that precedent exactly:
 * check once, at the start of the stream, log LOUDLY naming the stream, and —
 * because unlike the camera case there is a safe action available — **stop
 * pushing** rather than feed the engine samples from a clock its poses are not
 * in. A densifier fed IMU from the wrong epoch does not degrade gracefully; it
 * would either find no bracketing poses at all or, worse, integrate the wrong
 * interval.
 *
 * ## Lifecycle
 *
 * [start] at capture start (next to the mount extrinsic and the AR pipelines),
 * [stop] in the seal path before the engine handle goes away — same rule the
 * keyframe recorder and the phone-GNSS fallback already follow, and for the
 * same reason: this pushes into that handle.
 */
class PhoneImuRecorder(
    context: Context,
    /** Seam for tests: the JNI push. Returns a `scan_error_t`. */
    private val pushImu: (Long, Long, Float, Float, Float, Float, Float, Float) -> Int =
        { handle, t, gx, gy, gz, ax, ay, az ->
            ScanEngineNative.nativePushImu(handle, t, gx, gy, gz, ax, ay, az)
        },
    /** Seam for tests: `scan_engine_set_imu_extrinsics`. */
    private val setImuExtrinsics: (Long, DoubleArray) -> Int =
        { handle, quat -> ScanEngineNative.nativeSetImuExtrinsics(handle, quat) },
    /** Seam for tests: the delivery-time clock the timestamp domain is checked against. */
    private val elapsedRealtimeNanos: () -> Long = { SystemClock.elapsedRealtimeNanos() },
) : SensorEventListener {

    private val sensorManager: SensorManager? =
        context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager

    private val gyro: Sensor? = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    private val accel: Sensor? = sensorManager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)

    private val merger = ImuStreamMerger()
    private val rate = ImuRateMeter()

    private var thread: HandlerThread? = null
    private var handler: Handler? = null

    /** Written on the caller's thread at start/stop, read on the sensor thread. */
    @Volatile
    private var engineHandle: Long = 0L

    /** Set once the clock-domain probe fails; the push is then permanently off for this session. */
    @Volatile
    private var pushDisabled: String? = null

    // Sensor-thread-only state (see the single-HandlerThread note above).
    private var clockProbesLeft = 0
    private var pushed = 0L
    private var rejected = 0L
    private var lastStatusPublishNs = 0L

    private val _status = MutableStateFlow(PhoneImuStatus())
    val status: StateFlow<PhoneImuStatus> = _status.asStateFlow()

    /** True when this device has the sensors at all. A phone without a gyro simply gets the old slerp path. */
    val available: Boolean get() = gyro != null && accel != null

    /**
     * Starts the stream into [handle] and pushes [extrinsics] once.
     *
     * [extrinsics] comes from [ArCameraCharacteristicsProbe.cameraFromImu] — see
     * [com.lidarscan.core.capture.CameraFromImu] for the derivation from
     * `SENSOR_ORIENTATION`. When it could not be derived, identity is pushed and
     * a LOUD warning is logged saying the densifier's path shape will be
     * degraded; that is this item's own instruction, and it is much better than
     * a silent guess, which would be indistinguishable from a correct value in
     * every log the field ever produces.
     */
    fun start(handle: Long, extrinsics: CameraFromImuExtrinsics) {
        stop()
        val manager = sensorManager
        if (manager == null || gyro == null || accel == null) {
            val why = when {
                manager == null -> "no SensorManager"
                gyro == null -> "device has no TYPE_GYROSCOPE"
                else -> "device has no TYPE_ACCELEROMETER"
            }
            Log.w(TAG, "phone IMU not started: $why — the pose interpolator falls back to plain SLERP")
            _status.value = PhoneImuStatus(
                running = false,
                gyroPresent = gyro != null,
                accelPresent = accel != null,
                disabledReason = why,
            )
            return
        }

        merger.reset()
        rate.reset()
        pushed = 0L
        rejected = 0L
        clockProbesLeft = CLOCK_PROBE_SAMPLES
        pushDisabled = null
        lastStatusPublishNs = 0L
        engineHandle = handle

        if (handle != 0L) {
            if (extrinsics.derived) {
                Log.i(TAG, "camera_from_imu: ${extrinsics.why}")
            } else {
                // Loud: identity here is a KNOWN-degraded state, not a default.
                Log.w(
                    TAG,
                    "camera_from_imu could NOT be derived (${extrinsics.why}). Pushing IDENTITY — the " +
                        "IMU densifier will integrate gyro increments in the wrong frame and the " +
                        "densified path SHAPE will be degraded (StreamId ${ImuClockDomain.STREAM_NAME}).",
                )
            }
            // Pushed BEFORE the listeners are registered, deliberately: the C
            // ABI notes that applying an extrinsic REBUILDS the densifier,
            // dropping its buffered samples and its estimated gyro bias, and
            // asks for it "once during setup". Nothing has been pushed yet at
            // this point in `start()`, so there is nothing to drop.
            val err = setImuExtrinsics(handle, extrinsics.toXyzw())
            if (err != ScanEngineNative.ErrorCode.OK) {
                Log.w(TAG, "scan_engine_set_imu_extrinsics failed err=$err")
            }
        }

        val t = HandlerThread("phone-imu", android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
        t.start()
        val h = Handler(t.looper)
        thread = t
        handler = h

        // SENSOR_DELAY_FASTEST (0 us) with maxReportLatencyUs = 0: ask for
        // everything the device will give, delivered as it arrives. The
        // platform is free to give less; `measuredHz` below is what says what
        // it actually gave.
        val gyroOk = manager.registerListener(this, gyro, SensorManager.SENSOR_DELAY_FASTEST, 0, h)
        val accelOk = manager.registerListener(this, accel, SensorManager.SENSOR_DELAY_FASTEST, 0, h)
        if (!gyroOk || !accelOk) {
            Log.w(TAG, "registerListener refused (gyro=$gyroOk accel=$accelOk)")
        }

        _status.value = PhoneImuStatus(
            running = gyroOk && accelOk,
            gyroPresent = true,
            accelPresent = true,
            requestedHz = 0.0,
            sensorMaxHz = sensorCeilingHz(gyro),
            clockDomainOk = true,
            disabledReason = if (gyroOk && accelOk) null else "registerListener refused",
            extrinsicsDerived = extrinsics.derived,
            extrinsicsNote = extrinsics.why,
        )
        Log.i(
            TAG,
            "phone IMU started: handle=$handle gyro=${gyro.name} minDelay=${gyro.minDelay}us " +
                "(ceiling ${"%.0f".format(sensorCeilingHz(gyro))} Hz) accel=${accel.name}",
        )
    }

    /** Idempotent; safe to call from the seal path whether or not [start] ever ran. */
    fun stop() {
        engineHandle = 0L
        sensorManager?.unregisterListener(this)
        thread?.quitSafely()
        thread = null
        handler = null
        val final = _status.value.copy(running = false, measuredHz = rate.hz())
        _status.value = final
        if (final.samplesPushed > 0 || final.samplesRejected > 0) {
            Log.i(TAG, final.summary())
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {
        // Nothing to do. Accuracy is a hint about magnetometer/gyro calibration
        // quality; the densifier estimates residual bias itself, and dropping
        // samples on a LOW accuracy report would punch holes in the integration.
    }

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor?.type) {
            Sensor.TYPE_ACCELEROMETER -> merger.onAccel(event.timestamp, event.values[0], event.values[1], event.values[2])
            Sensor.TYPE_GYROSCOPE -> onGyroEvent(event)
            else -> Unit
        }
    }

    private fun onGyroEvent(event: SensorEvent) {
        // The clock-domain probe, on the first few samples of the stream, at
        // DELIVERY time. See the class KDoc.
        if (clockProbesLeft > 0) {
            clockProbesLeft--
            if (!ImuClockDomain.looksLikeBoottime(event.timestamp, elapsedRealtimeNanos())) {
                disableForClockDomain(event.timestamp)
            }
        }

        val sample = merger.onGyro(event.timestamp, event.values[0], event.values[1], event.values[2])
            ?: return
        rate.record(sample.tMonoNs)

        val handle = engineHandle
        if (handle != 0L && pushDisabled == null) {
            val err = pushImu(
                handle, sample.tMonoNs,
                sample.gx, sample.gy, sample.gz,
                sample.ax, sample.ay, sample.az,
            )
            if (err == ScanEngineNative.ErrorCode.OK) pushed++ else rejected++
        }

        publishStatusOccasionally(sample.tMonoNs)
    }

    private fun disableForClockDomain(timestampNs: Long) {
        if (pushDisabled != null) return
        val now = elapsedRealtimeNanos()
        val skewMs = (now - timestampNs) / 1_000_000.0
        val why = "SensorEvent.timestamp is not CLOCK_BOOTTIME (skew ${"%.0f".format(skewMs)} ms)"
        pushDisabled = why
        // Loud, and it names the stream, exactly as ArCameraCharacteristicsProbe
        // names StreamId.kPoseAr when the camera's timestamp source disagrees.
        Log.e(
            TAG,
            "StreamId ${ImuClockDomain.STREAM_NAME}: $why. The app treats CLOCK_BOOTTIME as the engine " +
                "clock and converts nowhere, so these samples would land in a different epoch from the " +
                "ARCore poses they are meant to densify. DISABLING the IMU push for this session — the " +
                "pose interpolator falls back to plain SLERP, which is degraded but correct.",
        )
        _status.value = _status.value.copy(clockDomainOk = false, disabledReason = why)
    }

    /**
     * Publishing a `StateFlow` 400 times a second would cost more than the push
     * it is reporting on, so the status is republished at [STATUS_PERIOD_NS]
     * and once more at [stop]. The counters themselves are exact; only their
     * visibility is throttled.
     */
    private fun publishStatusOccasionally(tMonoNs: Long) {
        if (lastStatusPublishNs != 0L && tMonoNs - lastStatusPublishNs < STATUS_PERIOD_NS) return
        lastStatusPublishNs = tMonoNs
        val hz = rate.hz()
        val ceiling = sensorCeilingHz(gyro)
        _status.value = _status.value.copy(
            samplesPushed = pushed,
            samplesRejected = rejected,
            samplesDropped = merger.droppedNoAccel + merger.droppedOutOfOrder,
            measuredHz = hz,
            sensorMaxHz = ceiling,
            // "Granted" means the measured rate reached the sensor's own
            // ceiling rather than the platform's 200 Hz cap. Within 20% because
            // a delivered rate is never exactly minDelay.
            requestedRateGranted = ceiling > 0.0 && hz >= ceiling * 0.8,
            clockDomainOk = pushDisabled == null,
            disabledReason = pushDisabled ?: _status.value.disabledReason,
        )
    }

    private companion object {
        const val TAG = "PhoneImuRecorder"

        /**
         * How many of the stream's first gyro samples get the clock check. More
         * than one so a single scheduling hiccup at start-up cannot condemn a
         * device whose clock is fine; few enough that the check costs nothing.
         */
        const val CLOCK_PROBE_SAMPLES = 8

        /** 500 ms between status republishes. */
        const val STATUS_PERIOD_NS = 500_000_000L

        /** `minDelay` is microseconds between samples; 0 means "on change", which a gyro never is. */
        fun sensorCeilingHz(sensor: Sensor?): Double {
            val minDelayUs = sensor?.minDelay ?: 0
            return if (minDelayUs <= 0) 0.0 else 1e6 / minDelayUs
        }
    }
}
