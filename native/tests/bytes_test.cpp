#include "loader/bytes.hpp"

#include "test_macros.hpp"

#include <cstdint>
#include <vector>

int main() {
    std::vector<uint8_t> data = {0x01, 0x00, 0x02, 0x00, 0x78, 0x56, 0x34, 0x12,
                                 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    codboz::Reader r(data);
    CHECK(r.readLe16() == 0x0001);
    CHECK(r.readLe16() == 0x0002);
    CHECK(r.readLe32() == 0x12345678);
    CHECK(r.readLe64() == 0x8070605040302010u);
    CHECK(r.remaining() == 0);

    uint8_t sd[] = {0x41, 0x42, 0x00, 0x43};
    codboz::Reader s(sd);
    CHECK(s.readCString(16) == "AB");
    CHECK(s.readU8() == 0x43);

    uint8_t tiny[] = {0x01, 0x02};
    CHECK_THROWS((codboz::Reader(tiny).readLe32()));
    uint8_t d4[] = {0x78, 0x56, 0x34, 0x12};
    CHECK(codboz::readLe32(d4) == 0x12345678);
    return 0;
}
