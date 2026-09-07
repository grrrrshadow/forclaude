package cz.dete.chuvickafinder.feedback

import android.media.AudioManager
import android.media.ToneGenerator
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.max

/**
 * Geiger-counter style audio feedback: the higher [level] (0..1, normalized signal strength
 * above the calibrated noise floor), the faster the clicks - so you can search for the
 * baby monitor by ear without staring at the screen.
 */
class GeigerBeeper {

    @Volatile
    private var level: Double = 0.0
    private val running = AtomicBoolean(false)
    private var thread: Thread? = null
    private var toneGenerator: ToneGenerator? = null

    fun setLevel(newLevel: Double) {
        level = newLevel.coerceIn(0.0, 1.0)
    }

    fun start() {
        if (running.getAndSet(true)) return
        toneGenerator = ToneGenerator(AudioManager.STREAM_MUSIC, 80)
        thread = Thread {
            while (running.get()) {
                val l = level
                // 900ms of silence at level 0, down to ~90ms between clicks at level 1.
                val delayMs = (900 - l * 810).toLong().coerceIn(90, 900)
                toneGenerator?.startTone(ToneGenerator.TONE_PROP_BEEP, 30)
                try {
                    Thread.sleep(max(delayMs, 30L))
                } catch (_: InterruptedException) {
                    break
                }
            }
        }.also { it.isDaemon = true; it.start() }
    }

    fun stop() {
        if (!running.getAndSet(false)) return
        thread?.interrupt()
        thread = null
        toneGenerator?.release()
        toneGenerator = null
    }

    val isRunning: Boolean get() = running.get()
}
