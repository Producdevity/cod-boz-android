#include "loader/file_io.hpp"
#include "s3e/file_system.hpp"

#include "test_macros.hpp"

#include <filesystem>
#include <vector>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "codboz_file_system_test";
    const auto game = root / "game";
    const auto saves = root / "saves";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(game / "assets");
    const std::vector<uint8_t> seed{'s', 'e', 'e', 'd'};
    codboz::writeFile((game / "assets/seed.dat").string().c_str(), seed);

    codboz::S3eFileSystem files(game.string(), saves.string());
    CHECK(!files.open("../outside.dat", "wb"));
    CHECK(!files.open("/outside.dat", "rb"));

    auto file = files.open("seed.dat", "rb");
    CHECK(file != 0);
    uint8_t buffer[8]{};
    CHECK(files.read(buffer, 4, file) == 4);
    CHECK(std::vector<uint8_t>(buffer, buffer + 4) == std::vector<uint8_t>({'s', 'e', 'e', 'd'}));
    CHECK(files.close(file));

    file = files.open("config/settings.dat", "wbU");
    CHECK(file != 0);
    const uint8_t value[] = {'o', 'k'};
    CHECK(files.write(value, sizeof(value), file) == sizeof(value));
    CHECK(files.flush(file));
    CHECK(files.close(file));
    CHECK(std::filesystem::exists(saves / "config/settings.dat"));

    file = files.open("seed.dat", "rb+U");
    CHECK(file != 0);
    CHECK(files.close(file));
    CHECK(std::filesystem::exists(saves / "seed.dat"));

    file = files.openMemory({'m', 'e', 'm'});
    CHECK(files.size(file) == 3);
    CHECK(files.seek(file, 1, SEEK_SET));
    CHECK(files.read(buffer, 2, file) == 2);
    CHECK(buffer[0] == 'e' && buffer[1] == 'm');
    CHECK(files.close(file));

    CHECK(files.makeDirectory("profiles/old"));
    file = files.open("profiles/old/player.i3d", "wb");
    CHECK(file != 0);
    CHECK(files.close(file));
    CHECK(files.rename("profiles/old/player.i3d", "profiles/player.i3d"));
    CHECK(files.remove("profiles/player.i3d"));

    std::filesystem::remove_all(root);
    return 0;
}
