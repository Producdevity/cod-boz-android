#include "file_io.hpp"

#include <cstdio>

namespace codboz {

std::vector<uint8_t> readFile(const char *path) {
    std::FILE *f = std::fopen(path, "rb");
    if (!f) {
        throw std::runtime_error("cannot open file");
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        throw std::runtime_error("cannot determine file size");
    }
    std::vector<uint8_t> out(static_cast<std::size_t>(size));
    if (size > 0 && std::fread(out.data(), 1, out.size(), f) != out.size()) {
        std::fclose(f);
        throw std::runtime_error("short read");
    }
    std::fclose(f);
    return out;
}

void writeFile(const char *path, std::span<const uint8_t> data) {
    std::FILE *f = std::fopen(path, "wb");
    if (!f) {
        throw std::runtime_error("cannot create file");
    }
    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        throw std::runtime_error("short write");
    }
    std::fclose(f);
}

} // namespace codboz
