#include "importer.hpp"

#include "apk_reader.hpp"
#include "bytes.hpp"
#include "crc32.hpp"
#include "file_io.hpp"
#include "lzma.hpp"
#include "s3e_image.hpp"
#include "sha256.hpp"
#include "source_profile.hpp"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace codboz {

namespace {
constexpr uint16_t kFlagEncrypted = 0x0001u;
constexpr uint16_t kMethodStored = 0u;
std::recursive_mutex gImportMutex;

std::string join(const std::string &base, const std::string &name) {
    return (std::filesystem::path(base) / name).string();
}

std::string jsonEscape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char c : value) {
        switch (c) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += hex[c >> 4];
                escaped += hex[c & 0xf];
            } else {
                escaped += static_cast<char>(c);
            }
        }
    }
    return escaped;
}

void removeTree(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        throw std::runtime_error("cannot remove stale import directory");
    }
}

std::vector<std::filesystem::path> resourcePaths() {
    std::vector<std::filesystem::path> paths;
    paths.reserve(kResourceArchives.size() + 1);
    for (const auto &archive : kResourceArchives) {
        paths.emplace_back(std::filesystem::path("assets") / archive.name);
    }
    paths.emplace_back("resource-manifest.json");
    return paths;
}

void makeReadOnly(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
        throw std::runtime_error("cannot secure imported game data");
    }
}

void recoverInterruptedResources(const std::filesystem::path &staging,
                                 const std::filesystem::path &previous) {
    std::error_code ec;
    for (const auto &relative : resourcePaths()) {
        const auto source = staging / relative;
        const auto destination = previous / relative;
        if (!std::filesystem::is_regular_file(source, ec)) {
            ec.clear();
            continue;
        }
        if (std::filesystem::exists(destination, ec)) {
            continue;
        }
        std::filesystem::rename(source, destination, ec);
        if (ec) {
            throw std::runtime_error("cannot recover downloaded resource archives");
        }
    }
}

void recoverInterruptedImport(const std::filesystem::path &target,
                              const std::filesystem::path &staging,
                              const std::filesystem::path &previous) {
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        if (std::filesystem::exists(previous, ec)) {
            removeTree(previous);
        }
        if (std::filesystem::exists(staging, ec)) {
            removeTree(staging);
        }
        return;
    }
    if (!std::filesystem::exists(previous, ec)) {
        if (std::filesystem::exists(staging, ec)) {
            removeTree(staging);
        }
        return;
    }
    recoverInterruptedResources(staging, previous);
    std::filesystem::rename(previous, target, ec);
    if (ec) {
        throw std::runtime_error("cannot recover the previous import");
    }
    if (std::filesystem::exists(staging, ec)) {
        removeTree(staging);
    }
}

void restoreResourceArchives(const std::filesystem::path &sourceRoot,
                             const std::filesystem::path &destinationRoot,
                             const std::vector<std::filesystem::path> &moved);

std::vector<std::filesystem::path>
moveResourceArchives(const std::filesystem::path &sourceRoot,
                     const std::filesystem::path &destinationRoot) {
    std::vector<std::filesystem::path> moved;
    if (!isPrepared(sourceRoot.string())) {
        return moved;
    }
    std::error_code ec;
    for (const auto &archive : kResourceArchives) {
        const auto relative = std::filesystem::path("assets") / archive.name;
        const auto source = sourceRoot / relative;
        if (!std::filesystem::is_regular_file(source, ec)) {
            ec.clear();
            continue;
        }
        if (std::filesystem::file_size(source, ec) != archive.size || ec) {
            ec.clear();
            continue;
        }
        std::filesystem::rename(source, destinationRoot / relative, ec);
        if (ec) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
                std::error_code ignored;
                std::filesystem::rename(destinationRoot / *it, sourceRoot / *it, ignored);
            }
            throw std::runtime_error("cannot preserve downloaded resource archives");
        }
        moved.push_back(relative);
    }
    const auto resourceManifest = sourceRoot / "resource-manifest.json";
    if (std::filesystem::is_regular_file(resourceManifest, ec)) {
        const auto relative = std::filesystem::path("resource-manifest.json");
        std::filesystem::rename(resourceManifest, destinationRoot / relative, ec);
        if (ec) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
                std::error_code ignored;
                std::filesystem::rename(destinationRoot / *it, sourceRoot / *it, ignored);
            }
            throw std::runtime_error("cannot preserve the resource manifest");
        }
        moved.push_back(relative);
    }
    try {
        for (const auto &relative : moved) {
            makeReadOnly(destinationRoot / relative);
        }
    } catch (...) {
        restoreResourceArchives(destinationRoot, sourceRoot, moved);
        throw;
    }
    return moved;
}

void restoreResourceArchives(const std::filesystem::path &sourceRoot,
                             const std::filesystem::path &destinationRoot,
                             const std::vector<std::filesystem::path> &moved) {
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
        std::error_code ec;
        std::filesystem::rename(sourceRoot / *it, destinationRoot / *it, ec);
        if (ec) {
            throw std::runtime_error("cannot restore downloaded resource archives");
        }
    }
}
} // namespace

std::string manifestPath(const std::string &gameDir) {
    return join(gameDir, "import-manifest.json");
}

bool isImported(const std::string &gameDir) {
    std::lock_guard lock(gImportMutex);
    try {
        recoverInterruptedImport(gameDir, gameDir + ".importing", gameDir + ".previous");
        std::error_code ec;
        const auto manifest = std::filesystem::path(manifestPath(gameDir));
        const auto packed = std::filesystem::path(join(gameDir, "assets/boz.s3e"));
        const auto unpacked = std::filesystem::path(join(gameDir, "assets/boz.s3e.unpacked"));
        if (!std::filesystem::is_regular_file(manifest, ec) || ec ||
            !std::filesystem::is_regular_file(packed, ec) || ec ||
            std::filesystem::file_size(packed, ec) != kSupportedGuestPackedSize || ec ||
            !std::filesystem::is_regular_file(unpacked, ec) || ec ||
            std::filesystem::file_size(unpacked, ec) != kSupportedGuestUnpackedSize || ec) {
            return false;
        }
        const auto packedHash = sha256Hex(readFile(packed.c_str()));
        const auto unpackedHash = sha256Hex(readFile(unpacked.c_str()));
        return std::string_view(packedHash.data()) == kSupportedGuestPackedSha256 &&
               std::string_view(unpackedHash.data()) == kSupportedGuestUnpackedSha256;
    } catch (const std::exception &) {
        return false;
    }
}

bool isPrepared(const std::string &gameDir) {
    std::lock_guard lock(gImportMutex);
    try {
        std::error_code ec;
        const auto resourceManifest = std::filesystem::path(gameDir) / "resource-manifest.json";
        const auto expectedManifest = resourceManifestJson();
        if (!isImported(gameDir) || !std::filesystem::is_regular_file(resourceManifest, ec) ||
            std::filesystem::file_size(resourceManifest, ec) != expectedManifest.size() || ec) {
            return false;
        }
        const auto manifest = readFile(resourceManifest.c_str());
        if (!std::equal(manifest.begin(), manifest.end(), expectedManifest.begin(),
                        expectedManifest.end())) {
            return false;
        }
        for (const auto &archive : kResourceArchives) {
            const auto path = std::filesystem::path(gameDir) / "assets" / archive.name;
            if (!std::filesystem::is_regular_file(path, ec) ||
                std::filesystem::file_size(path, ec) != archive.size || ec) {
                return false;
            }
        }
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

std::string toJson(const ImportOutcome &o) {
    std::string s = "{\n";
    s += "  \"ok\": " + std::string(o.ok ? "true" : "false") + ",\n";
    s += "  \"sha256\": \"" + jsonEscape(o.sha256Hex) + "\",\n";
    s += "  \"hash_matched\": " + std::string(o.hashMatched ? "true" : "false") + ",\n";
    s += "  \"source_version\": \"" + jsonEscape(o.sourceVersion) + "\",\n";
    s += "  \"entry_count\": " + std::to_string(o.entryCount) + ",\n";
    s += "  \"asset_count\": " + std::to_string(o.assetCount) + ",\n";
    s += "  \"s3e_symbols\": " + std::to_string(o.s3eSymbols) + ",\n";
    s += "  \"s3e_base_addr\": " + std::to_string(o.s3eBaseAddr) + ",\n";
    s += "  \"s3e_code_mem_size\": " + std::to_string(o.s3eCodeMemSize) + ",\n";
    s += "  \"s3e_unpacked_size\": " + std::to_string(o.s3eUnpackedSize) + ",\n";
    s += "  \"resource_base_url\": \"" + jsonEscape(o.resourceBaseUrl) + "\"";
    if (!o.error.empty()) {
        s += ",\n  \"error\": \"" + jsonEscape(o.error) + "\"";
    }
    s += "\n}\n";
    return s;
}

ImportOutcome importApk(const std::string &apkPath, const std::string &gameDir) {
    std::lock_guard lock(gImportMutex);
    ImportOutcome out;
    const std::filesystem::path target(gameDir);
    const std::filesystem::path staging(gameDir + ".importing");
    const std::filesystem::path previous(gameDir + ".previous");
    try {
        std::error_code ec;
        recoverInterruptedImport(target, staging, previous);
        auto raw = readFile(apkPath.c_str());
        auto hash = sha256Hex(raw);
        out.sha256Hex = hash.data();
        out.hashMatched = out.sha256Hex == kSupportedApkSha256;
        out.sourceVersion = kSupportedVersionName;
        if (!out.hashMatched) {
            out.error = "APK SHA-256 does not match the supported source (" +
                        std::string(kSupportedApkSha256.substr(0, 12)) + "...). Import refused.";
            return out;
        }

        ApkReader apk(raw);
        out.entryCount = static_cast<uint32_t>(apk.entries().size());

        const ZipEntry *s3e = nullptr;
        std::unordered_set<std::string> assetNames;
        for (const auto &e : apk.entries()) {
            if (!isSafeAssetName(e.name) || e.name.ends_with('/')) {
                continue;
            }
            if ((e.flags & kFlagEncrypted) != 0) {
                throw std::runtime_error("encrypted APK assets are not supported");
            }
            if (e.method != kMethodStored) {
                throw std::runtime_error("compressed APK assets are not supported");
            }
            if (!assetNames.insert(e.name).second) {
                throw std::runtime_error("APK contains a duplicate asset path");
            }
            ++out.assetCount;
            auto data = apk.entryData(e);
            if (crc32(data) != e.crc32) {
                out.error = "CRC mismatch extracting " + e.name;
                return out;
            }
            if (e.name == "assets/boz.s3e") {
                s3e = &e;
            }
        }

        if (s3e == nullptr) {
            out.error = "APK is missing assets/boz.s3e";
            return out;
        }

        auto inflated = lzmaDecompress(apk.entryData(*s3e));
        if (inflated.size() < 4 || readLe32(inflated.data()) != kS3eMagic) {
            out.error = "decompressed boz.s3e is not an S3E image";
            return out;
        }
        S3eImage image(inflated);
        auto fix = image.parseFixups();
        out.s3eUnpackedSize = static_cast<uint32_t>(inflated.size());
        out.s3eSymbols = static_cast<uint32_t>(fix.symbols.size());
        out.s3eBaseAddr = image.header().baseAddrOrig;
        out.s3eCodeMemSize = image.header().codeMemSize;
        out.resourceBaseUrl = findSupportedResourceBase(inflated);

        removeTree(staging);
        std::filesystem::create_directories(staging, ec);
        if (ec) {
            throw std::runtime_error("cannot create import directory");
        }
        for (const auto &e : apk.entries()) {
            if (!assetNames.contains(e.name)) {
                continue;
            }
            const auto output = staging / e.name;
            std::filesystem::create_directories(output.parent_path(), ec);
            if (ec) {
                throw std::runtime_error("cannot create asset directory");
            }
            writeFile(output.c_str(), apk.entryData(e));
            makeReadOnly(output);
        }
        writeFile((staging / "assets/boz.s3e.unpacked").c_str(), inflated);
        makeReadOnly(staging / "assets/boz.s3e.unpacked");
        out.ok = true;
        auto json = toJson(out);
        writeFile((staging / "import-manifest.json").c_str(),
                  {reinterpret_cast<const uint8_t *>(json.data()), json.size()});
        makeReadOnly(staging / "import-manifest.json");

        std::vector<std::filesystem::path> movedResources;
        if (std::filesystem::exists(target, ec)) {
            std::filesystem::rename(target, previous, ec);
            if (ec) {
                throw std::runtime_error("cannot replace the previous import");
            }
            movedResources = moveResourceArchives(previous, staging);
        }
        std::filesystem::rename(staging, target, ec);
        if (ec) {
            throw std::runtime_error("cannot activate the imported game data");
        }
        std::filesystem::remove_all(previous, ec);
    } catch (const std::exception &e) {
        out.ok = false;
        out.error = e.what();
        try {
            recoverInterruptedImport(target, staging, previous);
        } catch (const std::exception &recoveryError) {
            out.error += "; recovery pending: ";
            out.error += recoveryError.what();
        }
    }
    return out;
}

} // namespace codboz
