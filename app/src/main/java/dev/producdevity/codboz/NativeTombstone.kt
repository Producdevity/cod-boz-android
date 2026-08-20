package dev.producdevity.codboz

internal object NativeTombstone {
    fun summarize(data: ByteArray): String? =
        runCatching {
            val reader = ProtoReader(data)
            var crashingThread = 0L
            var signal: Signal? = null
            val threads = mutableListOf<Thread>()
            while (true) {
                val field = reader.nextField() ?: break
                when (field.number) {
                    6 -> {
                        crashingThread = reader.readVarint(field)
                    }

                    10 -> {
                        signal = parseSignal(reader.readMessage(field))
                    }

                    16 -> {
                        val thread = parseThreadEntry(reader.readMessage(field))
                        threads += thread
                        if (signal != null && thread.id == crashingThread) break
                    }

                    else -> {
                        reader.skip(field)
                    }
                }
            }

            val frames = threads.firstOrNull { it.id == crashingThread }?.frames.orEmpty()
            if (signal == null && frames.isEmpty()) return@runCatching null
            buildString {
                signal?.let {
                    append("signal          ")
                    append(it.name.ifBlank { "unknown" })
                    append(" (")
                    append(it.number)
                    appendLine(")")
                    if (it.code != 0 || it.codeName.isNotBlank()) {
                        append("signal code     ")
                        append(it.codeName.ifBlank { "unknown" })
                        append(" (")
                        append(it.code)
                        appendLine(")")
                    }
                    it.faultAddress?.let { address ->
                        appendLine("fault address   ${hex(address)}")
                    }
                }
                if (frames.isNotEmpty()) {
                    appendLine("native backtrace")
                    frames.take(MAX_FRAMES).forEachIndexed { index, frame ->
                        val address = if (frame.fileName.isBlank()) frame.pc else frame.relativePc
                        append("#${index.toString().padStart(2, '0')} pc ${hex(address)}")
                        frame.fileName.takeIf { it.isNotBlank() }?.let { append(" $it") }
                        frame.functionName.takeIf { it.isNotBlank() }?.let {
                            append(" ($it")
                            if (frame.functionOffset != 0L) append("+${frame.functionOffset}")
                            append(')')
                        }
                        frame.buildId.takeIf { it.isNotBlank() }?.let { append(" build $it") }
                        appendLine()
                    }
                }
            }.trimEnd()
        }.getOrNull()

    private fun parseSignal(reader: ProtoReader): Signal {
        var number = 0
        var name = ""
        var code = 0
        var codeName = ""
        var hasFaultAddress = false
        var faultAddress = 0L
        while (true) {
            val field = reader.nextField() ?: break
            when (field.number) {
                1 -> number = reader.readVarint(field).toInt()
                2 -> name = safeText(reader.readString(field), MAX_LABEL_LENGTH)
                3 -> code = reader.readVarint(field).toInt()
                4 -> codeName = safeText(reader.readString(field), MAX_LABEL_LENGTH)
                8 -> hasFaultAddress = reader.readVarint(field) != 0L
                9 -> faultAddress = reader.readVarint(field)
                else -> reader.skip(field)
            }
        }
        return Signal(number, name, code, codeName, faultAddress.takeIf { hasFaultAddress })
    }

    private fun parseThreadEntry(reader: ProtoReader): Thread {
        var id = 0L
        var frames = emptyList<Frame>()
        while (true) {
            val field = reader.nextField() ?: break
            when (field.number) {
                1 -> id = reader.readVarint(field)
                2 -> frames = parseThread(reader.readMessage(field))
                else -> reader.skip(field)
            }
        }
        return Thread(id, frames)
    }

    private fun parseThread(reader: ProtoReader): List<Frame> {
        val frames = mutableListOf<Frame>()
        while (true) {
            val field = reader.nextField() ?: break
            if (field.number == 4 && frames.size < MAX_FRAMES) {
                frames += parseFrame(reader.readMessage(field))
            } else {
                reader.skip(field)
            }
        }
        return frames
    }

    private fun parseFrame(reader: ProtoReader): Frame {
        var relativePc = 0L
        var pc = 0L
        var functionName = ""
        var functionOffset = 0L
        var fileName = ""
        var buildId = ""
        while (true) {
            val field = reader.nextField() ?: break
            when (field.number) {
                1 -> relativePc = reader.readVarint(field)
                2 -> pc = reader.readVarint(field)
                4 -> functionName = safeText(reader.readString(field), MAX_FUNCTION_LENGTH)
                5 -> functionOffset = reader.readVarint(field)
                6 -> fileName = safeFileName(reader.readString(field))
                8 -> buildId = safeBuildId(reader.readString(field))
                else -> reader.skip(field)
            }
        }
        return Frame(relativePc, pc, functionName, functionOffset, fileName, buildId)
    }

    private fun safeText(
        value: String,
        limit: Int,
    ): String = value.filter { it.code in 0x20..0x7e }.take(limit)

    private fun safeFileName(value: String): String =
        safeText(value, MAX_PATH_LENGTH)
            .replace('\\', '/')
            .substringAfterLast('/')
            .take(MAX_LABEL_LENGTH)

    private fun safeBuildId(value: String): String =
        value.filter { it in '0'..'9' || it.lowercaseChar() in 'a'..'f' }.take(MAX_BUILD_ID_LENGTH)

    private fun hex(value: Long): String = "0x${java.lang.Long.toUnsignedString(value, 16).padStart(16, '0')}"

    private data class Signal(
        val number: Int,
        val name: String,
        val code: Int,
        val codeName: String,
        val faultAddress: Long?,
    )

    private data class Thread(
        val id: Long,
        val frames: List<Frame>,
    )

    private data class Frame(
        val relativePc: Long,
        val pc: Long,
        val functionName: String,
        val functionOffset: Long,
        val fileName: String,
        val buildId: String,
    )

    private data class ProtoField(
        val number: Int,
        val wireType: Int,
    )

    private class ProtoReader(
        private val data: ByteArray,
        private var position: Int = 0,
        private val limit: Int = data.size,
    ) {
        fun nextField(): ProtoField? {
            if (position == limit) return null
            val tag = readRawVarint()
            val number = (tag ushr 3).toInt()
            val wireType = (tag and 7).toInt()
            require(number > 0)
            return ProtoField(number, wireType)
        }

        fun readVarint(field: ProtoField): Long {
            require(field.wireType == WIRE_VARINT)
            return readRawVarint()
        }

        fun readString(field: ProtoField): String {
            require(field.wireType == WIRE_LENGTH_DELIMITED)
            val length = readLength()
            val end = position + length
            require(end in position..limit)
            return data.decodeToString(position, end).also { position = end }
        }

        fun readMessage(field: ProtoField): ProtoReader {
            require(field.wireType == WIRE_LENGTH_DELIMITED)
            val length = readLength()
            val end = position + length
            require(end in position..limit)
            return ProtoReader(data, position, end).also { position = end }
        }

        fun skip(field: ProtoField) {
            when (field.wireType) {
                WIRE_VARINT -> readRawVarint()
                WIRE_FIXED64 -> advance(8)
                WIRE_LENGTH_DELIMITED -> advance(readLength())
                WIRE_FIXED32 -> advance(4)
                else -> error("unsupported protobuf wire type")
            }
        }

        private fun readLength(): Int {
            val value = readRawVarint()
            require(value in 0..Int.MAX_VALUE.toLong())
            return value.toInt()
        }

        private fun readRawVarint(): Long {
            var result = 0L
            repeat(10) { index ->
                require(position < limit)
                val value = data[position++].toInt() and 0xff
                if (index == 9) require(value <= 1)
                result = result or ((value and 0x7f).toLong() shl (index * 7))
                if (value and 0x80 == 0) return result
            }
            error("invalid protobuf varint")
        }

        private fun advance(count: Int) {
            require(count >= 0 && count <= limit - position)
            position += count
        }
    }

    private const val WIRE_VARINT = 0
    private const val WIRE_FIXED64 = 1
    private const val WIRE_LENGTH_DELIMITED = 2
    private const val WIRE_FIXED32 = 5
    private const val MAX_FRAMES = 16
    private const val MAX_LABEL_LENGTH = 96
    private const val MAX_FUNCTION_LENGTH = 160
    private const val MAX_PATH_LENGTH = 512
    private const val MAX_BUILD_ID_LENGTH = 64
}
