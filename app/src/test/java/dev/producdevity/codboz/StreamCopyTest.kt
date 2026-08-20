package dev.producdevity.codboz

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream

class StreamCopyTest {
    @Test
    fun copiesUpToLimit() {
        val source = byteArrayOf(1, 2, 3, 4)
        val output = ByteArrayOutputStream()

        assertEquals(4, copyWithLimit(ByteArrayInputStream(source), output, source.size.toLong()))
        assertArrayEquals(source, output.toByteArray())
    }

    @Test
    fun rejectsInputLargerThanLimit() {
        val output = ByteArrayOutputStream()

        assertThrows(IllegalArgumentException::class.java) {
            copyWithLimit(ByteArrayInputStream(ByteArray(9)), output, 8)
        }
        assertEquals(0, output.size())
    }
}
