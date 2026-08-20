package dev.producdevity.codboz

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files

class ResourceFilesTest {
    @Test
    fun clearsOnlyDownloadedResources() {
        val gameDir = Files.createTempDirectory("codboz-resource-files").toFile()
        try {
            val assets = gameDir.resolve("assets").apply { mkdirs() }
            val archive = ResourceArchive("resources.dz", 1, "a".repeat(64))
            val importedGuest = assets.resolve("boz.s3e").apply { writeText("guest") }
            val save = gameDir.resolve("save").apply { writeText("progress") }
            val resourceManifest = gameDir.resolve("resource-manifest.json").apply { writeText("manifest") }
            val complete = assets.resolve(archive.name).apply { writeText("complete") }
            val partial = assets.resolve("${archive.name}.part").apply { writeText("partial") }

            ResourceFiles.clearDownloads(gameDir, listOf(archive))

            assertFalse(resourceManifest.exists())
            assertFalse(complete.exists())
            assertFalse(partial.exists())
            assertTrue(importedGuest.exists())
            assertTrue(save.exists())
        } finally {
            gameDir.deleteRecursively()
        }
    }
}
