package dev.producdevity.codboz

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AndroidAudioBridgeTest {
    @Test
    fun reportsNativeOutputSampleRate() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val bridge = AndroidAudioBridge(context)
        try {
            assertTrue(bridge.outputSampleRate() in 4_000..192_000)
        } finally {
            bridge.shutdown()
        }
    }

    @Test
    fun voiceInputIsEmptyUntilVoiceIsStarted() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val bridge = AndroidAudioBridge(context)
        try {
            assertNull(bridge.takeVoiceInput())
            bridge.stopVoice()
            assertNull(bridge.takeVoiceInput())
        } finally {
            bridge.shutdown()
        }
    }

    @Test
    fun releasesCompletedStaticSoundAfterOutputTail() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val bridge = AndroidAudioBridge(context)
        try {
            assertTrue(bridge.playSound(0, ShortArray(512), 8_000, 256))
            assertTrue(waitUntil { bridge.activeSoundMask() == 0 })
            assertTrue(waitUntil { soundTrack(bridge, 0) == null })
        } finally {
            bridge.shutdown()
        }
    }

    private fun soundTrack(
        bridge: AndroidAudioBridge,
        channel: Int,
    ): Any? {
        val sounds = AndroidAudioBridge::class.java.getDeclaredField("sounds")
        sounds.isAccessible = true
        return (sounds.get(bridge) as Array<*>)[channel]
    }

    private fun waitUntil(condition: () -> Boolean): Boolean {
        val deadline = System.nanoTime() + 3_000_000_000L
        while (System.nanoTime() < deadline) {
            if (condition()) return true
            Thread.sleep(20)
        }
        return condition()
    }
}
