#pragma once

#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

#include <array>
#include <cstdint>

namespace codboz {

// Precise per-PC fault fixups for the known BOZ 1.0.11 null-deref bugs.
// Mirrors the reference loader's crash handlers (recover_bucket_allocator_fault,
// recover_null_buffer_write, recover_null_buffer_slot, null-string patches).
//
// After Dynarmic reports a fault, call tryFixup(). If it returns true the
// registers/memory have been patched and execution should resume; if false the
// fault is genuine and execution should terminate.
class CodbozFixups {
  public:
    enum class Kind {
        None,
        BucketAllocator,
        NullBuffer,
        NullString,
    };

    // Must be called once after the image is loaded, with the guest base address
    // and the guest memory (to allocate the bucket table and empty string).
    void init(GuestMemory &mem, GuestAddr imageBase);

    // Returns true if the fault at pc was handled.
    bool tryFixup(GuestMemory &mem, GuestAddr pc, uint32_t regs[16]);

    static std::array<GuestAddr, 10> recoveryPoints(GuestAddr imageBase);

    Kind lastKind() const {
        return lastKind_;
    }
    uint64_t bucketCount() const {
        return bucketFixupCount_;
    }
    uint64_t nullBufferCount() const {
        return nullBufferFixupCount_;
    }
    uint64_t nullStringCount() const {
        return nullStringFixupCount_;
    }
    static const char *name(Kind kind);

  private:
    GuestAddr base_ = 0;
    GuestAddr emptyString_ = 0; // 8-byte aligned "" in guest memory
    GuestAddr bucketTable_ = 0; // 33-entry uint32_t table in guest memory
    Kind lastKind_ = Kind::None;
    uint64_t bucketFixupCount_ = 0;
    uint64_t nullStringFixupCount_ = 0;
    uint64_t nullBufferFixupCount_ = 0;
};

} // namespace codboz
