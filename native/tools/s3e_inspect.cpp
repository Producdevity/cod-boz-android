// s3e_inspect: parse an S3E image (compressed or unpacked) and emit a JSON
// report of its header, fixup metadata, symbol list, and relocation counts.
// It never executes game code.
#include "loader/bytes.hpp"
#include "loader/file_io.hpp"
#include "loader/lzma.hpp"
#include "loader/s3e_image.hpp"
#include "loader/source_profile.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

namespace {

using codboz::S3eImage;

void escape(std::ostream &os, std::string_view s) {
    os << '"';
    for (char c : s) {
        switch (c) {
        case '"':
            os << "\\\"";
            break;
        case '\\':
            os << "\\\\";
            break;
        case '\n':
            os << "\\n";
            break;
        case '\r':
            os << "\\r";
            break;
        case '\t':
            os << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                os << buf;
            } else {
                os << c;
            }
        }
    }
    os << '"';
}

void print(const S3eImage &image, bool decompressed) {
    const auto &h = image.header();
    auto fix = image.parseFixups();
    const uint32_t codeSize = h.dataOffset == 0 ? h.codeFileSize : h.dataOffset;
    std::cout << "{\n";
    std::cout << "  \"decompressed\": " << (decompressed ? "true" : "false") << ",\n";
    std::cout << "  \"file_size\": " << image.rawData().size() << ",\n";
    std::cout << "  \"header\": {\n";
    std::cout << "    \"version\": " << h.version << ",\n";
    std::cout << "    \"flags\": " << h.flags << ",\n";
    std::cout << "    \"arch\": " << h.arch << ",\n";
    std::cout << "    \"architecture\": ";
    escape(std::cout, codboz::guestArchitectureName(h.arch));
    std::cout << ",\n";
    std::cout << "    \"fixup_offset\": " << h.fixupOffset << ",\n";
    std::cout << "    \"fixup_size\": " << h.fixupSize << ",\n";
    std::cout << "    \"code_offset\": " << h.codeOffset << ",\n";
    std::cout << "    \"code_file_size\": " << h.codeFileSize << ",\n";
    std::cout << "    \"code_mem_size\": " << h.codeMemSize << ",\n";
    std::cout << "    \"code_size\": " << codeSize << ",\n";
    std::cout << "    \"initialized_data_size\": " << h.codeFileSize - codeSize << ",\n";
    std::cout << "    \"bss_size\": " << h.codeMemSize - h.codeFileSize << ",\n";
    std::cout << "    \"entry_offset\": " << h.entryOffset << ",\n";
    std::cout << "    \"config_offset\": " << h.configOffset << ",\n";
    std::cout << "    \"config_size\": " << h.configSize << ",\n";
    std::cout << "    \"base_addr_orig\": " << h.baseAddrOrig << ",\n";
    std::cout << "    \"ext_header_size\": " << h.extHeaderSize << ",\n";
    std::cout << "    \"data_offset\": " << h.dataOffset << ",\n";
    std::cout << "    \"is_juice\": " << h.isJuice << "\n";
    std::cout << "  },\n";
    std::cout << "  \"relocations\": {\n";
    std::cout << "    \"internal\": " << fix.internalRelocCount << ",\n";
    std::cout << "    \"external\": " << fix.externalRelocCount << ",\n";
    std::cout << "    \"external_import_symbols\": " << fix.externalImportSymbols << ",\n";
    std::cout << "    \"external_types\": {";
    bool firstType = true;
    for (const auto &[type, count] : fix.externalRelocTypes) {
        if (!firstType) {
            std::cout << ", ";
        }
        std::cout << '"' << type << "\": " << count;
        firstType = false;
    }
    std::cout << "}\n";
    std::cout << "  },\n";
    std::cout << "  \"symbol_count\": " << fix.symbols.size() << ",\n";
    std::cout << "  \"symbols\": [";
    for (std::size_t i = 0; i < fix.symbols.size(); ++i) {
        if (i) {
            std::cout << ", ";
        }
        escape(std::cout, fix.symbols[i]);
    }
    std::cout << "],\n";
    std::cout << "  \"extension_hashes\": [";
    bool firstExtension = true;
    for (const auto &extension : codboz::kExtensionProfiles) {
        const uint32_t occurrences = codboz::countLittleEndianWord(image.rawData(), extension.hash);
        if (occurrences == 0) {
            continue;
        }
        if (!firstExtension) {
            std::cout << ", ";
        }
        char hash[9];
        std::snprintf(hash, sizeof(hash), "%08x", extension.hash);
        std::cout << "{\"name\": ";
        escape(std::cout, extension.name);
        std::cout << ", \"hash\": \"" << hash
                  << "\", \"little_endian_occurrences\": " << occurrences << '}';
        firstExtension = false;
    }
    std::cout << "]\n";
    std::cout << "}\n";
}

int run(const char *path) {
    auto raw = codboz::readFile(path);
    bool decompressed = false;
    std::span<const uint8_t> view = raw;
    std::vector<uint8_t> inflated;

    if (view.size() < 4 || codboz::readLe32(view.data()) != codboz::kS3eMagic) {
        inflated = codboz::lzmaDecompress(view);
        view = inflated;
        decompressed = true;
    }

    S3eImage image(view);
    print(image, decompressed);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <boz.s3e | boz.s3e.unpacked>\n", argv[0]);
        return 2;
    }
    try {
        return run(argv[1]);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "s3e_inspect: %s\n", e.what());
        return 1;
    }
}
