package dev.producdevity.codboz

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LegacyStunTest {
    @Test
    fun createsObservedBozBindingRequest() {
        val transaction = ByteArray(16) { it.toByte() }

        assertArrayEquals(
            hex("00010008000102030405060708090a0b0c0d0e0f0003000400000000"),
            LegacyStun.bindingRequest(transaction),
        )
    }

    @Test
    fun validatesExactBindingResponse() {
        val transaction = ByteArray(16) { it.toByte() }
        val response =
            hex(
                "01010024" +
                    "000102030405060708090a0b0c0d0e0f" +
                    "000100080001c3c5cb007109" +
                    "0004000800010d96c000020a" +
                    "0005000800010d97c6336414",
            )

        assertTrue(LegacyStun.isBindingResponse(response, response.size, transaction))
        assertFalse(LegacyStun.isBindingResponse(response, response.size - 1, transaction))
        assertFalse(LegacyStun.isBindingResponse(response, response.size, ByteArray(16)))
        response[0] = 0
        assertFalse(LegacyStun.isBindingResponse(response, response.size, transaction))
    }

    private fun hex(value: String): ByteArray = value.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
}
