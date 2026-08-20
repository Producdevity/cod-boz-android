#include "bridge/dispatcher.hpp"
#include "bridge/callback_list.hpp"
#include "loader/guest_loader.hpp"
#include "loader/source_profile.hpp"

#include "test_macros.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace codboz {

class PointerInputTestAccess {
  public:
    static void pump(Runtime &runtime) {
        runtime.pumpPointerEvents();
    }

    static std::size_t queuedEventCount(const Runtime &runtime) {
        return runtime.pointerEvents_.size();
    }

    static std::size_t pendingCallbackCount(const Runtime &runtime) {
        return runtime.pendingGuestCallbacks_.size();
    }

    static void setRunningGuestCallback(Runtime &runtime, bool running) {
        runtime.runningGuestCallback_ = running;
    }

    static GuestAddr pendingFunction(const Runtime &runtime, std::size_t index) {
        return runtime.pendingGuestCallbacks_[index].function;
    }

    static guest::PointerTouchEvent touchEvent(const Runtime &runtime, std::size_t index) {
        guest::PointerTouchEvent event{};
        const auto &payload = runtime.pendingGuestCallbacks_[index].payload;
        std::memcpy(&event, payload.data(), sizeof(event));
        return event;
    }

    static guest::PointerTouchMotionEvent motionEvent(const Runtime &runtime, std::size_t index) {
        guest::PointerTouchMotionEvent event{};
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
    void halt() override {}
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
};

} // namespace

int main() {
    struct Callback {
        codboz::GuestAddr function = 0;
        codboz::GuestAddr userData = 0;
    };
    codboz::CallbackList<Callback> callbacks;
    callbacks.add(0x1000, 0x10);
    callbacks.add(0x2000, 0x20);
    CHECK(callbacks.size() == 2);
    CHECK(callbacks.entries()[0].function == 0x1000);
    CHECK(callbacks.entries()[1].function == 0x2000);
    callbacks.add(0x1000, 0x30);
    CHECK(callbacks.size() == 2);
    CHECK(callbacks.entries()[0].userData == 0x30);
    callbacks.remove(0x1000);
    CHECK(callbacks.size() == 1);
    CHECK(callbacks.entries()[0].function == 0x2000);
    callbacks.remove(0);
    CHECK(callbacks.size() == 0);

    constexpr std::size_t kGetInt = 0;
    constexpr std::size_t kUpdate = 1;
    constexpr std::size_t kGetTouchState = 2;
    constexpr std::size_t kGetTouchX = 3;
    constexpr std::size_t kGetTouchY = 4;
    constexpr std::size_t kGetTouchPressure = 5;
    constexpr std::size_t kDeviceYield = 6;
    constexpr std::size_t kSetInt = 7;
    const std::vector<std::string> symbols = {
        "s3ePointerGetInt",    "s3ePointerUpdate",    "s3ePointerGetTouchState",
        "s3ePointerGetTouchX", "s3ePointerGetTouchY", "s3ePointerGetTouchPressure",
        "s3eDeviceYield",      "s3ePointerSetInt",
    };
    codboz::GuestMemory memory;
    codboz::Runtime::Config config;
    codboz::Runtime runtime(memory, symbols, config);
    TestCpu cpu;

    const std::array<uint32_t, 5> expectedProperties = {1, 0, 2, 2, 1};
    for (std::size_t property = 0; property < expectedProperties.size(); ++property) {
        cpu.setReg(0, static_cast<uint32_t>(property));
        runtime.dispatch(codboz::importTrapAddress(kGetInt), cpu);
        CHECK(cpu.reg(0) == expectedProperties[property]);
    }
    for (uint32_t property = 5; property <= 12; ++property) {
        cpu.setReg(0, property);
        cpu.setReg(1, property * 100);
        runtime.dispatch(codboz::importTrapAddress(kSetInt), cpu);
        CHECK(cpu.reg(0) == 0);
    }
    for (uint32_t property : {4u, 13u}) {
        cpu.setReg(0, property);
        runtime.dispatch(codboz::importTrapAddress(kSetInt), cpu);
        CHECK(cpu.reg(0) == 1);
    }

    {
        const std::vector<std::string> legacyCallbackSymbols = {
            "s3ePointerRegister",
            "s3ePointerUpdate",
            "s3eDeviceYield",
            "s3ePointerGetTouchX",
        };
        codboz::GuestMemory legacyCallbackMemory;
        codboz::Runtime legacyCallbackRuntime(legacyCallbackMemory, legacyCallbackSymbols, config);
        TestCpu legacyCallbackCpu;
        legacyCallbackCpu.setReg(0, 0);
        legacyCallbackCpu.setReg(1, 0x1000);
        legacyCallbackRuntime.dispatch(codboz::importTrapAddress(0), legacyCallbackCpu);
        legacyCallbackRuntime.queuePointerEvent(0, 1, 20, 30);
        legacyCallbackRuntime.queuePointerEvent(2, 1, 25, 35);
        legacyCallbackRuntime.dispatch(codboz::importTrapAddress(2), legacyCallbackCpu);
        legacyCallbackRuntime.dispatch(codboz::importTrapAddress(1), legacyCallbackCpu);
        legacyCallbackRuntime.dispatch(codboz::importTrapAddress(2), legacyCallbackCpu);
        legacyCallbackRuntime.dispatch(codboz::importTrapAddress(1), legacyCallbackCpu);
        legacyCallbackCpu.setReg(0, 0);
        legacyCallbackRuntime.dispatch(codboz::importTrapAddress(3), legacyCallbackCpu);
        CHECK(legacyCallbackCpu.reg(0) == 25);

        codboz::GuestMemory pressMotionMemory;
        codboz::Runtime pressMotionRuntime(pressMotionMemory, legacyCallbackSymbols, config);
        TestCpu pressMotionCpu;
        pressMotionCpu.setReg(0, 1);
        pressMotionCpu.setReg(1, 0x1000);
        pressMotionRuntime.dispatch(codboz::importTrapAddress(0), pressMotionCpu);
        pressMotionRuntime.queuePointerEvent(0, 1, 30, 40);
        pressMotionRuntime.queuePointerEvent(2, 1, 35, 45);
        pressMotionRuntime.dispatch(codboz::importTrapAddress(2), pressMotionCpu);
        pressMotionRuntime.dispatch(codboz::importTrapAddress(1), pressMotionCpu);
        pressMotionRuntime.dispatch(codboz::importTrapAddress(2), pressMotionCpu);
        pressMotionRuntime.dispatch(codboz::importTrapAddress(1), pressMotionCpu);
        pressMotionCpu.setReg(0, 0);
        pressMotionRuntime.dispatch(codboz::importTrapAddress(3), pressMotionCpu);
        CHECK(pressMotionCpu.reg(0) == 35);

        codboz::GuestMemory touchCallbackMemory;
        codboz::Runtime touchCallbackRuntime(touchCallbackMemory, legacyCallbackSymbols, config);
        TestCpu touchCallbackCpu;
        touchCallbackCpu.setReg(0, 2);
        touchCallbackCpu.setReg(1, 0x1000);
        touchCallbackRuntime.dispatch(codboz::importTrapAddress(0), touchCallbackCpu);
        touchCallbackRuntime.queuePointerEvent(0, 1, 20, 30);
        touchCallbackRuntime.queuePointerEvent(2, 1, 25, 35);
        touchCallbackRuntime.dispatch(codboz::importTrapAddress(2), touchCallbackCpu);
        touchCallbackRuntime.dispatch(codboz::importTrapAddress(1), touchCallbackCpu);
        touchCallbackRuntime.dispatch(codboz::importTrapAddress(2), touchCallbackCpu);
        touchCallbackRuntime.dispatch(codboz::importTrapAddress(1), touchCallbackCpu);
        touchCallbackCpu.setReg(0, 0);
        touchCallbackRuntime.dispatch(codboz::importTrapAddress(3), touchCallbackCpu);
        CHECK(touchCallbackCpu.reg(0) == 25);

        codboz::GuestMemory motionCallbackMemory;
        codboz::Runtime motionCallbackRuntime(motionCallbackMemory, legacyCallbackSymbols, config);
        TestCpu motionCallbackCpu;
        motionCallbackCpu.setReg(0, 3);
        motionCallbackCpu.setReg(1, 0x1000);
        motionCallbackRuntime.dispatch(codboz::importTrapAddress(0), motionCallbackCpu);
        motionCallbackRuntime.queuePointerEvent(0, 1, 20, 30);
        motionCallbackRuntime.queuePointerEvent(2, 1, 25, 35);
        motionCallbackRuntime.dispatch(codboz::importTrapAddress(2), motionCallbackCpu);
        motionCallbackRuntime.dispatch(codboz::importTrapAddress(1), motionCallbackCpu);
        motionCallbackRuntime.dispatch(codboz::importTrapAddress(2), motionCallbackCpu);
        motionCallbackRuntime.dispatch(codboz::importTrapAddress(1), motionCallbackCpu);
        motionCallbackCpu.setReg(0, 0);
        motionCallbackRuntime.dispatch(codboz::importTrapAddress(3), motionCallbackCpu);
        CHECK(motionCallbackCpu.reg(0) == 25);
    }

    {
        const std::vector<std::string> callbackSymbols = {
            "s3ePointerRegister",
            "s3ePointerUpdate",
        };
        codboz::GuestMemory callbackMemory;
        codboz::Runtime callbackRuntime(callbackMemory, callbackSymbols, config);
        TestCpu callbackCpu;
        constexpr codboz::GuestAddr touchCallback = 0x2000;
        constexpr codboz::GuestAddr motionCallback = 0x3000;
        callbackCpu.setReg(0, 2);
        callbackCpu.setReg(1, touchCallback);
        callbackRuntime.dispatch(codboz::importTrapAddress(0), callbackCpu);
        callbackCpu.setReg(0, 3);
        callbackCpu.setReg(1, motionCallback);
        callbackRuntime.dispatch(codboz::importTrapAddress(0), callbackCpu);

        callbackRuntime.queuePointerEvent(0, 31, 100, 110);
        callbackRuntime.queuePointerEvent(2, 31, 120, 130);
        callbackRuntime.queuePointerEvent(2, 31, 140, 150);
        callbackRuntime.queuePointerEvent(1, 31, 160, 170);
        CHECK(codboz::PointerInputTestAccess::queuedEventCount(callbackRuntime) == 4);
        codboz::PointerInputTestAccess::pump(callbackRuntime);
        CHECK(codboz::PointerInputTestAccess::queuedEventCount(callbackRuntime) == 3);
        CHECK(codboz::PointerInputTestAccess::pendingCallbackCount(callbackRuntime) == 1);
        codboz::PointerInputTestAccess::setRunningGuestCallback(callbackRuntime, true);
        callbackRuntime.dispatch(codboz::importTrapAddress(1), callbackCpu);
        codboz::PointerInputTestAccess::setRunningGuestCallback(callbackRuntime, false);
        codboz::PointerInputTestAccess::pump(callbackRuntime);
        CHECK(codboz::PointerInputTestAccess::queuedEventCount(callbackRuntime) == 3);
        callbackRuntime.dispatch(codboz::importTrapAddress(1), callbackCpu);
        codboz::PointerInputTestAccess::pump(callbackRuntime);
        CHECK(codboz::PointerInputTestAccess::queuedEventCount(callbackRuntime) == 1);
        CHECK(codboz::PointerInputTestAccess::pendingCallbackCount(callbackRuntime) == 3);
        codboz::PointerInputTestAccess::pump(callbackRuntime);
        CHECK(codboz::PointerInputTestAccess::queuedEventCount(callbackRuntime) == 0);
        CHECK(codboz::PointerInputTestAccess::pendingCallbackCount(callbackRuntime) == 4);
        CHECK(codboz::PointerInputTestAccess::pendingFunction(callbackRuntime, 0) == touchCallback);
        CHECK(codboz::PointerInputTestAccess::pendingFunction(callbackRuntime, 1) ==
              motionCallback);
        CHECK(codboz::PointerInputTestAccess::pendingFunction(callbackRuntime, 2) ==
              motionCallback);
        CHECK(codboz::PointerInputTestAccess::pendingFunction(callbackRuntime, 3) == touchCallback);
        const auto press = codboz::PointerInputTestAccess::touchEvent(callbackRuntime, 0);
        const auto firstMotion = codboz::PointerInputTestAccess::motionEvent(callbackRuntime, 1);
        const auto secondMotion = codboz::PointerInputTestAccess::motionEvent(callbackRuntime, 2);
        const auto release = codboz::PointerInputTestAccess::touchEvent(callbackRuntime, 3);
        CHECK(press.touch_id == 0);
        CHECK(press.pressed == 1);
        CHECK(press.x == 100);
        CHECK(press.y == 110);
        CHECK(firstMotion.touch_id == 0);
        CHECK(firstMotion.x == 120);
        CHECK(firstMotion.y == 130);
        CHECK(secondMotion.touch_id == 0);
        CHECK(secondMotion.x == 140);
        CHECK(secondMotion.y == 150);
        CHECK(release.touch_id == 0);
        CHECK(release.pressed == 0);
        CHECK(release.x == 160);
        CHECK(release.y == 170);
    }

    const auto pump = [&] { runtime.dispatch(codboz::importTrapAddress(kDeviceYield), cpu); };
    const auto update = [&] { runtime.dispatch(codboz::importTrapAddress(kUpdate), cpu); };
    const auto touchState = [&](uint32_t id) {
        cpu.setReg(0, id);
        runtime.dispatch(codboz::importTrapAddress(kGetTouchState), cpu);
        return cpu.reg(0);
    };
    const auto touchX = [&](uint32_t id) {
        cpu.setReg(0, id);
        runtime.dispatch(codboz::importTrapAddress(kGetTouchX), cpu);
        return cpu.reg(0);
    };
    const auto touchY = [&](uint32_t id) {
        cpu.setReg(0, id);
        runtime.dispatch(codboz::importTrapAddress(kGetTouchY), cpu);
        return cpu.reg(0);
    };

    runtime.queuePointerEvent(0, 99, 40, 50);
    pump();
    update();
    CHECK(touchState(0) == 3);
    CHECK(touchX(0) == 40);
    CHECK(touchY(0) == 50);
    cpu.setReg(0, 0);
    runtime.dispatch(codboz::importTrapAddress(kGetTouchPressure), cpu);
    CHECK(cpu.reg(0) == 1);
    cpu.setReg(0, 0);
    runtime.dispatch(codboz::importTrapAddress(kGetTouchPressure), cpu);
    CHECK(cpu.reg(0) == 1);
    runtime.dispatch(codboz::importTrapAddress(kDeviceYield), cpu);
    CHECK(touchState(0) == 3);
    update();
    CHECK(touchState(0) == 1);
    runtime.queuePointerEvent(1, 99, 40, 50);
    pump();
    update();
    CHECK(touchState(0) == 4);
    update();
    CHECK(touchState(0) == 0);

    runtime.queuePointerEvent(0, 31, 100, 110);
    runtime.queuePointerEvent(0, 17, 500, 300);
    pump();
    update();
    CHECK(touchState(0) == 3);
    CHECK(touchState(1) == 0);
    pump();
    update();
    CHECK(touchState(0) == 1);
    CHECK(touchState(1) == 3);
    update();
    CHECK(touchState(0) == 1);
    CHECK(touchState(1) == 1);

    runtime.queuePointerEvent(2, 31, 120, 130);
    runtime.queuePointerEvent(2, 31, 140, 150);
    runtime.queuePointerEvent(2, 31, 160, 170);
    pump();
    update();
    CHECK(touchState(0) == 1);
    CHECK(touchState(1) == 1);
    CHECK(touchX(0) == 160);
    CHECK(touchY(0) == 170);

    runtime.queuePointerEvent(2, 31, 180, 190);
    runtime.queuePointerEvent(2, 17, 520, 320);
    runtime.queuePointerEvent(2, 31, 200, 210);
    pump();
    update();
    CHECK(touchX(0) == 200);
    CHECK(touchY(0) == 210);
    CHECK(touchX(1) == 520);
    CHECK(touchY(1) == 320);

    runtime.queuePointerEvent(1, 31, 200, 210);
    pump();
    update();
    CHECK(touchState(0) == 4);
    CHECK(touchState(1) == 1);
    update();
    CHECK(touchState(0) == 0);

    runtime.queuePointerEvent(0, 24, 200, 210);
    pump();
    update();
    CHECK(touchState(0) == 3);
    CHECK(touchState(1) == 1);
    update();
    CHECK(touchState(0) == 1);
    CHECK(touchState(1) == 1);
    CHECK(touchX(0) == 200);
    CHECK(touchY(0) == 210);

    runtime.queuePointerEvent(1, 17, 500, 300);
    runtime.queuePointerEvent(1, 24, 200, 210);
    pump();
    update();
    CHECK(touchState(0) == 1);
    CHECK(touchState(1) == 4);
    pump();
    update();
    CHECK(touchState(0) == 4);
    CHECK(touchState(1) == 0);
    update();
    CHECK(touchState(0) == 0);

    runtime.queuePointerEvent(0, 7, 80, 90);
    runtime.queuePointerEvent(0, 8, 300, 310);
    runtime.queuePointerEvent(1, 7, 80, 90);
    runtime.queuePointerEvent(1, 8, 300, 310);
    pump();
    update();
    CHECK(touchState(0) == 3);
    CHECK(touchState(1) == 0);
    pump();
    update();
    CHECK(touchState(0) == 1);
    CHECK(touchState(1) == 3);
    pump();
    update();
    CHECK(touchState(0) == 4);
    CHECK(touchState(1) == 1);
    pump();
    update();
    CHECK(touchState(0) == 0);
    CHECK(touchState(1) == 4);
    update();
    CHECK(touchState(0) == 0);
    CHECK(touchState(1) == 0);

    runtime.queuePointerEvent(0, 44, 400, 220);
    runtime.queuePointerEvent(1, 44, 400, 220);
    pump();
    update();
    CHECK(touchState(0) == 3);
    pump();
    update();
    CHECK(touchState(0) == 4);
    update();
    CHECK(touchState(0) == 0);

    return 0;
}
