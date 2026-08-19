package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import kotlinx.serialization.Serializable
import kotlin.math.abs

/**
 * ROUND 6, owner item 23 — **the one-tap mount re-zero.**
 *
 * > "allow to set a starting imu position as calibrated position for d6 since
 * > the d6 use the phone as the imu and the mounting of d6 on the phone maybe
 * > adjusted each time"
 *
 * The COIN-D6 has no IMU (ROUND 5 item 11): the phone's ARCore VIO *is* the
 * trajectory, and A8's pushbroom resolves every return as
 * `world_from_lidar = world_from_phone(t) · phone_from_lidar`. `phone_from_lidar`
 * is [BracketNominals.cadNominal] until the calibration wizard has been run —
 * and that nominal assumes a *fixed* bracket. The owner's rig is clamped on by
 * hand and comes off between scans, so the real `phone_from_lidar` differs from
 * the nominal by an unknown rotation every session. That rotation goes straight
 * into every resolved point.
 *
 * ## The measurement, and exactly what it can and cannot recover
 *
 * A phone IMU cannot see how a lidar is bolted to its back. What it CAN see is
 * the **phone's own gravity-aligned attitude**, and that is enough if the
 * operator does the one thing the affordance asks: hold the rig in its intended
 * scanning pose and tap. Then:
 *
 * ```
 *   W  world (ARCore, gravity-aligned, defined at session start)
 *   P  phone
 *   L  lidar
 *
 *   nominal, by construction:  W_R_P = q_ref  and  P_R_L = N   (the CAD nominal)
 *   actual, same physical pose: W_R_P = q_hold (what ARCore reports)
 *
 *   the LIDAR's world attitude is the same in both (that is what "same physical
 *   pose" means), so:
 *
 *       q_ref · N  =  q_hold · (P_R_L)_actual
 *   =>  (P_R_L)_actual  =  q_hold⁻¹ · q_ref · N
 *   =>  trim  =  q_hold⁻¹ · q_ref
 * ```
 *
 * With [REFERENCE_HOLD] the identity — ARCore's world frame is defined by the
 * phone's attitude at session start, so "the pose the nominal assumes" *is*
 * identity — the trim collapses to `q_hold⁻¹`: literally "zero out the attitude
 * you are holding right now", which is the owner's own words for it.
 *
 * **Rotation only.** A re-clamp changes orientation; it also changes the lever
 * arm by a few millimetres, and there is nothing in this measurement that can
 * observe that. [composedWith] therefore keeps the nominal's translation
 * verbatim rather than inventing a number — the mount-calibration wizard remains
 * the way to recover translation.
 *
 * ## Rejection
 *
 * A trim captured while the rig was moving is worse than no trim, so
 * [MountTrimSampler] refuses one: the samples must span at least
 * [MIN_SAMPLE_SPAN_MS], the steadiness of the hold must clear both
 * [MAX_SPREAD_P90_DEG] and [MAX_SPREAD_OUTLIER_DEG], and ARCore must have been
 * tracking throughout. See the companion's constants for why the gate is two
 * numbers rather than one — ROUND 8 rebuilt it after it refused a real field
 * session seven times out of seven.
 */
@Serializable
data class MountTrim(
    /** The trim rotation, `(x, y, z, w)` — the engine's and ARCore's shared order. */
    val qx: Double,
    val qy: Double,
    val qz: Double,
    val qw: Double,
    /** Which sensor's nominal this trim is composed onto. A Mid-360 trim is not a D6 trim. */
    val sensor: SensorType = SensorType.COIN_D6,
    val capturedAtEpochMillis: Long = 0L,
    /** How many pose samples the average was taken over. */
    val sampleCount: Int = 0,
    /**
     * **Worst** orientation deviation from the mean across the hold window, in
     * degrees — the outlier, not the typical case.
     *
     * Kept under its original name and meaning because it is what every field
     * log line since ROUND 6 has printed (`spread=0.47deg`) and what every
     * persisted trim on a phone already carries. ROUND 8 added
     * [spreadP90Deg] next to it rather than redefining this one: silently
     * changing what a number in a field log means is how the next report gets
     * misread.
     */
    val spreadDeg: Double = 0.0,
    /**
     * ROUND 8: the **p90** orientation deviation from the mean, in degrees —
     * how steady the hold actually was, with the top decile of ARCore's own
     * VIO jitter excluded.
     *
     * This is the number the gate judges on now ([MountTrim.MAX_SPREAD_P90_DEG]);
     * [spreadDeg] survives as the outlier ceiling's evidence. Defaulted so a
     * trim persisted by 0.4.0 still decodes (`kotlinx.serialization` fills the
     * default for an absent field) — a shape change that stopped decoding
     * would put us straight back in ROUND 7's field bug 1.
     */
    val spreadP90Deg: Double = 0.0,
    /**
     * ROUND 12 — the number that actually means "how accurate is this trim".
     *
     * The split-half repeatability of the stored mean: the hold is cut in two,
     * each half averaged separately, and this is the angle between the two
     * answers. ROUND 11 computed it, showed it in the ring, and then **threw it
     * away** — nothing about it reached the container, so the only per-capture
     * evidence of trim quality on disk was [spreadP90Deg], which measures a
     * completely different thing (the jitter of individual ARCore frames about
     * the mean, which holding longer does not reduce).
     *
     * That gap produced a real misreading. The owner's `scan-028` carries
     * `sampleCount = 244, spreadP90Deg = 2.40` and `scan-026` carries
     * `sampleCount = 34, spreadP90Deg = 0.44`, which reads as a 5x difference
     * in trim quality — and the two stored trims are **1.33 apart**, which
     * is at most 3 cm of point displacement at 1.3 m. Re-resolving each
     * capture with the OTHER one's mount extrinsic moves the map by 0.15 % of
     * its occupied voxels. The two numbers were never comparable: 0.44 is a
     * one-second dispersion and 2.40 is an eight-second one.
     *
     * Negative means "not measured" (too few samples, or a pre-0.7.1 trim).
     */
    val stabilityDeg: Double = -1.0,
    /**
     * ROUND 20 (item 79) — true when this trim was taken through the
     * swing–twist decomposition, i.e. its about-gravity yaw was DISCARDED
     * rather than baked in. False for every trim persisted by 0.9.4 and
     * earlier (`kotlinx.serialization` fills the default for the absent
     * field), which is exactly the signal [yawNormalized] keys on.
     */
    val gravityReferenced: Boolean = false,
) {
    val rotation: Quat get() = Quat(qx, qy, qz, qw).normalized()

    /**
     * ROUND 12 — the one number to judge this trim by, in degrees, or `null`
     * when it was never measured (a trim taken before 0.7.1, or from a hold too
     * short to split).
     *
     * Read [stabilityDeg] and NOT [spreadP90Deg]: the latter is a dispersion
     * over whatever window happened to be averaged, so it is not comparable
     * between two trims taken with different hold lengths. See [stabilityDeg].
     */
    val accuracyDeg: Double? get() = stabilityDeg.takeIf { it >= 0.0 }

    /**
     * True when the trim is measured AND worse than [WARN_STABILITY_DEG] — i.e.
     * the hold passed the movement gate but never converged, and the operator
     * should be told before this trim goes into a scan. An unmeasured trim is
     * not "poor", it is unknown, and is reported as such rather than as a
     * warning nobody can act on.
     */
    val accuracyIsPoor: Boolean get() = accuracyDeg?.let { it > WARN_STABILITY_DEG } == true

    /**
     * Ranking for "is this candidate better than the one already held", used by
     * the auto-refresh at Start. Lower is better. A measured stability wins over
     * an unmeasured one; between two unmeasured trims the p90 breaks the tie,
     * but only as a last resort, because p90s over different hold lengths are
     * not comparable (which is exactly the trap ROUND 12 found).
     */
    val qualityRank: Double
        get() = accuracyDeg ?: (MountTrim.UNMEASURED_RANK_BASE + spreadP90Deg)

    /** How far this trim tilts the mount away from the nominal, in degrees. 0 = the rig was held exactly nominal. */
    val magnitudeDeg: Double get() = Math.toDegrees(Quat.IDENTITY.angleTo(rotation))

    /**
     * `phone_from_lidar` for this session: the trim rotation applied to
     * [nominal]'s rotation block, [nominal]'s translation kept.
     *
     * Row-major throughout, because that is the only layout
     * `scan_engine_set_mount_extrinsics` accepts (see [Mat4]).
     */
    fun composedWith(nominal: Mat4): Mat4 {
        val trimmedRotation = Mat4.fromRotationTranslation(rotation, Vec3.ZERO) * nominal
        val m = trimmedRotation.m.copyOf()
        // Translation is the nominal's — a re-zero measures attitude, not lever arm.
        val t = nominal.translation
        m[3] = t.x
        m[7] = t.y
        m[11] = t.z
        return Mat4(m)
    }

    /**
     * ROUND 20 (item 79) — re-derives this trim with its about-gravity yaw
     * discarded. A no-op (returns `this`) for a trim that is already
     * gravity-referenced.
     *
     * The arithmetic is exact: the stored trim is `q_hold⁻¹`, so the hold it
     * was taken from is `rotation.conjugate()`, and running that hold back
     * through [fromHoldOrientation] applies the round-20 decomposition to a
     * trim persisted by an older version. Every other field is preserved —
     * the spread and stability describe the hold's steadiness, which the
     * decomposition does not change.
     */
    fun yawNormalized(): MountTrim = if (gravityReferenced) {
        this
    } else {
        fromHoldOrientation(
            hold = rotation.conjugate(),
            sensor = sensor,
            capturedAtEpochMillis = capturedAtEpochMillis,
            sampleCount = sampleCount,
            spreadDeg = spreadDeg,
            spreadP90Deg = spreadP90Deg,
            stabilityDeg = stabilityDeg,
        )
    }

    /** Age in milliseconds at [nowMillis]; a trim from a previous session is worth re-taking. */
    fun ageMillis(nowMillis: Long): Long = (nowMillis - capturedAtEpochMillis).coerceAtLeast(0L)

    /** "2 min ago" / "just now" — the capture panel shows this beside the re-zero affordance. */
    fun ageLabel(nowMillis: Long): String {
        val seconds = ageMillis(nowMillis) / 1000
        return when {
            seconds < 20 -> "just now"
            seconds < 90 -> "${seconds}s ago"
            seconds < 3600 -> "${seconds / 60} min ago"
            seconds < 86_400 -> "${seconds / 3600} h ago"
            else -> "${seconds / 86_400} d ago"
        }
    }

    companion object {
        /**
         * The attitude the CAD nominal assumes the phone is in. Identity: ARCore
         * defines its world frame from the device's attitude at session start,
         * so an operator who sets the reference at the start of the walk with
         * the rig held the way it will be carried IS holding it at identity by
         * construction. Named rather than inlined so the assumption is a thing
         * that can be pointed at (and, if a future bracket wants a different
         * hold, changed in one place).
         */
        val REFERENCE_HOLD: Quat = Quat.IDENTITY

        // ── ROUND 8, owner item 30: "set mount reference look not working" ──
        //
        // It was not "look" not working. The gate was refusing every attempt.
        // From the owner's own Pixel 8 Pro + COIN-D6 log
        // (`~/Downloads/lidarscan-capture-log (1).txt`), 0.4.0:
        //
        //   00:50:43.157 [ar] mount re-zero refused: MOVING
        //   00:51:05.346 [ar] mount re-zero refused: MOVING
        //   00:51:16.648 [ar] mount re-zero refused: MOVING
        //   00:51:23.378 [ar] mount re-zero refused: MOVING
        //   00:51:24.533 [ar] mount re-zero refused: MOVING
        //   00:51:25.133 [ar] mount re-zero refused: MOVING
        //   00:51:26.967 [ar] mount re-zero refused: MOVING
        //   00:51:28.357 [session] start: … preset=OPTIMAL …
        //   00:51:28.428 [pushbroom] extrinsic applied: … trim=none
        //
        // **Seven refusals in 44 seconds and not one success**, then the
        // operator gave up and scanned on the bare CAD nominal. 00:53:09 is an
        // eighth refusal; every capture in that session logged `trim=none`.
        //
        // The 0.3.0 session on the same rig got through, but only barely — four
        // refusals, then a run of successes at `spread=0.47deg … 0.82deg
        // samples=36-37`, then four more refusals before the next one landed.
        // That is a gate sitting *on* the noise floor, not above it: a control
        // whose success is a coin flip reads to the operator exactly like a
        // control that does nothing, which is the report we got.
        //
        // ## Why the old gate could not be passed
        //
        // It was `spreadDeg = window.maxOf { deviation from mean }` against a
        // single 1.5° limit over a 1200 ms window. At the ~31 fps those logs
        // show (37 samples / 1200 ms) that is **every one of 37 consecutive
        // ARCore frames** having to land within 1.5° of the mean. ARCore's VIO
        // attitude is not that quiet in a hand: a single frame with a bad
        // feature match, one footstep transmitted up an arm, or the small yaw
        // correction that follows a relocalisation is enough to put one sample
        // out — and one sample out is a refusal under a max. The measurement
        // itself was never the problem; the successes prove the arithmetic
        // works. The *acceptance criterion* was the problem.
        //
        // ## The gate now
        //
        // Two numbers, because "steady" and "not moving" are two different
        // questions and one statistic cannot answer both:
        //
        //  * **[MAX_SPREAD_P90_DEG] on the p90 deviation** answers "was the hold
        //    steady?", and by construction ignores the worst 10 % of frames —
        //    which is where VIO jitter lives.
        //  * **[MAX_SPREAD_OUTLIER_DEG] on the max deviation** answers "was the
        //    rig actually still?", and is what still catches a genuinely moving
        //    rig: a walk, a hand-off, a rotation mid-tap. A percentile alone
        //    would happily average a rig swinging through 8° as long as it did
        //    so smoothly, and that trim would be worse than no trim.
        //
        // Both are needed. Dropping the outlier ceiling makes the gate
        // unfalsifiable; dropping the percentile puts us back where we started.

        /**
         * Shortest hold that can be averaged.
         *
         * 700 ms, down from 800. With [WINDOW_MS] at 1000 the operator's own
         * experience is: press, hold, done — ~1.5 s including the tap latency
         * and the frame that the tap itself wobbles. Long enough that a mean
         * over ~21 frames at 30 fps is a mean; short enough that nobody has to
         * hold a pose they cannot hold.
         */
        const val MIN_SAMPLE_SPAN_MS = 700L

        /**
         * Longest window averaged. Older samples are irrelevant to "how am I
         * holding it now".
         *
         * 1000 ms, down from 1200. The window is also the *exposure* of the
         * outlier ceiling: every extra 200 ms is six more chances for one bad
         * ARCore frame to veto the whole hold, for no gain in the mean's
         * quality (the mean of 30 samples and of 37 differ far below the
         * 0.5° the successes above were already achieving).
         */
        const val WINDOW_MS = 1_000L

        /** Minimum samples in the window — a two-frame "average" is not one. */
        const val MIN_SAMPLES = 8

        /**
         * The percentile of per-sample deviation the steadiness gate judges on.
         *
         * p90 and not the median: the median would pass a hold in which a fifth
         * of the frames were badly off, which is a rig being carried. p90 lets
         * roughly three frames of a 30-frame window be outliers — the observed
         * rate of ARCore attitude glitches — and no more.
         */
        const val SPREAD_PERCENTILE = 0.90

        /**
         * The steadiness limit, on the p90 deviation from the mean.
         *
         * 2.5°, against the 0.47–0.82° *max* the 0.3.0 successes actually
         * measured. So a hold as good as the ones that already worked clears
         * this by a factor of three or more, and a hold with a handful of 2–4°
         * VIO excursions — the ones that were being refused — now passes with
         * its p90 still around 1–2°. It is deliberately not tighter: the
         * quantity being measured is a mount angle that the CAD nominal is
         * already wrong about by ~132°, so 2° of averaging noise on the
         * correction is not the error that matters.
         */
        const val MAX_SPREAD_P90_DEG = 2.5

        /**
         * The absolute ceiling, on the **worst** deviation from the mean.
         *
         * 6°. This is the falsifiable half: a rig that is genuinely moving
         * cannot get under it, because motion is not an outlier — it is a
         * trend, and a trend puts the ends of the window several degrees from
         * their own mean. At a gentle 12°/s over a 1 s window the extremes sit
         * ±6° from the mean and this refuses; at walking-turn rates it is not
         * close. Six degrees of instantaneous VIO glitch, by contrast, is a
         * single frame and does not move a 30-sample chordal mean measurably.
         */
        const val MAX_SPREAD_OUTLIER_DEG = 6.0

        /**
         * ROUND 12 — the accuracy floor, on [stabilityDeg].
         *
         * ROUND 11 measured what a trim error costs through the production
         * assembler (`engine/tests/test_round11_mount_trim.cpp`): 0.8 degrees
         * paints an overhead feature 6.6 cm apart between the two legs of an
         * out-and-back at 3 m, 1.4 degrees paints it 16.3 cm apart. So one
         * degree is where a trim stops being a rounding error and starts being
         * the largest thing wrong with the scan.
         *
         * This is a WARNING threshold and not a refusal, deliberately. The gate
         * ([MAX_SPREAD_P90_DEG]) refuses a rig that is moving; this one flags a
         * hold that was still enough to pass and still did not converge, which
         * is a thing the operator can fix by holding longer or bracing better —
         * and refusing to start a scan over it would be worse than scanning
         * with a 1.2 degree trim.
         */
        const val WARN_STABILITY_DEG = 1.0

        /**
         * Every measured stability sorts ahead of every unmeasured one. The
         * gate already caps p90 at [MAX_SPREAD_P90_DEG] and stability at
         * [WARN_STABILITY_DEG] is still acceptable, so 100 is far outside both
         * scales and the ordering can never be ambiguous.
         */
        const val UNMEASURED_RANK_BASE = 100.0

        /** ARCore's world up (+Y). The one axis the decomposition below is about. */
        val WORLD_UP = Vec3(0.0, 1.0, 0.0)

        /**
         * The trim for a rig whose phone attitude reads [hold] while it is held
         * in its scanning pose. See the class header for the derivation of the
         * `q_hold⁻¹` form; ROUND 20 (item 79) changed WHAT of the hold is kept:
         *
         * ## Observable vs assumed — the item-79 decomposition
         *
         * A static hold against gravity observes exactly two degrees of
         * freedom: the phone's **tilt** (pitch/roll vs the gravity vector,
         * which ARCore's world +Y is aligned to by construction). The third —
         * the hold's **yaw about gravity** — is not a measurement of anything:
         * it is the angle between wherever the operator happened to face and
         * wherever the ARCore session's yaw origin happened to be, and since
         * every Start rebuilds the session (round 14), a trim taken before
         * Start referenced a DEAD yaw origin. ROUND 20 measured the damage on
         * the owner's scans 054/056: two trims 3.5 minutes apart differed by
         * 23.19 deg, of which 51.6 and 21.1 deg respectively (vs the scan's
         * own frame) was pure yaw junk; yaw-normalised, the same two trims are
         * 12.5 deg apart — all of it genuine hold-tilt difference.
         *
         * So the hold is swing–twist decomposed about world +Y
         * (`hold = twist ∘ swing`), the twist (yaw) is **discarded**, and the
         * trim is `swing⁻¹`:
         *
         *  * **kept (measured)**: the swing — the phone's full attitude with
         *    its about-gravity component removed. For an upright portrait hold
         *    this contains the Rz(90 deg)-class working rotation every healthy
         *    trim has always carried (the camera sensor sits landscape in a
         *    portrait phone), plus the mount/hand tilt, which is the signal.
         *  * **discarded (unobservable)**: the twist. The mount's yaw about
         *    gravity therefore stays at the NOMINAL convention — 0-degree mark
         *    up, cap facing the walk — which is the documented rig convention
         *    (owner-confirmed, round 9) and the only honest default.
         *
         * The invariant a test can hold: a gravity-referenced trim's
         * quaternion has **zero y-component** (the swing of a Y-decomposition
         * has none), and re-normalising it is a no-op.
         */
        fun fromHoldOrientation(
            hold: Quat,
            sensor: SensorType = SensorType.COIN_D6,
            capturedAtEpochMillis: Long = 0L,
            sampleCount: Int = 0,
            spreadDeg: Double = 0.0,
            spreadP90Deg: Double = 0.0,
            stabilityDeg: Double = -1.0,
        ): MountTrim {
            val swing = hold.normalized().swingAbout(WORLD_UP)
            val q = (swing.conjugate() * REFERENCE_HOLD).normalized()
            return MountTrim(
                qx = q.x, qy = q.y, qz = q.z, qw = q.w,
                sensor = sensor,
                capturedAtEpochMillis = capturedAtEpochMillis,
                sampleCount = sampleCount,
                spreadDeg = spreadDeg,
                spreadP90Deg = spreadP90Deg,
                stabilityDeg = stabilityDeg,
                gravityReferenced = true,
            )
        }
    }
}

/**
 * ROUND 7, field bug 1 — **a trim that survives the screen it was taken on.**
 *
 * The owner's own capture log:
 *
 * ```
 * 22:53:04 [ar]        mount re-zero captured: magnitude=132.44deg spread=0.47deg
 * 22:53:09 [pushbroom] extrinsic applied: source=nominal trim=132.81deg
 * 22:54:06 [pushbroom] extrinsic applied: source=nominal trim=none      ← 57 s later
 * ```
 *
 * Three good re-zeros, one 216 k-point scan on the trim, and then the very next
 * capture ran on the **bare CAD nominal** — 132° of unmodelled mount rotation
 * straight into every resolved point, which is a scan of walls that cannot be
 * straight in any frame. Nothing had gone wrong with the measurement: the trim
 * lived in one `MutableStateFlow` inside `CaptureViewModel`, which is
 * `viewModel(key = "capture-new-false")` on the Capture tab's own
 * `NavBackStackEntry`. Walking to Projects to look at the scan you just took
 * and coming back is enough to clear it, with no message anywhere.
 *
 * The owner's expectation is the physical one: **re-zero when the mount
 * shifts**, not before every capture. So the trim is persisted, and the two
 * facts that decide how much to trust a restored one travel with it: when it was
 * taken ([MountTrim.capturedAtEpochMillis]) and which app run took it
 * ([appRunId]). A trim from THIS run is the one the operator set minutes ago; a
 * trim from a previous run may predate the phone being put in a bag, so it is
 * still applied — losing it silently is what this fixes — but the panel says so.
 */
@Serializable
data class StoredMountTrim(
    val trim: MountTrim,
    /**
     * The app process run that captured this trim. Compared against the current
     * run's id (a fresh random string per `AppContainer`) to tell "set a moment
     * ago, on this screen" from "restored across an app restart".
     */
    val appRunId: String = "",
)

/** What the capture panel says about the trim in force, and whether it is a caution. */
data class MountTrimProvenance(
    val trim: MountTrim?,
    /** True when the trim was captured in an earlier app run — the app has restarted since. */
    val fromPreviousRun: Boolean,
    /** True when the trim is older than [MountTrimProvenances.STALE_AFTER_MILLIS]. */
    val stale: Boolean,
    /** How old the trim was when this was described, in milliseconds. */
    val ageMillis: Long,
    /** One sentence for the panel, verbatim. */
    val label: String,
    /** True when the sentence is a caution rather than a confirmation. */
    val warn: Boolean,
) {
    /**
     * ROUND 8, owner item 30c — **the state of the mount, at a glance, with no
     * sheet open.**
     *
     * `"MOUNT SET · 132.8° · 2 min ago"` / `"NO MOUNT REF · CAD NOMINAL"`.
     *
     * [label] is a whole sentence and lives in the mount sheet where there is
     * room to read it; this is the same fact compressed onto one always-visible
     * chip on the capture panel. The owner's ROUND 7 session ran five captures
     * on `trim=none` without ever being told — the panel said "Set mount
     * reference" and nothing else, which is a *button*, not a *state*. A chip
     * that says NO MOUNT REF cannot be mistaken for one.
     *
     * The age re-computes with the rest of this object on
     * `CaptureViewModel`'s 15 s tick, so "just now" becomes "2 min ago" on its
     * own — which is the point of showing an age at all.
     */
    val chipLabel: String
        get() = if (trim == null) {
            "NO MOUNT REF · CAD NOMINAL"
        } else {
            "MOUNT SET · %.1f° · %s".format(trim.magnitudeDeg, ageLabel)
        }

    /** Just the age, e.g. `"2 min ago"` — the half of [chipLabel] that has to re-tick. */
    val ageLabel: String
        get() = trim?.let { it.ageLabel(it.capturedAtEpochMillis + ageMillis) }.orEmpty()
    /** What `applyMountExtrinsic` writes into the capture log, so the next field report arrives with provenance. */
    val logSuffix: String
        get() = if (trim == null) {
            "trim=none"
        } else {
            "trim=%.2fdeg trimAgeMs=%d trimSource=%s".format(
                trim.magnitudeDeg,
                ageMillis,
                if (fromPreviousRun) "restored-previous-run" else "this-run",
            )
        }
}

object MountTrimProvenances {

    /**
     * Past this age a restored trim gets a caution rather than a confirmation.
     * Twelve hours is "you have not scanned since yesterday" — long enough that
     * a mount clamped by hand has plausibly been off the phone and back on,
     * short enough that a morning of scanning never nags.
     */
    const val STALE_AFTER_MILLIS = 12L * 60L * 60L * 1000L

    fun describe(stored: StoredMountTrim?, currentAppRunId: String, nowMillis: Long): MountTrimProvenance {
        val trim = stored?.trim
            ?: return MountTrimProvenance(
                trim = null,
                fromPreviousRun = false,
                stale = false,
                ageMillis = 0L,
                label = "No mount reference — the pushbroom is running on the bracket's CAD nominal. " +
                    "Hold the rig the way you will carry it and tap Set mount reference.",
                warn = false,
            )
        val fromPreviousRun = stored.appRunId != currentAppRunId
        val ageMillis = trim.ageMillis(nowMillis)
        val stale = ageMillis >= STALE_AFTER_MILLIS
        val age = trim.ageLabel(nowMillis)
        val magnitude = "%.1f°".format(trim.magnitudeDeg)
        val label = when {
            stale ->
                "Mount trim $magnitude · set $age — that is old. Re-zero if the D6 has been off the phone since."
            fromPreviousRun ->
                "Mount trim $magnitude · set $age, restored from your last session. " +
                    "Re-zero only if the mount has shifted."
            else -> "Mount trim $magnitude · set $age · travels with the project"
        }
        return MountTrimProvenance(
            trim = trim,
            fromPreviousRun = fromPreviousRun,
            stale = stale,
            ageMillis = ageMillis,
            label = label,
            warn = stale,
        )
    }
}

/** Why a re-zero attempt did not produce a trim. Each case is a sentence the panel can show verbatim. */
enum class MountTrimRejection(val message: String) {
    NO_POSES("No phone tracking yet — point the camera at something with detail and try again."),
    NOT_ENOUGH_SAMPLES("Hold the rig still for about a second, then tap again."),
    MOVING("The rig moved while measuring — hold it steady in the scanning pose and tap again."),
    NOT_TRACKING("Tracking was lost mid-measurement — wait for TRACKING, then tap again."),
}

/**
 * ROUND 8, owner item 30b — **what the gate actually measured.**
 *
 * The entire on-device record of eight consecutive failed re-zeros was:
 *
 * ```
 * 00:50:43.157 [ar] mount re-zero refused: MOVING
 * ```
 *
 * — eight times, byte-identical apart from the timestamp. A name, and nothing
 * else. There is no way to tell from that whether the operator was 0.2° or 20°
 * off the limit, whether the samples were even arriving, or whether the gate
 * was broken; all three were live hypotheses when this round started, and
 * answering the question needed a second field session that should not have
 * been necessary. Every refusal now carries its own numbers into both the log
 * and the panel, so the next report arrives already diagnosed.
 */
data class MountTrimMeasurement(
    /** p90 of the per-sample deviation from the mean, degrees. */
    val spreadP90Deg: Double,
    /** Worst per-sample deviation from the mean, degrees. */
    val spreadMaxDeg: Double,
    /** The limit [spreadP90Deg] was judged against — [MountTrim.MAX_SPREAD_P90_DEG]. */
    val limitDeg: Double,
    /** The limit [spreadMaxDeg] was judged against — [MountTrim.MAX_SPREAD_OUTLIER_DEG]. */
    val outlierLimitDeg: Double,
    /** Samples inside the window. */
    val samples: Int,
    /** Wall time the window spans, milliseconds. */
    val spanMs: Long,
) {
    /**
     * The suffix the capture log carries, e.g.
     * `p90=2.9deg max=5.1deg limit=2.5deg samples=31 spanMs=980`.
     *
     * Deliberately the same shape as [MountTrimProvenance.logSuffix]: one
     * `key=value` run, greppable, and readable in a screenshot of a log a
     * thousand miles away.
     */
    val logSuffix: String
        get() = "p90=%.2fdeg max=%.2fdeg limit=%.2fdeg outlierLimit=%.2fdeg samples=%d spanMs=%d".format(
            spreadP90Deg,
            spreadMaxDeg,
            limitDeg,
            outlierLimitDeg,
            samples,
            spanMs,
        )
}

sealed interface MountTrimResult {
    data class Captured(val trim: MountTrim) : MountTrimResult

    /**
     * A refusal, with the measurement behind it when there was one to make.
     *
     * [measurement] is null only for [MountTrimRejection.NO_POSES] — no samples
     * means nothing was measured, and inventing zeros for it would be a lie
     * that reads as "perfectly still".
     */
    data class Rejected(
        val reason: MountTrimRejection,
        val measurement: MountTrimMeasurement? = null,
    ) : MountTrimResult {

        /**
         * What the capture panel shows at the moment of the tap: the refusal,
         * the numbers, and **what to do about it**.
         *
         * The last clause is the part that was missing. "The rig moved while
         * measuring" tells an operator who believes they were holding still
         * nothing they can act on; "steadiness 2.9°, limit 2.5° — brace your
         * elbows and hold for about a second" does.
         */
        val sentence: String
            get() {
                val m = measurement ?: return reason.message
                return when (reason) {
                    MountTrimRejection.NO_POSES -> reason.message
                    MountTrimRejection.NOT_TRACKING -> reason.message
                    MountTrimRejection.NOT_ENOUGH_SAMPLES ->
                        "Not enough of a hold yet — ${m.samples} tracked frames over ${m.spanMs} ms. " +
                            "Keep the rig still for about a second (${MountTrim.MIN_SAMPLE_SPAN_MS} ms " +
                            "of tracking is enough), then tap again."

                    MountTrimRejection.MOVING -> (
                        "Too much movement to trust — steadiness %.1f° against a %.1f° limit, " +
                            "worst frame %.1f° against %.1f°, over %d frames in %d ms. " +
                            "Brace the phone against your body, hold still ~1 s, and tap again."
                        ).format(
                        m.spreadP90Deg,
                        m.limitDeg,
                        m.spreadMaxDeg,
                        m.outlierLimitDeg,
                        m.samples,
                        m.spanMs,
                    )
                }
            }
    }
}

/**
 * Averages a short window of pose orientations into a [MountTrim], or refuses.
 *
 * Pure `:core`, no ARCore types: `:app`'s controller feeds it
 * [com.lidarscan.core.capture.PoseSample]s straight off the pose stream, and
 * this is therefore unit-testable on a bare JVM — which matters, because the
 * arithmetic below is the whole of item 23 and no ARCore device exists here to
 * check it on.
 *
 * The average is a **chordal mean** (component-wise sum with sign alignment,
 * then normalise). For a set of orientations within a degree or two of each
 * other — which the rejection gates guarantee — it is indistinguishable from
 * the Riemannian mean and does not need an iteration that could fail to
 * converge on a phone.
 */
object MountTrimSampler {

    fun capture(
        samples: List<com.lidarscan.core.capture.PoseSample>,
        nowMillis: Long,
        sensor: SensorType = SensorType.COIN_D6,
        windowMs: Long = MountTrim.WINDOW_MS,
    ): MountTrimResult {
        if (samples.isEmpty()) return MountTrimResult.Rejected(MountTrimRejection.NO_POSES)
        val newest = samples.maxOf { it.tMonoNs }
        val window = samples.filter { it.tMonoNs >= newest - windowMs * 1_000_000L }
        val spanMs = if (window.isEmpty()) {
            0L
        } else {
            (window.maxOf { it.tMonoNs } - window.minOf { it.tMonoNs }) / 1_000_000L
        }

        // ROUND 8 (item 30b): the measurement is built even for the refusals
        // that do not need a spread, so every rejected path can report SOMETHING
        // rather than only a name. The two spread figures are meaningless before
        // the window is big enough to have a mean, hence the `takeIf` below.
        fun measure(p90: Double, max: Double) = MountTrimMeasurement(
            spreadP90Deg = p90,
            spreadMaxDeg = max,
            limitDeg = MountTrim.MAX_SPREAD_P90_DEG,
            outlierLimitDeg = MountTrim.MAX_SPREAD_OUTLIER_DEG,
            samples = window.size,
            spanMs = spanMs,
        )

        if (window.size < MountTrim.MIN_SAMPLES) {
            return MountTrimResult.Rejected(MountTrimRejection.NOT_ENOUGH_SAMPLES, measure(0.0, 0.0))
        }
        if (window.any { !it.tracking }) {
            return MountTrimResult.Rejected(MountTrimRejection.NOT_TRACKING, measure(0.0, 0.0))
        }
        if (spanMs < MountTrim.MIN_SAMPLE_SPAN_MS) {
            return MountTrimResult.Rejected(MountTrimRejection.NOT_ENOUGH_SAMPLES, measure(0.0, 0.0))
        }

        val mean = meanOrientation(window.map { it.orientation })
        val deviations = window.map { Math.toDegrees(mean.angleTo(it.orientation)) }
        val spreadMaxDeg = deviations.max()
        val spreadP90Deg = percentile(deviations, MountTrim.SPREAD_PERCENTILE)

        // ROUND 8: TWO gates, not one. See MountTrim's companion for the
        // 7-refusals-in-44-seconds field log that made a single max-based
        // threshold indefensible. The p90 is the steadiness question; the max is
        // the still-vs-moving question, and it is the one that keeps this
        // falsifiable — a rig being carried cannot get under it.
        if (spreadP90Deg > MountTrim.MAX_SPREAD_P90_DEG || spreadMaxDeg > MountTrim.MAX_SPREAD_OUTLIER_DEG) {
            return MountTrimResult.Rejected(
                MountTrimRejection.MOVING,
                measure(spreadP90Deg, spreadMaxDeg),
            )
        }

        return MountTrimResult.Captured(
            MountTrim.fromHoldOrientation(
                hold = mean,
                sensor = sensor,
                capturedAtEpochMillis = nowMillis,
                sampleCount = window.size,
                spreadDeg = spreadMaxDeg,
                spreadP90Deg = spreadP90Deg,
            ),
        )
    }

    /**
     * The [p]-th percentile of [values], nearest-rank.
     *
     * Nearest-rank rather than an interpolating definition on purpose: with
     * 20–40 samples the interpolated and ranked answers differ by less than the
     * jitter being measured, and a rank is a value that genuinely occurred —
     * which matters when the number ends up in a log line an operator reads
     * next to "worst frame".
     */
    fun percentile(values: List<Double>, p: Double): Double {
        require(values.isNotEmpty()) { "no values to take a percentile of" }
        val sorted = values.sorted()
        val rank = Math.ceil(p * sorted.size).toInt().coerceIn(1, sorted.size)
        return sorted[rank - 1]
    }

    /**
     * Chordal mean of unit quaternions. Signs are aligned against the first
     * sample before summing — `q` and `-q` are the same rotation and ARCore
     * hands out either, so an unaligned sum of a still rig can cancel to zero
     * (the same double-cover hazard A8 §3.4 documents for SLERP).
     */
    fun meanOrientation(orientations: List<Quat>): Quat {
        require(orientations.isNotEmpty()) { "no orientations to average" }
        val first = orientations.first().normalized()
        var x = 0.0
        var y = 0.0
        var z = 0.0
        var w = 0.0
        for (raw in orientations) {
            val q = raw.normalized()
            val dot = q.x * first.x + q.y * first.y + q.z * first.z + q.w * first.w
            val s = if (dot < 0.0) -1.0 else 1.0
            x += s * q.x; y += s * q.y; z += s * q.z; w += s * q.w
        }
        val n = Quat(x, y, z, w)
        return if (abs(n.norm) < 1e-12) first else n.normalized()
    }
}
