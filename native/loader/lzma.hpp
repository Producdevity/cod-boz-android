#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace codboz {

// Decode an LZMA-alone stream (5-byte properties + 8-byte little-endian size,
// then the compressed payload). Throws on malformed input or decode failure.
std::vector<uint8_t> lzmaDecompress(std::span<const uint8_t> src);

} // namespace codboz
