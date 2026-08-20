package dev.producdevity.codboz

import android.content.Context
import org.json.JSONObject

internal data class ResourceArchive(
    val name: String,
    val size: Long,
    val sha256: String,
)

internal data class SourceProfile(
    val resourceBaseUrl: String,
    val archives: List<ResourceArchive>,
) {
    val totalResourceSize: Long = archives.sumOf { it.size }

    companion object {
        private val archiveName = Regex("[A-Za-z0-9_.-]+")
        private val sha256 = Regex("[0-9a-f]{64}")

        fun load(context: Context): SourceProfile =
            parseJson(
                context.assets.open("source-profile.json").bufferedReader().use { reader ->
                    reader.readText()
                },
            )

        private fun parseJson(json: String): SourceProfile {
            val document = JSONObject(json)
            val resources = document.getJSONObject("resources")
            val archivesJson = resources.getJSONArray("archives")
            val archives =
                buildList {
                    for (index in 0 until archivesJson.length()) {
                        val archive = archivesJson.getJSONObject(index)
                        val name = archive.getString("name")
                        val size = archive.getLong("size")
                        val sha256 = archive.getString("sha256")
                        add(ResourceArchive(name, size, sha256))
                    }
                }
            val baseUrl = resources.getString("base_url")
            return create(baseUrl, archives)
        }

        internal fun create(
            resourceBaseUrl: String,
            archives: List<ResourceArchive>,
        ): SourceProfile {
            require(resourceBaseUrl.startsWith("http://") || resourceBaseUrl.startsWith("https://"))
            require(resourceBaseUrl.endsWith('/'))
            require(archives.isNotEmpty())
            require(archives.map { it.name }.distinct().size == archives.size)
            archives.forEach { archive ->
                require(archive.name.matches(archiveName))
                require(archive.size > 0)
                require(archive.sha256.matches(sha256))
            }
            return SourceProfile(resourceBaseUrl, archives)
        }
    }
}
