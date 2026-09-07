package cz.dete.chuvickafinder.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.util.AttributeSet
import android.view.View
import kotlin.math.roundToInt

/**
 * Simple horizontal "how hot/cold" bar meter, 0.0 (no signal above noise floor) to 1.0
 * (strongest signal seen so far). Green -> yellow -> red like a classic signal-hunting meter.
 */
class SignalMeterView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    private var level: Float = 0f

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#2A323A")
        style = Paint.Style.FILL
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val tickPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#40FFFFFF")
        strokeWidth = 2f
    }

    private var gradient: LinearGradient? = null

    fun setLevel(newLevel: Float) {
        level = newLevel.coerceIn(0f, 1f)
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        gradient = LinearGradient(
            0f, 0f, w.toFloat(), 0f,
            intArrayOf(
                Color.parseColor("#2ECC71"),
                Color.parseColor("#F1C40F"),
                Color.parseColor("#E74C3C"),
            ),
            floatArrayOf(0f, 0.6f, 1f),
            Shader.TileMode.CLAMP,
        )
        fillPaint.shader = gradient
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        val corner = h / 2f

        val track = RectF(0f, 0f, w, h)
        canvas.drawRoundRect(track, corner, corner, trackPaint)

        val fillWidth = w * level
        if (fillWidth > 1f) {
            canvas.save()
            canvas.clipRect(0f, 0f, fillWidth, h)
            canvas.drawRoundRect(track, corner, corner, fillPaint)
            canvas.restore()
        }

        // Tick marks every 10%.
        for (i in 1 until 10) {
            val x = w * i / 10f
            canvas.drawLine(x, h * 0.15f, x, h * 0.85f, tickPaint)
        }
    }

    fun percentText(): String = "${(level * 100).roundToInt()}%"
}
