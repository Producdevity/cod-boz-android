#include "gl/gl_compat.hpp"

#include "test_macros.hpp"

int main() {
    const auto textureSize = codboz::glcompat::integerQueryUpperBound(0x0D33);
    CHECK(textureSize.has_value());
    CHECK(textureSize->count == 1);
    CHECK(textureSize->values[0] == 8192);

    const auto textureUnits = codboz::glcompat::integerQueryUpperBound(0x8B4D);
    CHECK(textureUnits.has_value());
    CHECK(textureUnits->values[0] == 64);

    const auto compressedFormats = codboz::glcompat::fixedIntegerQuery(0x86A3);
    CHECK(compressedFormats.has_value());
    CHECK(compressedFormats->count == 1);
    CHECK(compressedFormats->values[0] == 0x8D64);

    CHECK(!codboz::glcompat::integerQueryUpperBound(0x0BA2).has_value());
    CHECK(!codboz::glcompat::fixedIntegerQuery(0x0BA2).has_value());
    CHECK(codboz::glcompat::queryComponentCount(0x0BA2) == 4);
    CHECK(codboz::glcompat::queryComponentCount(0x846D) == 2);
    CHECK(codboz::glcompat::queryComponentCount(0x86A3, 7) == 7);
    CHECK(codboz::glcompat::queryComponentCount(0x8DF8, 0, 3) == 3);
    CHECK(codboz::glcompat::queryComponentCount(0x8CA6) == 1);
    return 0;
}
