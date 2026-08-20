// Load the real boz.s3e into the guest, relocate it with import traps, and run
// from the S3E entry. Runs only when CODBOZ_TEST_APK is set.
#include "cpu/dynarmic_cpu.hpp"
#include "guest/guest_memory.hpp"
#include "loader/apk_reader.hpp"
#include "loader/bytes.hpp"
#include "loader/file_io.hpp"
#include "loader/guest_loader.hpp"
#include "loader/lzma.hpp"
#include "loader/s3e_image.hpp"

#include "test_macros.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

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
        std::cout << "SKIP cpu_first_import_test: CODBOZ_TEST_APK not set\n";
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
    auto loaded = codboz::loadAndRelocate(mem, image);
    CHECK(loaded.base == 0x4A000000u);
    CHECK(loaded.entry == loaded.base);
    CHECK(loaded.externalApplied == image.externalRelocs().size());

    // No external-relocation slot may hold a host pointer: every one must be a
    // trap address in the reserved range.
    for (const auto &er : image.externalRelocs()) {
        uint32_t slot = mem.read32(loaded.base + er.offset);
        CHECK(codboz::isInTrapRange(slot));
    }

    // Stack region for guest startup.
    mem.mapRegion(0x70000000u, 0x100000u);

    std::optional<std::string> firstImport;
    auto handler = [&](codboz::GuestAddr trapAddr, codboz::GuestCpu &cpu) {
        if (!firstImport) {
            const auto &syms = image.symbols();
            uint32_t importId = codboz::trapToSymbolIndex(trapAddr);
            firstImport = (importId < syms.size()) ? syms[importId] : std::string("(unknown)");
        }
        cpu.halt();
    };

    codboz::DynarmicCpu cpu(mem, handler);
    cpu.setReg(13, 0x700FFF00u); // sp
    cpu.setReg(14, codboz::kCallbackReturnTrap);
    cpu.setThumb(false);
    cpu.setPc(loaded.entry);

    auto outcome = cpu.run();
    CHECK(firstImport.has_value());
    CHECK(outcome == codboz::RunOutcome::Halted);
    std::cout << "first S3E import reached: " << *firstImport
              << " (outcome=" << static_cast<int>(outcome) << ")\n";
    return 0;
}
