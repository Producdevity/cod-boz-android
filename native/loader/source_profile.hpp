#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace codboz {

struct ResourceArchiveProfile {
    std::string_view name;
    uint64_t size;
    std::string_view sha256;
};

struct ExtensionProfile {
    std::string_view name;
    uint32_t hash;
};

#include "loader/source_profile_data.inc"

std::string findSupportedResourceBase(std::span<const uint8_t> image);
std::string resourceManifestJson();
uint32_t countLittleEndianWord(std::span<const uint8_t> image, uint32_t value);
uint32_t countBytes(std::span<const uint8_t> image, std::string_view value);
std::string_view guestArchitectureName(uint32_t s3eArch);
constexpr uint32_t extensionHash(std::string_view name) {
    for (const auto &extension : kExtensionProfiles) {
        if (extension.name == name) {
            return extension.hash;
        }
    }
    return 0;
}

} // namespace codboz
