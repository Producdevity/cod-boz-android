package dev.producdevity.codboz

object ConfigDocument {
    const val MAX_PLAYER_NAME = 13
    const val MAX_SIZE = 64 * 1024
    val DEFAULT: String = BuildConfig.DEFAULT_CONFIG
    val DEFAULT_SERVER: String = values(DEFAULT).getValue("multiplayer_server")

    private val playerNameChars = Regex("[A-Za-z0-9 ._\\-]+")
    private val serverLabel = Regex("[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?")
    private val defaultValues =
        linkedMapOf(
            "multiplayer_server" to DEFAULT_SERVER,
            "player_name" to "Player",
            "voice_chat_enabled" to "0",
        )

    fun validPlayerName(name: String): Boolean {
        val trimmed = name.trim()
        return trimmed.isNotEmpty() && trimmed.length <= MAX_PLAYER_NAME && playerNameChars.matches(trimmed)
    }

    fun validMultiplayerServer(server: String): Boolean {
        val value = server.trim()
        if (value.isEmpty()) return true
        if (value.length > 253 || value.startsWith('.') || value.endsWith('.')) return false
        if (value.all { it.isDigit() || it == '.' }) {
            val octets = value.split('.')
            return octets.size == 4 &&
                octets.all {
                    it.isNotEmpty() &&
                        (it.length == 1 || !it.startsWith('0')) &&
                        it.toIntOrNull() in 0..255
                }
        }
        return value.split('.').all { serverLabel.matches(it) }
    }

    fun values(content: String): Map<String, String> {
        val values = LinkedHashMap<String, String>()
        for (raw in content.lineSequence()) {
            val clean = raw.substringBefore('#').trim()
            if (clean.isEmpty() || '=' !in clean) continue
            val (key, value) = clean.split('=', limit = 2)
            values[key.trim()] = value.trim().removeSurrounding("\"")
        }
        return values
    }

    fun normalize(content: String): String {
        require(content.toByteArray(Charsets.UTF_8).size <= MAX_SIZE) {
            "Configuration is larger than 64 KiB"
        }
        require('\u0000' !in content) { "Configuration contains a null byte" }

        val values = LinkedHashMap<String, String>()
        for ((index, raw) in content.lineSequence().withIndex()) {
            val clean = raw.substringBefore('#').trim()
            if (clean.isEmpty() || '=' !in clean) continue
            val (keyPart, valuePart) = clean.split('=', limit = 2)
            val key = keyPart.trim()
            val value = valuePart.trim().removeSurrounding("\"")
            require(key.isNotEmpty()) { "Empty key on line ${index + 1}" }
            require(key !in values) { "Duplicate key '$key' on line ${index + 1}" }
            values[key] = value
        }

        values["player_name"]?.let {
            require(validPlayerName(it)) { "Invalid player_name" }
        }
        values["multiplayer_server"]?.let {
            require(validMultiplayerServer(it)) { "Invalid multiplayer_server" }
        }
        values["voice_chat_enabled"]?.let {
            require(it == "0" || it == "1") { "Invalid voice_chat_enabled" }
        }

        return defaultValues
            .mapValues { (key, fallback) -> values[key] ?: fallback }
            .entries
            .joinToString(separator = "\n", postfix = "\n") { (key, value) -> "$key=$value" }
    }
}
