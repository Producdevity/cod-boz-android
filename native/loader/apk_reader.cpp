#include "apk_reader.hpp"

#include "bytes.hpp"

#include <algorithm>
#include <stdexcept>

namespace codboz {

namespace {
constexpr uint32_t kZipEocdSig = 0x06054b50u;
constexpr uint32_t kZipCentralSig = 0x02014b50u;
constexpr uint32_t kZipLocalSig = 0x04034b50u;
constexpr uint32_t kZip64Sentinel32 = 0xffffffffu;
constexpr uint16_t kZip64Sentinel16 = 0xffffu;
constexpr uint32_t kMaxZipComment = 65535u;

std::size_t findEocd(std::span<const uint8_t> data) {
    if (data.size() < 22) {
        throw std::runtime_error("APK is too small to be a ZIP file");
    }
    std::size_t tailSize = std::min<std::size_t>(data.size(), kMaxZipComment + 22u);
    std::size_t base = data.size() - tailSize;
    for (std::size_t pos = tailSize - 22u + 1u; pos-- > 0;) {
        if (readLe32(data.data() + base + pos) == kZipEocdSig) {
            uint16_t commentLen = readLe16(data.data() + base + pos + 20);
            if (pos + 22u + commentLen == tailSize) {
                return base + pos;
            }
        }
        if (pos == 0) {
            break;
        }
    }
    throw std::runtime_error("APK does not contain a valid ZIP end record");
}
} // namespace

ApkReader::ApkReader(std::span<const uint8_t> data) : data_(data) {
    std::size_t eocd = findEocd(data);
    Reader r(data);
    r.seek(eocd);
    r.readLe32(); // signature
    if (r.readLe16() != 0 || r.readLe16() != 0) {
        throw std::runtime_error("multi-disk ZIP/APK files are not supported");
    }
    uint16_t entryCountDisk = r.readLe16();
    uint16_t entryCountTotal = r.readLe16();
    uint32_t centralSize = r.readLe32();
    uint32_t centralOffset = r.readLe32();
    if (entryCountDisk != entryCountTotal) {
        throw std::runtime_error("inconsistent ZIP central directory entry count");
    }
    if (entryCountTotal == kZip64Sentinel16 || centralSize == kZip64Sentinel32 ||
        centralOffset == kZip64Sentinel32) {
        throw std::runtime_error("ZIP64 APK files are not supported");
    }
    if (static_cast<std::size_t>(centralOffset) + centralSize > data.size()) {
        throw std::runtime_error("ZIP central directory is outside the APK");
    }

    r.seek(centralOffset);
    std::size_t consumed = 0;
    entries_.reserve(entryCountTotal);
    while (consumed < centralSize) {
        std::size_t headerStart = r.position();
        if (r.readLe32() != kZipCentralSig) {
            throw std::runtime_error("invalid ZIP central directory entry");
        }
        r.readLe16(); // version made by
        r.readLe16(); // version needed
        ZipEntry e;
        e.flags = r.readLe16();
        e.method = r.readLe16();
        r.readLe16(); // mod time
        r.readLe16(); // mod date
        e.crc32 = r.readLe32();
        e.compressedSize = r.readLe32();
        e.uncompressedSize = r.readLe32();
        uint16_t nameLen = r.readLe16();
        uint16_t extraLen = r.readLe16();
        uint16_t commentLen = r.readLe16();
        r.readLe16(); // disk number start
        r.readLe16(); // internal attrs
        r.readLe32(); // external attrs
        e.localHeaderOffset = r.readLe32();
        e.name = std::string(reinterpret_cast<const char *>(r.readBytes(nameLen).data()), nameLen);
        if (extraLen) {
            r.readBytes(extraLen);
        }
        if (commentLen) {
            r.readBytes(commentLen);
        }
        entries_.push_back(std::move(e));
        consumed += r.position() - headerStart;
    }
}

const ZipEntry *ApkReader::find(std::string_view name) const {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const ZipEntry &e) { return e.name == name; });
    return it == entries_.end() ? nullptr : &*it;
}

std::span<const uint8_t> ApkReader::entryData(const ZipEntry &e) const {
    Reader r(data_);
    r.seek(e.localHeaderOffset);
    if (r.readLe32() != kZipLocalSig) {
        throw std::runtime_error("invalid ZIP local file header");
    }
    r.readLe16(); // version
    const uint16_t flags = r.readLe16();
    const uint16_t method = r.readLe16();
    r.readLe16(); // time
    r.readLe16(); // date
    r.readLe32(); // crc
    r.readLe32(); // compressed size
    r.readLe32(); // uncompressed size
    uint16_t nameLen = r.readLe16();
    uint16_t extraLen = r.readLe16();
    const auto nameBytes = r.readBytes(nameLen);
    const std::string_view localName(reinterpret_cast<const char *>(nameBytes.data()), nameLen);
    if (flags != e.flags || method != e.method || localName != e.name) {
        throw std::runtime_error("ZIP local header does not match the central directory");
    }
    std::size_t off = e.localHeaderOffset + 30u + nameLen + extraLen;
    if (off > data_.size() || e.compressedSize > data_.size() - off) {
        throw std::runtime_error("ZIP entry payload is outside the APK");
    }
    return data_.subspan(off, e.compressedSize);
}

bool isSafeAssetName(std::string_view name) {
    if (name.size() <= 7 || name.substr(0, 7) != "assets/") {
        return false;
    }
    if (name.front() == '/' || name.find("../") != std::string_view::npos ||
        name.find("/..") != std::string_view::npos || name.find("//") != std::string_view::npos) {
        return false;
    }
    return true;
}

} // namespace codboz
