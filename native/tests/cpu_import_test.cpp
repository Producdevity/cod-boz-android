// Import dispatch: the guest "calls" import 0 by jumping to its trap address.
// The host handler reads r0, doubles it, and writes it back; execution returns
// to the caller and halts at the callback-return trap.
#include "cpu/dynarmic_cpu.hpp"
#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

#include "test_macros.hpp"

int main() {
    codboz::GuestMemory mem;
    mem.mapRegion(0x80000, 0x1000); // stack only; the trap needs no code

    codboz::GuestAddr seenTrap = 0;
    auto handler = [&](codboz::GuestAddr trapAddr, codboz::GuestCpu &cpu) {
        seenTrap = trapAddr;
        if (trapAddr == codboz::kTrapBase) {
            cpu.setReg(0, cpu.reg(0) * 2);
        }
    };

    codboz::DynarmicCpu cpu(mem, handler);
    cpu.setReg(0, 21);
    cpu.setReg(1, 2);
    cpu.setReg(13, 0x80800);                     // sp
    cpu.setReg(14, codboz::kCallbackReturnTrap); // lr
    cpu.setThumb(false);
    cpu.setPc(codboz::kTrapBase); // import 0 trap address

    auto outcome = cpu.run();
    CHECK(seenTrap == codboz::kTrapBase);
    CHECK(outcome == codboz::RunOutcome::CallbackReturned);
    CHECK(cpu.reg(0) == 42);
    return 0;
}
