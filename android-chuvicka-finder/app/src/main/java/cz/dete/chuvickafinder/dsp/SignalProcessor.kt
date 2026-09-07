package cz.dete.chuvickafinder.dsp

import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min

/**
 * Turns raw interleaved unsigned 8-bit I/Q samples (as streamed by rtl_tcp) into a single
 * "how strong is the signal right around the tuned frequency" power reading in dB.
 *
 * The dongle's own mixer/PLL already puts the frequency we care about at (or near) DC in the
 * baseband IQ stream, so instead of looking at the whole captured bandwidth we only sum the
 * FFT bins closest to DC (both the positive and the wrapped-around negative side). That makes
 * the reading reasonably selective even though the capture itself is much wider than one
 * baby-monitor channel, and tolerant of a few kHz of tuning/crystal drift.
 */
class SignalProcessor(private val fftSize: Int = 1024) {

    private val fft = Fft(fftSize)
    private val window = Fft.hannWindow(fftSize)
    private val re = DoubleArray(fftSize)
    private val im = DoubleArray(fftSize)

    /**
     * @return power of the signal near the tuned center frequency in dB (arbitrary
     * reference, only meaningful relative to another call of this function), or null if
     * [count] doesn't contain a full FFT window worth of samples.
     */
    fun centerPowerDb(bytes: ByteArray, count: Int, sampleRateHz: Int, bandwidthHz: Double = 100_000.0): Double? {
        val needed = fftSize * 2
        if (count < needed) return null

        for (i in 0 until fftSize) {
            val iSample = (bytes[2 * i].toInt() and 0xFF) - 127.5
            val qSample = (bytes[2 * i + 1].toInt() and 0xFF) - 127.5
            val w = window[i]
            re[i] = (iSample / 128.0) * w
            im[i] = (qSample / 128.0) * w
        }

        fft.transform(re, im)

        val binHz = sampleRateHz.toDouble() / fftSize
        val halfBins = max(1, min(fftSize / 2 - 1, (bandwidthHz / 2.0 / binHz).toInt()))

        var sumPower = 0.0
        var n = 0
        for (k in 0..halfBins) {
            sumPower += re[k] * re[k] + im[k] * im[k]
            n++
            if (k > 0) {
                val idx = fftSize - k
                sumPower += re[idx] * re[idx] + im[idx] * im[idx]
                n++
            }
        }
        val avgPower = sumPower / n
        return 10.0 * log10(avgPower + 1e-12)
    }
}

/** Simple exponential moving average to smooth a noisy meter reading. */
class ExponentialAverage(private val alpha: Double = 0.25) {
    private var value: Double? = null

    fun update(sample: Double): Double {
        val prev = value
        val next = if (prev == null) sample else prev + alpha * (sample - prev)
        value = next
        return next
    }

    fun reset() { value = null }
    fun current(): Double? = value
}
