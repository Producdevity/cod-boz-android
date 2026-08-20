#pragma once

#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"
#include "loader/s3e_image.hpp"

#include <string>
#include <unordered_map>

namespace codboz {

struct GuestLoadResult {
    GuestAddr base = 0;
    GuestAddr entry = 0;
    uint32_t internalApplied = 0;
    uint32_t externalApplied = 0;
    // Trap address -> imported symbol name, for dispatch and diagnostics.
    std::unordered_map<GuestAddr, std::string> trapSymbols;
};

// Map an S3E image into the guest address space at its preferred base, apply
// internal relocations, and write a stable import-trap address into every
// external relocation slot (importId == symbol index). No host pointer is ever
// written into guest memory.
GuestLoadResult loadAndRelocate(GuestMemory &mem, const S3eImage &image);

// Stable trap address for an import symbol index, and its inverse.
inline GuestAddr importTrapAddress(uint16_t symbolIndex) {
    return kTrapBase + static_cast<GuestAddr>(symbolIndex) * 4u;
}
inline uint16_t trapToSymbolIndex(GuestAddr trap) {
    return static_cast<uint16_t>((trap - kTrapBase) / 4u);
}

} // namespace codboz
