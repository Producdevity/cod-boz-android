#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace codboz {

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
std::array<char, 65> sha256Hex(std::span<const uint8_t> data); // lower-case, NUL-terminated

} // namespace codboz
