package dev.producdevity.codboz

import java.io.InputStream
import java.io.OutputStream

internal fun copyWithLimit(
    input: InputStream,
    output: OutputStream,
    maxBytes: Long,
): Long {
    require(maxBytes >= 0)
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    var total = 0L
    while (true) {
        val count = input.read(buffer)
        if (count < 0) return total
        if (count == 0) continue
        require(total <= maxBytes - count) { "Selected APK is larger than 256 MiB" }
        output.write(buffer, 0, count)
        total += count
    }
}
