#include "codboz_fixups.hpp"

#include <cstring>

namespace codboz {

// Image-relative offsets of the faulting instructions, from the reference.
// Bucket allocator.
constexpr uint32_t OFF_BUCKET_FAULT_A = 0x374be8u;
constexpr uint32_t OFF_BUCKET_FAULT_B = 0x374bf4u;
constexpr uint32_t OFF_BUCKET_SKIP = 0x374c34u;
constexpr uint32_t BUCKET_TABLE_SLOT = 0x4cu;
constexpr uint32_t BUCKET_TABLE_ENTRIES = 33;

// Null-buffer write.
constexpr uint32_t OFF_NULL_BUF_WRITE = 0xcb8eeu;

// Null-buffer slot.
constexpr uint32_t OFF_NULL_BUF_SLOT_A = 0xd291cu;
constexpr uint32_t OFF_NULL_BUF_SLOT_B = 0xd293au;

// Null-string dereferences.
constexpr uint32_t OFF_NULL_STR_R1_A = 0x368ddcu;
constexpr uint32_t OFF_NULL_STR_R1_B = 0x368d2cu;
constexpr uint32_t OFF_NULL_STR_R0_A = 0x24ba00u;
constexpr uint32_t OFF_NULL_STR_R0_B = 0x24ba30u;
constexpr uint32_t OFF_NULL_STR_SKIP = 0x368fa4u;
constexpr uint32_t OFF_NULL_STR_SKIP_TARGET = 0x369024u;

std::array<GuestAddr, 10> CodbozFixups::recoveryPoints(GuestAddr imageBase) {
    return {
        imageBase + OFF_BUCKET_FAULT_A,  imageBase + OFF_BUCKET_FAULT_B,
        imageBase + OFF_NULL_BUF_WRITE,  imageBase + OFF_NULL_BUF_SLOT_A,
        imageBase + OFF_NULL_BUF_SLOT_B, imageBase + OFF_NULL_STR_R1_A,
        imageBase + OFF_NULL_STR_R1_B,   imageBase + OFF_NULL_STR_R0_A,
        imageBase + OFF_NULL_STR_R0_B,   imageBase + OFF_NULL_STR_SKIP,
    };
}

const char *CodbozFixups::name(Kind kind) {
    switch (kind) {
    case Kind::BucketAllocator:
        return "bucket-allocator";
    case Kind::NullBuffer:
        return "null-buffer";
    case Kind::NullString:
        return "null-string";
    case Kind::None:
        return "none";
    }
    return "none";
}

void CodbozFixups::init(GuestMemory &mem, GuestAddr imageBase) {
    base_ = imageBase;

    // Allocate the empty string (8-byte aligned) in guest memory.
    emptyString_ = 0x00010000u;
    mem.mapRegion(emptyString_, 0x100u);
    mem.writeString(emptyString_, "");

    // Allocate the bucket allocator table (33 x uint32_t, zeroed).
    bucketTable_ = emptyString_ + 0x100u;
    mem.mapRegion(bucketTable_, 0x100u);
    for (uint32_t i = 0; i < BUCKET_TABLE_ENTRIES; ++i) {
        mem.write32(bucketTable_ + i * 4u, 0);
    }
}

bool CodbozFixups::tryFixup(GuestMemory &mem, GuestAddr pc, uint32_t regs[16]) {
    lastKind_ = Kind::None;
    if (base_ == 0) {
        return false;
    }

    auto at = [this](uint32_t off) { return base_ + off; };
    auto sp = [&regs]() { return regs[13]; };

    // --- Bucket allocator fault (recover_bucket_allocator_fault) ---
    if (pc == at(OFF_BUCKET_FAULT_A) || pc == at(OFF_BUCKET_FAULT_B)) {
        uint32_t object = regs[5] ? regs[5] : regs[0];
        uint32_t index = regs[4] ? regs[4] : regs[1];
        if (!object) {
            return false;
        }
        if (index >= 32 && pc == at(OFF_BUCKET_FAULT_A)) {
            index = 1;
        }
        if (index >= 32) {
            return false;
        }

        // Attach the table at object+0x4c.
        mem.write32(object + BUCKET_TABLE_SLOT, bucketTable_);

        regs[0] = object;
        regs[1] = index;
        regs[2] = bucketTable_;
        regs[4] = index;
        regs[5] = object;

        if (pc == at(OFF_BUCKET_FAULT_A)) {
            // Zero the table and retry.
            for (uint32_t i = 0; i < BUCKET_TABLE_ENTRIES; ++i) {
                mem.write32(bucketTable_ + i * 4u, 0);
            }
            regs[15] = at(OFF_BUCKET_FAULT_A);
        } else {
            mem.write32(bucketTable_ + index * 4u, 0);
            regs[15] = at(OFF_BUCKET_SKIP);
        }
        lastKind_ = Kind::BucketAllocator;
        ++bucketFixupCount_;
        return true;
    }

    // --- Null-buffer write (recover_null_buffer_write) ---
    if (pc == at(OFF_NULL_BUF_WRITE) && regs[0] == 0) {
        GuestAddr stack = sp();
        regs[4] = mem.read32(stack);
        regs[5] = mem.read32(stack + 4);
        regs[6] = mem.read32(stack + 8);
        regs[0] = 0;
        regs[13] = stack + 16;
        regs[15] = mem.read32(stack + 12) & ~1u;
        lastKind_ = Kind::NullBuffer;
        ++nullBufferFixupCount_;
        return true;
    }

    // --- Null-buffer slot (recover_null_buffer_slot) ---
    if ((pc == at(OFF_NULL_BUF_SLOT_A) || pc == at(OFF_NULL_BUF_SLOT_B)) && regs[2] == 0) {
        GuestAddr stack = sp();
        regs[3] = mem.read32(stack);
        regs[4] = mem.read32(stack + 4);
        regs[5] = mem.read32(stack + 8);
        regs[6] = mem.read32(stack + 12);
        regs[7] = mem.read32(stack + 16);
        regs[0] = 0;
        regs[13] = stack + 24;
        regs[15] = mem.read32(stack + 20) & ~1u;
        lastKind_ = Kind::NullBuffer;
        ++nullBufferFixupCount_;
        return true;
    }

    // --- Null-string dereferences ---
    // r1 == 0 at these PCs: patch r1 to the empty string.
    if ((pc == at(OFF_NULL_STR_R1_A) || pc == at(OFF_NULL_STR_R1_B)) && regs[1] == 0) {
        regs[1] = emptyString_;
        lastKind_ = Kind::NullString;
        ++nullStringFixupCount_;
        return true;
    }

    // r0 == 0 at these PCs: patch r0 to the empty string.
    if ((pc == at(OFF_NULL_STR_R0_A) || pc == at(OFF_NULL_STR_R0_B)) && regs[0] == 0) {
        regs[0] = emptyString_;
        lastKind_ = Kind::NullString;
        ++nullStringFixupCount_;
        return true;
    }

    // Skip block at 0x368fa4: r1==0, set r0=0, r2=0, jump to 0x369024.
    if (pc == at(OFF_NULL_STR_SKIP) && regs[1] == 0) {
        regs[0] = 0;
        regs[2] = 0;
        regs[15] = at(OFF_NULL_STR_SKIP_TARGET);
        lastKind_ = Kind::NullString;
        ++nullStringFixupCount_;
        return true;
    }

    return false;
}

} // namespace codboz
