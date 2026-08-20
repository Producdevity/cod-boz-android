package dev.producdevity.codboz

import java.nio.ByteBuffer
import java.nio.ByteOrder

internal object LegacyStun {
    const val REQUEST_SIZE = 28
    const val RESPONSE_SIZE = 56

    fun bindingRequest(transactionId: ByteArray): ByteArray {
        require(transactionId.size == TRANSACTION_ID_SIZE)
        return ByteBuffer
            .allocate(REQUEST_SIZE)
            .order(ByteOrder.BIG_ENDIAN)
            .apply {
                putShort(BINDING_REQUEST.toShort())
                putShort(REQUEST_BODY_SIZE.toShort())
                put(transactionId)
                putShort(CHANGE_REQUEST.toShort())
                putShort(CHANGE_REQUEST_SIZE.toShort())
                putInt(0)
            }.array()
    }

    fun isBindingResponse(
        packet: ByteArray,
        length: Int,
        transactionId: ByteArray,
    ): Boolean {
        if (length != RESPONSE_SIZE || packet.size < length || transactionId.size != TRANSACTION_ID_SIZE) {
            return false
        }
        val buffer = ByteBuffer.wrap(packet, 0, length).order(ByteOrder.BIG_ENDIAN)
        if (buffer.short.toInt() and 0xffff != BINDING_RESPONSE ||
            buffer.short.toInt() and 0xffff != RESPONSE_BODY_SIZE
        ) {
            return false
        }
        val receivedTransaction = ByteArray(TRANSACTION_ID_SIZE)
        buffer.get(receivedTransaction)
        if (!receivedTransaction.contentEquals(transactionId)) return false

        return ADDRESS_ATTRIBUTES.all { expectedType ->
            val type = buffer.short.toInt() and 0xffff
            val attributeLength = buffer.short.toInt() and 0xffff
            val reserved = buffer.get().toInt() and 0xff
            val family = buffer.get().toInt() and 0xff
            val port = buffer.short.toInt() and 0xffff
            buffer.int
            type == expectedType &&
                attributeLength == ADDRESS_ATTRIBUTE_SIZE &&
                reserved == 0 &&
                family == IPV4_FAMILY &&
                port != 0
        }
    }

    private const val TRANSACTION_ID_SIZE = 16
    private const val BINDING_REQUEST = 0x0001
    private const val BINDING_RESPONSE = 0x0101
    private const val REQUEST_BODY_SIZE = 8
    private const val RESPONSE_BODY_SIZE = 36
    private const val CHANGE_REQUEST = 0x0003
    private const val CHANGE_REQUEST_SIZE = 4
    private const val ADDRESS_ATTRIBUTE_SIZE = 8
    private const val IPV4_FAMILY = 1
    private val ADDRESS_ATTRIBUTES = intArrayOf(0x0001, 0x0004, 0x0005)
}
