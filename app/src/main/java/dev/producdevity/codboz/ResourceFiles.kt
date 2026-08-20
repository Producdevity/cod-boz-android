package dev.producdevity.codboz

import java.io.File

internal object ResourceFiles {
    fun clearDownloads(
        gameDir: File,
        archives: List<ResourceArchive>,
    ) {
        delete(File(gameDir, "resource-manifest.json"))
        val assets = File(gameDir, "assets")
        archives.forEach { archive ->
            delete(File(assets, archive.name))
            delete(File(assets, "${archive.name}.part"))
        }
    }

    private fun delete(file: File) {
        check(!file.exists() || file.delete()) { "Cannot remove ${file.name}" }
    }
}
