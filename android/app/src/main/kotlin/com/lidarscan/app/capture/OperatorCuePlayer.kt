package com.lidarscan.app.capture

import android.content.Context
import android.media.AudioManager
import android.media.ToneGenerator
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import com.lidarscan.core.capture.CueKind
import com.lidarscan.core.capture.CuePatterns
import java.util.concurrent.Executors

/**
 * ROUND 11 (owner item 43) — the Android half of the operator cues.
 *
 * Everything that decides *whether* to play lives in `:core`
 * ([com.lidarscan.core.capture.CueScheduler]) and is unit-tested; this class is
 * the shell that makes noise, and it is deliberately thin enough not to need a
 * test of its own.
 *
 * ## Not on the caller's thread
 *
 * `Vibrator.vibrate` and `ToneGenerator.startTone` both cross a binder to
 * system services. The cue is decided on the ViewModel's 500 ms hint ticker,
 * which runs on the main dispatcher — the same thread Compose recomposes on and
 * the same thread that would stutter the live viewport. So every call is posted
 * to a single-threaded executor: single, not pooled, so two cues can never
 * overlap into one unrecognizable pattern, and so the tone's `Thread.sleep` is
 * never on anything that matters.
 *
 * ## Why ToneGenerator and not SoundPool
 *
 * SoundPool needs an asset, an APK entry and a load-completion callback; a tone
 * needs none of those and is what the platform ships for exactly this (a short
 * synthetic beep over `STREAM_NOTIFICATION`, which respects the phone's
 * notification volume and its silent mode). Three cues that differ by COUNT
 * rather than by timbre — see `CuePatterns` — do not need sampled audio to be
 * told apart at hip height.
 *
 * The generator is created lazily and kept: constructing one costs an AudioTrack
 * allocation, and doing it per cue is how you get a 200 ms lag on the one signal
 * that has to be immediate.
 */
class OperatorCuePlayer(context: Context) {

    private val appContext = context.applicationContext
    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "lidarscan-cues").apply { isDaemon = true }
    }

    private val vibrator: Vibrator? by lazy {
        runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val manager =
                    appContext.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager
                manager?.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                appContext.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
            }
        }.getOrNull()
    }

    @Volatile
    private var tone: ToneGenerator? = null

    /**
     * Play one cue. Safe to call from any thread; returns immediately.
     *
     * Every failure path is swallowed: a phone with no vibrator, a device whose
     * audio service is busy, an OEM that throws from `startTone` — none of them
     * is a reason for a capture to stop, and the visual hints the cue duplicates
     * are all still on screen.
     */
    fun play(kind: CueKind) {
        val pattern = CuePatterns.of(kind)
        executor.execute {
            runCatching { vibrate(pattern.pattern, pattern.amplitudes) }
            runCatching { beep(pattern.toneMillis, pattern.toneRepeats) }
        }
    }

    fun release() {
        executor.execute {
            runCatching { tone?.release() }
            tone = null
        }
        executor.shutdown()
    }

    private fun vibrate(timings: LongArray, amplitudes: IntArray) {
        val v = vibrator ?: return
        if (!v.hasVibrator()) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val effect = if (v.hasAmplitudeControl() && amplitudes.size == timings.size) {
                VibrationEffect.createWaveform(timings, amplitudes, -1)
            } else {
                VibrationEffect.createWaveform(timings, -1)
            }
            v.vibrate(effect)
        } else {
            @Suppress("DEPRECATION")
            v.vibrate(timings, -1)
        }
    }

    /**
     * `TONE_PROP_BEEP` rather than a frequency: `ToneGenerator` synthesises from
     * a fixed tone table and does not take an arbitrary Hz, so `CuePattern.toneHz`
     * is documentation of intent (low = calm, high = urgent) that the vibration
     * carries and the tone approximates by repeat count. Saying so here rather
     * than pretending the number is used.
     */
    private fun beep(millis: Int, repeats: Int) {
        val generator = tone ?: ToneGenerator(AudioManager.STREAM_NOTIFICATION, TONE_VOLUME)
            .also { tone = it }
        repeat(repeats.coerceIn(1, 4)) { i ->
            generator.startTone(ToneGenerator.TONE_PROP_BEEP, millis)
            if (i < repeats - 1) Thread.sleep((millis + GAP_MS).toLong())
        }
    }

    private companion object {
        const val TONE_VOLUME = 80
        const val GAP_MS = 60
    }
}
