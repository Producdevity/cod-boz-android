#include "game/codboz_controller_bridge.hpp"

#include "test_macros.hpp"

#include <array>
#include <cstdint>

namespace {

constexpr codboz::GuestAddr kImageBase = 0x4a000000u;
constexpr codboz::GuestAddr kManagerSlot = kImageBase + 0x467858u;
constexpr codboz::GuestAddr kManager = kImageBase + 0x466000u;

struct Site {
    uint32_t offset;
    std::array<uint8_t, 10> bytes;
};

constexpr std::array<Site, 10> kSites = {{
    {0x1a6d9cu, {0x4b, 0x1e, 0xf7, 0xb5, 0x04, 0x46, 0x10, 0x2b, 0x00, 0xf2}},
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

struct FrontendSite {
    uint32_t offset;
    std::array<uint8_t, 4> bytes;
};

constexpr std::array<FrontendSite, 4> kFrontendSites = {{
    {0x2575a8u, {0xe1, 0xa3, 0xf6, 0xeb}},
    {0x2575c0u, {0xdb, 0xa3, 0xf6, 0xeb}},
    {0x2576a0u, {0xa3, 0xa3, 0xf6, 0xeb}},
    {0x257668u, {0x64, 0x30, 0xd4, 0xe5}},
}};

void loadSites(codboz::GuestMemory &memory) {
    memory.mapRegion(kImageBase, 0x468000u);
    for (const auto &site : kSites) {
        memory.write(kImageBase + site.offset, site.bytes.data(),
                     static_cast<codboz::GuestSize>(site.bytes.size()));
    }
    for (const auto &site : kFrontendSites) {
        memory.write(kImageBase + site.offset, site.bytes.data(),
                     static_cast<codboz::GuestSize>(site.bytes.size()));
    }
    memory.write32(kManagerSlot, kManager);
}

} // namespace

int main() {
    codboz::GuestMemory memory;
    loadSites(memory);

    codboz::CodbozControllerBridge bridge;
    CHECK(bridge.initialize(memory, kImageBase));
    CHECK(bridge.isFrontendKeyboardQuery(kImageBase + 0x2575acu));
    CHECK(bridge.isFrontendKeyboardQuery(kImageBase + 0x2575c4u));
    CHECK(bridge.isFrontendKeyboardQuery(kImageBase + 0x2576a4u));
    CHECK(!bridge.isFrontendKeyboardQuery(kImageBase + 0x0dc501u));
    CHECK(bridge.validFrontendManager(memory, kManager));
    CHECK(!bridge.frontendActive(memory, kManager));
    memory.write8(kManager + 0x64u, 1);
    CHECK(bridge.frontendActive(memory, kManager));
    memory.write8(kManager + 0x64u, 0);
    CHECK(!bridge.validFrontendManager(memory, kManager + 1u));

    struct ExpectedCall {
        codboz::GuestControllerAction action;
        uint32_t offset;
    };
    constexpr std::array<ExpectedCall, 10> expected = {{
        {codboz::GuestControllerAction::Sprint, 0x1a6d9cu},
        {codboz::GuestControllerAction::Reload, 0x1a71c4u},
        {codboz::GuestControllerAction::Melee, 0x1a726cu},
        {codboz::GuestControllerAction::Grenade, 0x1a72a4u},
        {codboz::GuestControllerAction::Aim, 0x1a71fcu},
        {codboz::GuestControllerAction::Fire, 0x1a7234u},
        {codboz::GuestControllerAction::TacticalGrenade, 0x1a72dcu},
        {codboz::GuestControllerAction::CrouchProne, 0x1a7314u},
        {codboz::GuestControllerAction::AlternateFire, 0x1a734cu},
        {codboz::GuestControllerAction::ChangeWeapon, 0x1a7194u},
    }};
    for (const auto &entry : expected) {
        const auto down = bridge.resolve(memory, entry.action, true);
        const auto up = bridge.resolve(memory, entry.action, false);
        CHECK(down.has_value());
        CHECK(up.has_value());
        CHECK(down->function == kImageBase + entry.offset + 1u);
        CHECK(down->manager == kManager);
        CHECK(down->arguments[0] == kManager);
        CHECK(down->arguments[1] == 1u || entry.action == codboz::GuestControllerAction::Sprint);
        CHECK(up->arguments[1] == 0u || entry.action == codboz::GuestControllerAction::Sprint);
        CHECK(bridge.valid(memory, *down));
    }

    const auto sprintDown = bridge.resolve(memory, codboz::GuestControllerAction::Sprint, true);
    const auto sprintUp = bridge.resolve(memory, codboz::GuestControllerAction::Sprint, false);
    CHECK(sprintDown->arguments[1] == 2u);
    CHECK(sprintUp->arguments[1] == 3u);

    const auto aim = bridge.resolve(memory, codboz::GuestControllerAction::Aim, true);
    memory.write32(kManagerSlot, 0);
    CHECK(!bridge.resolve(memory, codboz::GuestControllerAction::Aim, true));
    CHECK(!bridge.valid(memory, *aim));

    codboz::GuestMemory unsupported;
    loadSites(unsupported);
    unsupported.write8(kImageBase + 0x1a71fcu, 0);
    codboz::CodbozControllerBridge unsupportedBridge;
    CHECK(!unsupportedBridge.initialize(unsupported, kImageBase));

    codboz::GuestMemory unsupportedFrontend;
    loadSites(unsupportedFrontend);
    unsupportedFrontend.write8(kImageBase + 0x2576a0u, 0);
    CHECK(!unsupportedBridge.initialize(unsupportedFrontend, kImageBase));

    codboz::GuestMemory unsupportedFrontendState;
    loadSites(unsupportedFrontendState);
    unsupportedFrontendState.write8(kImageBase + 0x257668u, 0);
    CHECK(!unsupportedBridge.initialize(unsupportedFrontendState, kImageBase));

    return 0;
}
