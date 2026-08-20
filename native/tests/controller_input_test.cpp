#include "bridge/dispatcher.hpp"
#include "loader/guest_loader.hpp"

#include "test_macros.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace codboz {

class ControllerInputTestAccess {
  public:
    static void pump(Runtime &runtime) {
        runtime.pumpControllerInput();
    }

    static bool touchpadActive(const Runtime &runtime, std::size_t id) {
        return runtime.touchpadActive_[id];
    }

    static uint8_t touchpadState(const Runtime &runtime, std::size_t id) {
        return runtime.touchpadState_[id];
    }

    static int32_t touchpadX(const Runtime &runtime, std::size_t id) {
        return runtime.touchpadX_[id];
    }

    static int32_t touchpadY(const Runtime &runtime, std::size_t id) {
        return runtime.touchpadY_[id];
    }

    static std::size_t pendingCount(const Runtime &runtime) {
        return runtime.pendingGuestCallbacks_.size();
    }

    static GuestAddr pendingFunction(const Runtime &runtime, std::size_t index) {
        return runtime.pendingGuestCallbacks_[index].function;
    }

    static guest::TouchpadButtonEvent buttonEvent(const Runtime &runtime, std::size_t index) {
        guest::TouchpadButtonEvent event{};
        const auto &payload = runtime.pendingGuestCallbacks_[index].payload;
        std::memcpy(&event, payload.data(), sizeof(event));
        return event;
    }

    static guest::TouchpadMotionEvent motionEvent(const Runtime &runtime, std::size_t index) {
        guest::TouchpadMotionEvent event{};
        const auto &payload = runtime.pendingGuestCallbacks_[index].payload;
        std::memcpy(&event, payload.data(), sizeof(event));
        return event;
    }
};

} // namespace codboz

namespace {

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
        return status;
    }
    void setCpsr(uint32_t value) override {
        status = value;
    }
    codboz::GuestAddr pc() const override {
        return programCounter;
    }
    void setPc(codboz::GuestAddr value) override {
        programCounter = value;
    }
    void setThumb(bool) override {}
    std::array<uint32_t, 64> fpRegs() const override {
        return {};
    }
    void setFpRegs(const std::array<uint32_t, 64> &) override {}
    uint32_t fpscr() const override {
        return 0;
    }
    void setFpscr(uint32_t) override {}
    uint32_t stackArg(int index) const override {
        return regs[13] == 0 ? 0 : stack[static_cast<std::size_t>(index)];
    }

    std::array<uint32_t, 16> regs{};
    std::array<uint32_t, 8> stack{};
    uint32_t status = 0;
    codboz::GuestAddr programCounter = 0;
    bool halted = false;
};

} // namespace

int main() {
    constexpr std::size_t kUpdate = 0;
    constexpr std::size_t kGetState = 1;
    constexpr std::size_t kExtGetHash = 2;
    std::vector<std::string> symbols = {
        "s3eKeyboardUpdate",
        "s3eKeyboardGetState",
        "s3eExtGetHash",
    };
    codboz::GuestMemory memory;
    memory.mapRegion(0x1000, 0x1000);
    codboz::Runtime::Config config;
    config.xperiaControls = true;
    codboz::Runtime runtime(memory, symbols, config);
    TestCpu cpu;

    runtime.queueControllerButton(codboz::ControllerAction::Accept, true);
    runtime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    cpu.setReg(0, 208);
    runtime.dispatch(codboz::importTrapAddress(kGetState), cpu);
    CHECK(cpu.reg(0) == 0);
    runtime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    cpu.setReg(0, 208);
    runtime.dispatch(codboz::importTrapAddress(kGetState), cpu);
    CHECK(cpu.reg(0) == 0);
    runtime.queueControllerButton(codboz::ControllerAction::Accept, false);
    runtime.dispatch(codboz::importTrapAddress(kUpdate), cpu);
    cpu.setReg(0, 208);
    runtime.dispatch(codboz::importTrapAddress(kGetState), cpu);
    CHECK(cpu.reg(0) == 0);

    cpu.setReg(0, 0x1dbd7ce8u);
    cpu.setReg(1, 0x1000);
    cpu.setReg(2, 20);
    runtime.dispatch(codboz::importTrapAddress(kExtGetHash), cpu);
    CHECK(cpu.reg(0) == 0);
    const auto getTouchpadInt = memory.read32(0x1010);
    cpu.setReg(0, 1);
    runtime.dispatch(getTouchpadInt, cpu);
    CHECK(cpu.reg(0) == 960);
    cpu.setReg(0, 2);
    runtime.dispatch(getTouchpadInt, cpu);
    CHECK(cpu.reg(0) == 544);

    constexpr codboz::GuestAddr buttonCallback = 0x2000;
    constexpr codboz::GuestAddr motionCallback = 0x3000;
    const auto registerTouchpad = memory.read32(0x1000);
    cpu.setReg(0, 0);
    cpu.setReg(1, buttonCallback);
    runtime.dispatch(registerTouchpad, cpu);
    cpu.setReg(0, 1);
    cpu.setReg(1, motionCallback);
    runtime.dispatch(registerTouchpad, cpu);

    runtime.queueControllerAxes(0.5f, -0.5f, -1.0f, 1.0f);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(codboz::ControllerInputTestAccess::touchpadActive(runtime, 0));
    CHECK(codboz::ControllerInputTestAccess::touchpadActive(runtime, 1));
    CHECK(codboz::ControllerInputTestAccess::touchpadState(runtime, 0) == 3);
    CHECK(codboz::ControllerInputTestAccess::touchpadState(runtime, 1) == 3);
    CHECK(codboz::ControllerInputTestAccess::touchpadX(runtime, 0) == 288);
    CHECK(codboz::ControllerInputTestAccess::touchpadY(runtime, 0) == 136);
    CHECK(codboz::ControllerInputTestAccess::touchpadX(runtime, 1) == 648);
    CHECK(codboz::ControllerInputTestAccess::touchpadY(runtime, 1) == 392);
    CHECK(codboz::ControllerInputTestAccess::pendingCount(runtime) == 4);
    CHECK(codboz::ControllerInputTestAccess::pendingFunction(runtime, 0) == motionCallback);
    CHECK(codboz::ControllerInputTestAccess::pendingFunction(runtime, 1) == buttonCallback);
    const auto leftMotion = codboz::ControllerInputTestAccess::motionEvent(runtime, 0);
    const auto leftPress = codboz::ControllerInputTestAccess::buttonEvent(runtime, 1);
    CHECK(leftMotion.id == 0);
    CHECK(leftMotion.x == 288);
    CHECK(leftMotion.y == 136);
    CHECK(leftPress.id == 0);
    CHECK(leftPress.pressed == 1);
    CHECK(leftPress.x == leftMotion.x);
    CHECK(leftPress.y == leftMotion.y);

    runtime.queueControllerAxes(0.0f, 0.0f, 0.0f, 0.0f);
    codboz::ControllerInputTestAccess::pump(runtime);
    CHECK(!codboz::ControllerInputTestAccess::touchpadActive(runtime, 0));
    CHECK(!codboz::ControllerInputTestAccess::touchpadActive(runtime, 1));
    CHECK(codboz::ControllerInputTestAccess::touchpadState(runtime, 0) == 4);
    CHECK(codboz::ControllerInputTestAccess::touchpadState(runtime, 1) == 4);

    return 0;
}
