#include "gl_compat.hpp"

namespace codboz::glcompat {

namespace {

constexpr IntegerQuery scalar(int32_t value) {
    return {{{value, 0, 0, 0}}, 1};
}

constexpr IntegerQuery pair(int32_t first, int32_t second) {
    return {{{first, second, 0, 0}}, 2};
}

} // namespace

std::optional<IntegerQuery> integerQueryUpperBound(uint32_t name) {
    switch (name) {
    case 0x0D33: // GL_MAX_TEXTURE_SIZE
    case 0x84BF: // GL_MAX_CUBE_MAP_TEXTURE_SIZE
    case 0x84E8: // GL_MAX_RENDERBUFFER_SIZE
        return scalar(8192);
    case 0x0D3A: // GL_MAX_VIEWPORT_DIMS
        return pair(8192, 8192);
    case 0x8869: // GL_MAX_VERTEX_ATTRIBS
        return scalar(16);
    case 0x8B4D: // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
        return scalar(64);
    default:
        return std::nullopt;
    }
}

std::optional<IntegerQuery> fixedIntegerQuery(uint32_t name) {
    switch (name) {
    case 0x8DF9: // GL_NUM_SHADER_BINARY_FORMATS
        return scalar(0);
    case 0x8DF8: // GL_SHADER_BINARY_FORMATS
        return IntegerQuery{};
    case 0x86A2: // GL_NUM_COMPRESSED_TEXTURE_FORMATS
        return scalar(1);
    case 0x86A3:               // GL_COMPRESSED_TEXTURE_FORMATS
        return scalar(0x8D64); // GL_ETC1_RGB8_OES
    default:
        return std::nullopt;
    }
}

std::size_t queryComponentCount(uint32_t name, std::size_t compressedFormatCount,
                                std::size_t shaderBinaryFormatCount) {
    switch (name) {
    case 0x0BA2: // GL_VIEWPORT
    case 0x0C10: // GL_SCISSOR_BOX
    case 0x0C22: // GL_COLOR_CLEAR_VALUE
    case 0x0C23: // GL_COLOR_WRITEMASK
    case 0x8005: // GL_BLEND_COLOR
        return 4;
    case 0x0B70: // GL_DEPTH_RANGE
    case 0x0D3A: // GL_MAX_VIEWPORT_DIMS
    case 0x846D: // GL_ALIASED_POINT_SIZE_RANGE
    case 0x846E: // GL_ALIASED_LINE_WIDTH_RANGE
        return 2;
    case 0x86A3: // GL_COMPRESSED_TEXTURE_FORMATS
        return compressedFormatCount;
    case 0x8DF8: // GL_SHADER_BINARY_FORMATS
        return shaderBinaryFormatCount;
    default:
        return 1;
    }
}

} // namespace codboz::glcompat
