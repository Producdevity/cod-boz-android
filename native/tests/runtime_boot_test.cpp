// Run the real boz.s3e through the Runtime dispatcher and report how far guest
// execution advances. Runs only when CODBOZ_TEST_APK is set.
#include "bridge/dispatcher.hpp"
#include "loader/apk_reader.hpp"
#include "loader/bytes.hpp"
#include "loader/file_io.hpp"
#include "loader/lzma.hpp"
#include "loader/s3e_image.hpp"

#include "test_macros.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
constexpr uint32_t kZipLocalSig = 0x04034b50u;

std::size_t localDataOffset(std::span<const uint8_t> apk, const codboz::ZipEntry &e) {
    auto p = apk.data() + e.localHeaderOffset;
    if (codboz::readLe32(p) != kZipLocalSig) {
        throw std::runtime_error("bad local header");
    }
    uint16_t nameLen = codboz::readLe16(p + 26);
    uint16_t extraLen = codboz::readLe16(p + 28);
    return e.localHeaderOffset + 30u + nameLen + extraLen;
}
} // namespace

int main() {
    const char *path = std::getenv("CODBOZ_TEST_APK");
    if (path == nullptr || path[0] == '\0') {
        std::cout << "SKIP runtime_boot_test: CODBOZ_TEST_APK not set\n";
        return 0;
    }

    auto raw = codboz::readFile(path);
    codboz::ApkReader apk(raw);
    const auto *s3e = apk.find("assets/boz.s3e");
    CHECK(s3e != nullptr);
    auto off = localDataOffset(raw, *s3e);
    auto inflated = codboz::lzmaDecompress({raw.data() + off, s3e->compressedSize});
    codboz::S3eImage image(inflated);

    codboz::GuestMemory mem;
    codboz::Runtime::Config cfg;
    cfg.importCap = 200000; // bounded: reach past the file phase
    cfg.debugLog = std::getenv("CODBOZ_TEST_DEBUG_LOG") != nullptr;
    const char *root = std::getenv("CODBOZ_TEST_ROOT");
    if (root) {
        cfg.root = root;
        cfg.saveRoot = (std::filesystem::path(root) / "saves").string();
    }
    codboz::Runtime runtime(mem, image.symbols(), cfg);
    const auto started = std::chrono::steady_clock::now();
    auto result = runtime.boot(image);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "boot outcome: " << static_cast<int>(result.outcome) << "\n";
    std::cout << "imports handled: " << result.importsHandled << "\n";
    std::cout << "elapsed: " << elapsedMs << " ms\n";
    std::cout << "last import: " << result.lastImport << "\n";
    if (!result.faultDetail.empty()) {
        std::cout << result.faultDetail << "\n";
    }
    std::cout << "first " << result.firstImports.size() << " imports:\n";
    for (std::size_t i = 0; i < result.firstImports.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << result.firstImports[i] << "\n";
    }

    CHECK(result.importsHandled > 1);
    if (root) {
        CHECK(result.outcome != codboz::RunOutcome::Fault);
        CHECK(result.importsHandled == cfg.importCap);
    }
    return 0;
}
