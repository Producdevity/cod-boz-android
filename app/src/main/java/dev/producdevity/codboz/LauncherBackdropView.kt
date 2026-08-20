package dev.producdevity.codboz

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Shader
import android.util.AttributeSet
import android.view.View
import kotlin.math.max

class LauncherBackdropView
    @JvmOverloads
    constructor(
        context: Context,
        attributes: AttributeSet? = null,
    ) : View(context, attributes) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        private val slash = Path()
        private var glow: LinearGradient? = null

        override fun onSizeChanged(
            width: Int,
            height: Int,
            oldWidth: Int,
            oldHeight: Int,
        ) {
            super.onSizeChanged(width, height, oldWidth, oldHeight)
            glow =
                LinearGradient(
                    width.toFloat(),
                    0f,
                    width * 0.28f,
                    height.toFloat(),
                    intArrayOf(Color.argb(58, 214, 43, 34), Color.TRANSPARENT),
                    null,
                    Shader.TileMode.CLAMP,
                )
        }

        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            canvas.drawColor(Color.rgb(8, 9, 10))

            paint.shader = glow
            canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
            paint.shader = null

            slash.reset()
            val depth = max(width, height).toFloat()
            slash.moveTo(width * 0.82f, -depth * 0.12f)
            slash.lineTo(width * 1.08f, -depth * 0.12f)
            slash.lineTo(width * 0.58f, depth * 1.12f)
            slash.lineTo(width * 0.32f, depth * 1.12f)
            slash.close()
            paint.color = Color.argb(16, 255, 255, 255)
            canvas.drawPath(slash, paint)

            paint.color = Color.argb(12, 255, 255, 255)
            paint.strokeWidth = resources.displayMetrics.density
            val step = 18f * resources.displayMetrics.density
            var y = 0f
            while (y < height) {
                canvas.drawLine(0f, y, width.toFloat(), y, paint)
                y += step
            }
        }
    }
