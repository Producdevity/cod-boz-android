package dev.producdevity.codboz

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.nio.charset.CodingErrorAction
import java.util.concurrent.atomic.AtomicBoolean

object CrashDiagnostics {
    data class ExitRecord(
        val timestampMillis: Long,
        val pid: Int,
        val reason: String,
        val status: Int,
        val importance: Int,
        val pssKb: Long,
        val rssKb: Long,
        val description: String?,
        val processState: String?,
        val trace: String?,
        val traceNote: String?,
        val requiresAttention: Boolean,
    )

    data class Snapshot(
        val exits: List<ExitRecord>,
        val javaCrash: String?,
        val javaCrashTimestampMillis: Long?,
        val recentEvents: String?,
        val acknowledgedTimestampMillis: Long,
    ) {
        val attentionTimestampMillis: Long?
            get() =
                maxOfOrNull(
                    exits.filter { it.requiresAttention }.maxOfOrNull { it.timestampMillis },
                    javaCrashTimestampMillis,
                )

        val hasUnacknowledgedFailure: Boolean
            get() = attentionTimestampMillis?.let { it > acknowledgedTimestampMillis } == true

        val latestFailureLabel: String?
            get() {
                val latestExit =
                    exits
                        .filter { it.requiresAttention }
                        .maxByOrNull { it.timestampMillis }
                return if (
                    javaCrashTimestampMillis != null &&
                    javaCrashTimestampMillis > (latestExit?.timestampMillis ?: Long.MIN_VALUE)
                ) {
                    "java-crash"
                } else {
                    latestExit?.reason
                }
            }
    }

    fun install(context: Context) {
        if (!installed.compareAndSet(false, true)) return
        val applicationContext = context.applicationContext
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching { writeJavaCrash(applicationContext, thread, error) }
            if (previous != null) {
                previous.uncaughtException(thread, error)
            } else {
                android.os.Process.killProcess(android.os.Process.myPid())
                kotlin.system.exitProcess(10)
            }
        }
    }

    fun collect(context: Context): Snapshot {
        val javaCrashFile = File(context.filesDir, JAVA_CRASH_FILE)
        val javaCrash = readBoundedText(javaCrashFile, MAX_JAVA_CRASH_BYTES)
        val javaCrashTimestamp =
            javaCrash
                ?.lineSequence()
                ?.firstOrNull { it.startsWith("timestamp_ms=") }
                ?.substringAfter('=')
                ?.toLongOrNull()
        return Snapshot(
            exits = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) collectExits(context) else emptyList(),
            javaCrash = javaCrash,
            javaCrashTimestampMillis = javaCrashTimestamp,
            recentEvents = readBoundedText(File(context.filesDir, EVENT_FILE), MAX_EVENT_BYTES),
            acknowledgedTimestampMillis =
                context
                    .getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
                    .getLong(KEY_ACKNOWLEDGED_TIMESTAMP, 0),
        )
    }

    fun acknowledge(
        context: Context,
        timestampMillis: Long?,
    ) {
        if (timestampMillis == null) return
        context
            .getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            .edit()
            .putLong(KEY_ACKNOWLEDGED_TIMESTAMP, timestampMillis)
            .apply()
    }

    fun setProcessState(
        context: Context,
        state: String,
    ) {
        val value = state.filter { it.code in 0x20..0x7e }.take(MAX_PROCESS_STATE_BYTES)
        recordEvent(context, value)
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        runCatching {
            context.getSystemService(ActivityManager::class.java).setProcessStateSummary(value.toByteArray())
        }
    }

    fun recordEvent(
        context: Context,
        event: String,
    ) {
        appendEvent(
            context,
            event.filter { it.code in 0x20..0x7e }.take(MAX_EVENT_RECORD_BYTES),
        )
    }

    private fun writeJavaCrash(
        context: Context,
        thread: Thread,
        error: Throwable,
    ) {
        recordEvent(context, "java-crash")
        val stack = StringWriter()
        PrintWriter(stack).use { error.printStackTrace(it) }
        val report =
            buildString {
                appendLine("timestamp_ms=${System.currentTimeMillis()}")
                appendLine("thread=${thread.name.take(80)}")
                append(stack.toString().take(MAX_JAVA_CRASH_BYTES))
            }
        File(context.filesDir, JAVA_CRASH_FILE).writeText(report)
    }

    private fun appendEvent(
        context: Context,
        event: String,
    ) {
        if (event.isBlank()) return
        synchronized(eventFileLock) {
            runCatching {
                val file = File(context.filesDir, EVENT_FILE)
                if (file.length() > MAX_EVENT_BYTES - MAX_EVENT_RECORD_BYTES) {
                    val retained = file.readLines().takeLast(MAX_EVENT_LINES)
                    file.writeText(retained.joinToString("\n", postfix = "\n"))
                }
                file.appendText("${System.currentTimeMillis()} $event\n")
            }
        }
    }

    private fun readBoundedText(
        file: File,
        maxBytes: Int,
    ): String? {
        if (!file.isFile || file.length() !in 1..maxBytes.toLong()) return null
        return runCatching { file.readText(Charsets.UTF_8) }.getOrNull()
    }

    private fun maxOfOrNull(
        first: Long?,
        second: Long?,
    ): Long? =
        when {
            first == null -> second
            second == null -> first
            else -> maxOf(first, second)
        }

    @android.annotation.TargetApi(Build.VERSION_CODES.R)
    private fun collectExits(context: Context): List<ExitRecord> {
        val manager = context.getSystemService(ActivityManager::class.java)
        return runCatching {
            manager
                .getHistoricalProcessExitReasons(context.packageName, 0, MAX_EXIT_RECORDS)
                .map { info ->
                    val requiresAttention = info.reason in attentionReasons
                    val trace = if (requiresAttention) readTrace(info) else TraceResult(null, null)
                    ExitRecord(
                        timestampMillis = info.timestamp,
                        pid = info.pid,
                        reason = reasonName(info.reason),
                        status = info.status,
                        importance = info.importance,
                        pssKb = info.pss,
                        rssKb = info.rss,
                        description = info.description,
                        processState = info.processStateSummary?.decodeToString(),
                        trace = trace.text,
                        traceNote = trace.note,
                        requiresAttention = requiresAttention,
                    )
                }
        }.getOrDefault(emptyList())
    }

    @android.annotation.TargetApi(Build.VERSION_CODES.R)
    private fun readTrace(info: ApplicationExitInfo): TraceResult {
        val nativeCrash = info.reason == ApplicationExitInfo.REASON_CRASH_NATIVE
        val maxBytes = if (nativeCrash) MAX_NATIVE_TRACE_BYTES else MAX_TRACE_BYTES
        val bytes =
            runCatching {
                info.traceInputStream?.use { input ->
                    val output = ByteArrayOutputStream()
                    val buffer = ByteArray(8192)
                    while (output.size() <= maxBytes) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        output.write(buffer, 0, count)
                    }
                    output.toByteArray()
                }
            }.getOrNull() ?: return TraceResult(null, "No system trace was retained for this exit.")
        val truncated = bytes.size > maxBytes
        val bounded = if (truncated) bytes.copyOf(maxBytes) else bytes
        if (nativeCrash) {
            val summary = NativeTombstone.summarize(bounded)
            return TraceResult(
                summary,
                when {
                    summary == null -> "The retained native tombstone could not be decoded."
                    truncated -> "Sanitized native tombstone; input was truncated to 2 MiB."
                    else -> "Sanitized native tombstone; paths, registers, memory, logs and file descriptors omitted."
                },
            )
        }
        if (bounded.any { it == 0.toByte() }) {
            return TraceResult(null, "The retained system trace is binary and was excluded for privacy.")
        }
        val decoder =
            Charsets.UTF_8
                .newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
        val text = runCatching { decoder.decode(java.nio.ByteBuffer.wrap(bounded)).toString() }.getOrNull()
        return if (text == null) {
            TraceResult(null, "The retained system trace is not valid text and was excluded for privacy.")
        } else {
            TraceResult(text, if (truncated) "System trace truncated to 256 KiB." else null)
        }
    }

    @android.annotation.TargetApi(Build.VERSION_CODES.R)
    private fun reasonName(reason: Int): String =
        when (reason) {
            ApplicationExitInfo.REASON_EXIT_SELF -> "normal-exit"
            ApplicationExitInfo.REASON_SIGNALED -> "signal"
            ApplicationExitInfo.REASON_LOW_MEMORY -> "low-memory"
            ApplicationExitInfo.REASON_CRASH -> "java-crash"
            ApplicationExitInfo.REASON_CRASH_NATIVE -> "native-crash"
            ApplicationExitInfo.REASON_ANR -> "anr"
            ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "initialization-failure"
            ApplicationExitInfo.REASON_PERMISSION_CHANGE -> "permission-change"
            ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "excessive-resource-use"
            ApplicationExitInfo.REASON_USER_REQUESTED -> "user-requested"
            ApplicationExitInfo.REASON_USER_STOPPED -> "user-stopped"
            ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "dependency-died"
            ApplicationExitInfo.REASON_OTHER -> "other"
            ApplicationExitInfo.REASON_FREEZER -> "freezer"
            ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE -> "package-state-change"
            ApplicationExitInfo.REASON_PACKAGE_UPDATED -> "package-updated"
            else -> "unknown-$reason"
        }

    private data class TraceResult(
        val text: String?,
        val note: String?,
    )

    private val installed = AtomicBoolean()
    private val eventFileLock = Any()

    @android.annotation.TargetApi(Build.VERSION_CODES.R)
    private val attentionReasons =
        setOf(
            ApplicationExitInfo.REASON_SIGNALED,
            ApplicationExitInfo.REASON_LOW_MEMORY,
            ApplicationExitInfo.REASON_CRASH,
            ApplicationExitInfo.REASON_CRASH_NATIVE,
            ApplicationExitInfo.REASON_ANR,
            ApplicationExitInfo.REASON_INITIALIZATION_FAILURE,
            ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE,
            ApplicationExitInfo.REASON_DEPENDENCY_DIED,
        )

    private const val PREFERENCES = "crash-diagnostics"
    private const val KEY_ACKNOWLEDGED_TIMESTAMP = "acknowledged-timestamp"
    private const val JAVA_CRASH_FILE = "last-java-crash.txt"
    private const val EVENT_FILE = "diagnostic-events.txt"
    private const val MAX_EXIT_RECORDS = 5
    private const val MAX_TRACE_BYTES = 256 * 1024
    private const val MAX_NATIVE_TRACE_BYTES = 2 * 1024 * 1024
    private const val MAX_JAVA_CRASH_BYTES = 64 * 1024
    private const val MAX_EVENT_BYTES = 32 * 1024
    private const val MAX_EVENT_RECORD_BYTES = 256
    private const val MAX_EVENT_LINES = 160
    private const val MAX_PROCESS_STATE_BYTES = 128
}
