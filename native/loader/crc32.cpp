#include "crc32.hpp"

namespace codboz {

uint32_t crc32(std::span<const uint8_t> data) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xffffffffu;
    for (uint8_t b : data) {
        crc = table[(crc ^ b) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

} // namespace codboz
