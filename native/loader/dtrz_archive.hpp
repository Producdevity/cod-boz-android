#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codboz {

// A DTRZ resource archive (.dz file). Contains a header with an entry index,
// followed by the data section. Each entry maps a logical filename to an
// (offset, size) within the archive.
class DtrzArchive {
  public:
    struct Entry {
        std::string name;
        uint32_t offset;
        uint32_t size;
    };

    // Load and parse a .dz file. Returns false on invalid format.
    bool load(const std::string &path);

    // Find an entry by name (case-insensitive, matches full name or base name).
    const Entry *find(std::string_view name) const;

    // Read an entry's data from disk.
    std::vector<uint8_t> read(const Entry &entry) const;

    bool loaded() const {
        return loaded_;
    }
    std::size_t entryCount() const {
        return entries_.size();
    }
    const std::vector<Entry> &entries() const {
        return entries_;
    }
    const std::string &path() const {
        return path_;
    }

  private:
    bool loaded_ = false;
    std::string path_;
    std::vector<Entry> entries_;
    uint32_t archiveSize_ = 0;
};

} // namespace codboz
