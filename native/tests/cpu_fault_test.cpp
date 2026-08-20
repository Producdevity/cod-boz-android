// Fault containment: a guest load from unmapped memory must produce a controlled
// Fault outcome, never a host crash. ldr r0,[r1] with r1 pointing nowhere.
#include "cpu/dynarmic_cpu.hpp"
#include "guest/guest_memory.hpp"

#include "test_macros.hpp"

#include <array>

int main() {
    codboz::GuestMemory mem;
    constexpr codboz::GuestAddr kCode = 0x1000;
    mem.mapRegion(kCode, 0x1000); // code only; no data region mapped

    // ldr r0,[r1]  (0xE5910000) ; bx lr (0xE12FFF1E)
    mem.write32(kCode + 0x0, 0xE5910000u);
    mem.write32(kCode + 0x4, 0xE12FFF1Eu);

    codboz::DynarmicCpu cpu(mem, /*handler=*/nullptr);
    cpu.setReg(1, 0x70000); // unmapped
    cpu.setReg(14, codboz::kCallbackReturnTrap);
    cpu.setThumb(false);
    cpu.setPc(kCode);

    auto outcome = cpu.run();
    CHECK(outcome == codboz::RunOutcome::Fault);
    CHECK(cpu.pc() == kCode);
    CHECK(cpu.lastFaultAddr() == 0x70000);

    // The game runtime avoids a halt check after every memory access. Recovery
    // points instead get one-instruction blocks with a terminal halt check.
    mem.write32(kCode + 0x0, 0xE3A0202Au); // mov r2,#42
    mem.write32(kCode + 0x4, 0xE5910000u); // ldr r0,[r1]
    mem.write32(kCode + 0x8, 0xE3A03007u); // mov r3,#7
    mem.write32(kCode + 0xC, 0xE12FFF1Eu); // bx lr
    constexpr std::array recoveryPoints = {kCode + 0x4};
    codboz::DynarmicCpu preciseCpu(mem, /*handler=*/nullptr, recoveryPoints);
    preciseCpu.setReg(1, 0x70000);
    preciseCpu.setReg(14, codboz::kCallbackReturnTrap);
    preciseCpu.setThumb(false);
    preciseCpu.setPc(kCode);
    CHECK(preciseCpu.run() == codboz::RunOutcome::Fault);
    CHECK(preciseCpu.pc() == kCode + 0x4);
    CHECK(preciseCpu.reg(2) == 42);
    CHECK(preciseCpu.reg(3) == 0);
    CHECK(preciseCpu.lastFaultAddr() == 0x70000);

    // A real guest SVC must not be mistaken for a synthesized import trap.
    mem.write32(kCode, 0xEF000000u);
    codboz::DynarmicCpu svcCpu(mem, /*handler=*/nullptr);
    svcCpu.setThumb(false);
    svcCpu.setPc(kCode);
    CHECK(svcCpu.run() == codboz::RunOutcome::Fault);
    return 0;
}
