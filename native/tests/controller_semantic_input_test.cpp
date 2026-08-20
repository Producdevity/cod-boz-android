#include "bridge/dispatcher.hpp"
#include "loader/guest_loader.hpp"

#include "test_macros.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace codboz {

class ControllerInputTestAccess {
  public:
    static bool initializeBridge(Runtime &runtime, GuestAddr imageBase) {
        return runtime.controllerBridge_.initialize(runtime.mem_, imageBase);
    }

    static void pump(Runtime &runtime) {
        runtime.pumpControllerInput();
    }

    static std::size_t pendingCount(const Runtime &runtime) {
        return runtime.pendingGuestCallbacks_.size();
    }

    static GuestAddr pendingFunction(const Runtime &runtime, std::size_t index) {
        return runtime.pendingGuestCallbacks_[index].function;
    }

    static std::array<uint32_t, 4> pendingArguments(const Runtime &runtime, std::size_t index) {
        return runtime.pendingGuestCallbacks_[index].arguments;
    }
};

} // namespace codboz

namespace {

constexpr codboz::GuestAddr kImageBase = 0x4a000000u;
constexpr codboz::GuestAddr kManager = kImageBase + 0x466000u;
constexpr codboz::GuestAddr kFrontendManager = kImageBase + 0x466100u;

class TestCpu final : public codboz::GuestCpu {
  public:
    void reset() override {
        regs.fill(0);
    }
    codboz::RunOutcome run() override {
        return codboz::RunOutcome::Halted;
    }
    void halt() override {
        halted = true;
    }
    uint32_t reg(int index) const override {
        return regs[static_cast<std::size_t>(index)];
    }
    void setReg(int index, uint32_t value) override {
        regs[static_cast<std::size_t>(index)] = value;
    }
    uint32_t cpsr() const override {
        return 0;
    }
    void setCpsr(uint32_t) override {}
    codboz::GuestAddr pc() const override {
        return 0;
    }
    void setPc(codboz::GuestAddr) override {}
    void setThumb(bool) override {}
    std::array<uint32_t, 64> fpRegs() const override {
        return {};
    }
    void setFpRegs(const std::array<uint32_t, 64> &) override {}
    uint32_t fpscr() const override {
        return 0;
    }
    void setFpscr(uint32_t) override {}
    uint32_t stackArg(int) const override {
        return 0;
    }

    std::array<uint32_t, 16> regs{};
    bool halted = false;
};

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
    memory.write32(kImageBase + 0x467858u, kManager);
}

} // namespace

int main() {
    codboz::GuestMemory memory;
    loadSites(memory);
    codboz::Runtime::Config config;
    codboz::Runtime runtime(memory, std::vector<std::string>{}, config);
    CHECK(codboz::ControllerInputTestAccess::initializeBridge(runtime, kImageBase));

    runtime.queueControllerButton(codboz::ControllerAction::Sprint, true);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(runtime) == 1);
    CHECK(codboz::ControllerInputTestAccess::pendingFunction(runtime, 0) ==
          kImageBase + 0x1a6d9cu + 1u);
    CHECK(codboz::ControllerInputTestAccess::pendingArguments(runtime, 0)[0] == kManager);
    CHECK(codboz::ControllerInputTestAccess::pendingArguments(runtime, 0)[1] == 2u);

    runtime.queueControllerButton(codboz::ControllerAction::Sprint, false);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(runtime) == 2);
    CHECK(codboz::ControllerInputTestAccess::pendingArguments(runtime, 1)[1] == 3u);

    runtime.queueControllerButton(codboz::ControllerAction::Aim, true);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::pendingFunction(runtime, 2) ==
          kImageBase + 0x1a71fcu + 1u);
    CHECK(codboz::ControllerInputTestAccess::pendingArguments(runtime, 2)[1] == 1u);
    runtime.queueControllerButton(codboz::ControllerAction::Aim, false);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::pendingArguments(runtime, 3)[1] == 0u);

    runtime.queueControllerButton(codboz::ControllerAction::Back, true);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(runtime) == 5);
    CHECK(codboz::ControllerInputTestAccess::pendingFunction(runtime, 4) ==
          kImageBase + 0x1a7314u + 1u);
    runtime.queueControllerButton(codboz::ControllerAction::DpadDown, true);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(runtime) == 5);
    runtime.queueControllerButton(codboz::ControllerAction::Back, false);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(runtime) == 6);
    runtime.queueControllerButton(codboz::ControllerAction::DpadDown, false);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(runtime) == 6);
    CHECK(codboz::ControllerInputTestAccess::pendingArguments(runtime, 5)[1] == 0u);

    codboz::GuestMemory uiMemory;
    loadSites(uiMemory);
    constexpr std::size_t kUpdate = 0;
    constexpr std::size_t kGetState = 1;
    codboz::Runtime uiRuntime(uiMemory, {"s3eKeyboardUpdate", "s3eKeyboardGetState"}, config);
    CHECK(codboz::ControllerInputTestAccess::initializeBridge(uiRuntime, kImageBase));
    TestCpu cpu;

    cpu.setReg(0, 209);
    cpu.setReg(4, kFrontendManager);
    cpu.setReg(14, kImageBase + 0x2575acu);
    uiRuntime.dispatch(codboz::importTrapAddress(kGetState), cpu);

    uiRuntime.queueControllerButton(codboz::ControllerAction::Accept, true);
    uiRuntime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    cpu.setReg(0, 208);
    cpu.setReg(14, kImageBase + 0x0dc501u);
    uiRuntime.dispatch(codboz::importTrapAddress(kGetState), cpu);
    CHECK(cpu.reg(0) == 0);
    cpu.setReg(0, 208);
    cpu.setReg(4, kFrontendManager);
    cpu.setReg(14, kImageBase + 0x2576a4u);
    uiRuntime.dispatch(codboz::importTrapAddress(kGetState), cpu);
    CHECK(cpu.reg(0) == 3);

    uiRuntime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    cpu.setReg(0, 208);
    cpu.setReg(14, kImageBase + 0x2576a4u);
    uiRuntime.dispatch(codboz::importTrapAddress(kGetState), cpu);
    CHECK(cpu.reg(0) == 1);
    uiMemory.write8(kFrontendManager + 0x64u, 1);
    uiRuntime.queueControllerButton(codboz::ControllerAction::Back, true);
    uiRuntime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(uiRuntime) == 0);
    cpu.setReg(0, 210);
    cpu.setReg(14, kImageBase + 0x2576a4u);
    uiRuntime.dispatch(codboz::importTrapAddress(kGetState), cpu);
    CHECK(cpu.reg(0) == 3);

    uiRuntime.queueControllerButton(codboz::ControllerAction::Back, false);
    uiRuntime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(uiRuntime) == 0);
    cpu.setReg(0, 210);
    cpu.setReg(14, kImageBase + 0x2576a4u);
    uiRuntime.dispatch(codboz::importTrapAddress(kGetState), cpu);
    CHECK(cpu.reg(0) == 4);

    uiMemory.write8(kFrontendManager + 0x64u, 0);
    uiRuntime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    uiRuntime.queueControllerButton(codboz::ControllerAction::Reload, true);
    uiRuntime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(uiRuntime) == 1);
    CHECK(codboz::ControllerInputTestAccess::pendingFunction(uiRuntime, 0) ==
          kImageBase + 0x1a71c4u + 1u);

    return 0;
}
