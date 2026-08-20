#include "loader/dtrz_archive.hpp"
#include "loader/file_io.hpp"

#include "test_macros.hpp"

#include <filesystem>
#include <vector>

namespace {

void append16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8u));
}

void append32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8u));
    out.push_back(static_cast<uint8_t>(value >> 16u));
    out.push_back(static_cast<uint8_t>(value >> 24u));
}

std::vector<uint8_t> fixture(uint32_t declaredSize = 4) {
    std::vector<uint8_t> out{'D', 'T', 'R', 'Z'};
    append16(out, 1);
    append16(out, 1);
    out.push_back(0);
    const char name[] = "bootstrap.group.bin";
    out.insert(out.end(), name, name + sizeof(name));
    append32(out, 1);
    out.insert(out.end(), 6, 0);
    const uint32_t payloadOffset = static_cast<uint32_t>(out.size() + 16);
    append32(out, payloadOffset);
    append32(out, declaredSize);
    out.insert(out.end(), 8, 0);
    out.insert(out.end(), {'b', 'o', 'o', 't'});
    return out;
}

} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() / "codboz_dtrz_test.dz";
    auto data = fixture();
    codboz::writeFile(path.string().c_str(), data);

    codboz::DtrzArchive archive;
    CHECK(archive.load(path.string()));
    CHECK(archive.entryCount() == 1);
    const auto *entry = archive.find("bootstrap/bootstrap.group.bin");
    CHECK(entry != nullptr);
    CHECK(archive.find("BOOTSTRAP.GROUP.BIN") == entry);
    CHECK(archive.read(*entry) == std::vector<uint8_t>({'b', 'o', 'o', 't'}));

    data = fixture(1000);
    codboz::writeFile(path.string().c_str(), data);
    CHECK(!archive.load(path.string()));

    std::filesystem::remove(path);
    return 0;
}
