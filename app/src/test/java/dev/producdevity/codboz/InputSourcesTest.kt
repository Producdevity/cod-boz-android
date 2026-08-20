package dev.producdevity.codboz

import android.view.InputDevice
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class InputSourcesTest {
    @Test
    fun recognizesGameControllersWithoutTreatingOtherInputsAsControllers() {
        assertTrue(isGameControllerSource(InputDevice.SOURCE_GAMEPAD))
        assertTrue(isGameControllerSource(InputDevice.SOURCE_JOYSTICK))
        assertFalse(isGameControllerSource(InputDevice.SOURCE_KEYBOARD))
        assertFalse(isGameControllerSource(InputDevice.SOURCE_TOUCHSCREEN))
    }

    @Test
    fun acceptsNonVirtualGameControllers() {
        assertTrue(
            isPhysicalExternalController(
                InputDevice.SOURCE_GAMEPAD or InputDevice.SOURCE_JOYSTICK,
                isVirtual = false,
            ),
        )
        assertFalse(
            isPhysicalExternalController(
                InputDevice.SOURCE_GAMEPAD,
                isVirtual = true,
            ),
        )
        assertTrue(
            isPhysicalExternalController(
                InputDevice.SOURCE_JOYSTICK,
                isVirtual = false,
            ),
        )
    }

    @Test
    fun normalizesCenteredAxesAcrossCommonHidRanges() {
        assertEquals(0f, normalizeCenteredAxis(0.04f, -1f, 1f, 0.05f), 0.0001f)
        assertEquals(0.5f, normalizeCenteredAxis(0.5f, -1f, 1f, 0.05f), 0.0001f)
        assertEquals(-0.5f, normalizeCenteredAxis(-0.5f, -1f, 1f, 0.05f), 0.0001f)
        assertEquals(1f, normalizeCenteredAxis(255f, 0f, 255f, 4f), 0.0001f)
        assertEquals(-1f, normalizeCenteredAxis(0f, 0f, 255f, 4f), 0.0001f)
    }

    @Test
    fun normalizesTriggerAxesAcrossSignedAndUnsignedRanges() {
        assertEquals(0f, normalizeTriggerAxis(-1f, -1f, 1f), 0.0001f)
        assertEquals(0.5f, normalizeTriggerAxis(0f, -1f, 1f), 0.0001f)
        assertEquals(1f, normalizeTriggerAxis(1f, -1f, 1f), 0.0001f)
        assertEquals(0.5f, normalizeTriggerAxis(127.5f, 0f, 255f), 0.0001f)
    }
}
