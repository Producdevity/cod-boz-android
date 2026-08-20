// apk_inspect: hash an APK, verify it against the known source hash, list its
// ZIP entries, locate the game payloads, decompress boz.s3e, and report the
// S3E header. No game code is executed.
#include "loader/apk_reader.hpp"
#include "loader/bytes.hpp"
#include "loader/file_io.hpp"
#include "loader/lzma.hpp"
#include "loader/s3e_image.hpp"
#include "loader/sha256.hpp"
#include "loader/source_profile.hpp"
#include "bridge/import_inventory.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>

namespace {

constexpr uint32_t kZipLocalSig = 0x04034b50u;

void printJsonString(std::ostream &output, std::string_view value) {
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
                output << escaped;
            } else {
                output << character;
            }
        }
    }
    output << '"';
}

std::size_t localDataOffset(std::span<const uint8_t> data, const codboz::ZipEntry &e) {
    codboz::Reader r(data);
    r.seek(e.localHeaderOffset);
    if (r.readLe32() != kZipLocalSig) {
        throw std::runtime_error("invalid ZIP local file header");
    }
    r.readLe16(); // version
    r.readLe16(); // flags
    r.readLe16(); // method
    r.readLe16(); // time
    r.readLe16(); // date
    r.readLe32(); // crc
    r.readLe32(); // compressed size
    r.readLe32(); // uncompressed size
    uint16_t nameLen = r.readLe16();
    uint16_t extraLen = r.readLe16();
    return e.localHeaderOffset + 30u + nameLen + extraLen;
}

void printS3eSummary(std::span<const uint8_t> data) {
    auto inflated = codboz::lzmaDecompress(data);
    const auto packedHash = codboz::sha256Hex(data);
    const auto unpackedHash = codboz::sha256Hex(inflated);
    codboz::S3eImage image(inflated);
    const auto &h = image.header();
    auto fix = image.parseFixups();
    const uint32_t codeSize = h.dataOffset == 0 ? h.codeFileSize : h.dataOffset;
    std::cout << "  \"s3e\": {\n";
    std::cout << "    \"packed_size\": " << data.size() << ",\n";
    std::cout << "    \"packed_sha256\": \"" << packedHash.data() << "\",\n";
    std::cout << "    \"unpacked_size\": " << inflated.size() << ",\n";
    std::cout << "    \"unpacked_sha256\": \"" << unpackedHash.data() << "\",\n";
    std::cout << "    \"version\": " << h.version << ",\n";
    std::cout << "    \"arch\": " << h.arch << ",\n";
    std::cout << "    \"architecture\": ";
    printJsonString(std::cout, codboz::guestArchitectureName(h.arch));
    std::cout << ",\n";
    std::cout << "    \"entry_offset\": " << h.entryOffset << ",\n";
    std::cout << "    \"base_addr_orig\": " << h.baseAddrOrig << ",\n";
    std::cout << "    \"sections\": {\n";
    std::cout << "      \"code_size\": " << codeSize << ",\n";
    std::cout << "      \"initialized_data_size\": " << h.codeFileSize - codeSize << ",\n";
    std::cout << "      \"code_mem_size\": " << h.codeMemSize << ",\n";
    std::cout << "      \"bss_size\": " << h.codeMemSize - h.codeFileSize << ",\n";
    std::cout << "      \"data_offset\": " << h.dataOffset << "\n";
    std::cout << "    },\n";
    std::cout << "    \"symbols\": " << fix.symbols.size() << ",\n";
    std::cout << "    \"imports\": [";
    for (std::size_t index = 0; index < fix.symbols.size(); ++index) {
        if (index) {
            std::cout << ", ";
        }
        printJsonString(std::cout, fix.symbols[index]);
    }
    std::cout << "],\n";
    std::cout << "    \"imports_match_supported_inventory\": "
              << (codboz::matchesSourceImportInventory(fix.symbols) ? "true" : "false") << ",\n";
    std::cout << "    \"internal_relocs\": " << fix.internalRelocCount << ",\n";
    std::cout << "    \"external_relocs\": " << fix.externalRelocCount << ",\n";
    std::cout << "    \"external_reloc_types\": {";
    bool firstType = true;
    for (const auto &[type, count] : fix.externalRelocTypes) {
        if (!firstType) {
            std::cout << ", ";
        }
        std::cout << '"' << type << "\": " << count;
        firstType = false;
    }
    std::cout << "},\n";
    std::cout << "    \"extension_hashes\": [";
    bool firstExtension = true;
    for (const auto &extension : codboz::kExtensionProfiles) {
        const uint32_t occurrences = codboz::countLittleEndianWord(inflated, extension.hash);
        if (occurrences == 0) {
            continue;
        }
        if (!firstExtension) {
            std::cout << ", ";
        }
        char hash[9];
        std::snprintf(hash, sizeof(hash), "%08x", extension.hash);
        std::cout << "{\"name\": ";
        printJsonString(std::cout, extension.name);
        std::cout << ", \"hash\": \"" << hash
                  << "\", \"little_endian_occurrences\": " << occurrences << '}';
        firstExtension = false;
    }
    std::cout << "],\n";
    std::cout << "    \"extension_hash_detection\": \"little-endian constant scan\",\n";
    std::cout << "    \"resource_base_url\": ";
    printJsonString(std::cout, codboz::findSupportedResourceBase(inflated));
    std::cout << ",\n";
    std::cout << "    \"resource_archives\": [";
    for (std::size_t index = 0; index < codboz::kResourceArchives.size(); ++index) {
        const auto &archive = codboz::kResourceArchives[index];
        if (index) {
            std::cout << ", ";
        }
        std::cout << "{\"name\": ";
        printJsonString(std::cout, archive.name);
        std::cout << ", \"size\": " << archive.size << ", \"sha256\": \"" << archive.sha256
                  << "\", \"guest_string_occurrences\": "
                  << codboz::countBytes(inflated, archive.name) << '}';
    }
    std::cout << "]\n";
    std::cout << "  }\n";
}

int run(const char *path) {
    auto raw = codboz::readFile(path);
    auto hash = codboz::sha256Hex(raw);
    bool known = std::string_view(hash.data()) == codboz::kSupportedApkSha256;

    codboz::ApkReader apk(raw);
    std::size_t assetCount = 0;
    for (const auto &e : apk.entries()) {
        if (codboz::isSafeAssetName(e.name)) {
            ++assetCount;
        }
    }
    const auto *s3e = apk.find("assets/boz.s3e");
    const auto *loader = apk.find("assets/blackops_loader.dz");

    std::cout << "{\n";
    std::cout << "  \"file\": ";
    printJsonString(std::cout, path);
    std::cout << ",\n";
    std::cout << "  \"size\": " << raw.size() << ",\n";
    std::cout << "  \"sha256\": \"" << hash.data() << "\",\n";
    std::cout << "  \"sha256_matches_known\": " << (known ? "true" : "false") << ",\n";
    std::cout << "  \"expected_source\": {\n";
    std::cout << "    \"package\": ";
    printJsonString(std::cout, codboz::kSupportedPackage);
    std::cout << ",\n";
    std::cout << "    \"version_code\": " << codboz::kSupportedVersionCode << ",\n";
    std::cout << "    \"version_name\": ";
    printJsonString(std::cout, codboz::kSupportedVersionName);
    std::cout << ",\n";
    std::cout << "    \"min_sdk\": " << codboz::kSupportedMinSdk << ",\n";
    std::cout << "    \"target_sdk\": " << codboz::kSupportedTargetSdk << "\n";
    std::cout << "  },\n";
    std::cout << "  \"entry_count\": " << apk.entries().size() << ",\n";
    std::cout << "  \"asset_count\": " << assetCount << ",\n";
    std::cout << "  \"has_boz_s3e\": " << (s3e ? "true" : "false") << ",\n";
    std::cout << "  \"has_blackops_loader_dz\": " << (loader ? "true" : "false");
    if (s3e) {
        std::cout << ",\n";
        auto off = localDataOffset(raw, *s3e);
        std::span<const uint8_t> s3eData(raw.data() + off, s3e->compressedSize);
        printS3eSummary(s3eData);
    } else {
        std::cout << "\n";
    }
    std::cout << "}\n";
    return known ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <game.apk>\n", argv[0]);
        return 2;
    }
    try {
        return run(argv[1]);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "apk_inspect: %s\n", e.what());
        return 1;
    }
}
