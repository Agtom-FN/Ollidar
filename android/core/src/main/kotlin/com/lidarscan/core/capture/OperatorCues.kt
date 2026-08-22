package com.lidarscan.core.capture

/**
 * ROUND 11 item 43 — what the operator is told when they cannot look at the
 * screen.
 *
 * > "Phone is held facing forward; the operator cannot watch the screen.
 * >  Vibrate + tone on: tracking degraded, section break, moving too fast for
 * >  target ring density. Distinct patterns, debounced, toggleable in settings,
 * >  default ON."
 *
 * The phone is the sensor mount. On this rig the screen faces sideways at hip
 * height while the operator walks and looks where they are going, so every hint
 * the app has added since ROUND 5.3 — the motion hint, the tracking chip, the
 * section counter — is written for a reader who is not there. This file is the
 * channel that is.
 *
 * ## Why the decision lives in `:core` and the buzzing does not
 *
 * Everything hard about a cue is timing, and timing is exactly what a unit test
 * can hold still: which cue wins when two fire in the same frame, how long a
 * cue stays quiet after firing, whether a condition that is true for thirty
 * seconds buzzes once or sixty times. `VibratorManager` and `ToneGenerator` can
 * then be a thin, untested shell that does what it is told — and, importantly,
 * does it somewhere other than the render thread.
 *
 * ## Debounce, and why the three intervals differ
 *
 *  * **Tracking degraded** repeats every 4 s while it lasts. It is a state, not
 *    an event, and it is the one the operator must act on (stop, point the
 *    camera at something textured, wait). A single buzz at the start of a
 *    twenty-second tracking loss would be missed by exactly the person it is
 *    for.
 *  * **Section break** never repeats — it IS an event, one per relocalization,
 *    and it is the most serious of the three (everything after it is in a
 *    different world frame). It gets a 1 s floor only so that a burst of breaks
 *    inside one bad second does not become a rattle.
 *  * **Too fast** repeats every 3 s. It is a state the operator can fix in one
 *    step, and it must not nag so hard that it becomes background noise; 3 s is
 *    long enough to have slowed down and felt it stop.
 *
 * ## Priority
 *
 * One cue at a time, and the highest-priority READY cue wins. Two haptic
 * patterns overlapping are one unrecognizable pattern, which is worse than
 * either alone. A cue that loses does not consume its debounce, so it fires on
 * the next tick if it is still true.
 */
enum class CueKind {
    /** ARCore lost or degraded tracking. Everything scanned now is being thrown away. */
    TRACKING_DEGRADED,

    /** ARCore relocalized: a new section. Everything after this is in a different frame. */
    SECTION_BREAK,

    /** Moving faster than the target ring density can keep up with. */
    TOO_FAST,

    /**
     * ROUND 14 — turning the phone without walking it.
     *
     * The owner's headline complaint about 0.8.0 was that scans are good when
     * he walks a loop and bad when he stands and sweeps the phone around the
     * room. Measured on his own three captures, sweeping costs nothing that
     * this project can normally see: the resolved map's self-consistency at
     * 8 s separation is 1.97 cm walking against 2.45 and 1.74 cm sweeping, the
     * mount watchdog reads 0.00% impossible elevations on all three, ARCore's
     * orientation tracks the 400 Hz gyro to 0.22 deg p90 even at 90-150 deg/s,
     * and the IMU densifier falls back at 31.3% / 31.9% / 31.8% regardless.
     * Sweeping does not degrade the geometry.
     *
     * What it degrades is the CAMERA's ability to hold a frame. Over 1 s
     * windows the walking capture gave ARCore **2.43 cm of translation per
     * degree of rotation**; the two sweeps gave **0.53 and 0.56** — a fifth as
     * much. Rotation without translation is the degenerate case for monocular
     * visual tracking: bearings change, nothing about depth does, no new
     * feature can be triangulated, and the tracker has to lean on the map it
     * already has. Leaning on the map is what produces a relocalisation snap,
     * and scan-035's was 162.57 deg of pose change in 33 ms against 1.56 deg
     * of measured gyro.
     *
     * So this is not "sweep slower". The owner's sweep RATE is fine — his
     * median is 9-10 deg/s, which at the D6's 10 Hz revolution puts adjacent
     * fan planes 0.92-1.03 deg apart against a 0.90 deg within-fan pitch, i.e.
     * almost exactly isotropic sampling. It is "keep walking while you sweep".
     */
    PARALLAX_STARVED,

    /**
     * ROUND 20 (item 78) — "GO, start walking": the Start hold-steady stage
     * has converged (or honestly timed out onto the persisted trim) and the
     * capture has begun. Played ONCE, directly, never through the scheduler —
     * it is a confirmation, not a condition, so it has no debounce row and no
     * priority slot. One short, light tick: the operator is holding the rig
     * still at that exact moment, and round 13 measured what a heavy buzz does
     * to a tracker that has just settled.
     */
    GO_START,

    /**
     * ROUND 33 item 179(b) — **the hold-still card's posture just went out of
     * tolerance.**
     *
     * Fired ONCE, on the good→bad edge, from the one placement that has words
     * on it. Like [GO_START] it is a confirmation of an event rather than a
     * condition, so it never goes near [CueScheduler]: there is nothing to
     * debounce, because the edge cannot repeat without the posture first coming
     * back inside — the transition IS the debounce.
     *
     * The recording strip deliberately has no cue of its own. Item 179(c) is
     * explicit about it: the walk already carries four patterns the operator is
     * learning to tell apart through a pocket, and a fifth that fires on a hand
     * wobble would devalue the four that mean something.
     */
    POSTURE_OFF,
}

/**
 * A cue's physical shape, in a form both `Vibrator` and `ToneGenerator` can be
 * driven from. Timings are milliseconds; [pattern] alternates OFF, ON, OFF, ON …
 * exactly as `VibrationEffect.createWaveform` expects, so `:app` passes it
 * through rather than translating it.
 *
 * The three are deliberately different in COUNT rather than in length: through a
 * jacket pocket, at walking pace, "how many buzzes" is the only dimension that
 * survives, and it is the one a person can learn in one session.
 */
data class CuePattern(
    val kind: CueKind,
    /** OFF, ON, OFF, ON … milliseconds. */
    val pattern: LongArray,
    /** 0..255 per ON segment, same length as the ON segments. */
    val amplitudes: IntArray,
    /** Tone frequency band, low = calm, high = urgent. Hertz. */
    val toneHz: Int,
    val toneMillis: Int,
    /** How many times the tone repeats, spaced by [toneMillis]. */
    val toneRepeats: Int,
) {
    // Data classes with array members need these spelled out or equality is
    // identity, which would make the tests below assert nothing.
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is CuePattern) return false
        return kind == other.kind &&
            pattern.contentEquals(other.pattern) &&
            amplitudes.contentEquals(other.amplitudes) &&
            toneHz == other.toneHz &&
            toneMillis == other.toneMillis &&
            toneRepeats == other.toneRepeats
    }

    override fun hashCode(): Int {
        var h = kind.hashCode()
        h = 31 * h + pattern.contentHashCode()
        h = 31 * h + amplitudes.contentHashCode()
        h = 31 * h + toneHz
        h = 31 * h + toneMillis
        h = 31 * h + toneRepeats
        return h
    }
}

object CuePatterns {
    /** Two firm buzzes, mid tone. "Something is wrong and it is still wrong." */
    val TRACKING_DEGRADED = CuePattern(
        kind = CueKind.TRACKING_DEGRADED,
        pattern = longArrayOf(0, 120, 90, 120),
        amplitudes = intArrayOf(0, 200, 0, 200),
        toneHz = 660,
        toneMillis = 120,
        toneRepeats = 2,
    )

    /**
     * Three short buzzes, high tone. The most serious of the three, and
     * **ROUND 13 turned its amplitude down from 255 to 150.**
     *
     * The reason is a measurement, not a preference. Isolating 30-200 Hz
     * accelerometer energy across the owner's scan-028/029/030 finds bursts
     * that map 1:1 onto this app's own logged cues (10 for 10 on scan-030) —
     * so this pattern is, physically, the largest shake the phone experiences
     * during a capture that nothing outside the app caused. And scan-030's
     * FOURTH section break came 0.51 s after the buzz fired for the third: it
     * is the only one of the four with any high-frequency energy in the half
     * second before it (z = 178 against 2.0 / 7.3 / 10.2 for the others).
     *
     * One coincidence is not proof, and it is not claimed as one. But firing
     * the loudest haptic the phone has at the exact moment ARCore is trying to
     * re-establish itself is a risk taken for no benefit — the operator can
     * feel three buzzes at 150 through a pocket perfectly well, which is the
     * amplitude TRACKING_DEGRADED has always used. Paired with
     * [CueScheduler.postBreakQuietMillis].
     */
    val SECTION_BREAK = CuePattern(
        kind = CueKind.SECTION_BREAK,
        pattern = longArrayOf(0, 70, 60, 70, 60, 70),
        amplitudes = intArrayOf(0, 150, 0, 150, 0, 150),
        toneHz = 880,
        toneMillis = 90,
        toneRepeats = 3,
    )

    /** One long, soft buzz, low tone. A nudge, not an alarm. */
    val TOO_FAST = CuePattern(
        kind = CueKind.TOO_FAST,
        pattern = longArrayOf(0, 260),
        amplitudes = intArrayOf(0, 130),
        toneHz = 440,
        toneMillis = 200,
        toneRepeats = 1,
    )

    /**
     * ROUND 14. Two soft buzzes at the SECTION_BREAK amplitude — deliberately
     * the gentlest thing that is still countable through a pocket, because
     * this cue fires while the tracker is in the state it is least able to
     * absorb a shake, which is the whole reason it is firing.
     */
    val PARALLAX_STARVED = CuePattern(
        kind = CueKind.PARALLAX_STARVED,
        pattern = longArrayOf(0, 120, 140, 120),
        amplitudes = intArrayOf(0, 130, 0, 130),
        toneHz = 620,
        toneMillis = 130,
        toneRepeats = 2,
    )

    /**
     * ROUND 20 (item 78). One short tick at the PARALLAX amplitude, one bright
     * tone — deliberately the lightest pattern in the table, because it fires
     * with the rig held still and a settled tracker (see CueKind.GO_START).
     */
    val GO_START = CuePattern(
        kind = CueKind.GO_START,
        pattern = longArrayOf(0, 60),
        amplitudes = intArrayOf(0, 130),
        toneHz = 990,
        toneMillis = 80,
        toneRepeats = 1,
    )

    /**
     * ROUND 33 item 179(b). ONE tick, at the GO_START amplitude and shorter
     * than it, with the lowest tone in the table.
     *
     * It fires while the operator is standing still being told to hold still —
     * the single moment in a capture when a shake costs the most (round 13
     * measured what a heavy buzz does to a tracker that is settling), and the
     * card he is holding already says the same thing in a word. So this is the
     * smallest signal that can still be felt: it exists to move the eye to the
     * ghost, not to carry the message itself.
     */
    val POSTURE_OFF = CuePattern(
        kind = CueKind.POSTURE_OFF,
        pattern = longArrayOf(0, 40),
        amplitudes = intArrayOf(0, 130),
        toneHz = 440,
        toneMillis = 60,
        toneRepeats = 1,
    )

    fun of(kind: CueKind): CuePattern = when (kind) {
        CueKind.TRACKING_DEGRADED -> TRACKING_DEGRADED
        CueKind.SECTION_BREAK -> SECTION_BREAK
        CueKind.TOO_FAST -> TOO_FAST
        CueKind.PARALLAX_STARVED -> PARALLAX_STARVED
        CueKind.GO_START -> GO_START
        CueKind.POSTURE_OFF -> POSTURE_OFF
    }
}

/**
 * The conditions the scheduler is asked about on every tick. All three are
 * levels except [sectionBreaks], which is a monotonically increasing count — a
 * break is an event and the scheduler fires on the DELTA, so a re-tick with the
 * same count is silent even if the debounce has expired.
 */
data class CueConditions(
    val trackingDegraded: Boolean = false,
    val movingTooFast: Boolean = false,
    val sectionBreaks: Int = 0,
    /**
     * ROUND 14 — the operator is turning the phone and not walking it. See
     * [CueKind.PARALLAX_STARVED] and [ParallaxWatch], which is where the
     * decision is actually made.
     */
    val turningWithoutMoving: Boolean = false,
)

class CueScheduler(
    private val trackingRepeatMillis: Long = 4_000L,
    private val tooFastRepeatMillis: Long = 3_000L,
    /**
     * ROUND 14. Much longer than the others on purpose. "Keep walking" is a
     * change of technique, not a reaction to an event, and a person needs
     * seconds to act on it; repeating every three would be nagging, and every
     * repeat is another shake of the tracker this cue exists to protect.
     */
    private val parallaxRepeatMillis: Long = 12_000L,
    private val sectionFloorMillis: Long = 1_000L,
    /**
     * ROUND 13. For this long after a section-break cue, NOTHING buzzes.
     *
     * A section break is ARCore re-anchoring, and the seconds immediately
     * after one are the seconds it is re-establishing itself — the worst
     * possible moment to shake the phone. scan-030's fourth break arrived
     * 0.51 s after the third break's cue, inside this window.
     *
     * It is a quiet period and not a longer debounce on purpose: the danger is
     * to the TRACKER, so every cue has to hold off, not just the one that
     * fired. A break inside the window is still counted and still shown on
     * screen; only the buzz is withheld.
     */
    private val postBreakQuietMillis: Long = 1_500L,
) {
    private val lastFiredAt = HashMap<CueKind, Long>()
    private var lastSectionBreaks = 0
    private var started = false

    /**
     * Called once per tick. Returns the cue to play, or null.
     *
     * `enabled = false` still advances the internal state — so turning cues on
     * mid-capture does not immediately fire a backlog of everything that
     * happened while they were off.
     */
    fun tick(conditions: CueConditions, nowMillis: Long, enabled: Boolean = true): CueKind? {
        if (!started) {
            // The first tick establishes the baseline. A capture that starts
            // while ARCore is still initializing must not buzz at the operator
            // before they have taken a step.
            started = true
            lastSectionBreaks = conditions.sectionBreaks
            return null
        }

        val sectionFired = conditions.sectionBreaks > lastSectionBreaks
        lastSectionBreaks = conditions.sectionBreaks

        // ROUND 13: the tracker's recovery window. Silent for everything.
        val lastBreak = lastFiredAt[CueKind.SECTION_BREAK]
        if (lastBreak != null && nowMillis - lastBreak < postBreakQuietMillis) return null

        val candidate = when {
            sectionFired && ready(CueKind.SECTION_BREAK, nowMillis, sectionFloorMillis) ->
                CueKind.SECTION_BREAK
            conditions.trackingDegraded &&
                ready(CueKind.TRACKING_DEGRADED, nowMillis, trackingRepeatMillis) ->
                CueKind.TRACKING_DEGRADED
            conditions.movingTooFast && ready(CueKind.TOO_FAST, nowMillis, tooFastRepeatMillis) ->
                CueKind.TOO_FAST
            // ROUND 14, and LAST on purpose. Everything above it is an event
            // that has already cost the operator something; this is a warning
            // that one is being made likely.
            conditions.turningWithoutMoving &&
                ready(CueKind.PARALLAX_STARVED, nowMillis, parallaxRepeatMillis) ->
                CueKind.PARALLAX_STARVED
            else -> null
        } ?: return null

        if (!enabled) return null
        // Only the cue that actually PLAYS consumes its debounce. A cue that
        // lost to a higher-priority one is still due on the next tick.
        lastFiredAt[candidate] = nowMillis
        return candidate
    }

    /**
     * Between captures. Not the same as constructing a new one only because the
     * ViewModel holds exactly one for the app's life, next to everything else
     * ROUND 10 found had to be reset per session rather than per process.
     */
    fun reset() {
        lastFiredAt.clear()
        lastSectionBreaks = 0
        started = false
    }

    private fun ready(kind: CueKind, nowMillis: Long, gap: Long): Boolean {
        val last = lastFiredAt[kind] ?: return true
        return nowMillis - last >= gap
    }
}
