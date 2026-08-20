package dev.producdevity.codboz

import android.util.AtomicFile
import java.io.File

class ConfigStore(
    private val configDir: File,
) {
    private val file get() = File(configDir, "config.txt")

    val path: String get() = file.absolutePath

    fun ensureDefault() {
        configDir.mkdirs()
        if (!file.exists()) {
            write(ConfigDocument.DEFAULT)
            return
        }
        val current = file.readText()
        val normalized = ConfigDocument.normalize(current)
        if (normalized != current) write(normalized)
    }

    fun read(): Map<String, String> {
        if (!file.exists()) return emptyMap()
        return ConfigDocument.values(file.readText())
    }

    fun text(): String {
        ensureDefault()
        return file.readText()
    }

    fun update(changes: Map<String, String>) {
        if (!file.exists()) {
            ensureDefault()
        }
        val lines = file.readLines().toMutableList()
        val handled = HashSet<String>()
        for (i in lines.indices) {
            val raw = lines[i].trim()
            if (raw.isEmpty() || raw.startsWith("#") || '=' !in raw) continue
            val key = raw.split('=', limit = 2)[0].trim()
            if (key in changes) {
                lines[i] = "$key=${changes[key]}"
                handled += key
            }
        }
        for ((k, v) in changes) {
            if (k !in handled) lines += "$k=$v"
        }
        write(ConfigDocument.normalize(lines.joinToString("\n", postfix = "\n")))
    }

    fun replace(content: String) = write(ConfigDocument.normalize(content))

    private fun write(content: String) {
        configDir.mkdirs()
        val atomic = AtomicFile(file)
        val output = atomic.startWrite()
        try {
            output.writer(Charsets.UTF_8).apply {
                write(content)
                flush()
            }
            atomic.finishWrite(output)
        } catch (error: Throwable) {
            atomic.failWrite(output)
            throw error
        }
    }
}
