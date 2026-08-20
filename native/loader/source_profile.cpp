#include "source_profile.hpp"

#include <algorithm>
#include <stdexcept>

namespace codboz {

namespace {
constexpr std::string_view kResourceLink = "ResDownloadLink=\"";

void appendJsonString(std::string &output, std::string_view value) {
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '/':
            output += "\\/";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (character < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(hex[character >> 4]);
                output.push_back(hex[character & 0x0f]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    output.push_back('"');
}
} // namespace

std::string findSupportedResourceBase(std::span<const uint8_t> image) {
    const auto text = std::string_view(reinterpret_cast<const char *>(image.data()), image.size());
    std::size_t offset = 0;
    while ((offset = text.find(kResourceLink, offset)) != std::string_view::npos) {
        const std::size_t start = offset + kResourceLink.size();
        const std::size_t end = text.find_first_of("\"\r\n\0", start);
        const auto value =
            text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        if (value == kResourceBaseUrl) {
            return std::string(value);
        }
        offset = start;
    }
    throw std::runtime_error("supported APK has no recognized resource source");
}

std::string resourceManifestJson() {
    std::string output = "{\n  \"source\": ";
    appendJsonString(output, kResourceBaseUrl);
    output += ",\n  \"archives\": {\n";
    for (std::size_t index = 0; index < kResourceArchives.size(); ++index) {
        const auto &archive = kResourceArchives[index];
        output += "    ";
        appendJsonString(output, archive.name);
        output += ": ";
        appendJsonString(output, archive.sha256);
        output += index + 1 == kResourceArchives.size() ? "\n" : ",\n";
    }
    output += "  }\n}\n";
    return output;
}

uint32_t countLittleEndianWord(std::span<const uint8_t> image, uint32_t value) {
    const std::array<uint8_t, 4> bytes = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24),
    };
    uint32_t count = 0;
    for (std::size_t offset = 0; offset + bytes.size() <= image.size(); ++offset) {
        if (std::equal(bytes.begin(), bytes.end(), image.begin() + offset)) {
            ++count;
        }
    }
    return count;
}

uint32_t countBytes(std::span<const uint8_t> image, std::string_view value) {
    if (value.empty()) {
        return 0;
    }
    const auto text = std::string_view(reinterpret_cast<const char *>(image.data()), image.size());
    uint32_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(value, offset)) != std::string_view::npos) {
        ++count;
        offset += value.size();
    }
    return count;
}

std::string_view guestArchitectureName(uint32_t s3eArch) {
    return s3eArch == kSupportedS3eArch ? kSupportedGuestArchitecture : "unknown";
}

} // namespace codboz
