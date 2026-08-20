#include "game/codboz_fixups.hpp"

#include "test_macros.hpp"

#include <string>

int main() {
    using namespace codboz;
    constexpr GuestAddr base = 0x4a000000u;
    constexpr GuestAddr stack = 0x70001000u;

    GuestMemory memory;
    memory.mapRegion(base, 0x380000u);
    memory.mapRegion(stack, 0x1000u);
    CodbozFixups fixups;
    fixups.init(memory, base);

    uint32_t registers[16]{};
    registers[1] = 0;
    CHECK(fixups.tryFixup(memory, base + 0x368ddcu, registers));
    CHECK(registers[1] != 0);
    CHECK(memory.readString(registers[1], 1).empty());
    CHECK(fixups.lastKind() == CodbozFixups::Kind::NullString);
    CHECK(fixups.nullStringCount() == 1);
    CHECK(std::string(CodbozFixups::name(fixups.lastKind())) == "null-string");

    registers[0] = 0;
    registers[13] = stack;
    memory.write32(stack, 4);
    memory.write32(stack + 4, 5);
    memory.write32(stack + 8, 6);
    memory.write32(stack + 12, base + 0x1001u);
    CHECK(fixups.tryFixup(memory, base + 0xcb8eeu, registers));
    CHECK(registers[4] == 4);
    CHECK(registers[5] == 5);
    CHECK(registers[6] == 6);
    CHECK(registers[13] == stack + 16);
    CHECK(registers[15] == base + 0x1000u);
    CHECK(fixups.lastKind() == CodbozFixups::Kind::NullBuffer);
    CHECK(fixups.nullBufferCount() == 1);

    registers[0] = 0x4a200000u;
    registers[1] = 2;
    registers[4] = 2;
    registers[5] = registers[0];
    CHECK(fixups.tryFixup(memory, base + 0x374be8u, registers));
    CHECK(memory.read32(registers[0] + 0x4cu) == registers[2]);
    CHECK(fixups.lastKind() == CodbozFixups::Kind::BucketAllocator);
    CHECK(fixups.bucketCount() == 1);

    CHECK(!fixups.tryFixup(memory, base + 0x1234u, registers));
    CHECK(fixups.lastKind() == CodbozFixups::Kind::None);
    return 0;
}
