package dev.producdevity.codboz

import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream

class NativeTombstoneTest {
    @Test
    fun summarizesSignalAndCrashingThreadWithoutPrivateTombstoneData() {
        val signal =
            message(
                varintField(1, 7),
                stringField(2, "SIGBUS"),
                varintField(3, 1),
                stringField(4, "BUS_ADRALN"),
                varintField(8, 1),
                varintField(9, 0x1234),
            )
        val frame =
            message(
                varintField(1, 0x2468),
                varintField(2, 0x11223344),
                stringField(4, "codboz::Runtime::dispatch"),
                varintField(5, 12),
                stringField(6, "/data/app/private-id/lib/arm64/libcodboz_runtime.so"),
                stringField(8, "aabbccdd"),
            )
        val thread = message(messageField(4, frame), stringField(5, "private memory"))
        val threadEntry = message(varintField(1, 42), messageField(2, thread))
        val tombstone =
            message(
                stringField(2, "private build fingerprint"),
                varintField(6, 42),
                messageField(10, signal),
                messageField(16, threadEntry),
                stringField(18, "private log buffer"),
            )

        val summary = requireNotNull(NativeTombstone.summarize(tombstone))

        assertTrue(summary.contains("SIGBUS (7)"))
        assertTrue(summary.contains("BUS_ADRALN (1)"))
        assertTrue(summary.contains("0x0000000000001234"))
        assertTrue(summary.contains("pc 0x0000000000002468 libcodboz_runtime.so"))
        assertTrue(summary.contains("codboz::Runtime::dispatch+12"))
        assertTrue(summary.contains("build aabbccdd"))
        assertFalse(summary.contains("/data/app"))
        assertFalse(summary.contains("private"))
    }

    @Test
    fun rejectsMalformedProtobuf() {
        assertNull(NativeTombstone.summarize(byteArrayOf(0x52, 0x80.toByte())))
    }

    private fun varintField(
        number: Int,
        value: Long,
    ): ByteArray = message(varint((number shl 3).toLong()), varint(value))

    private fun stringField(
        number: Int,
        value: String,
    ): ByteArray = messageField(number, value.toByteArray())

    private fun messageField(
        number: Int,
        value: ByteArray,
    ): ByteArray = message(varint(((number shl 3) or 2).toLong()), varint(value.size.toLong()), value)

    private fun varint(value: Long): ByteArray {
        var remaining = value
        val output = ByteArrayOutputStream()
        while (true) {
            val byte = (remaining and 0x7f).toInt()
            remaining = remaining ushr 7
            if (remaining == 0L) {
                output.write(byte)
                return output.toByteArray()
            }
            output.write(byte or 0x80)
        }
    }

    private fun message(vararg values: ByteArray): ByteArray =
        ByteArrayOutputStream().run {
            values.forEach { write(it) }
            toByteArray()
        }
}
