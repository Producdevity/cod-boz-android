#pragma once

#include <cstdint>

namespace codboz {

// All guest-facing widths are 32-bit, independent of the AArch64 host pointer
// width. No host pointer is ever stored in a guest-visible structure.
using GuestAddr = uint32_t;
using GuestSize = uint32_t;
using GuestHandle = uint32_t;
using GuestCallback = uint32_t;

// Guest page size. The original ARMv7 game assumes 4 KiB pages.
constexpr uint32_t kGuestPageSize = 4096;

// Reserved range for host-import trampolines. Every S3E import is assigned a
// stable address in this range; the JIT stops here and the dispatcher reads
// AAPCS32 arguments.
constexpr GuestAddr kTrapBase = 0xFFF00000u;
constexpr GuestAddr kTrapEnd = 0xFFFFFFFFu;

// Extension function tables returned via s3eExtGetHash use a separate trap range
// (their entries are host functions not present in the import symbol table).
constexpr GuestAddr kExtTrapBase = 0xFFE00000u;
constexpr GuestAddr kExtTrapEnd = 0xFFEFFFFFu;

// Address the JIT returns to after a host→guest callback completes.
constexpr GuestAddr kCallbackReturnTrap = 0xFFFF0000u;

inline bool isInTrapRange(GuestAddr addr) {
    return addr >= kTrapBase && addr <= kTrapEnd;
}
// Any trap address: imports, extension tables, or the callback return trap.
inline bool isTrapAddress(GuestAddr addr) {
    return addr >= kExtTrapBase && addr <= kTrapEnd;
}

constexpr GuestAddr guestPageBase(GuestAddr addr) {
    return addr & ~(kGuestPageSize - 1);
}

constexpr GuestAddr guestPageOffset(GuestAddr addr) {
    return addr & (kGuestPageSize - 1);
}

} // namespace codboz
