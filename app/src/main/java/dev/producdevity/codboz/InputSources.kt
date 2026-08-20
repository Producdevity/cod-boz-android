package dev.producdevity.codboz

import android.view.InputDevice
import kotlin.math.abs

internal fun isGameControllerSource(source: Int): Boolean =
    source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
        source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK

internal fun isPhysicalExternalController(
    source: Int,
    isVirtual: Boolean,
): Boolean = isGameControllerSource(source) && !isVirtual

internal fun normalizeCenteredAxis(
    value: Float,
    min: Float,
    max: Float,
    flat: Float,
): Float {
    if (!value.isFinite() || !min.isFinite() || !max.isFinite() || max <= min) return 0f
    val center = (min + max) / 2f
    val offset = value - center
    if (abs(offset) <= flat.coerceAtLeast(0f)) return 0f
    val extent = if (offset < 0f) center - min else max - center
    if (extent <= 0f) return 0f
    return (offset / extent).coerceIn(-1f, 1f)
}

internal fun normalizeTriggerAxis(
    value: Float,
    min: Float,
    max: Float,
): Float {
    if (!value.isFinite() || !min.isFinite() || !max.isFinite() || max <= min) return 0f
    return ((value - min) / (max - min)).coerceIn(0f, 1f)
}
