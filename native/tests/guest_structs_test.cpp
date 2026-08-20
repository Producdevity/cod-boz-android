#include "guest/guest_memory.hpp"
#include "guest/guest_structs.hpp"

#include "test_macros.hpp"

#include <cstring>

// The static_asserts in guest_structs.hpp already lock the layouts. This test
// exercises a round-trip through GuestMemory to confirm field offsets line up
// with the asserted guest layout.
int main() {
    using namespace codboz;
    CHECK(sizeof(guest::InetAddress) == 0x124);
    CHECK(sizeof(guest::ZeroConfFound) == 0x38);
    CHECK(sizeof(guest::ZeroConfTxtUpdate) == 0x0c);
    CHECK(sizeof(guest::SoundEndSampleInfo) == 16);
    CHECK(sizeof(guest::SoundGenAudioInfo) == 32);
    CHECK(sizeof(guest::AudioUnitBuffer) == 12);

    GuestMemory mem;
    mem.mapRegion(0x1000, 0x1000);

    guest::InetAddress addr{};
    addr.type = 1;
    addr.ip_address = 0xC0A80101u;
    addr.port = 0x5000u;
    addr.next = 0xDEAD0000u;
    mem.write(0x1000, reinterpret_cast<const uint8_t *>(&addr), sizeof(addr));

    CHECK(mem.read32(0x1000) == 1);                   // type
    CHECK(mem.read32(0x1000 + 0x88) == 0xC0A80101u);  // ip_address
    CHECK(mem.read16(0x1000 + 0x9c) == 0x5000u);      // port
    CHECK(mem.read32(0x1000 + 0x120) == 0xDEAD0000u); // next

    guest::AudioUnitBuffer audio{};
    audio.sample_count = 512;
    audio.samples = 0x6200000cu;
    audio.sample_rate = 8000;
    mem.write(0x1200, reinterpret_cast<const uint8_t *>(&audio), sizeof(audio));
    CHECK(mem.read32(0x1200) == 512);
    CHECK(mem.read32(0x1204) == 0x6200000cu);
    CHECK(mem.read32(0x1208) == 8000);
    return 0;
}
