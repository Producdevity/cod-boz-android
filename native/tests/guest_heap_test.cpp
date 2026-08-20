#include "guest/guest_heap.hpp"

#include "test_macros.hpp"

int main() {
    codboz::GuestMemory mem;
    constexpr codboz::GuestAddr kBase = 0x100000;
    constexpr codboz::GuestSize kSize = 0x10000; // 64 KiB
    codboz::GuestHeap heap(mem, kBase, kSize);

    auto a = heap.alloc(0x100);
    auto b = heap.alloc(0x200, 16);
    auto c = heap.alloc(0x50);
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(c != 0);
    CHECK((a & 0x7) == 0); // 8-byte aligned
    CHECK((b & 0xF) == 0); // 16-byte aligned
    CHECK(a != b && b != c);

    // Writes through the heap land in guest memory.
    mem.write32(a, 0x11223344);
    CHECK(mem.read32(a) == 0x11223344);

    CHECK(heap.allocatedBytes() >= 0x100 + 0x200 + 0x50);

    // Free then realloc reuses space.
    CHECK(heap.free(b));
    CHECK(!heap.free(b));             // double free rejected
    CHECK(!heap.free(kBase + kSize)); // out of range

    auto d = heap.alloc(0x80);
    CHECK(d != 0);

    // realloc grows into a new block and copies.
    mem.write8(c, 0x77);
    auto c2 = heap.realloc(c, 0x400);
    CHECK(c2 != 0);
    CHECK(mem.read8(c2) == 0x77);

    // Exhaust the heap.
    while (heap.alloc(0x100) != 0) {
    }
    CHECK(heap.alloc(0x100) == 0); // OOM
    return 0;
}
