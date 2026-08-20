#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace codboz {

std::vector<uint8_t> readFile(const char *path);
void writeFile(const char *path, std::span<const uint8_t> data);

} // namespace codboz
