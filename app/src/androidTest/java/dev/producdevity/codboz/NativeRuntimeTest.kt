package dev.producdevity.codboz

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class NativeRuntimeTest {
    @Test
    fun executesGuestMemorySelfTest() {
        val result = NativeRuntime.nativeCpuSelfTest()

        assertTrue(result, result.contains("r0=12"))
        assertTrue(result, result.contains("cross=1"))
        assertTrue(result, result.contains("ordered=1"))
        assertTrue(result, result.contains("fault=1"))
        assertTrue(result, result.contains(if (NativeRuntime.nativePageSize() == 4096) "fastmem=1" else "fastmem=0"))
        assertTrue(result, result.contains("rwx=0"))
    }
}
