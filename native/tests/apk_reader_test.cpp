#include "loader/apk_reader.hpp"

#include "test_macros.hpp"

#include <cstdint>
#include <vector>

namespace {
void le16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
}
void le32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; ++i) {
        v.push_back((x >> (8 * i)) & 0xff);
    }
}
} // namespace

// One STORED entry "assets/a.txt" containing "hi".
static std::vector<uint8_t> buildZip() {
    const std::string name = "assets/a.txt";
    const std::string data = "hi";
    std::vector<uint8_t> z;

    std::size_t localStart = z.size();
    le32(z, 0x04034b50); // local sig
    le16(z, 20);         // version
    le16(z, 0);          // flags
    le16(z, 0);          // method (stored)
    le16(z, 0);          // time
    le16(z, 0);          // date
    le32(z, 0xdeadbeef); // crc
    le32(z, 2);          // compressed size
    le32(z, 2);          // uncompressed size
    le16(z, static_cast<uint16_t>(name.size()));
    le16(z, 0); // extra
    z.insert(z.end(), name.begin(), name.end());
    z.insert(z.end(), data.begin(), data.end());

    std::size_t centralOffset = z.size();
    le32(z, 0x02014b50); // central sig
    le16(z, 20);         // version made by
    le16(z, 20);         // version needed
    le16(z, 0);          // flags
    le16(z, 0);          // method
    le16(z, 0);          // time
    le16(z, 0);          // date
    le32(z, 0xdeadbeef); // crc
    le32(z, 2);          // compressed size
    le32(z, 2);          // uncompressed size
    le16(z, static_cast<uint16_t>(name.size()));
    le16(z, 0); // extra
    le16(z, 0); // comment
    le16(z, 0); // disk start
    le16(z, 0); // internal attrs
    le32(z, 0); // external attrs
    le32(z, static_cast<uint32_t>(localStart));
    z.insert(z.end(), name.begin(), name.end());
    std::size_t centralEnd = z.size();
    uint32_t centralSize = static_cast<uint32_t>(centralEnd - centralOffset);

    le32(z, 0x06054b50); // EOCD sig
    le16(z, 0);          // disk
    le16(z, 0);          // disk with central dir
    le16(z, 1);          // entries this disk
    le16(z, 1);          // entries total
    le32(z, centralSize);
    le32(z, static_cast<uint32_t>(centralOffset));
    le16(z, 0); // comment length
    return z;
}

int main() {
    auto zip = buildZip();
    codboz::ApkReader apk(zip);
    CHECK(apk.entries().size() == 1);
    const auto &e = apk.entries()[0];
    CHECK(e.name == "assets/a.txt");
    CHECK(e.method == 0);
    CHECK(e.compressedSize == 2);
    CHECK(e.uncompressedSize == 2);
    CHECK(e.localHeaderOffset == 0);
    CHECK(apk.find("assets/a.txt") != nullptr);
    CHECK(apk.find("missing") == nullptr);
    CHECK(apk.entryData(e).size() == 2);

    auto mismatched = zip;
    mismatched[30] = 'x';
    codboz::ApkReader mismatchedApk(mismatched);
    CHECK_THROWS((mismatchedApk.entryData(mismatchedApk.entries()[0])));

    CHECK(codboz::isSafeAssetName("assets/a.txt"));
    CHECK(!codboz::isSafeAssetName("assets/../x"));
    CHECK(!codboz::isSafeAssetName("assets//x"));
    CHECK(!codboz::isSafeAssetName("lib/x.so"));
    CHECK(!codboz::isSafeAssetName("assets/x/../y"));

    // Too small / no EOCD must be rejected.
    uint8_t tiny[] = {0x50, 0x4b, 0x05, 0x06};
    CHECK_THROWS((codboz::ApkReader(tiny)));
    return 0;
}
