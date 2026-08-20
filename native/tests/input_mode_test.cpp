#include "bridge/dispatcher.hpp"
#include "loader/guest_loader.hpp"

#include "test_macros.hpp"

#include <array>
#include <vector>

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

constexpr std::size_t kExtGetHash = 0;
constexpr std::size_t kDeviceGetInt = 1;
constexpr std::size_t kDeviceGetString = 2;
constexpr uint32_t kTouchpadHash = 0x1dbd7ce8u;

std::string deviceString(codboz::Runtime &runtime, codboz::GuestMemory &memory, TestCpu &cpu,
                         uint32_t key) {
    cpu.setReg(0, key);
    runtime.dispatch(codboz::importTrapAddress(kDeviceGetString), cpu);
    return memory.readString(cpu.reg(0), 128);
}

} // namespace

int main() {
    const std::vector<std::string> symbols = {
        "s3eExtGetHash",
        "s3eDeviceGetInt",
        "s3eDeviceGetString",
    };

    codboz::GuestMemory touchMemory;
    touchMemory.mapRegion(0x1000, 0x1000);
    codboz::Runtime::Config touchConfig;
    touchConfig.deviceModel = "Pixel 10 Pro";
    codboz::Runtime touchRuntime(touchMemory, symbols, touchConfig);
    TestCpu cpu;

    cpu.setReg(0, kTouchpadHash);
    cpu.setReg(1, 0x1000);
    cpu.setReg(2, 20);
    touchRuntime.dispatch(codboz::importTrapAddress(kExtGetHash), cpu);
    CHECK(cpu.reg(0) == 1);
    CHECK(deviceString(touchRuntime, touchMemory, cpu, 2) == "Pixel 10 Pro");
    CHECK(deviceString(touchRuntime, touchMemory, cpu, 0x15) == "Pixel 10 Pro");

    cpu.setReg(0, 0);
    touchRuntime.dispatch(codboz::importTrapAddress(kDeviceGetInt), cpu);
    CHECK(cpu.reg(0) == 0x12);
    cpu.setReg(0, 10);
    touchRuntime.dispatch(codboz::importTrapAddress(kDeviceGetInt), cpu);
    CHECK(cpu.reg(0) == 0x00080101);

    touchRuntime.setControllerConnected(true);
    cpu.setReg(0, kTouchpadHash);
    cpu.setReg(1, 0x1000);
    cpu.setReg(2, 20);
    touchRuntime.dispatch(codboz::importTrapAddress(kExtGetHash), cpu);
    CHECK(cpu.reg(0) == 0);
    CHECK(deviceString(touchRuntime, touchMemory, cpu, 2) == "R800i");

    codboz::GuestMemory controllerMemory;
    controllerMemory.mapRegion(0x1000, 0x1000);
    codboz::Runtime::Config controllerConfig;
    controllerConfig.xperiaControls = true;
    controllerConfig.deviceModel = "Pixel 10 Pro";
    codboz::Runtime controllerRuntime(controllerMemory, symbols, controllerConfig);

    cpu.setReg(0, kTouchpadHash);
    cpu.setReg(1, 0x1000);
    cpu.setReg(2, 20);
    controllerRuntime.dispatch(codboz::importTrapAddress(kExtGetHash), cpu);
    CHECK(cpu.reg(0) == 0);
    CHECK(deviceString(controllerRuntime, controllerMemory, cpu, 2) == "R800i");
    CHECK(deviceString(controllerRuntime, controllerMemory, cpu, 0x15) ==
          "Sony Ericsson Xperia Play");
    cpu.setReg(0, 0);
    controllerRuntime.dispatch(codboz::importTrapAddress(kDeviceGetInt), cpu);
    CHECK(cpu.reg(0) == 0x12);

    controllerRuntime.setControllerConnected(false);
    cpu.setReg(0, kTouchpadHash);
    cpu.setReg(1, 0x1000);
    cpu.setReg(2, 20);
    controllerRuntime.dispatch(codboz::importTrapAddress(kExtGetHash), cpu);
    CHECK(cpu.reg(0) == 1);
    CHECK(deviceString(controllerRuntime, controllerMemory, cpu, 2) == "Pixel 10 Pro");

    return 0;
}
