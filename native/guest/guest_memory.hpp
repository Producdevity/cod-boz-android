#pragma once

#include "guest_address.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace codboz {

// Controlled failure on a guest access. The runtime turns these into guest
// exceptions rather than letting the host crash.
class GuestMemoryError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Checked, page-backed 32-bit guest address space. Reads and writes validate
// that the full range is mapped; unmapped access throws GuestMemoryError. This
// is the correctness baseline; a 4 GiB fast reservation may replace it later.
class GuestMemory {
  public:
    GuestMemory();
    ~GuestMemory();

    GuestMemory(const GuestMemory &) = delete;
    GuestMemory &operator=(const GuestMemory &) = delete;

    // Back [base, base+size) with zeroed pages, then copy `data` (dataSize bytes)
    // into the start of the region. dataSize may be 0.
    void mapRegion(GuestAddr base, GuestSize size, const uint8_t *data = nullptr,
                   GuestSize dataSize = 0);

    // Copy host bytes into a mapped region (used to load code/data).
    void write(GuestAddr addr, const uint8_t *data, GuestSize size);

    bool isMapped(GuestAddr addr, GuestSize size) const;

    // Fill a Dynarmic-style page table (1M entries, each a host pointer to a 4KiB
    // page). Indexed by guestAddr >> 12.
    void populatePageTable(std::uint8_t **table, std::size_t count) const;

    std::optional<std::uintptr_t> fastmemPointer() const;

    uint8_t read8(GuestAddr addr) const;
    uint16_t read16(GuestAddr addr) const;
    uint32_t read32(GuestAddr addr) const;
    uint64_t read64(GuestAddr addr) const;

    void write8(GuestAddr addr, uint8_t value);
    void write16(GuestAddr addr, uint16_t value);
    void write32(GuestAddr addr, uint32_t value);
    void write64(GuestAddr addr, uint64_t value);

    // Copy guest bytes out into a host buffer.
    bool copyOut(GuestAddr addr, uint8_t *data, GuestSize size) const;
    const uint8_t *contiguousData(GuestAddr addr, GuestSize size) const;
    std::vector<uint8_t> read(GuestAddr addr, GuestSize size) const;

    // Write a NUL-terminated guest string at addr; returns addr just past the NUL.
    GuestAddr writeString(GuestAddr addr, std::string_view s);

    // Read a NUL-terminated guest string (bounded by maxLen).
    std::string readString(GuestAddr addr, GuestSize maxLen) const;

  private:
    struct Mapping {
        uint8_t *data;
        std::size_t size;
    };

    uint8_t *page(GuestAddr pageBaseAddr);
    const uint8_t *findPage(GuestAddr pageBaseAddr) const;
    void ensureMapped(GuestAddr addr, GuestSize size) const;
    void mapMissingPages(GuestAddr firstPage, GuestAddr lastPage);

    std::unordered_map<uint32_t, uint8_t *> pages_;
    std::vector<Mapping> mappings_;
    uint8_t *addressSpace_ = nullptr;
};

} // namespace codboz
