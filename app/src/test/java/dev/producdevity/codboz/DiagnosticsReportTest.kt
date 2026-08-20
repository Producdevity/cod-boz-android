package dev.producdevity.codboz

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files

class DiagnosticsReportTest {
    @Test
    fun reportIncludesOnlyWhitelistedRuntimeFields() {
        val file = Files.createTempFile("codboz-runtime", ".txt").toFile()
        file.deleteOnExit()
        file.writeText(
            "timestamp_ms=1700000000000\n" +
                "status=fault\n" +
                "imports=1234\n" +
                "fault=fault pc=0x12345678 addr=0x00000000\n" +
                "guest_pc=0x4a001234\n" +
                "uptime_ms=42000\n" +
                "last_import=glDrawElements\n" +
                "renderer=Android Emulator OpenGL ES Translator\n" +
                "gl_version=OpenGL ES 3.2\n" +
                "gl_diagnostics=bounded GLES2 profile; no GL failures\n" +
                "player_name=Private\n" +
                "multiplayer_server=private.example\n",
        )

        val report =
            DiagnosticsReport.create(
                appVersion = "0.1.0",
                hostApkHash = "0123456789abcdef",
                runtimeVersion = "runtime 0.1.0",
                sdk = 36,
                androidRelease = "16",
                deviceManufacturer = "Google",
                deviceModel = "Test Phone",
                hostAbis = listOf("arm64-v8a"),
                pageSize = 16_384,
                displayWidthPx = 2400,
                displayHeightPx = 1080,
                densityDpi = 420,
                refreshRateHz = 120f,
                memoryClassMb = 512,
                guestWidth = 854,
                guestHeight = 480,
                cpuSelfTest = "outcome=2 r0=12",
                imported = true,
                prepared = true,
                sourceHash = "359ee68b",
                importCount = 391,
                voiceChatEnabled = true,
                microphonePermissionGranted = false,
                runtimeReport = file,
                exits =
                    listOf(
                        CrashDiagnostics.ExitRecord(
                            timestampMillis = 1_700_000_000_000,
                            pid = 1234,
                            reason = "anr",
                            status = 0,
                            importance = 100,
                            pssKb = 200_000,
                            rssKb = 300_000,
                            description = "server private.example timed out",
                            processState = "game-running",
                            trace = "at https://private.example/api from 192.168.1.4",
                            traceNote = null,
                            requiresAttention = true,
                        ),
                    ),
                javaCrash = null,
                recentEvents = "1700000000000 game-running-touch\n",
                sensitiveValues = listOf("private.example"),
            )

        assertTrue(report.contains("status         fault"))
        assertTrue(report.contains("host APK SHA   0123456789abcdef"))
        assertTrue(report.contains("2023-11-14 22:13:20.000 UTC  game-running-touch"))
        assertTrue(report.contains("time utc       2023-11-14 22:13:20.000 UTC"))
        assertTrue(report.contains("imports        1234"))
        assertTrue(report.contains("pc=0x12345678"))
        assertTrue(report.contains("guest pc       0x4a001234"))
        assertTrue(report.contains("uptime ms      42000"))
        assertTrue(report.contains("last import    glDrawElements"))
        assertTrue(report.contains("guest display  854x480"))
        assertTrue(report.contains("host renderer  Android Emulator OpenGL ES Translator"))
        assertTrue(report.contains("host GL        OpenGL ES 3.2"))
        assertTrue(report.contains("GL diagnostics bounded GLES2 profile; no GL failures"))
        assertFalse(report.contains("Private"))
        assertFalse(report.contains("private.example"))
        assertFalse(report.contains("192.168.1.4"))
        assertTrue(report.contains("reason         anr"))
        assertTrue(report.contains("device         Google Test Phone"))
        assertTrue(report.contains("voice chat     enabled"))
        assertTrue(report.contains("microphone     not granted"))
    }
}
