#pragma once

#include "guest/guest_address.hpp"

#include <cstdint>

namespace codboz {

inline constexpr uint32_t kGuestDisplayBytesPerPixel = 4;

struct GuestDisplay {
    uint32_t width = 854;
    uint32_t height = 480;

    constexpr bool isValid() const {
        return width >= 320 && width <= 4096 && height >= 240 && height <= 4096;
    }

    constexpr GuestSize pitch() const {
        return width * kGuestDisplayBytesPerPixel;
    }

    constexpr GuestSize bufferSize() const {
        return pitch() * height;
    }
};

} // namespace codboz
