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
import com.lidarscan.core.calib.HoldOrientation
import com.lidarscan.core.calib.Quat
import com.lidarscan.core.capture.LiveAttitudeFeed
import kotlinx.coroutines.flow.StateFlow

/**
 * ROUND 30 item 175 — **the Android half of the attitude instrument's feed.**
 *
 * All of the deciding is [LiveAttitudeFeed] in `:core`: which of the three
 * possible streams wins, how the samples are filtered and throttled, and when a
 * listener should be held at all. What is left here is the part that cannot be
 * tested on a JVM and therefore should be as small as it is possible to make
 * it — a `SensorManager`, a `HandlerThread`, and the sensor to pick.
 *
 * **The sensor to pick** is `TYPE_GRAVITY` where the device has one: it is the
 * platform's own gyro-fused gravity estimate, smoother and faster-settling than
 * anything this app could build out of raw acceleration, and the AOSP fusion
 * provides it even on the emulator (`dumpsys sensorservice` lists an
 * `android.sensor.gravity` on top of the Goldfish accelerometer, which is why
 * the round-30 AVD screenshots settle in well under a second). Not every device
 * has one, so `TYPE_ACCELEROMETER` is a documented fallback carrying its own,
 * longer, time constant — [com.lidarscan.core.capture.LiveAttitude
 * .TAU_RAW_ACCEL_MS] — rather than an accident that inherits the wrong one.
 *
 * Nothing is registered until [acquire]; see [LiveAttitudeFeed]'s header for
 * the reference count and the priority rules, and `CaptureScreen`'s
 * `attitudeWanted` for the two placements that acquire it.
 */
class DeviceAttitudeSource(
    context: Context,
    /** Seam for tests: the millisecond clock samples are stamped with. */
    clock: () -> Long = { SystemClock.elapsedRealtime() },
) : SensorEventListener {

    private val sensorManager: SensorManager? =
        context.applicationContext.getSystemService(Context.SENSOR_SERVICE) as? SensorManager

    private val gravitySensor: Sensor? =
        sensorManager?.getDefaultSensor(Sensor.TYPE_GRAVITY)
            ?: sensorManager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)

    private val gravityIsFused: Boolean = gravitySensor?.type == Sensor.TYPE_GRAVITY

    private val feed = LiveAttitudeFeed(clock)

    private val registrationLock = Any()
    private var thread: HandlerThread? = null

    /** The live hold at 20 Hz; null means the instrument has no reading. */
    val attitude: StateFlow<HoldOrientation?> = feed.attitude

    /** True when this device can produce a reading at all. */
    val available: Boolean get() = gravitySensor != null

    init {
        feed.onSensorWantedChanged = { wanted -> if (wanted) register() else unregister() }
    }

    /** One more surface is drawing a needle. Balanced by [release]. */
    fun acquire() = feed.acquire()

    /** One fewer; at zero the listener, the thread and the reading all go. */
    fun release() = feed.release()

    /** Wired in `AppContainer` to `CaptureArController.onAttitude`. */
    fun onCameraPose(worldFromCamera: Quat, sensorOrientationDeg: Int?, tracking: Boolean) =
        feed.onCameraPose(worldFromCamera, sensorOrientationDeg, tracking)

    /** Wired in `AppContainer` to [PhoneImuRecorder]'s accelerometer. */
    fun onImuFeedStarted() = feed.onImuFeedStarted()

    /** Ditto, on its stop. */
    fun onImuFeedStopped() = feed.onImuFeedStopped()

    /** One 400 Hz accelerometer sample the densifier was handed anyway. */
    fun onImuAccel(ax: Float, ay: Float, az: Float) =
        feed.onImuAccel(ax.toDouble(), ay.toDouble(), az.toDouble())

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    override fun onSensorChanged(event: SensorEvent) {
        val values = event.values ?: return
        if (values.size < 3) return
        feed.onGravity(values[0].toDouble(), values[1].toDouble(), values[2].toDouble(), gravityIsFused)
    }

    private fun register() {
        val manager = sensorManager ?: return
        val sensor = gravitySensor ?: return
        synchronized(registrationLock) {
            if (thread != null) return
            // Its own thread rather than the main looper: 50 callbacks a second
            // on the thread Compose draws on, for a value Compose then reads at
            // 20 Hz, is the wrong way round.
            val t = HandlerThread("attitude").apply { start() }
            // SENSOR_DELAY_GAME is 20 ms — 50 Hz in, 20 Hz out after the
            // feed's throttle. SENSOR_DELAY_UI (60 ms) is *slower* than the
            // publication rate and would make the throttle a no-op with a
            // visibly coarser needle.
            val ok = manager.registerListener(this, sensor, SensorManager.SENSOR_DELAY_GAME, Handler(t.looper))
            if (!ok) {
                t.quitSafely()
                Log.w(TAG, "registerListener refused for ${sensor.name}; the instrument stays unread")
                return
            }
            thread = t
        }
    }

    private fun unregister() {
        synchronized(registrationLock) {
            val t = thread ?: return
            sensorManager?.unregisterListener(this)
            t.quitSafely()
            thread = null
        }
    }

    private companion object {
        const val TAG = "DeviceAttitudeSource"
    }
}
