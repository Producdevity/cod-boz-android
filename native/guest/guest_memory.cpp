#include "guest_memory.hpp"

#include <cstring>
#include <format>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace codboz {

namespace {
constexpr std::size_t kGuestAddressSpaceSize = std::size_t{1} << 32;

std::string rangeMsg(GuestAddr addr, GuestSize size) {
    return std::format("guest access unmapped: addr=0x{:08x} size={}", addr, size);
}
} // namespace

GuestMemory::GuestMemory() {
    if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
        return;
    }
    if (sysconf(_SC_PAGESIZE) != kGuestPageSize) {
        return;
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void *mapping = ::mmap(nullptr, kGuestAddressSpaceSize, PROT_NONE, flags, -1, 0);
    if (mapping != MAP_FAILED) {
        addressSpace_ = static_cast<uint8_t *>(mapping);
    }
}

GuestMemory::~GuestMemory() {
    if (addressSpace_) {
        ::munmap(addressSpace_, kGuestAddressSpaceSize);
        return;
    }
    for (const auto &mapping : mappings_) {
        ::munmap(mapping.data, mapping.size);
    }
}

uint8_t *GuestMemory::page(GuestAddr base) {
    return pages_.at(base);
}

const uint8_t *GuestMemory::findPage(GuestAddr base) const {
    auto it = pages_.find(base);
    return it == pages_.end() ? nullptr : it->second;
}

void GuestMemory::mapMissingPages(GuestAddr firstPage, GuestAddr lastPage) {
    const std::size_t pageCount =
        (static_cast<uint64_t>(lastPage) - firstPage) / kGuestPageSize + 1u;
    pages_.reserve(pages_.size() + pageCount);
    std::size_t index = 0;
    while (index < pageCount) {
        const GuestAddr address = firstPage + static_cast<GuestAddr>(index * kGuestPageSize);
        if (findPage(address)) {
            ++index;
            continue;
        }

        const std::size_t runStart = index;
        while (index < pageCount) {
            const GuestAddr runAddress = firstPage + static_cast<GuestAddr>(index * kGuestPageSize);
            if (findPage(runAddress)) {
                break;
            }
            ++index;
        }
        const std::size_t runPages = index - runStart;
        const std::size_t runSize = runPages * kGuestPageSize;
        uint8_t *bytes;
        if (addressSpace_) {
            bytes = addressSpace_ + firstPage + runStart * kGuestPageSize;
            if (::mprotect(bytes, runSize, PROT_READ | PROT_WRITE) != 0) {
                throw std::bad_alloc();
            }
        } else {
            void *mapping = ::mmap(nullptr, runSize, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapping == MAP_FAILED) {
                throw std::bad_alloc();
            }
            bytes = static_cast<uint8_t *>(mapping);
            mappings_.push_back({bytes, runSize});
        }
        for (std::size_t pageIndex = 0; pageIndex < runPages; ++pageIndex) {
            const GuestAddr pageAddress =
                firstPage + static_cast<GuestAddr>((runStart + pageIndex) * kGuestPageSize);
            pages_.emplace(pageAddress, bytes + pageIndex * kGuestPageSize);
        }
    }
}

std::optional<std::uintptr_t> GuestMemory::fastmemPointer() const {
    if (!addressSpace_) {
        return std::nullopt;
    }
    return reinterpret_cast<std::uintptr_t>(addressSpace_);
}

bool GuestMemory::isMapped(GuestAddr addr, GuestSize size) const {
    if (size == 0) {
        return true;
    }
    GuestAddr end = addr + (size - 1); // wraps on purpose at 0xFFFFFFFF
    if (end < addr) {
        return false; // range wraps the 4 GiB space
    }
    GuestAddr a = addr;
    while (true) {
        if (findPage(guestPageBase(a)) == nullptr) {
            return false;
        }
        if (a == end) {
            break;
        }
        GuestAddr next = guestPageBase(a) + kGuestPageSize;
        a = (next > end) ? end : next;
    }
    return true;
}

void GuestMemory::ensureMapped(GuestAddr addr, GuestSize size) const {
    if (!isMapped(addr, size)) {
        throw GuestMemoryError(rangeMsg(addr, size));
    }
}

void GuestMemory::mapRegion(GuestAddr base, GuestSize size, const uint8_t *data,
                            GuestSize dataSize) {
    if (size == 0) {
        return;
    }
    GuestAddr end = base + (size - 1);
    if (end < base) {
        throw GuestMemoryError("mapRegion wraps the guest address space");
    }
    if (dataSize > size) {
        throw GuestMemoryError("mapRegion data exceeds region");
    }
    mapMissingPages(guestPageBase(base), guestPageBase(end));
    if (data && dataSize > 0) {
        write(base, data, dataSize);
    }
}

void GuestMemory::write(GuestAddr addr, const uint8_t *data, GuestSize size) {
    ensureMapped(addr, size);
    GuestSize off = 0;
    while (off < size) {
        GuestAddr cur = addr + off;
        auto *p = page(guestPageBase(cur));
        GuestSize pageOff = guestPageOffset(cur);
        GuestSize chunk = std::min<GuestSize>(kGuestPageSize - pageOff, size - off);
        std::memcpy(p + pageOff, data + off, chunk);
        off += chunk;
    }
}

std::vector<uint8_t> GuestMemory::read(GuestAddr addr, GuestSize size) const {
    std::vector<uint8_t> out(size);
    if (!copyOut(addr, out.data(), size)) {
        throw GuestMemoryError(rangeMsg(addr, size));
    }
    return out;
}

bool GuestMemory::copyOut(GuestAddr addr, uint8_t *data, GuestSize size) const {
    if (size == 0) {
        return true;
    }
    if (!data || addr + (size - 1) < addr) {
        return false;
    }
    GuestSize off = 0;
    while (off < size) {
        GuestAddr cur = addr + off;
        const auto *p = findPage(guestPageBase(cur));
        if (!p) {
            return false;
        }
        GuestSize pageOff = guestPageOffset(cur);
        GuestSize chunk = std::min<GuestSize>(kGuestPageSize - pageOff, size - off);
        std::memcpy(data + off, p + pageOff, chunk);
        off += chunk;
    }
    return true;
}

const uint8_t *GuestMemory::contiguousData(GuestAddr addr, GuestSize size) const {
    if (size == 0 || addr + (size - 1) < addr) {
        return nullptr;
    }
    const GuestAddr firstPage = guestPageBase(addr);
    const auto *first = findPage(firstPage);
    if (!first) {
        return nullptr;
    }
    const GuestAddr lastPage = guestPageBase(addr + size - 1);
    for (GuestAddr pageAddress = firstPage; pageAddress < lastPage; pageAddress += kGuestPageSize) {
        const auto *next = findPage(pageAddress + kGuestPageSize);
        const auto pageOffset = static_cast<std::size_t>(pageAddress - firstPage);
        if (next != first + pageOffset + kGuestPageSize) {
            return nullptr;
        }
    }
    return first + guestPageOffset(addr);
}

uint8_t GuestMemory::read8(GuestAddr addr) const {
    ensureMapped(addr, 1);
    return findPage(guestPageBase(addr))[guestPageOffset(addr)];
}

uint16_t GuestMemory::read16(GuestAddr addr) const {
    ensureMapped(addr, 2);
    GuestSize pageOff = guestPageOffset(addr);
    if (pageOff <= kGuestPageSize - 2) {
        const auto *p = findPage(guestPageBase(addr));
        return static_cast<uint16_t>(p[pageOff]) | (static_cast<uint16_t>(p[pageOff + 1]) << 8);
    }
    auto b = read(addr, 2);
    return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

uint32_t GuestMemory::read32(GuestAddr addr) const {
    ensureMapped(addr, 4);
    GuestSize pageOff = guestPageOffset(addr);
    if (pageOff <= kGuestPageSize - 4) {
        const auto *p = findPage(guestPageBase(addr));
        return static_cast<uint32_t>(p[pageOff]) | (static_cast<uint32_t>(p[pageOff + 1]) << 8) |
               (static_cast<uint32_t>(p[pageOff + 2]) << 16) |
               (static_cast<uint32_t>(p[pageOff + 3]) << 24);
    }
    auto b = read(addr, 4);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

uint64_t GuestMemory::read64(GuestAddr addr) const {
    ensureMapped(addr, 8);
    auto b = read(addr, 8);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(b[i]) << (8 * i);
    }
    return v;
}

void GuestMemory::write8(GuestAddr addr, uint8_t value) {
    ensureMapped(addr, 1);
    page(guestPageBase(addr))[guestPageOffset(addr)] = value;
}

void GuestMemory::write16(GuestAddr addr, uint16_t value) {
    GuestSize pageOff = guestPageOffset(addr);
    if (pageOff <= kGuestPageSize - 2) {
        ensureMapped(addr, 2);
        auto *p = page(guestPageBase(addr));
        p[pageOff] = value & 0xff;
        p[pageOff + 1] = (value >> 8) & 0xff;
    } else {
        uint8_t b[2] = {static_cast<uint8_t>(value & 0xff),
                        static_cast<uint8_t>((value >> 8) & 0xff)};
        write(addr, b, 2);
    }
}

void GuestMemory::write32(GuestAddr addr, uint32_t value) {
    GuestSize pageOff = guestPageOffset(addr);
    if (pageOff <= kGuestPageSize - 4) {
        ensureMapped(addr, 4);
        auto *p = page(guestPageBase(addr));
        p[pageOff] = value & 0xff;
        p[pageOff + 1] = (value >> 8) & 0xff;
        p[pageOff + 2] = (value >> 16) & 0xff;
        p[pageOff + 3] = (value >> 24) & 0xff;
    } else {
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) {
            b[i] = (value >> (8 * i)) & 0xff;
        }
        write(addr, b, 4);
    }
}

void GuestMemory::write64(GuestAddr addr, uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = (value >> (8 * i)) & 0xff;
    }
    write(addr, b, 8);
}

GuestAddr GuestMemory::writeString(GuestAddr addr, std::string_view s) {
    write(addr, reinterpret_cast<const uint8_t *>(s.data()), static_cast<GuestSize>(s.size()));
    write8(addr + static_cast<GuestSize>(s.size()), 0);
    return addr + static_cast<GuestSize>(s.size()) + 1;
}

std::string GuestMemory::readString(GuestAddr addr, GuestSize maxLen) const {
    std::string out;
    for (GuestSize i = 0; i < maxLen; ++i) {
        uint8_t c = read8(addr + i);
        if (c == 0) {
            break;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

void GuestMemory::populatePageTable(std::uint8_t **table, std::size_t count) const {
    for (const auto &[pageBase, page] : pages_) {
        std::size_t idx = pageBase / kGuestPageSize;
        if (idx < count) {
            table[idx] = page;
        }
    }
}

} // namespace codboz
