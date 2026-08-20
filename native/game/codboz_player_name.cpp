#include "codboz_player_name.hpp"

#include <algorithm>
#include <array>

namespace codboz {

bool applyPlayerName(GuestMemory &memory, GuestAddr imageBase, GuestAddr playerName) {
    constexpr GuestAddr referenceOffset = 0x18f74cu;
    constexpr GuestAddr referencePcOffset = 0x18f60au;
    constexpr GuestAddr formatOffset = 0x3af131u;
    constexpr std::array<uint8_t, 10> format{'P', 'l', 'a', 'y', 'e', 'r', '-', '%', 'd', 0};

    if (!playerName ||
        !memory.isMapped(imageBase + formatOffset, static_cast<GuestSize>(format.size())) ||
        !memory.isMapped(imageBase + referenceOffset, sizeof(uint32_t))) {
        return false;
    }
    const auto actual =
        memory.read(imageBase + formatOffset, static_cast<GuestSize>(format.size()));
    if (!std::equal(actual.begin(), actual.end(), format.begin()) ||
        memory.read32(imageBase + referenceOffset) != formatOffset - referencePcOffset) {
        return false;
    }
    memory.write32(imageBase + referenceOffset, playerName - (imageBase + referencePcOffset));
    return true;
}

} // namespace codboz
