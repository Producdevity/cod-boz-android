package dev.producdevity.codboz

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class ProprietaryImportTest {
    private val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
    private val testContext = InstrumentationRegistry.getInstrumentation().context
    private val testRoot = File(targetContext.cacheDir, "proprietary-import-test")

    @After
    fun cleanUp() {
        testRoot.deleteRecursively()
    }

    @Test
    fun importsSupportedApkAndExecutesArmGuestSelfTest() {
        val source =
            runCatching { testContext.assets.open(SOURCE_ASSET) }.getOrElse {
                println("SKIP ProprietaryImportTest: CODBOZ_TEST_APK was not set at build time")
                null
            }
        assumeTrue("CODBOZ_TEST_APK was not set at build time", source != null)

        testRoot.deleteRecursively()
        assertTrue(testRoot.mkdirs())
        val sourceFile = File(testRoot, "source.apk")
        requireNotNull(source).use { input ->
            sourceFile.outputStream().use(input::copyTo)
        }

        val gameDir = File(testRoot, "game")
        val result = JSONObject(NativeRuntime.nativeImportApk(sourceFile.absolutePath, gameDir.absolutePath))

        assertTrue(result.optString("error"), result.getBoolean("ok"))
        assertTrue(result.getBoolean("hash_matched"))
        assertEquals("1.0.11", result.getString("source_version"))
        assertEquals(391, result.getInt("s3e_symbols"))
        assertTrue(NativeRuntime.nativeIsImported(gameDir.absolutePath))
        assertTrue(File(gameDir, "import-manifest.json").canRead())
        assertTrue(!File(gameDir, "import-manifest.json").canWrite())
        assertTrue(!File(gameDir, "assets/boz.s3e.unpacked").canWrite())
        val selfTest = NativeRuntime.nativeCpuSelfTest()
        assertTrue(selfTest, selfTest.contains("r0=12"))
        assertTrue(selfTest, selfTest.contains("rwx=0"))
    }

    private companion object {
        const val SOURCE_ASSET = "source-game.apk"
    }
}
