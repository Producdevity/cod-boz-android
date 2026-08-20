package dev.producdevity.codboz

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class SourceProfileTest {
    @Test
    fun parsesResourceMetadata() {
        val profile =
            SourceProfile.create(
                "https://example.invalid/game/",
                listOf(
                    ResourceArchive(
                        "resources.dz",
                        123,
                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    ),
                ),
            )

        assertEquals("https://example.invalid/game/", profile.resourceBaseUrl)
        assertEquals(123L, profile.totalResourceSize)
        assertEquals("resources.dz", profile.archives.single().name)
    }

    @Test
    fun rejectsUnsafeArchiveNamesAndIncompleteUrls() {
        assertThrows(IllegalArgumentException::class.java) {
            SourceProfile.create(
                "example.invalid/game",
                listOf(
                    ResourceArchive(
                        "../resources.dz",
                        123,
                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    ),
                ),
            )
        }
    }
}
