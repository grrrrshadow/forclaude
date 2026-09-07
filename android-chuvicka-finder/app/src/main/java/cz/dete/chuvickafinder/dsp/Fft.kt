package cz.dete.chuvickafinder.dsp

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

/**
 * Minimal in-place iterative radix-2 Cooley-Tukey FFT. [size] must be a power of two.
 * Operates on parallel real/imaginary arrays to avoid allocating complex objects per sample.
 */
class Fft(private val size: Int) {

    init {
        require(size > 1 && (size and (size - 1)) == 0) { "FFT size must be a power of two" }
    }

    private val cosTable = DoubleArray(size / 2)
    private val sinTable = DoubleArray(size / 2)
    private val bitReversal = IntArray(size)

    init {
        for (i in 0 until size / 2) {
            val angle = -2.0 * PI * i / size
            cosTable[i] = cos(angle)
            sinTable[i] = sin(angle)
        }
        var bits = 0
        var n = size
        while (n > 1) { n = n shr 1; bits++ }
        for (i in 0 until size) {
            bitReversal[i] = Integer.reverse(i) ushr (32 - bits)
        }
    }

    /** Transforms [re]/[im] in place. Both arrays must have length == [size]. */
    fun transform(re: DoubleArray, im: DoubleArray) {
        for (i in 0 until size) {
            val j = bitReversal[i]
            if (j > i) {
                var t = re[i]; re[i] = re[j]; re[j] = t
                t = im[i]; im[i] = im[j]; im[j] = t
            }
        }

        var len = 2
        while (len <= size) {
            val half = len / 2
            val tableStep = size / len
            var i = 0
            while (i < size) {
                var k = 0
                for (j in 0 until half) {
                    val evenRe = re[i + j]
                    val evenIm = im[i + j]
                    val c = cosTable[k]
                    val s = sinTable[k]
                    val oddRe = re[i + j + half] * c - im[i + j + half] * s
                    val oddIm = re[i + j + half] * s + im[i + j + half] * c
                    re[i + j] = evenRe + oddRe
                    im[i + j] = evenIm + oddIm
                    re[i + j + half] = evenRe - oddRe
                    im[i + j + half] = evenIm - oddIm
                    k += tableStep
                }
                i += len
            }
            len = len shl 1
        }
    }

    companion object {
        fun hannWindow(n: Int): DoubleArray = DoubleArray(n) { i ->
            0.5 - 0.5 * cos(2.0 * PI * i / (n - 1))
        }
    }
}
