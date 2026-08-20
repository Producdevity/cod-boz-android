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

} // namespace

int main() {
    constexpr std::size_t kSurfaceGetInt = 0;
    constexpr std::size_t kGlGetInt = 1;
    constexpr std::size_t kEglQuerySurface = 2;
    const std::vector<std::string> symbols = {
        "s3eSurfaceGetInt",
        "s3eGLGetInt",
        "eglQuerySurface",
    };

    codboz::GuestMemory memory;
    memory.mapRegion(0x1000, 0x1000);
    codboz::Runtime::Config config;
    config.display = {1067, 480};
    codboz::Runtime runtime(memory, symbols, config);
    TestCpu cpu;

    cpu.setReg(0, 0);
    runtime.dispatch(codboz::importTrapAddress(kSurfaceGetInt), cpu);
    CHECK(cpu.reg(0) == 1067);
    cpu.setReg(0, 1);
    runtime.dispatch(codboz::importTrapAddress(kSurfaceGetInt), cpu);
    CHECK(cpu.reg(0) == 480);
    cpu.setReg(0, 2);
    runtime.dispatch(codboz::importTrapAddress(kSurfaceGetInt), cpu);
    CHECK(cpu.reg(0) == 1067 * 4);

    cpu.setReg(0, 0);
    runtime.dispatch(codboz::importTrapAddress(kGlGetInt), cpu);
    CHECK(cpu.reg(0) == 1067);
    cpu.setReg(0, 1);
    runtime.dispatch(codboz::importTrapAddress(kGlGetInt), cpu);
    CHECK(cpu.reg(0) == 480);

    cpu.setReg(2, 0x3057);
    cpu.setReg(3, 0x1000);
    runtime.dispatch(codboz::importTrapAddress(kEglQuerySurface), cpu);
    CHECK(cpu.reg(0) == 1);
    CHECK(memory.read32(0x1000) == 1067);
    cpu.setReg(2, 0x3056);
    runtime.dispatch(codboz::importTrapAddress(kEglQuerySurface), cpu);
    CHECK(memory.read32(0x1000) == 480);

    codboz::GuestMemory invalidMemory;
    codboz::Runtime::Config invalidConfig;
    invalidConfig.display = {0, 480};
    CHECK_THROWS(codboz::Runtime(invalidMemory, symbols, invalidConfig));
    return 0;
}
