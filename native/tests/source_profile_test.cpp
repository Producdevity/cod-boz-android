#include "loader/source_profile.hpp"

#include "test_macros.hpp"

#include <array>
#include <cstdint>
#include <string>

int main() {
    CHECK(codboz::kSupportedApkSha256.size() == 64);
    CHECK(codboz::kSupportedPackage == "com.activision.boz");
    CHECK(codboz::kSupportedVersionName == "1.0.11");
    CHECK(codboz::kSupportedS3eArch == 268);
    CHECK(codboz::kSupportedGuestPackedSize == 1902539);
    CHECK(codboz::kSupportedGuestPackedSha256 ==
          "f458c15a7111779ad320af377d0bb751294119788ba06d430bee0cc977539fee");
    CHECK(codboz::kSupportedGuestUnpackedSize == 4550559);
    CHECK(codboz::kSupportedGuestUnpackedSha256 ==
          "dbf342663fcd8c7f8fcedced1693eb532cbea8053ef472c0cb837323f3b57d95");
    CHECK(codboz::guestArchitectureName(268) == "ARM32 (A32/Thumb)");
    CHECK(codboz::guestArchitectureName(0) == "unknown");
    CHECK(codboz::kResourceArchives.size() == 2);
    CHECK(codboz::kExtensionProfiles.size() == 4);
    CHECK(codboz::extensionHash("s3eTouchpad") == 0x1dbd7ce8u);
    CHECK(codboz::extensionHash("missing") == 0);

    std::string config = "ResDownloadLink=\"http://invalid.example/\"\nResDownloadLink=\"" +
                         std::string(codboz::kResourceBaseUrl) + "\"\n";
    const auto bytes =
        std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(config.data()), config.size());
    CHECK(codboz::findSupportedResourceBase(bytes) == codboz::kResourceBaseUrl);

    constexpr uint32_t value = 0x1dbd7ce8u;
    const std::array<uint8_t, 9> words = {0xe8, 0x7c, 0xbd, 0x1d, 0x00, 0xe8, 0x7c, 0xbd, 0x1d};
    CHECK(codboz::countLittleEndianWord(words, value) == 2);
    const std::string names = "first.dz second.dz first.dz";
    const auto nameBytes =
        std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(names.data()), names.size());
    CHECK(codboz::countBytes(nameBytes, "first.dz") == 2);
    CHECK(codboz::countBytes(nameBytes, "") == 0);

    const auto manifest = codboz::resourceManifestJson();
    CHECK(manifest.starts_with("{\n  \"source\": \"http:\\/\\/"));
    CHECK(manifest.find("\"blackops_etc.dz\": \"670cefce") != std::string::npos);
    CHECK(manifest.find("\"blackops_gles1.dz\": \"60846cf7") != std::string::npos);
    CHECK(manifest.ends_with("  }\n}\n"));
    return 0;
}
