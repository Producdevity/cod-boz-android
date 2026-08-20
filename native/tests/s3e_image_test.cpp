#include "loader/s3e_image.hpp"

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

// Build a minimal S3E image with a symbol table, an internal reloc block and
// an external reloc block, then a tiny code region.
static std::vector<uint8_t> buildImage() {
    std::vector<uint8_t> fix;
    // type 0: symbols
    le32(fix, 0);
    le32(fix, 8 + 2 + 7 + 6);
    le16(fix, 2);
    for (uint8_t c : std::string("s3eFoo"))
        fix.push_back(c);
    fix.push_back(0);
    for (uint8_t c : std::string("glBar"))
        fix.push_back(c);
    fix.push_back(0);
    // type 1: internal relocs
    le32(fix, 1);
    le32(fix, 8 + 4 + 12);
    le32(fix, 3);
    le32(fix, 0x10);
    le32(fix, 0x20);
    le32(fix, 0x30);
    // type 2: external relocs
    le32(fix, 2);
    le32(fix, 8 + 4 + 12);
    le32(fix, 2);
    le16(fix, 0);
    le16(fix, 0x10);
    le16(fix, 0);
    le16(fix, 0);
    le16(fix, 0x40);
    le16(fix, 1);

    const uint32_t fixupOffset = 68;
    const uint32_t fixupSize = static_cast<uint32_t>(fix.size());
    const uint32_t codeOffset = fixupOffset + fixupSize;
    const uint32_t codeFileSize = 16;

    std::vector<uint8_t> img;
    le32(img, codboz::kS3eMagic);
    le32(img, 0x100); // version
    le16(img, 0);     // flags
    le16(img, 0x40);  // arch
    le32(img, fixupOffset);
    le32(img, fixupSize);
    le32(img, codeOffset);
    le32(img, codeFileSize);
    le32(img, 0x10000); // code_mem_size
    le32(img, 0);       // sig_offset
    le32(img, 0);       // sig_size
    le32(img, 0);       // entry_offset
    le32(img, 0);       // config_offset
    le32(img, 0);       // config_size
    le32(img, 0x10000); // base_addr_orig
    le32(img, 0);       // extra_offset
    le32(img, 0);       // extra_size
    le32(img, 0);       // ext_header_size
    img.insert(img.end(), fix.begin(), fix.end());
    img.insert(img.end(), codeFileSize, 0);
    return img;
}

int main() {
    auto img = buildImage();
    codboz::S3eImage image(img);
    CHECK(image.header().baseAddrOrig == 0x10000);
    CHECK(image.header().arch == 0x40);
    CHECK(image.header().codeMemSize == 0x10000);
    CHECK(image.code().size() == 16);

    auto fix = image.parseFixups();
    CHECK(fix.symbols.size() == 2);
    CHECK(fix.symbols[0] == "s3eFoo");
    CHECK(fix.symbols[1] == "glBar");
    CHECK(fix.internalRelocCount == 3);
    CHECK(fix.externalRelocCount == 2);
    CHECK(fix.externalImportSymbols == 2);
    CHECK(fix.externalRelocTypes.size() == 1);
    CHECK(fix.externalRelocTypes.at(2) == 2);
    CHECK(image.externalRelocs()[0].type == 2);

    // Bad magic must be rejected.
    std::vector<uint8_t> bad = img;
    bad[0] = 0;
    CHECK_THROWS((codboz::S3eImage(bad)));

    // Truncated header must be rejected.
    uint8_t tiny[] = {0x58, 0x45, 0x33, 0x55, 0x00};
    CHECK_THROWS((codboz::S3eImage(tiny)));

    // Extended headers may not place initialized data beyond the file image.
    std::vector<uint8_t> invalidDataOffset = img;
    invalidDataOffset[64] = 0x0c;
    invalidDataOffset.resize(invalidDataOffset.size() + 8);
    invalidDataOffset[68] = 0xff;
    invalidDataOffset[69] = 0xff;
    CHECK_THROWS((codboz::S3eImage(invalidDataOffset)));
    return 0;
}
