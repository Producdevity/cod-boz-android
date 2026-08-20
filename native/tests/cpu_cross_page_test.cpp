#include "cpu/dynarmic_cpu.hpp"
#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

#include "test_macros.hpp"

int main() {
    codboz::GuestMemory mem;
    constexpr codboz::GuestAddr kCode = 0x1000;
    constexpr codboz::GuestAddr kData = 0x2fff;
    mem.mapRegion(kCode, 0x1000);
    mem.mapRegion(0x2000, 0x1000);
    for (uint32_t page = 0; page < 512; ++page) {
        mem.mapRegion(0x100000 + page * 0x2000, 0x1000);
    }
    mem.mapRegion(0x3000, 0x1000);
    mem.write32(kData, 0x78563412u);

    mem.write32(kCode, 0xE5910000u);
    mem.write32(kCode + 4, 0xE12FFF1Eu);

    codboz::DynarmicCpu cpu(mem, nullptr);
    cpu.setReg(1, kData);
    cpu.setReg(14, codboz::kCallbackReturnTrap);
    cpu.setThumb(false);
    cpu.setPc(kCode);

    CHECK(cpu.run() == codboz::RunOutcome::CallbackReturned);
    CHECK(cpu.reg(0) == 0x78563412u);
    return 0;
}
