package dev.producdevity.codboz

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticsSanitizerTest {
    @Test
    fun removesUserAndNetworkValues() {
        val result =
            DiagnosticsSanitizer.sanitize(
                "Player Alice connected to https://secret.example/a at 10.0.0.4 " +
                    "from /data/user/0/dev.producdevity.codboz/files/config.txt",
                listOf("Alice", "secret.example"),
            )

        assertFalse(result.contains("Alice"))
        assertFalse(result.contains("secret.example"))
        assertFalse(result.contains("10.0.0.4"))
        assertFalse(result.contains("/data/user"))
        assertTrue(result.contains("<redacted>"))
        assertTrue(result.contains("<ip-address>"))
        assertTrue(result.contains("<app-data>"))
    }
}
