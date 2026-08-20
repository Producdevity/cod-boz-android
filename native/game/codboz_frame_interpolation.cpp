#include "codboz_frame_interpolation.hpp"

#include <array>
#include <cstdint>

namespace codboz {
namespace {

constexpr GuestAddr kActiveSetPointerOffset = 0x45f2b0u;
constexpr GuestAddr kDirectorFixedSlotOffset = 0x3e6490u;
constexpr GuestAddr kDirectorInterpolateSlotOffset = 0x3e6494u;
constexpr GuestAddr kDirectorFixedCallbackOffset = 0x0ba98du;
constexpr GuestAddr kDirectorInterpolateCallbackOffset = 0x0ba7dbu;
constexpr GuestAddr kCaptureTransformsOffset = 0x0e8787u;
constexpr GuestAddr kApplyFactorOffset = 0x0e87c9u;

constexpr std::array<uint8_t, 40> kFixedTrampoline = {
    0x01, 0x29, 0x07, 0xd1, 0x1f, 0xb5, 0x05, 0x4c, 0x20, 0x68, 0x08, 0xb1, 0x04, 0x4c,
    0xa0, 0x47, 0xbd, 0xe8, 0x1f, 0x40, 0xdf, 0xf8, 0x0c, 0xc0, 0x60, 0x47, 0x00, 0xbf,
    0x11, 0x11, 0x11, 0x11, 0x23, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33,
};

constexpr std::array<uint8_t, 64> kInterpolateTrampoline = {
    0x01, 0x29, 0x15, 0xd1, 0x10, 0xb5, 0x0b, 0x4c, 0x20, 0x68, 0x80, 0xb1, 0x00, 0xee, 0x10, 0x3a,
    0xdf, 0xed, 0x0a, 0x0a, 0x20, 0xee, 0x20, 0x0a, 0xbc, 0xee, 0xc0, 0x0a, 0x10, 0xee, 0x10, 0x1a,
    0x41, 0xf2, 0x00, 0x02, 0x91, 0x42, 0x88, 0xbf, 0x11, 0x46, 0x03, 0x4c, 0xa0, 0x47, 0x10, 0xbd,
    0x70, 0x47, 0x00, 0xbf, 0x11, 0x11, 0x11, 0x11, 0x45, 0x44, 0x44, 0x44, 0x00, 0x00, 0x80, 0x45,
};

} // namespace

bool CodbozFrameInterpolation::apply(GuestMemory &memory, GuestAddr imageBase) {
    const GuestAddr activeSetPointer = imageBase + kActiveSetPointerOffset;
    const GuestAddr fixedSlot = imageBase + kDirectorFixedSlotOffset;
    const GuestAddr interpolateSlot = imageBase + kDirectorInterpolateSlotOffset;
    const GuestAddr fixedCallback = imageBase + kDirectorFixedCallbackOffset;
    const GuestAddr interpolateCallback = imageBase + kDirectorInterpolateCallbackOffset;
    const GuestAddr captureTransforms = imageBase + kCaptureTransformsOffset;
    const GuestAddr applyFactor = imageBase + kApplyFactorOffset;

    if (!memory.isMapped(activeSetPointer, sizeof(uint32_t)) ||
        !memory.isMapped(fixedSlot, 2u * sizeof(uint32_t)) ||
        memory.read32(fixedSlot) != fixedCallback ||
        memory.read32(interpolateSlot) != interpolateCallback ||
        !memory.isMapped(captureTransforms & ~1u, sizeof(uint16_t)) ||
        !memory.isMapped(applyFactor & ~1u, sizeof(uint16_t))) {
        return false;
    }

    memory.mapRegion(kPatchBase, kGuestPageSize);
    memory.write(kPatchBase, kFixedTrampoline.data(), kFixedTrampoline.size());
    memory.write(kPatchBase + kInterpolateOffset, kInterpolateTrampoline.data(),
                 kInterpolateTrampoline.size());

    memory.write32(kPatchBase + 0x1cu, activeSetPointer);
    memory.write32(kPatchBase + 0x20u, captureTransforms);
    memory.write32(kPatchBase + 0x24u, fixedCallback);
    memory.write32(kPatchBase + kInterpolateOffset + 0x34u, activeSetPointer);
    memory.write32(kPatchBase + kInterpolateOffset + 0x38u, applyFactor);

    memory.write32(fixedSlot, fixedTrampoline());
    memory.write32(interpolateSlot, interpolateTrampoline());
    return true;
}

} // namespace codboz
