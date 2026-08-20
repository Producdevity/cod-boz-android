#include "cpu/dynarmic_cpu.hpp"
#include "guest/guest_call.hpp"
#include "guest/guest_memory.hpp"

#include "test_macros.hpp"

#include <array>
#include <bit>
#include <cmath>

int main() {
    using namespace codboz;

    GuestMemory memory;
    constexpr GuestAddr armCode = 0x1000;
    constexpr GuestAddr thumbCode = 0x2000;
    constexpr GuestAddr stackBase = 0x80000;
    memory.mapRegion(armCode, 0x1000);
    memory.mapRegion(thumbCode, 0x1000);
    memory.mapRegion(stackBase, 0x2000);

    memory.write32(armCode + 0x00, 0xE59D4000u);
    memory.write32(armCode + 0x04, 0xE0800001u);
    memory.write32(armCode + 0x08, 0xE0800002u);
    memory.write32(armCode + 0x0c, 0xE0800003u);
    memory.write32(armCode + 0x10, 0xE0800004u);
    memory.write32(armCode + 0x14, 0xE12FFF1Eu);

    memory.write16(thumbCode + 0x00, 0x200cu);
    memory.write16(thumbCode + 0x02, 0x4770u);

    DynarmicCpu cpu(memory, nullptr);
    cpu.setReg(0, 0x11111111u);
    cpu.setReg(1, 0x22222222u);
    cpu.setReg(4, 0x44444444u);
    cpu.setReg(13, stackBase + 0x1800);
    cpu.setReg(14, 0x55555555u);
    cpu.setPc(0x66666666u);
    auto savedFp = cpu.fpRegs();
    savedFp[0] = 0x12345678u;
    savedFp[63] = 0x87654321u;
    cpu.setFpRegs(savedFp);
    cpu.setFpscr(0x00c00000u);
    const uint32_t savedCpsr = cpu.cpsr();

    const std::array arguments = {GuestArgument::u32(1), GuestArgument::u32(2),
                                  GuestArgument::u32(3), GuestArgument::u32(4),
                                  GuestArgument::u32(5)};
    const auto armResult = callGuest(cpu, memory, armCode, arguments);
    CHECK(armResult.status == GuestCallStatus::Returned);
    CHECK(static_cast<uint32_t>(armResult.returnValue) == 15);
    CHECK(cpu.reg(0) == 0x11111111u);
    CHECK(cpu.reg(1) == 0x22222222u);
    CHECK(cpu.reg(4) == 0x44444444u);
    CHECK(cpu.reg(13) == stackBase + 0x1800);
    CHECK(cpu.reg(14) == 0x55555555u);
    CHECK(cpu.pc() == 0x66666666u);
    CHECK(cpu.cpsr() == savedCpsr);
    CHECK(cpu.fpRegs() == savedFp);
    CHECK(cpu.fpscr() == 0x00c00000u);

    const auto thumbResult = callGuest(cpu, memory, thumbCode | 1u, {});
    CHECK(thumbResult.status == GuestCallStatus::Returned);
    CHECK(static_cast<uint32_t>(thumbResult.returnValue) == 12);

    GuestCallOptions interruptedOptions;
    interruptedOptions.interrupted = [] { return true; };
    const auto interruptedResult = callGuest(cpu, memory, armCode, arguments, interruptedOptions);
    CHECK(interruptedResult.status == GuestCallStatus::Interrupted);
    CHECK(interruptedResult.outcome == RunOutcome::Halted);
    CHECK(interruptedResult.slices == 0);
    CHECK(cpu.reg(0) == 0x11111111u);
    CHECK(cpu.reg(13) == stackBase + 0x1800);
    CHECK(cpu.pc() == 0x66666666u);
    CHECK(cpu.fpRegs() == savedFp);

    constexpr uint64_t integer64 = 0x8877665544332211ull;
    constexpr uint64_t return64 = 0xfedcba9876543210ull;
    bool importCalled = false;
    DynarmicCpu importCpu(memory, [&](GuestAddr trap, GuestCpu &guestCpu) {
        CHECK(trap == kTrapBase);
        GuestArguments decoded(guestCpu);
        CHECK(decoded.u32() == 7);
        CHECK(decoded.u64() == integer64);
        CHECK(std::abs(decoded.f32() - 1.25f) < 0.0001f);
        CHECK(std::abs(decoded.f64() - 9.5) < 0.0001);
        GuestArguments::setU64Return(guestCpu, return64);
        importCalled = true;
    });
    importCpu.setReg(0, 7);
    importCpu.setReg(2, static_cast<uint32_t>(integer64));
    importCpu.setReg(3, static_cast<uint32_t>(integer64 >> 32u));
    importCpu.setReg(13, stackBase + 0x1000);
    importCpu.setReg(14, kCallbackReturnTrap);
    memory.write32(stackBase + 0x1000, std::bit_cast<uint32_t>(1.25f));
    memory.write32(stackBase + 0x1004, 0);
    const uint64_t doubleBits = std::bit_cast<uint64_t>(9.5);
    memory.write32(stackBase + 0x1008, static_cast<uint32_t>(doubleBits));
    memory.write32(stackBase + 0x100c, static_cast<uint32_t>(doubleBits >> 32u));
    importCpu.setThumb(false);
    importCpu.setPc(kTrapBase);
    CHECK(importCpu.run() == RunOutcome::CallbackReturned);
    CHECK(importCalled);
    CHECK(importCpu.reg(0) == static_cast<uint32_t>(return64));
    CHECK(importCpu.reg(1) == static_cast<uint32_t>(return64 >> 32u));
    return 0;
}
