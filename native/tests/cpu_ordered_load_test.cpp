#include "cpu/dynarmic_cpu.hpp"
#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

#include "test_macros.hpp"

int main() {
    codboz::GuestMemory mem;
    constexpr codboz::GuestAddr kCode = 0x1000;
    constexpr codboz::GuestAddr kData = 0x2000;
    mem.mapRegion(kCode, 0x1000);
    mem.mapRegion(kData, 0x1000);
    mem.write32(kData, 0x78563412u);

    mem.write32(kCode, 0xE1910C9Fu);     // lda r0, [r1]
    mem.write32(kCode + 4, 0xE12FFF1Eu); // bx lr

    codboz::DynarmicCpu cpu(mem, nullptr);
    cpu.syncPageTable();
    cpu.setReg(1, kData);
    cpu.setReg(14, codboz::kCallbackReturnTrap);
    cpu.setThumb(false);
    cpu.setPc(kCode);

    CHECK(cpu.run() == codboz::RunOutcome::CallbackReturned);
    CHECK(cpu.reg(0) == 0x78563412u);
    return 0;
}
