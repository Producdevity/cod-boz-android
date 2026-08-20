#include "guest/guest_memory.hpp"

#include "test_macros.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

int main() {
    codboz::GuestMemory mem;
    mem.mapRegion(0x1000, 0x3000); // 3 pages at 0x1000..0x3fff

    CHECK(mem.isMapped(0x1000, 0x3000));
    CHECK(!mem.isMapped(0x0, 0x1000));
    CHECK(!mem.isMapped(0x1000, 0x3001));

    std::vector<std::uint8_t *> pageTable(4, nullptr);
    mem.populatePageTable(pageTable.data(), pageTable.size());
    CHECK(pageTable[1] != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(pageTable[1]) % 16 == 0);
    if (mem.fastmemPointer()) {
        CHECK(pageTable[1] == reinterpret_cast<std::uint8_t *>(*mem.fastmemPointer() + 0x1000));
    }

    mem.mapRegion(0x2000, 0x3000);
    CHECK(mem.isMapped(0x1000, 0x4000));
    std::vector<std::uint8_t *> expandedPageTable(5, nullptr);
    mem.populatePageTable(expandedPageTable.data(), expandedPageTable.size());
    CHECK(expandedPageTable[1] == pageTable[1]);
    CHECK(expandedPageTable[4] != nullptr);

    mem.write32(0x1000, 0xDEADBEEF);
    CHECK(mem.read32(0x1000) == 0xDEADBEEF);
    mem.write8(0x1004, 0xAB);
    CHECK(mem.read8(0x1004) == 0xAB);
    mem.write16(0x1005, 0x1234);
    CHECK(mem.read16(0x1005) == 0x1234);

    // 64-bit value straddling a page boundary (0x1ffc..0x2003).
    mem.write64(0x1ffc, 0x0123456789ABCDEFULL);
    CHECK(mem.read64(0x1ffc) == 0x0123456789ABCDEFULL);

    // Bulk write/read across pages.
    std::vector<uint8_t> payload(0x500, 0);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i);
    }
    mem.write(0x2800, payload.data(), static_cast<codboz::GuestSize>(payload.size()));
    auto back = mem.read(0x2800, static_cast<codboz::GuestSize>(payload.size()));
    CHECK(back.size() == payload.size());
    CHECK(std::memcmp(back.data(), payload.data(), payload.size()) == 0);

    std::vector<uint8_t> copied(payload.size());
    CHECK(mem.copyOut(0x2800, copied.data(), static_cast<codboz::GuestSize>(copied.size())));
    CHECK(copied == payload);
    CHECK(!mem.copyOut(0x4f00, copied.data(), static_cast<codboz::GuestSize>(copied.size())));
    const auto *contiguous = mem.contiguousData(0x2800, payload.size());
    CHECK(contiguous != nullptr);
    CHECK(std::memcmp(contiguous, payload.data(), payload.size()) == 0);
    CHECK(mem.contiguousData(0x4f00, payload.size()) == nullptr);

    // String round-trip.
    mem.writeString(0x2000, "hello");
    CHECK(mem.readString(0x2000, 16) == "hello");

    // Unmapped access is a controlled exception, never a host crash.
    CHECK_THROWS(mem.read32(0x5000));
    CHECK_THROWS(mem.write32(0x9000, 1));
    return 0;
}
