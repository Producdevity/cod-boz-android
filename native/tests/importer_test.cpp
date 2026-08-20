// Proprietary integration test for the importer. Runs only under
// CODBOZ_TEST_APK; imports the source APK into a temp dir and verifies the
// extracted payload and manifest without executing game code.
#include "loader/file_io.hpp"
#include "loader/importer.hpp"
#include "loader/source_profile.hpp"

#include "test_macros.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
    codboz::ImportOutcome error;
    error.error = "bad \"asset\"\npath";
    const auto json = codboz::toJson(error);
    CHECK(json.find("bad \\\"asset\\\"\\npath") != std::string::npos);

    auto incomplete = std::filesystem::temp_directory_path() / "codboz_incomplete_import_test";
    std::filesystem::remove_all(incomplete);
    std::filesystem::create_directories(incomplete);
    codboz::writeFile((incomplete / "import-manifest.json").c_str(), {});
    CHECK(!codboz::isImported(incomplete.string()));
    std::filesystem::remove_all(incomplete);

    const char *path = std::getenv("CODBOZ_TEST_APK");
    if (path == nullptr || path[0] == '\0') {
        std::cout << "SKIP importer_test: CODBOZ_TEST_APK not set\n";
        return 0;
    }

    auto tmp = std::filesystem::temp_directory_path() / "codboz_import_test";
    std::filesystem::remove_all(tmp);
    auto out = codboz::importApk(path, tmp.string());
    CHECK(out.ok);
    CHECK(out.hashMatched);
    CHECK(out.sourceVersion == "1.0.11");
    CHECK(out.s3eSymbols == 391);
    CHECK(out.s3eBaseAddr == 0x4A000000u);
    CHECK(out.resourceBaseUrl == "http://cdn-boz-android.callofduty.com/PROD/CODBOZ/1_0_9/");
    CHECK(codboz::isImported(tmp.string()));
    const auto importManifest = codboz::readFile((tmp / "import-manifest.json").c_str());
    CHECK(std::string(importManifest.begin(), importManifest.end())
              .find("\"source_version\": \"1.0.11\"") != std::string::npos);
    CHECK(!codboz::isPrepared(tmp.string()));
    CHECK((std::filesystem::status(tmp / "import-manifest.json").permissions() &
           std::filesystem::perms::owner_write) == std::filesystem::perms::none);

    auto unpacked = (tmp / "assets" / "boz.s3e.unpacked").string();
    CHECK(std::filesystem::exists(unpacked));
    CHECK(std::filesystem::file_size(unpacked) == out.s3eUnpackedSize);

    codboz::writeFile((tmp / "assets/blackops_etc.dz").c_str(), {});
    codboz::writeFile((tmp / "assets/blackops_gles1.dz").c_str(), {});
    std::filesystem::resize_file(tmp / "assets/blackops_etc.dz", 487603915u);
    std::filesystem::resize_file(tmp / "assets/blackops_gles1.dz", 337594086u);
    constexpr uint8_t invalidManifest[] = {'{', '}', '\n'};
    codboz::writeFile((tmp / "resource-manifest.json").c_str(), invalidManifest);
    CHECK(!codboz::isPrepared(tmp.string()));

    const auto manifest = codboz::resourceManifestJson();
    codboz::writeFile((tmp / "resource-manifest.json").c_str(),
                      {reinterpret_cast<const uint8_t *>(manifest.data()), manifest.size()});
    CHECK(codboz::isPrepared(tmp.string()));

    const auto previous = std::filesystem::path(tmp.string() + ".previous");
    const auto interrupted = std::filesystem::path(tmp.string() + ".importing");
    std::filesystem::rename(tmp, previous);
    std::filesystem::create_directories(interrupted / "assets");
    std::filesystem::rename(previous / "assets/blackops_etc.dz",
                            interrupted / "assets/blackops_etc.dz");
    std::filesystem::rename(previous / "resource-manifest.json",
                            interrupted / "resource-manifest.json");

    CHECK(codboz::isPrepared(tmp.string()));
    CHECK(!std::filesystem::exists(previous));
    CHECK(!std::filesystem::exists(interrupted));

    auto replacement = codboz::importApk(path, tmp.string());
    CHECK(replacement.ok);
    CHECK(codboz::isPrepared(tmp.string()));
    CHECK((std::filesystem::status(tmp / "assets/blackops_etc.dz").permissions() &
           std::filesystem::perms::owner_write) == std::filesystem::perms::none);
    CHECK((std::filesystem::status(tmp / "resource-manifest.json").permissions() &
           std::filesystem::perms::owner_write) == std::filesystem::perms::none);
    CHECK(!std::filesystem::exists(tmp.string() + ".previous"));
    CHECK(!std::filesystem::exists(tmp.string() + ".importing"));

    std::filesystem::remove_all(tmp);
    std::cout << "ok importer_test: " << out.s3eSymbols << " symbols imported\n";
    return 0;
}
