// Synthetic ARM execution: a tiny ARM program computes r0 = 7 + 5 and returns
// through the callback-return trap. Verifies the Dynarmic A32 JIT executes real
// ARM instructions over GuestMemory and stops at the halt trap.
#include "cpu/dynarmic_cpu.hpp"
#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

#include "test_macros.hpp"

int main() {
    codboz::GuestMemory mem;
    constexpr codboz::GuestAddr kCode = 0x1000;
    mem.mapRegion(kCode, 0x1000);   // code
    mem.mapRegion(0x80000, 0x1000); // stack

    // mov r0,#7 ; mov r1,#5 ; add r0,r0,r1 ; bx lr
    mem.write32(kCode + 0x0, 0xE3A00007u);
    mem.write32(kCode + 0x4, 0xE3A01005u);
    mem.write32(kCode + 0x8, 0xE0800001u);
    mem.write32(kCode + 0xC, 0xE12FFF1Eu);

    codboz::DynarmicCpu cpu(mem, /*handler=*/nullptr);
    cpu.setReg(13, 0x80800);                     // sp
    cpu.setReg(14, codboz::kCallbackReturnTrap); // lr -> halt trap
    cpu.setThumb(false);
    cpu.setPc(kCode);

    auto outcome = cpu.run();
    CHECK(outcome == codboz::RunOutcome::CallbackReturned);
    CHECK(cpu.reg(0) == 12);
    return 0;
}
