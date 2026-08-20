package dev.producdevity.codboz

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class ConfigDocumentTest {
    @Test
    fun defaultConfigIsValidAndComplete() {
        assertEquals(ConfigDocument.DEFAULT, ConfigDocument.normalize(ConfigDocument.DEFAULT))
        val values = ConfigDocument.values(ConfigDocument.DEFAULT)
        assertEquals("Player", values["player_name"])
        assertEquals(ConfigDocument.DEFAULT_SERVER, values["multiplayer_server"])
        assertEquals("0", values["voice_chat_enabled"])
    }

    @Test
    fun normalizeUsesOnlySupportedSettings() {
        val imported = "# personal settings\ncustom_option=kept\nplayer_name=Richtofen\n"
        val normalized = ConfigDocument.normalize(imported)

        assertFalse(ConfigDocument.values(normalized).containsKey("custom_option"))
        assertEquals("Richtofen", ConfigDocument.values(normalized)["player_name"])
        assertEquals(ConfigDocument.DEFAULT_SERVER, ConfigDocument.values(normalized)["multiplayer_server"])
        assertEquals("0", ConfigDocument.values(normalized)["voice_chat_enabled"])
    }

    @Test
    fun voiceChatAcceptsOnlyBooleanValues() {
        assertEquals(
            "1",
            ConfigDocument.values(ConfigDocument.normalize("voice_chat_enabled=1\n"))["voice_chat_enabled"],
        )
        assertThrows(IllegalArgumentException::class.java) {
            ConfigDocument.normalize("voice_chat_enabled=true\n")
        }
    }

    @Test
    fun invalidKnownValuesAndDuplicatesAreRejected() {
        assertThrows(IllegalArgumentException::class.java) {
            ConfigDocument.normalize("player_name=Player\nplayer_name=Other\n")
        }
        assertThrows(IllegalArgumentException::class.java) {
            ConfigDocument.normalize("multiplayer_server=https://example.org\n")
        }
    }

    @Test
    fun serverValidationMatchesRuntimeContract() {
        assertTrue(ConfigDocument.validMultiplayerServer("boz-online.xubi.org"))
        assertTrue(ConfigDocument.validMultiplayerServer("192.0.2.10"))
        assertTrue(ConfigDocument.validMultiplayerServer(""))
        assertFalse(ConfigDocument.validMultiplayerServer("192.0.2.999"))
        assertFalse(ConfigDocument.validMultiplayerServer("192.168.001.1"))
        assertFalse(ConfigDocument.validMultiplayerServer("server.example.org:3074"))
        assertFalse(ConfigDocument.validMultiplayerServer("-server.example.org"))
    }
}
