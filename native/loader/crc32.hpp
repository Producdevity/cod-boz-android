#pragma once

#include <cstdint>
#include <span>

namespace codboz {

uint32_t crc32(std::span<const uint8_t> data);

} // namespace codboz
