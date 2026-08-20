package dev.producdevity.codboz

internal data class SurfaceSize(
    val width: Int,
    val height: Int,
)

internal data class GuestPointerPosition(
    val x: Int,
    val y: Int,
    val insideContent: Boolean,
)

internal object DisplayGeometry {
    private const val GUEST_HEIGHT = 480
    private const val STANDARD_GUEST_WIDTH = 640
    private const val WIDESCREEN_GUEST_WIDTH = 854

    fun guestSurface(
        hostWidth: Int,
        hostHeight: Int,
    ): SurfaceSize {
        require(hostWidth > 0)
        require(hostHeight > 0)

        val landscapeWidth = maxOf(hostWidth, hostHeight)
        val landscapeHeight = minOf(hostWidth, hostHeight)
        val scaledWidth =
            (landscapeWidth.toLong() * GUEST_HEIGHT / landscapeHeight).toInt()
        val width =
            if (
                kotlin.math.abs(scaledWidth - STANDARD_GUEST_WIDTH) <=
                kotlin.math.abs(scaledWidth - WIDESCREEN_GUEST_WIDTH)
            ) {
                STANDARD_GUEST_WIDTH
            } else {
                WIDESCREEN_GUEST_WIDTH
            }
        return SurfaceSize(width, GUEST_HEIGHT)
    }

    fun mapCoordinate(
        position: Float,
        viewExtent: Int,
        guestExtent: Int,
    ): Int {
        require(viewExtent > 0)
        require(guestExtent > 0)
        return (position * guestExtent / viewExtent)
            .toInt()
            .coerceIn(0, guestExtent - 1)
    }

    fun fitInside(
        hostWidth: Int,
        hostHeight: Int,
        contentWidth: Int,
        contentHeight: Int,
    ): SurfaceSize {
        require(hostWidth > 0)
        require(hostHeight > 0)
        require(contentWidth > 0)
        require(contentHeight > 0)

        return if (hostWidth.toLong() * contentHeight > hostHeight.toLong() * contentWidth) {
            SurfaceSize(
                width =
                    (hostHeight.toDouble() * contentWidth / contentHeight)
                        .toInt()
                        .coerceAtMost(hostWidth),
                height = hostHeight,
            )
        } else {
            SurfaceSize(
                width = hostWidth,
                height =
                    (hostWidth.toDouble() * contentHeight / contentWidth)
                        .toInt()
                        .coerceAtMost(hostHeight),
            )
        }
    }

    fun mapPointer(
        x: Float,
        y: Float,
        viewWidth: Int,
        viewHeight: Int,
        guestWidth: Int,
        guestHeight: Int,
    ): GuestPointerPosition =
        GuestPointerPosition(
            x = mapCoordinate(x, viewWidth, guestWidth),
            y = mapCoordinate(y, viewHeight, guestHeight),
            insideContent = x >= 0 && x < viewWidth && y >= 0 && y < viewHeight,
        )
}
