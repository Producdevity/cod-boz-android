package dev.producdevity.codboz

import android.content.Context
import android.widget.FrameLayout

internal class AspectRatioFrameLayout(
    context: Context,
    private val contentWidth: Int,
    private val contentHeight: Int,
) : FrameLayout(context) {
    override fun onMeasure(
        widthMeasureSpec: Int,
        heightMeasureSpec: Int,
    ) {
        val availableWidth = MeasureSpec.getSize(widthMeasureSpec).coerceAtLeast(1)
        val availableHeight = MeasureSpec.getSize(heightMeasureSpec).coerceAtLeast(1)
        val fitted =
            DisplayGeometry.fitInside(
                availableWidth,
                availableHeight,
                contentWidth,
                contentHeight,
            )
        super.onMeasure(
            MeasureSpec.makeMeasureSpec(fitted.width, MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(fitted.height, MeasureSpec.EXACTLY),
        )
    }
}
