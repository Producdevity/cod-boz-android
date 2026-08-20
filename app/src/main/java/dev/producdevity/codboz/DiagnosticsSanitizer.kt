package dev.producdevity.codboz

object DiagnosticsSanitizer {
    fun sanitize(
        value: String,
        sensitiveValues: Collection<String> = emptyList(),
    ): String {
        var sanitized = value
        sensitiveValues
            .asSequence()
            .filter { it.length >= 2 }
            .sortedByDescending { it.length }
            .forEach { sanitized = sanitized.replace(it, "<redacted>", ignoreCase = true) }
        sanitized = sanitized.replace(EMAIL, "<email>")
        sanitized = sanitized.replace(URL, "<url>")
        sanitized = sanitized.replace(IPV4, "<ip-address>")
        sanitized = sanitized.replace(IPV6, "<ip-address>")
        sanitized = sanitized.replace(APP_DATA_PATH, "<app-data>")
        sanitized = sanitized.replace(APK_PATH, "<apk>")
        return sanitized
            .lineSequence()
            .joinToString("\n") { line -> line.filter { it == '\t' || it.code in 0x20..0x7e } }
            .take(MAX_REPORT_TEXT)
    }

    private val EMAIL = Regex("(?i)\\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}\\b")
    private val URL = Regex("(?i)\\b(?:https?|wss?)://[^\\s<>()]+")
    private val IPV4 = Regex("(?<![0-9])(?:[0-9]{1,3}\\.){3}[0-9]{1,3}(?![0-9])")
    private val IPV6 = Regex("(?i)(?<![0-9a-f:])(?:[0-9a-f]{1,4}:){2,7}[0-9a-f]{0,4}(?![0-9a-f:])")
    private val APP_DATA_PATH =
        Regex("/(?:data/(?:user(?:_de)?/[0-9]+|data)|storage/emulated/[0-9]+)/[^\\s:]+")
    private val APK_PATH = Regex("/data/app/[^\\s:]+")
    private const val MAX_REPORT_TEXT = 512 * 1024
}
