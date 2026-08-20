#include "dtrz_archive.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>

namespace codboz {

namespace {

uint16_t readLe16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8u);
}

uint32_t readLe32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
           (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

bool readExact(std::istream &input, void *out, std::size_t size) {
    return static_cast<bool>(
        input.read(static_cast<char *>(out), static_cast<std::streamsize>(size)));
}

bool readCString(std::istream &input, std::string &out) {
    out.clear();
    for (std::size_t i = 0; i < 256; ++i) {
        char value = 0;
        if (!input.get(value)) {
            return false;
        }
        if (value == '\0') {
            return true;
        }
        out.push_back(value);
    }
    return false;
}

std::string toLower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string baseName(std::string_view path) {
    auto pos = path.find_last_of("/\\");
    return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

} // namespace

bool DtrzArchive::load(const std::string &path) {
    loaded_ = false;
    path_.clear();
    entries_.clear();
    archiveSize_ = 0;

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 9 || static_cast<uint64_t>(end) > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    archiveSize_ = static_cast<uint32_t>(end);
    input.seekg(0, std::ios::beg);

    std::array<uint8_t, 9> header{};
    if (!readExact(input, header.data(), header.size()) ||
        std::memcmp(header.data(), "DTRZ", 4) != 0) {
        return false;
    }

    const uint16_t fileCount = readLe16(header.data() + 4);
    uint16_t groupCount = readLe16(header.data() + 6);
    if (groupCount > 0) {
        --groupCount;
    }
    if (fileCount > 512) {
        return false;
    }

    entries_.reserve(fileCount);
    for (uint16_t i = 0; i < fileCount; ++i) {
        Entry entry;
        if (!readCString(input, entry.name)) {
            return false;
        }
        entries_.push_back(std::move(entry));
    }
    for (uint16_t i = 0; i < groupCount; ++i) {
        std::string ignored;
        if (!readCString(input, ignored)) {
            return false;
        }
    }

    std::array<uint8_t, 4> marker{};
    if (!readExact(input, marker.data(), marker.size()) || readLe32(marker.data()) != 1) {
        return false;
    }
    input.seekg(static_cast<std::streamoff>(fileCount) * 6, std::ios::cur);
    if (!input) {
        return false;
    }

    for (auto &entry : entries_) {
        std::array<uint8_t, 16> record{};
        if (!readExact(input, record.data(), record.size())) {
            return false;
        }
        entry.offset = readLe32(record.data());
        entry.size = readLe32(record.data() + 4);
        if (entry.offset > archiveSize_ || entry.size > archiveSize_ - entry.offset) {
            return false;
        }
    }

    path_ = path;
    loaded_ = true;
    return true;
}

const DtrzArchive::Entry *DtrzArchive::find(std::string_view name) const {
    if (!loaded_) {
        return nullptr;
    }
    const std::string lowerName = toLower(name);
    const std::string lowerBase = toLower(baseName(lowerName));
    for (const auto &entry : entries_) {
        const std::string entryName = toLower(entry.name);
        if (entryName == lowerName || toLower(baseName(entryName)) == lowerBase) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<uint8_t> DtrzArchive::read(const Entry &entry) const {
    if (!loaded_ || entry.offset > archiveSize_ || entry.size > archiveSize_ - entry.offset) {
        return {};
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return {};
    }
    input.seekg(entry.offset, std::ios::beg);
    std::vector<uint8_t> out(entry.size);
    if (!out.empty() && !readExact(input, out.data(), out.size())) {
        return {};
    }
    return out;
}

} // namespace codboz
