package dev.producdevity.codboz

import org.junit.Assert.assertEquals
import org.junit.Test

class CrashDiagnosticsTest {
    @Test
    fun latestFailureLabelUsesNewestTimestamp() {
        val snapshot =
            CrashDiagnostics.Snapshot(
                exits =
                    listOf(
                        CrashDiagnostics.ExitRecord(
                            timestampMillis = 100,
                            pid = 1,
                            reason = "native-crash",
                            status = 0,
                            importance = 0,
                            pssKb = 0,
                            rssKb = 0,
                            description = null,
                            processState = null,
                            trace = null,
                            traceNote = null,
                            requiresAttention = true,
                        ),
                    ),
                javaCrash = "stack",
                javaCrashTimestampMillis = 200,
                recentEvents = null,
                acknowledgedTimestampMillis = 0,
            )

        assertEquals("java-crash", snapshot.latestFailureLabel)
    }
}
