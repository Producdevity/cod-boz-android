#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace codboz {

struct ZipEntry {
    std::string name;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t localHeaderOffset = 0;
};

// Read-only view over an APK's ZIP central directory. The whole APK must be
// memory-resident (mmap or loaded); no streaming.
class ApkReader {
  public:
    explicit ApkReader(std::span<const uint8_t> data);

    const std::vector<ZipEntry> &entries() const {
        return entries_;
    }
    const ZipEntry *find(std::string_view name) const;

    // Payload bytes of an entry. For STORED entries this is the decompressed
    // content; for deflated entries it is the raw compressed data. Throws on a
    // malformed local header.
    std::span<const uint8_t> entryData(const ZipEntry &e) const;

  private:
    std::span<const uint8_t> data_;
    std::vector<ZipEntry> entries_;
};

// An assets/ entry name that is safe to extract (no traversal, no doubleslash).
bool isSafeAssetName(std::string_view name);

} // namespace codboz
