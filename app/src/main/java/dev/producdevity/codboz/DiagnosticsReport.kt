package dev.producdevity.codboz

import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

object DiagnosticsReport {
    private val runtimeKeys =
        setOf(
            "timestamp_ms",
            "status",
            "imports",
            "fault",
            "guest_pc",
            "uptime_ms",
            "last_import",
            "renderer",
            "gl_version",
            "gl_diagnostics",
            "runtime_diagnostics",
            "voice_diagnostics",
        )

    fun create(
        appVersion: String,
        hostApkHash: String,
        runtimeVersion: String,
        sdk: Int,
        androidRelease: String,
        deviceManufacturer: String,
        deviceModel: String,
        hostAbis: List<String>,
        pageSize: Int,
        displayWidthPx: Int,
        displayHeightPx: Int,
        densityDpi: Int,
        refreshRateHz: Float,
        memoryClassMb: Int,
        guestWidth: Int,
        guestHeight: Int,
        cpuSelfTest: String,
        imported: Boolean,
        prepared: Boolean,
        sourceHash: String?,
        importCount: Int?,
        voiceChatEnabled: Boolean,
        microphonePermissionGranted: Boolean,
        runtimeReport: File,
        exits: List<CrashDiagnostics.ExitRecord>,
        javaCrash: String?,
        recentEvents: String?,
        sensitiveValues: Collection<String>,
    ): String =
        buildString {
            appendLine("COD BOZ diagnostics")
            appendLine("report format  6")
            appendLine("created utc    ${formatTimestamp(System.currentTimeMillis())}")
            appendLine("app version    $appVersion")
            appendLine("host APK SHA   $hostApkHash")
            appendLine("runtime        $runtimeVersion")
            appendLine("android        $androidRelease (SDK $sdk)")
            appendLine("device         ${safe(deviceManufacturer, sensitiveValues)} ${safe(deviceModel, sensitiveValues)}")
            appendLine("host abis      ${hostAbis.joinToString()}")
            appendLine("page size      $pageSize")
            appendLine("host display   ${displayWidthPx}x$displayHeightPx @ ${formatRate(refreshRateHz)} Hz, $densityDpi dpi")
            appendLine("memory class   $memoryClassMb MiB")
            appendLine("guest cpu      ARMv7/Thumb through Dynarmic")
            appendLine("guest display  ${guestWidth}x$guestHeight")
            appendLine("graphics       EGL / OpenGL ES 2")
            appendLine("cpu self-test  $cpuSelfTest")
            appendLine("imported       $imported")
            appendLine("prepared       $prepared")
            appendLine("source hash    ${sourceHash ?: "not loaded"}")
            appendLine("imports        ${importCount ?: "not loaded"}")
            appendLine("voice chat     ${if (voiceChatEnabled) "enabled" else "disabled"}")
            appendLine("microphone     ${if (microphonePermissionGranted) "granted" else "not granted"}")

            appendLine()
            appendLine("Recent app events")
            if (recentEvents.isNullOrBlank()) {
                appendLine("not recorded")
            } else {
                recentEvents.lineSequence().forEach { line ->
                    val separator = line.indexOf(' ')
                    if (separator <= 0) return@forEach
                    val timestamp = line.substring(0, separator).toLongOrNull() ?: return@forEach
                    val event = line.substring(separator + 1)
                    if (event.isNotBlank()) {
                        appendLine("${formatTimestamp(timestamp)}  ${safe(event, sensitiveValues)}")
                    }
                }
            }

            appendLine()
            appendLine("Last guest runtime")
            val values = readRuntimeReport(runtimeReport)
            if (values.isEmpty()) {
                appendLine("status         not recorded")
            } else {
                values["timestamp_ms"]?.toLongOrNull()?.let {
                    appendLine("time utc       ${formatTimestamp(it)}")
                }
                values["status"]?.let { appendLine("status         $it") }
                values["imports"]?.let { appendLine("imports        $it") }
                values["fault"]?.let { appendLine("fault          $it") }
                values["guest_pc"]?.let { appendLine("guest pc       $it") }
                values["uptime_ms"]?.let { appendLine("uptime ms      $it") }
                values["last_import"]?.let { appendLine("last import    $it") }
                values["renderer"]?.let { appendLine("host renderer  $it") }
                values["gl_version"]?.let { appendLine("host GL        $it") }
                values["gl_diagnostics"]?.let { appendLine("GL diagnostics $it") }
                values["runtime_diagnostics"]?.let { appendLine("runtime        $it") }
                values["voice_diagnostics"]?.let { appendLine("voice pipeline $it") }
            }

            appendLine()
            appendLine("Recent process exits")
            if (exits.isEmpty()) {
                appendLine("not available on this Android version or device")
            } else {
                exits.forEachIndexed { index, exit ->
                    appendLine()
                    appendLine("exit ${index + 1}")
                    appendLine("time utc       ${formatTimestamp(exit.timestampMillis)}")
                    appendLine("reason         ${exit.reason}")
                    appendLine("pid            ${exit.pid}")
                    appendLine("status         ${exit.status}")
                    appendLine("importance     ${exit.importance}")
                    appendLine("memory         PSS ${exit.pssKb} KiB, RSS ${exit.rssKb} KiB")
                    exit.description?.takeIf { it.isNotBlank() }?.let {
                        appendLine("description    ${safe(it, sensitiveValues)}")
                    }
                    exit.processState?.takeIf { it.isNotBlank() }?.let {
                        appendLine("app state      ${safe(it, sensitiveValues)}")
                    }
                    exit.traceNote?.let { appendLine("trace          $it") }
                    exit.trace?.takeIf { it.isNotBlank() }?.let {
                        appendLine("system trace")
                        appendLine(safe(it, sensitiveValues))
                    }
                }
            }

            if (!javaCrash.isNullOrBlank()) {
                appendLine()
                appendLine("Last in-process Java crash")
                appendLine(safe(javaCrash, sensitiveValues))
            }
        }

    private fun safe(
        value: String,
        sensitiveValues: Collection<String>,
    ): String = DiagnosticsSanitizer.sanitize(value, sensitiveValues)

    private fun formatTimestamp(timestampMillis: Long): String =
        SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS 'UTC'", Locale.US).run {
            timeZone = TimeZone.getTimeZone("UTC")
            format(Date(timestampMillis))
        }

    private fun formatRate(rate: Float): String = String.format(Locale.US, "%.2f", rate)

    private fun readRuntimeReport(file: File): Map<String, String> {
        if (!file.isFile || file.length() > MAX_RUNTIME_REPORT_BYTES) return emptyMap()
        return runCatching {
            buildMap {
                file.forEachLine(Charsets.UTF_8) { line ->
                    val separator = line.indexOf('=')
                    if (separator <= 0) return@forEachLine
                    val key = line.substring(0, separator)
                    if (key !in runtimeKeys || key in this) return@forEachLine
                    val value =
                        line
                            .substring(separator + 1)
                            .filter { it.code in 0x20..0x7e }
                            .take(MAX_RUNTIME_VALUE_LENGTH)
                    if (value.isNotBlank()) put(key, value)
                }
            }
        }.getOrDefault(emptyMap())
    }

    private const val MAX_RUNTIME_REPORT_BYTES = 8 * 1024
    private const val MAX_RUNTIME_VALUE_LENGTH = 1_024
}
