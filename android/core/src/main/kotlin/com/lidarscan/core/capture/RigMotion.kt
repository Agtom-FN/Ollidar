package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import kotlin.math.abs

/** One pose sample as it arrives from ARCore, in the engine's clock domain. */
data class PoseSample(
    val tMonoNs: Long,
    val position: Vec3,
    val orientation: Quat,
    val tracking: Boolean,
)

/** The rig's motion at an instant — the two numbers `frames.idx` records per keyframe. */
data class RigMotionEstimate(
    val angularRateRadPerS: Float,
    val linearSpeedMPerS: Float,
    val valid: Boolean,
)

/**
 * Angular rate and linear speed by **finite differences over the ARCore pose
 * stream**.
 *
 * Why finite differences and not the IMU: A11 records these two numbers per
 * keyframe precisely so that "colorization works from a `.lscan` alone, on a
 * desktop, with no IMU stream to re-integrate" (A11 §3.3 item 4), and the
 * phone's own IMU is not a stream this app pushes into the engine at all —
 * ARCore consumes it internally and hands back poses. So the pose stream *is*
 * the motion source here. A caller who later has a real IMU stream can pass
 * an `AngularRateFn` to the colorizer and that wins, by A11's own rule.
 *
 * The estimate is a **centred difference over a short window** (default
 * ±100 ms, i.e. ~3 ARCore frames either side at 30 Hz): a single 33 ms
 * frame-to-frame difference is dominated by ARCore's own per-frame pose
 * jitter, and a window much longer than 200 ms smooths through exactly the
 * turn the motion gate is trying to detect. A8 §3.7 measured the relevant
 * scale from the other direction: a 1.3 Hz vertical bob puts 185 µm of lerp
 * error across one 33 ms ARCore interval.
 *
 * Not thread-safe; B8's recorder feeds it from one thread.
 */
class RigMotionTracker(
    private val windowNs: Long = 100_000_000L,
    private val capacity: Int = 64,
) {
    private val samples = ArrayDeque<PoseSample>()

    fun add(sample: PoseSample) {
        // Out-of-order poses are dropped rather than sorted in: the engine
        // rejects them too (`scan_engine_push_pose` returns
        // SCAN_ERR_INVALID_ARGUMENT "rather than silently corrupting every
        // interpolation that follows"), and a motion estimate built from a
        // stream the engine refused would disagree with the recorded poses.
        val last = samples.lastOrNull()
        if (last != null && sample.tMonoNs <= last.tMonoNs) return
        samples.addLast(sample)
        while (samples.size > capacity) samples.removeFirst()
    }

    fun latest(): PoseSample? = samples.lastOrNull()

    fun size(): Int = samples.size

    fun clear() = samples.clear()

    /**
     * Motion at [tMonoNs], from the samples bracketing it within the window.
     * Returns `valid = false` (and zeros) when there is not enough of a
     * window — B8 then clears `kKeyframeFlagMotionValid` rather than writing a
     * made-up rate, because a colorizer reading a zero rate would treat the
     * frame as ideally still.
     */
    fun estimateAt(tMonoNs: Long): RigMotionEstimate {
        val before = samples.lastOrNull { it.tMonoNs <= tMonoNs + windowNs && it.tMonoNs >= tMonoNs - windowNs && it.tMonoNs <= tMonoNs }
            ?: samples.firstOrNull { it.tMonoNs >= tMonoNs - windowNs }
            ?: return RigMotionEstimate(0f, 0f, false)
        val after = samples.lastOrNull { it.tMonoNs <= tMonoNs + windowNs }
            ?: return RigMotionEstimate(0f, 0f, false)

        val a = if (before.tMonoNs <= after.tMonoNs) before else after
        val b = if (before.tMonoNs <= after.tMonoNs) after else before
        val dtNs = b.tMonoNs - a.tMonoNs
        if (dtNs <= 0L) return RigMotionEstimate(0f, 0f, false)
        val dt = dtNs / 1e9

        val dTheta = a.orientation.angleTo(b.orientation)
        val dPos = (b.position - a.position).norm
        return RigMotionEstimate(
            angularRateRadPerS = (dTheta / dt).toFloat(),
            linearSpeedMPerS = (dPos / dt).toFloat(),
            valid = a.tracking && b.tracking,
        )
    }
}

/**
 * B8's keyframe cadence + motion gate.
 *
 * Tech Spec §3.5 asks for "keyframes JPEG-recorded at 2–5 fps"; S6's error
 * budget (WIZARD.md §4) says the dominant term by far is **time sync x turn
 * rate — 16.7 px of a 20.2 px budget** — and that the mitigation is to
 * "gate turning at <= 15 °/s", i.e. **prefer keyframes taken while the rig is
 * turning slowly**. A11 §7.3 measured the resulting gate.
 *
 * So the selector is not a metronome. Within each cadence slot it:
 *
 *  1. refuses any frame while ARCore is not tracking (a keyframe with a
 *     flagged pose is worse than no keyframe — the colorizer would have to
 *     discard it anyway),
 *  2. refuses any frame over [maxAngularRateRadPerS] outright,
 *  3. among the frames that pass, takes the SLOWEST one in the slot rather
 *     than the first — which is what "prefer slow frames" means when the
 *     cadence has already decided a frame is due.
 *
 * A slot in which nothing qualifies produces no keyframe, and the deadline
 * carries over. The alternative (forcing one anyway) would put exactly the
 * frames S6 identifies as the budget's biggest term into the index.
 */
class KeyframeSelector(
    private val targetFps: Double = 3.0,
    private val maxAngularRateRadPerS: Double = Math.toRadians(15.0),
    private val maxLinearSpeedMPerS: Double = 1.5,
    /** How long to keep looking for a slower frame once one already qualifies, as a fraction of the slot. */
    private val slotHoldFraction: Double = 0.5,
) {
    private val slotNs: Long = (1e9 / targetFps).toLong()
    private var nextDueNs: Long = Long.MIN_VALUE
    private var bestInSlot: Candidate? = null

    data class Candidate(val tMonoNs: Long, val angularRateRadPerS: Double, val linearSpeedMPerS: Double)

    /**
     * Offer one ARCore frame. Returns the frame to record, or null.
     *
     * The two-phase shape (accumulate, then emit) is what lets "slowest in the
     * slot" work at all: the decision cannot be made on the first qualifying
     * frame without seeing the rest of the slot.
     */
    fun offer(
        tMonoNs: Long,
        tracking: Boolean,
        angularRateRadPerS: Double,
        linearSpeedMPerS: Double,
        motionValid: Boolean,
    ): Candidate? {
        if (nextDueNs == Long.MIN_VALUE) nextDueNs = tMonoNs
        if (tMonoNs < nextDueNs) return null

        val qualifies = tracking &&
            (!motionValid || (angularRateRadPerS <= maxAngularRateRadPerS && linearSpeedMPerS <= maxLinearSpeedMPerS))

        if (qualifies) {
            val here = Candidate(tMonoNs, angularRateRadPerS, linearSpeedMPerS)
            val best = bestInSlot
            if (best == null || angularRateRadPerS < best.angularRateRadPerS) bestInSlot = here
        }

        // Emit once the hold window past the due time has elapsed (or
        // immediately, if the caller is already a full slot late — a dropped
        // frame burst must not stall the cadence indefinitely).
        val holdNs = (slotNs * slotHoldFraction).toLong()
        val emitAt = nextDueNs + holdNs
        if (tMonoNs < emitAt) return null

        val chosen = bestInSlot
        bestInSlot = null
        nextDueNs = maxOf(nextDueNs + slotNs, tMonoNs - slotNs / 2)
        return chosen
    }

    fun reset() {
        nextDueNs = Long.MIN_VALUE
        bestInSlot = null
    }

    /** True when this rate is inside S6's "turning slowly" band. Exposed for the capture UI's live hint. */
    fun withinMotionGate(angularRateRadPerS: Double): Boolean =
        abs(angularRateRadPerS) <= maxAngularRateRadPerS
}
