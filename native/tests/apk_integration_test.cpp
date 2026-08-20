// Proprietary integration test: runs only when CODBOZ_TEST_APK points at the
// source APK. Verifies hash, locates payloads, and parses the real S3E header
// without executing game code. Skipped otherwise.
#include "loader/apk_reader.hpp"
#include "loader/bytes.hpp"
#include "loader/file_io.hpp"
#include "loader/lzma.hpp"
#include "loader/s3e_image.hpp"
#include "loader/sha256.hpp"
#include "loader/source_profile.hpp"

#include "test_macros.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
constexpr uint32_t kZipLocalSig = 0x04034b50u;

std::size_t dataOffset(std::span<const uint8_t> data, const codboz::ZipEntry &e) {
    codboz::Reader r(data);
    r.seek(e.localHeaderOffset);
    CHECK(r.readLe32() == kZipLocalSig);
    r.readLe16();
    r.readLe16();
    r.readLe16();
    r.readLe16();
    r.readLe16();
    r.readLe32();
    r.readLe32();
    r.readLe32();
    uint16_t nameLen = r.readLe16();
    uint16_t extraLen = r.readLe16();
    return e.localHeaderOffset + 30u + nameLen + extraLen;
}
} // namespace

int main() {
    const char *path = std::getenv("CODBOZ_TEST_APK");
    if (path == nullptr || path[0] == '\0') {
        std::cout << "SKIP apk_integration_test: CODBOZ_TEST_APK not set\n";
        return 0;
    }

    auto raw = codboz::readFile(path);
    auto hash = codboz::sha256Hex(raw);
    CHECK(std::string_view(hash.data()) == codboz::kSupportedApkSha256);

    codboz::ApkReader apk(raw);
    const auto *s3e = apk.find("assets/boz.s3e");
    const auto *loader = apk.find("assets/blackops_loader.dz");
    CHECK(s3e != nullptr);
    CHECK(loader != nullptr);

    auto off = dataOffset(raw, *s3e);
    auto inflated = codboz::lzmaDecompress({raw.data() + off, s3e->compressedSize});
    CHECK(codboz::readLe32(inflated.data()) == codboz::kS3eMagic);

    codboz::S3eImage image(inflated);
    auto fix = image.parseFixups();
    CHECK(fix.symbols.size() > 100);
    CHECK(fix.externalRelocCount > 0);

    std::cout << "ok apk_integration_test: " << fix.symbols.size() << " symbols, "
              << fix.externalRelocCount << " external relocs, arch=" << image.header().arch
              << ", base=0x" << std::hex << image.header().baseAddrOrig << std::dec << "\n";
    return 0;
}
