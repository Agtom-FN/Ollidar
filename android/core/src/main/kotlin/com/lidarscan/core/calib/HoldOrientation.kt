package com.lidarscan.core.calib

import com.lidarscan.core.capture.CameraFromImu
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.abs
import kotlin.math.sin
import java.util.Locale

/**
 * ROUND 26 item 125(b) — **which way up the phone was when the scan started,
 * read from gravity rather than from the rotation setting.**
 *
 * ### Why gravity and not `Display.rotation`
 *
 * `Display.rotation` describes how Android decided to *draw*, not how the
 * operator is *holding*. An operator with auto-rotate switched off — which is
 * a normal thing for someone walking a building with a rig strapped to a
 * phone — holds the phone in landscape and the display still reports
 * `ROTATION_0`. Every layout decision the app makes from `LocalConfiguration`
 * is then wrong, and, far worse, anything that inferred the mount from it
 * would be wrong too. Gravity does not have an accessibility setting.
 *
 * ### The chain, in full, because each hop is a frame change
 *
 * 1. `hold` is the mean ARCore camera attitude over the hold-still stage —
 *    `q_world_from_camera`, from `Frame.getCamera().getPose()` and **not**
 *    `getDisplayOrientedPose()` (see `CaptureArController.publishPose`). ARCore's
 *    world is gravity-aligned with `+Y` up.
 * 2. World-up expressed in the CAMERA frame is `hold⁻¹ · (0, 1, 0)`.
 * 3. The camera frame is not the device frame. They share `+Z` for a rear
 *    camera and differ by a rotation of `SENSOR_ORIENTATION` about it — the
 *    derivation is written out at [CameraFromImu], which round 9 already
 *    needed for the IMU densifier, so this file reuses it rather than
 *    restating a 90° constant that is only *usually* 90.
 * 4. World-up in the DEVICE frame (`+X` right along the screen, `+Y` up along
 *    the screen, both defined against the device's NATURAL orientation and
 *    never remapped) is then read as an angle in the screen plane, and the
 *    quadrant of that angle is the answer.
 *
 * ### What this does NOT change
 *
 * **The mount reference itself needs no per-orientation branch, and that is a
 * finding rather than an omission.** Round 20's trim is
 * `swing(hold, gravity)⁻¹` — it cancels *everything except the operator's
 * yaw*, and a landscape hold's 90° roll is part of "everything else". So the
 * extrinsic already comes out correct in all four quadrants and the map is
 * already level either way; `HoldOrientationTest` proves it for synthetic
 * gravity in each quadrant rather than leaving it as a claim. What was
 * genuinely missing was that nothing ever *decoded* the orientation the trim
 * had silently absorbed, so nothing could log it, show it, or lock it.
 */
enum class DeviceOrientation(
    /** The token written into the capture log — `[ar] start orientation: landscape-left`. */
    val logName: String,
    /** Six words or fewer; this reaches the operator. */
    val label: String,
) {
    /** The device's natural orientation: the screen's own "up" points at the sky. */
    PORTRAIT("portrait", "Portrait"),

    /**
     * The device turned a quarter-turn to the LEFT from portrait, so its
     * natural RIGHT edge (`+X`) is now uppermost. This is Android's
     * `Surface.ROTATION_90`, and "landscape-left" is the name it goes by in
     * every camera app, so it is the name used here.
     */
    LANDSCAPE_LEFT("landscape-left", "Landscape"),

    /** Upside down: `Surface.ROTATION_180`. Rare on a rig, but not impossible. */
    PORTRAIT_REVERSE("portrait-upside-down", "Portrait, upside down"),

    /** A quarter-turn to the RIGHT: `Surface.ROTATION_270`. */
    LANDSCAPE_RIGHT("landscape-right", "Landscape");

    /** True for the two landscape quadrants — the layout question, asked once. */
    val isLandscape: Boolean get() = this == LANDSCAPE_LEFT || this == LANDSCAPE_RIGHT
}

/**
 * A classified hold. [confident] is false when the phone is lying too flat for
 * the screen-plane angle to mean anything, in which case [orientation] is the
 * conservative [DeviceOrientation.PORTRAIT] and the caller must say so rather
 * than report a guess as a measurement — the same rule
 * [com.lidarscan.core.capture.CameraFromImuExtrinsics] applies to an
 * underivable extrinsic.
 */
data class HoldOrientation(
    val orientation: DeviceOrientation,
    /**
     * Where world-up sits in the screen plane, degrees, `(-180, 180]`,
     * measured from the screen's own "up" and positive towards the screen's
     * right. 0° is portrait, +90° is landscape-left.
     */
    val screenUpAngleDeg: Double,
    /**
     * ROUND 33 item 179(a) — **the other angle of the same vector.**
     *
     * How far the screen has fallen back from vertical, degrees, `[-90, +90]`:
     * the elevation of world-up out of the screen plane, `atan2(z, hypot(x, y))`
     * of the very vector [screenUpAngleDeg] is the in-plane bearing of. Zero is
     * a phone held upright; **positive is leaning BACK** — the top edge away
     * from the operator, the screen tipped towards the sky and the rear camera
     * therefore aimed DOWN; negative is leaning forward. ±90° is flat on a
     * table, face up and face down respectively.
     *
     * Two properties of that definition are the reason it is this and not a
     * device roll about `+X`:
     *
     *  * it is **invariant under the hold quadrant**, because the screen normal
     *    is the one axis a roll about `+Z` does not move — so a landscape hold's
     *    pitch is the same physical lean as a portrait hold's, with no branch,
     *    exactly as [screenUpAngleDeg]'s deviation-from-square is;
     *  * together with [screenUpAngleDeg] it is a **complete** description of
     *    the up vector — `hypot(x, y) = cos(pitch)` — so the pair adds no third
     *    degree of freedom and can never disagree with [tiltFromFlatDeg], which
     *    is its unsigned complement: `tiltFromFlat = 90 − |pitch|`.
     */
    val screenPitchDeg: Double,
    /** How far off flat the phone is, degrees: 90° = perfectly upright, 0° = face up or down. */
    val tiltFromFlatDeg: Double,
    val confident: Boolean,
) {
    /** The capture-log line's payload, e.g. `landscape-left (roll +88.6°, tilt 74.2°)`. */
    fun logSuffix(): String = buildString {
        append(orientation.logName)
        append(" (roll ")
        append(if (screenUpAngleDeg >= 0) "+" else "")
        append(String.format(Locale.US, "%.1f", screenUpAngleDeg))
        append("°, tilt ")
        append(String.format(Locale.US, "%.1f", tiltFromFlatDeg))
        append("°)")
        if (!confident) append(" — too flat to be sure, assuming portrait")
    }
}

object StartOrientation {

    /** ARCore's world up. The same constant [MountTrim] decomposes the swing about. */
    val WORLD_UP = Vec3(0.0, 1.0, 0.0)

    /**
     * How far off flat the phone has to be before the screen-plane angle is
     * worth believing. At 20° the horizontal component of gravity is
     * `sin 20° = 0.34` of a g, which is an order of magnitude above the noise
     * of a mean taken over a one-second hold; below it, a few degrees of hand
     * tremble can swing the reported quadrant right round, and a scan that
     * silently locked to the wrong orientation would be worse than one that
     * admitted it did not know.
     *
     * A capture with the phone this flat is a ceiling or floor scan, where
     * "which way up" genuinely has no answer.
     */
    const val MIN_TILT_FROM_FLAT_DEG: Double = 20.0

    /** The quadrant boundary: 45° either side of each cardinal direction. */
    private const val QUADRANT_HALF_WIDTH_DEG: Double = 45.0

    /**
     * World-up expressed in the DEVICE (Android sensor) frame, given the mean
     * hold attitude and the rear camera's `SENSOR_ORIENTATION`.
     *
     * [sensorOrientationDeg] null means the characteristics probe failed or the
     * camera is front-facing; 90 is then assumed, because it is what essentially
     * every phone rear camera reports — but [classify] marks that case, so the
     * assumption is never laundered into a measurement.
     */
    fun worldUpInDevice(hold: Quat, sensorOrientationDeg: Int?): Vec3 {
        val upInCamera = hold.normalized().conjugate().rotate(WORLD_UP)
        val cameraFromDevice = CameraFromImu.rearCamera(sensorOrientationDeg ?: 90)
        return cameraFromDevice.conjugate().rotate(upInCamera)
    }

    /**
     * The quadrant of a world-up vector already expressed in the device frame.
     * Split out from [classify] so the four-quadrant tests can be written as
     * literal gravity vectors — "the sky is towards the screen's right edge" —
     * with no quaternion algebra between the statement and the assertion.
     */
    fun fromDeviceUp(up: Vec3): HoldOrientation {
        val horizontal = hypot(up.x, up.y)
        val tiltDeg = Math.toDegrees(atan2(horizontal, abs(up.z)))
        val confident = horizontal >= sin(Math.toRadians(MIN_TILT_FROM_FLAT_DEG))
        // atan2(x, y): 0 when up is the screen's own up, +90 when it is the
        // screen's right edge. Positive towards the right is the same sense
        // Surface.ROTATION_90 turns in, which is what makes the table below
        // read the same way as Android's own constants.
        val angleDeg = Math.toDegrees(atan2(up.x, up.y))
        // ROUND 33 item 179(a): the elevation of the SAME vector out of the
        // screen plane. atan2 against the in-plane length rather than asin
        // against the magnitude, so it needs no unit vector and no clamp — the
        // callers that pass a raw 9.81 m/s² gravity sample get the same answer
        // as the ones that pass a normalised one.
        val pitchDeg = Math.toDegrees(atan2(up.z, horizontal))
        val orientation = when {
            !confident -> DeviceOrientation.PORTRAIT
            angleDeg >= -QUADRANT_HALF_WIDTH_DEG && angleDeg < QUADRANT_HALF_WIDTH_DEG ->
                DeviceOrientation.PORTRAIT
            angleDeg >= QUADRANT_HALF_WIDTH_DEG && angleDeg < 180.0 - QUADRANT_HALF_WIDTH_DEG ->
                DeviceOrientation.LANDSCAPE_LEFT
            angleDeg >= -(180.0 - QUADRANT_HALF_WIDTH_DEG) && angleDeg < -QUADRANT_HALF_WIDTH_DEG ->
                DeviceOrientation.LANDSCAPE_RIGHT
            else -> DeviceOrientation.PORTRAIT_REVERSE
        }
        return HoldOrientation(orientation, angleDeg, pitchDeg, tiltDeg, confident)
    }

    /** [fromDeviceUp] of [worldUpInDevice] — the whole chain, one call. */
    fun classify(hold: Quat, sensorOrientationDeg: Int?): HoldOrientation =
        fromDeviceUp(worldUpInDevice(hold, sensorOrientationDeg))

    /**
     * The same answer from a trim rather than from a raw hold, for the one
     * caller that has already thrown the hold away.
     *
     * [MountTrim.fromHoldOrientation] stores `swing(hold)⁻¹`, so the swing is
     * recoverable by conjugating it back. The yaw the swing dropped is exactly
     * the rotation about gravity, which cannot change where gravity sits in the
     * screen plane — so classifying the swing gives the same quadrant as
     * classifying the hold it came from, and [HoldOrientationTest] pins that
     * equivalence rather than trusting the argument.
     */
    fun fromTrim(trim: MountTrim, sensorOrientationDeg: Int?): HoldOrientation =
        classify(trim.rotation.conjugate(), sensorOrientationDeg)

    /**
     * A synthetic hold in a named quadrant, for tests and for the replay path:
     * the phone upright ([tiltFromFlatDeg] = 90) and rolled into [orientation],
     * facing [yawDeg] around gravity.
     *
     * It lives in main rather than in the test source set because it is the
     * executable form of the frame convention this file's KDoc describes in
     * prose, and a convention that only a test knows is a convention that drifts.
     */
    fun syntheticHold(
        orientation: DeviceOrientation,
        sensorOrientationDeg: Int = 90,
        yawDeg: Double = 0.0,
        tiltFromFlatDeg: Double = 90.0,
    ): Quat {
        // Build the DEVICE attitude first — it is the frame the statement is in
        // — then rotate it into the camera frame, which is the frame ARCore
        // reports in.
        val rollRad = Math.toRadians(
            when (orientation) {
                DeviceOrientation.PORTRAIT -> 0.0
                DeviceOrientation.LANDSCAPE_LEFT -> 90.0
                DeviceOrientation.PORTRAIT_REVERSE -> 180.0
                DeviceOrientation.LANDSCAPE_RIGHT -> -90.0
            },
        )
        // Start from "device upright, screen facing the operator, camera looking
        // along world -Z": device +Y = world +Y, device +Z = world +Z.
        val pitchRad = Math.toRadians(90.0 - tiltFromFlatDeg)
        val yaw = Quat.fromAxisAngle(WORLD_UP, Math.toRadians(yawDeg))
        val pitch = Quat.fromAxisAngle(Vec3(1.0, 0.0, 0.0), pitchRad)
        // Roll is about the device's own +Z (out of the screen, towards the
        // operator's face). A POSITIVE rotation about +Z carries device +X —
        // the natural RIGHT edge — towards device +Y, i.e. upwards; so
        // +90° is the quarter-turn to the LEFT that puts the right edge at the
        // top, which is Surface.ROTATION_90.
        val roll = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), rollRad)
        val worldFromDevice = (yaw * pitch * roll).normalized()
        return (worldFromDevice * CameraFromImu.rearCamera(sensorOrientationDeg).conjugate())
            .normalized()
    }
}
