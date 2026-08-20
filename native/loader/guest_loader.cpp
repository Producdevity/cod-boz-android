#include "guest_loader.hpp"

#include <stdexcept>

namespace codboz {

GuestLoadResult loadAndRelocate(GuestMemory &mem, const S3eImage &image) {
    const auto &h = image.header();
    GuestLoadResult r;
    r.base = h.baseAddrOrig;
    r.entry = h.baseAddrOrig + h.entryOffset;

    if (h.baseAddrOrig == 0) {
        throw std::runtime_error("S3E image has no preferred base address");
    }

    // Map code plus zero-filled BSS at the preferred base.
    auto code = image.code();
    mem.mapRegion(h.baseAddrOrig, h.codeMemSize, code.data(), h.codeFileSize);

    // Internal relocations: absolute references are fixed up by the load delta.
    // Loading at baseAddrOrig makes the delta zero, but apply it regardless.
    const int32_t loadDelta =
        static_cast<int32_t>(static_cast<GuestAddr>(h.baseAddrOrig) - h.baseAddrOrig);
    for (uint32_t offset : image.internalRelocs()) {
        GuestAddr slot = h.baseAddrOrig + offset;
        uint32_t value = mem.read32(slot);
        mem.write32(slot, static_cast<uint32_t>(static_cast<int64_t>(value) + loadDelta));
        ++r.internalApplied;
    }

    // External relocations: each slot receives the trap address of its symbol.
    const auto &symbols = image.symbols();
    for (const auto &er : image.externalRelocs()) {
        GuestAddr slot = h.baseAddrOrig + er.offset;
        GuestAddr trap = importTrapAddress(er.symbolIndex);
        mem.write32(slot, trap);
        if (er.symbolIndex < symbols.size()) {
            r.trapSymbols[trap] = symbols[er.symbolIndex];
        }
        ++r.externalApplied;
    }

    return r;
}

} // namespace codboz
