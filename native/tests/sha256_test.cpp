#include "loader/sha256.hpp"

#include "test_macros.hpp"

#include <cstring>
#include <string>

static std::string hex(const uint8_t *p, std::size_t n) {
    static const char *k = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += k[p[i] >> 4];
        s += k[p[i] & 0xf];
    }
    return s;
}

int main() {
    auto check = [](std::string_view input, const char *expect) {
        auto d = codboz::sha256({reinterpret_cast<const uint8_t *>(input.data()), input.size()});
        CHECK(hex(d.data(), d.size()) == expect);
    };

    check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    auto apkHex = codboz::sha256Hex({reinterpret_cast<const uint8_t *>("abc"), 3});
    CHECK(std::strcmp(apkHex.data(),
                      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    return 0;
}
