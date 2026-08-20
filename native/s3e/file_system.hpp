#pragma once

#include "guest/guest_address.hpp"
#include "guest/guest_handles.hpp"
#include "loader/dtrz_archive.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace codboz {

class S3eFileSystem {
  public:
    S3eFileSystem(std::string gameRoot, std::string saveRoot);

    GuestHandle open(const std::string &name, const std::string &mode);
    GuestHandle openMemory(std::vector<uint8_t> data);
    bool close(GuestHandle handle);
    uint32_t read(uint8_t *out, uint32_t size, GuestHandle handle);
    uint32_t write(const uint8_t *data, uint32_t size, GuestHandle handle);
    bool seek(GuestHandle handle, int32_t offset, int32_t origin);
    int32_t tell(GuestHandle handle);
    int32_t size(GuestHandle handle);
    bool exists(const std::string &name);
    bool flush(GuestHandle handle);
    bool makeDirectory(const std::string &name);
    bool remove(const std::string &name);
    bool rename(const std::string &oldName, const std::string &newName);
    int error() const;
    std::string errorString() const;
    std::string resourceStyle();
    std::optional<std::string> hostPath(const std::string &name);

  private:
    struct HostFile {
        std::FILE *fp = nullptr;
        std::vector<uint8_t> mem;
        uint32_t memPos = 0;
        bool isMem = false;
        uint32_t archiveOffset = 0;
        uint32_t archiveSize = 0;
        bool isArchiveEntry = false;
    };

    std::string gameRoot_;
    std::string saveRoot_;
    HandleTable<HostFile> files_;
    DtrzArchive dtrz_;
    bool dtrzTried_ = false;
    int lastError_ = 0;

    static std::optional<std::string> normalize(const std::string &name);
    std::optional<std::string> resolveRead(const std::string &name);
    std::string savePath(const std::string &name) const;
    void recordError(int fallback);
    void tryLoadDtrz();
};

} // namespace codboz
