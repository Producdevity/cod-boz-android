#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace codboz::glcompat {

struct IntegerQuery {
    std::array<int32_t, 4> values{};
    std::size_t count = 0;
};

std::optional<IntegerQuery> integerQueryUpperBound(uint32_t name);
std::optional<IntegerQuery> fixedIntegerQuery(uint32_t name);
std::size_t queryComponentCount(uint32_t name, std::size_t compressedFormatCount = 0,
                                std::size_t shaderBinaryFormatCount = 0);

} // namespace codboz::glcompat
