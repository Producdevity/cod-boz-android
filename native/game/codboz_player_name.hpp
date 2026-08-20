#pragma once

#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

namespace codboz {

bool applyPlayerName(GuestMemory &memory, GuestAddr imageBase, GuestAddr playerName);

} // namespace codboz
