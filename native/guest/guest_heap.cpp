#include "guest_heap.hpp"

#include <algorithm>
#include <stdexcept>

namespace codboz {

GuestHeap::GuestHeap(GuestMemory &mem, GuestAddr base, GuestSize size)
    : mem_(mem), base_(base), end_(base + size) {
    if (size == 0 || end_ < base) {
        throw std::invalid_argument("invalid guest heap region");
    }
    mem_.mapRegion(base, size);
    free_.push_back({base, size});
}

GuestSize GuestHeap::alignUp(GuestSize value, GuestSize align) const {
    return (value + align - 1) & ~(align - 1);
}

void GuestHeap::insertFree(GuestAddr addr, GuestSize size) {
    auto pos = std::lower_bound(free_.begin(), free_.end(), addr,
                                [](const FreeBlock &b, GuestAddr a) { return b.addr < a; });
    pos = free_.insert(pos, {addr, size});
    // Coalesce with previous.
    if (pos != free_.begin()) {
        auto prev = pos - 1;
        if (prev->addr + prev->size == pos->addr) {
            prev->size += pos->size;
            pos = free_.erase(pos) - 1;
        }
    }
    // Coalesce with next.
    if (pos + 1 != free_.end() && pos->addr + pos->size == (pos + 1)->addr) {
        pos->size += (pos + 1)->size;
        free_.erase(pos + 1);
    }
}

GuestAddr GuestHeap::alloc(GuestSize size, GuestSize align) {
    if (size == 0 || align == 0 || (align & (align - 1)) != 0) {
        return 0;
    }
    GuestSize need = alignUp(size, 8);
    for (std::size_t i = 0; i < free_.size(); ++i) {
        FreeBlock &b = free_[i];
        GuestAddr aligned = base_ + alignUp(b.addr - base_, align);
        if (aligned >= b.addr + b.size) {
            continue;
        }
        GuestSize avail = (b.addr + b.size) - aligned;
        if (avail < need) {
            continue;
        }
        // Split: prefix [b.addr, aligned), allocated [aligned, aligned+need),
        // suffix [aligned+need, b.addr+b.size).
        GuestAddr prefixAddr = b.addr;
        GuestSize prefixSize = aligned - b.addr;
        GuestAddr suffixAddr = aligned + need;
        GuestSize suffixSize = (b.addr + b.size) - suffixAddr;

        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
        if (prefixSize > 0) {
            insertFree(prefixAddr, prefixSize);
        }
        if (suffixSize > 0) {
            insertFree(suffixAddr, suffixSize);
        }
        allocated_[aligned] = need;
        allocatedBytes_ += need;
        return aligned;
    }
    return 0;
}

bool GuestHeap::free(GuestAddr addr) {
    auto it = allocated_.find(addr);
    if (it == allocated_.end()) {
        return false; // not allocated, or double free
    }
    GuestSize size = it->second;
    allocated_.erase(it);
    allocatedBytes_ -= size;
    insertFree(addr, size);
    return true;
}

GuestAddr GuestHeap::realloc(GuestAddr addr, GuestSize size) {
    if (addr == 0) {
        return alloc(size);
    }
    auto it = allocated_.find(addr);
    if (it == allocated_.end()) {
        return 0;
    }
    if (size <= it->second) {
        return addr; // fits in place
    }
    GuestAddr next = alloc(size);
    if (next == 0) {
        return 0;
    }
    auto data = mem_.read(addr, it->second);
    mem_.write(next, data.data(), static_cast<GuestSize>(data.size()));
    free(addr);
    return next;
}

GuestSize GuestHeap::sizeOf(GuestAddr addr) const {
    auto it = allocated_.find(addr);
    return it == allocated_.end() ? 0 : it->second;
}

} // namespace codboz
