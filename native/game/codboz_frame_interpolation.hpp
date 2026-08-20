#pragma once

#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

namespace codboz {

class CodbozFrameInterpolation {
  public:
    static bool apply(GuestMemory &memory, GuestAddr imageBase);

    static constexpr GuestAddr fixedTrampoline() {
        return kPatchBase | 1u;
    }

    static constexpr GuestAddr interpolateTrampoline() {
        return (kPatchBase + kInterpolateOffset) | 1u;
    }

  private:
    static constexpr GuestAddr kPatchBase = 0x63000000u;
    static constexpr GuestAddr kInterpolateOffset = 0x40u;
};

} // namespace codboz
