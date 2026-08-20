#pragma once

#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace codboz {

enum class GuestControllerAction : uint8_t {
    Sprint,
    Reload,
    Melee,
    Grenade,
    Aim,
    Fire,
    TacticalGrenade,
    CrouchProne,
    AlternateFire,
    ChangeWeapon,
    Count,
};

struct GuestControllerCall {
    GuestAddr function = 0;
    GuestAddr manager = 0;
    std::array<uint32_t, 4> arguments{};
};

class CodbozControllerBridge {
  public:
    bool initialize(const GuestMemory &memory, GuestAddr imageBase);
    std::optional<GuestControllerCall> resolve(const GuestMemory &memory,
                                               GuestControllerAction action, bool pressed) const;
    bool valid(const GuestMemory &memory, const GuestControllerCall &call) const;
    bool isFrontendKeyboardQuery(GuestAddr returnAddress) const;
    bool validFrontendManager(const GuestMemory &memory, GuestAddr manager) const;
    bool frontendActive(const GuestMemory &memory, GuestAddr manager) const;

  private:
    GuestAddr imageBase_ = 0;
    GuestAddr managerSlot_ = 0;
};

} // namespace codboz
