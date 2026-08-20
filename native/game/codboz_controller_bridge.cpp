#include "codboz_controller_bridge.hpp"

#include <algorithm>
#include <array>

namespace codboz {
namespace {

constexpr uint32_t kManagerSlotOffset = 0x467858u;
constexpr uint32_t kCommandDispatcherOffset = 0x1a6d9cu;
constexpr uint32_t kManagerStateSize = 0x4eu;
constexpr uint32_t kFrontendActiveOffset = 0x64u;
constexpr uint32_t kFrontendActiveLoadOffset = 0x257668u;
constexpr std::array<uint8_t, 4> kFrontendActiveLoadSignature = {0x64, 0x30, 0xd4, 0xe5};

struct ActionSite {
    uint32_t offset;
    std::array<uint8_t, 10> signature;
};

struct FrontendKeyboardSite {
    uint32_t callOffset;
    std::array<uint8_t, 4> signature;
};

constexpr std::array<ActionSite, 10> kActionSites = {{
    {kCommandDispatcherOffset, {0x4b, 0x1e, 0xf7, 0xb5, 0x04, 0x46, 0x10, 0x2b, 0x00, 0xf2}},
    {0x1a7194u, {0x03, 0x46, 0x07, 0xb5, 0x0a, 0x46, 0x83, 0xf8, 0x42, 0x10}},
    {0x1a71c4u, {0x07, 0xb5, 0x0a, 0x46, 0x80, 0xf8, 0x43, 0x10, 0x20, 0x30}},
    {0x1a71fcu, {0x07, 0xb5, 0x0a, 0x46, 0x80, 0xf8, 0x44, 0x10, 0x20, 0x30}},
    {0x1a7234u, {0x07, 0xb5, 0x0a, 0x46, 0x80, 0xf8, 0x46, 0x10, 0x20, 0x30}},
    {0x1a726cu, {0x07, 0xb5, 0x0a, 0x46, 0x80, 0xf8, 0x47, 0x10, 0x20, 0x30}},
    {0x1a72a4u, {0x07, 0xb5, 0x0a, 0x46, 0x80, 0xf8, 0x48, 0x10, 0x20, 0x30}},
    {0x1a72dcu, {0x07, 0xb5, 0x0a, 0x46, 0x80, 0xf8, 0x49, 0x10, 0x20, 0x30}},
    {0x1a7314u, {0x07, 0xb5, 0x0a, 0x46, 0x80, 0xf8, 0x4a, 0x10, 0x20, 0x30}},
    {0x1a734cu, {0x07, 0xb5, 0x0a, 0x46, 0x80, 0xf8, 0x4b, 0x10, 0x20, 0x30}},
}};

constexpr std::array<FrontendKeyboardSite, 3> kFrontendKeyboardSites = {{
    {0x2575a8u, {0xe1, 0xa3, 0xf6, 0xeb}},
    {0x2575c0u, {0xdb, 0xa3, 0xf6, 0xeb}},
    {0x2576a0u, {0xa3, 0xa3, 0xf6, 0xeb}},
}};

uint32_t publisherOffset(GuestControllerAction action) {
    switch (action) {
    case GuestControllerAction::ChangeWeapon:
        return 0x1a7194u;
    case GuestControllerAction::Reload:
        return 0x1a71c4u;
    case GuestControllerAction::Aim:
        return 0x1a71fcu;
    case GuestControllerAction::Fire:
        return 0x1a7234u;
    case GuestControllerAction::Melee:
        return 0x1a726cu;
    case GuestControllerAction::Grenade:
        return 0x1a72a4u;
    case GuestControllerAction::TacticalGrenade:
        return 0x1a72dcu;
    case GuestControllerAction::CrouchProne:
        return 0x1a7314u;
    case GuestControllerAction::AlternateFire:
        return 0x1a734cu;
    case GuestControllerAction::Sprint:
        return kCommandDispatcherOffset;
    case GuestControllerAction::Count:
        return 0;
    }
    return 0;
}

} // namespace

bool CodbozControllerBridge::initialize(const GuestMemory &memory, GuestAddr imageBase) {
    imageBase_ = 0;
    managerSlot_ = 0;
    for (const auto &site : kActionSites) {
        if (!memory.isMapped(imageBase + site.offset,
                             static_cast<GuestSize>(site.signature.size()))) {
            return false;
        }
        const auto bytes =
            memory.read(imageBase + site.offset, static_cast<GuestSize>(site.signature.size()));
        if (!std::equal(bytes.begin(), bytes.end(), site.signature.begin())) {
            return false;
        }
    }
    for (const auto &site : kFrontendKeyboardSites) {
        if (!memory.isMapped(imageBase + site.callOffset,
                             static_cast<GuestSize>(site.signature.size()))) {
            return false;
        }
        const auto bytes =
            memory.read(imageBase + site.callOffset, static_cast<GuestSize>(site.signature.size()));
        if (!std::equal(bytes.begin(), bytes.end(), site.signature.begin())) {
            return false;
        }
    }
    if (!memory.isMapped(imageBase + kFrontendActiveLoadOffset,
                         static_cast<GuestSize>(kFrontendActiveLoadSignature.size()))) {
        return false;
    }
    const auto activeLoad =
        memory.read(imageBase + kFrontendActiveLoadOffset,
                    static_cast<GuestSize>(kFrontendActiveLoadSignature.size()));
    if (!std::equal(activeLoad.begin(), activeLoad.end(), kFrontendActiveLoadSignature.begin())) {
        return false;
    }
    const GuestAddr managerSlot = imageBase + kManagerSlotOffset;
    if (!memory.isMapped(managerSlot, sizeof(uint32_t))) {
        return false;
    }
    imageBase_ = imageBase;
    managerSlot_ = managerSlot;
    return true;
}

std::optional<GuestControllerCall> CodbozControllerBridge::resolve(const GuestMemory &memory,
                                                                   GuestControllerAction action,
                                                                   bool pressed) const {
    if (!imageBase_ || !managerSlot_ || !memory.isMapped(managerSlot_, sizeof(uint32_t))) {
        return std::nullopt;
    }
    const GuestAddr manager = memory.read32(managerSlot_);
    if (!manager || (manager & 3u) != 0 || !memory.isMapped(manager, kManagerStateSize)) {
        return std::nullopt;
    }

    GuestControllerCall call;
    const uint32_t offset = publisherOffset(action);
    if (!offset) {
        return std::nullopt;
    }
    call.function = imageBase_ + offset + 1u;
    call.manager = manager;
    if (action == GuestControllerAction::Sprint) {
        call.arguments = {manager, pressed ? 2u : 3u, 0u, 0u};
    } else {
        call.arguments = {manager, pressed ? 1u : 0u, 0u, 0u};
    }
    return call;
}

bool CodbozControllerBridge::valid(const GuestMemory &memory,
                                   const GuestControllerCall &call) const {
    return managerSlot_ && call.manager && memory.isMapped(managerSlot_, sizeof(uint32_t)) &&
           memory.isMapped(call.manager, kManagerStateSize) &&
           memory.read32(managerSlot_) == call.manager;
}

bool CodbozControllerBridge::isFrontendKeyboardQuery(GuestAddr returnAddress) const {
    if (!imageBase_) {
        return false;
    }
    const GuestAddr address = returnAddress & ~1u;
    return std::ranges::any_of(kFrontendKeyboardSites, [this, address](const auto &site) {
        return address == imageBase_ + site.callOffset + 4u;
    });
}

bool CodbozControllerBridge::validFrontendManager(const GuestMemory &memory,
                                                  GuestAddr manager) const {
    return manager && (manager & 3u) == 0 && memory.isMapped(manager, kFrontendActiveOffset + 1u);
}

bool CodbozControllerBridge::frontendActive(const GuestMemory &memory, GuestAddr manager) const {
    return validFrontendManager(memory, manager) &&
           memory.read8(manager + kFrontendActiveOffset) != 0;
}

} // namespace codboz
