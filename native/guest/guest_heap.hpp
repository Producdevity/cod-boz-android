#pragma once

#include "guest_address.hpp"
#include "guest_memory.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace codboz {

// Guest heap living inside a region of GuestMemory. Allocation metadata is kept
// on the host; the guest only sees 32-bit addresses. Allocations are 8-byte
// aligned (ARM EABI). Double-free and overflow are detected and rejected.
class GuestHeap {
  public:
    GuestHeap(GuestMemory &mem, GuestAddr base, GuestSize size);

    // Returns 0 on failure (guest OOM). align must be a power of two.
    GuestAddr alloc(GuestSize size, GuestSize align = 8);
    bool free(GuestAddr addr);
    GuestAddr realloc(GuestAddr addr, GuestSize size);

    GuestSize allocatedBytes() const {
        return allocatedBytes_;
    }
    GuestSize sizeOf(GuestAddr addr) const;

  private:
    struct FreeBlock {
        GuestAddr addr;
        GuestSize size;
    };

    GuestSize alignUp(GuestSize value, GuestSize align) const;
    void insertFree(GuestAddr addr, GuestSize size);

    GuestMemory &mem_;
    GuestAddr base_;
    GuestAddr end_;
    std::vector<FreeBlock> free_; // sorted by addr, non-adjacent
    std::unordered_map<GuestAddr, GuestSize> allocated_;
    GuestSize allocatedBytes_ = 0;
};

} // namespace codboz
