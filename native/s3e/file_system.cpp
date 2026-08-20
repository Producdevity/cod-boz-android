#include "file_system.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sys/stat.h>

namespace codboz {

namespace {
bool fileExists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}
std::string baseName(const std::string &name) {
    auto pos = name.find_last_of("/\\");
    return pos == std::string::npos ? name : name.substr(pos + 1);
}

bool equalIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

std::string sanitizeMode(const std::string &mode) {
    std::string result;
    for (char value : mode.empty() ? std::string("rb") : mode) {
        if (value != 'U') {
            result.push_back(value);
        }
    }
    return result.empty() ? "rb" : result;
}

bool isUserFile(const std::string &name, const std::string &mode) {
    const auto extension = std::filesystem::path(name).extension().string();
    return mode.find('U') != std::string::npos || mode.find('+') != std::string::npos ||
           (!mode.empty() && (mode.front() == 'w' || mode.front() == 'a')) ||
           equalIgnoreCase(extension, ".i3d");
}
} // namespace

S3eFileSystem::S3eFileSystem(std::string gameRoot, std::string saveRoot)
    : gameRoot_(std::move(gameRoot)), saveRoot_(std::move(saveRoot)) {}

std::optional<std::string> S3eFileSystem::normalize(const std::string &name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        return std::nullopt;
    }

    std::string path = name;
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string result;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string_view component(path.data() + start,
                                         (end == std::string::npos ? path.size() : end) - start);
        if (component == "..") {
            return std::nullopt;
        }
        if (!component.empty() && component != ".") {
            if (!result.empty()) {
                result.push_back('/');
            }
            result.append(component);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
}

std::string S3eFileSystem::savePath(const std::string &name) const {
    return (std::filesystem::path(saveRoot_) / name).string();
}

std::optional<std::string> S3eFileSystem::resolveRead(const std::string &name) {
    const std::string saved = savePath(name);
    if (fileExists(saved)) {
        return saved;
    }

    if (equalIgnoreCase(baseName(name), "blackops_gles1.dz")) {
        tryLoadDtrz();
        if (dtrz_.loaded() && !equalIgnoreCase(baseName(dtrz_.path()), "blackops_gles1.dz")) {
            return dtrz_.path();
        }
    }

    const std::string candidates[] = {
        gameRoot_ + "/" + name,
        gameRoot_ + "/assets/" + name,
        gameRoot_ + "/assets/data-gles1/" + name,
        gameRoot_ + "/assets/data-sw/" + name,
    };
    for (const auto &candidate : candidates) {
        if (fileExists(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

void S3eFileSystem::tryLoadDtrz() {
    if (dtrzTried_) {
        return;
    }
    dtrzTried_ = true;
    static const char *archives[] = {
        "blackops_etc.dz",
        "blackops_atitc.dz",
        "blackops_dxt.dz",
        "blackops_gles1.dz",
    };
    for (const char *a : archives) {
        std::string path = gameRoot_ + "/assets/" + a;
        if (fileExists(path) && dtrz_.load(path)) {
            return;
        }
    }
    std::string loader = gameRoot_ + "/assets/blackops_loader.dz";
    if (fileExists(loader)) {
        dtrz_.load(loader);
    }
}

bool S3eFileSystem::exists(const std::string &name) {
    const auto normalized = normalize(name);
    if (!normalized) {
        lastError_ = EACCES;
        return false;
    }
    if (resolveRead(*normalized)) {
        lastError_ = 0;
        return true;
    }
    tryLoadDtrz();
    const bool found = dtrz_.loaded() && dtrz_.find(*normalized) != nullptr;
    lastError_ = found ? 0 : ENOENT;
    return found;
}

std::optional<std::string> S3eFileSystem::hostPath(const std::string &name) {
    const auto normalized = normalize(name);
    if (!normalized) {
        lastError_ = EACCES;
        return std::nullopt;
    }
    const auto result = resolveRead(*normalized);
    lastError_ = result ? 0 : ENOENT;
    return result;
}

GuestHandle S3eFileSystem::open(const std::string &name, const std::string &mode) {
    const auto normalized = normalize(name);
    if (!normalized) {
        lastError_ = EACCES;
        return 0;
    }
    const std::string fileMode = sanitizeMode(mode);
    const bool userFile = isUserFile(*normalized, mode);

    if (userFile) {
        const std::string path = savePath(*normalized);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        if (ec) {
            lastError_ = EIO;
            return 0;
        }
        if (!fileExists(path) && !fileMode.empty() && fileMode.front() == 'r') {
            if (const auto seed = resolveRead(*normalized)) {
                std::filesystem::copy_file(*seed, path, std::filesystem::copy_options::none, ec);
                if (ec) {
                    lastError_ = EIO;
                    return 0;
                }
            }
        }
        std::FILE *file = std::fopen(path.c_str(), fileMode.c_str());
        if (!file) {
            recordError(EIO);
            return 0;
        }
        lastError_ = 0;
        return files_.create(HostFile{file, {}, 0, false});
    }

    if (const auto path = resolveRead(*normalized)) {
        std::FILE *f = std::fopen(path->c_str(), fileMode.c_str());
        if (f) {
            lastError_ = 0;
            return files_.create(HostFile{f, {}, 0, false});
        }
    }

    tryLoadDtrz();
    if (dtrz_.loaded()) {
        const auto *entry = dtrz_.find(*normalized);
        if (entry) {
            std::FILE *archive = std::fopen(dtrz_.path().c_str(), "rb");
            if (archive && std::fseek(archive, static_cast<long>(entry->offset), SEEK_SET) == 0) {
                lastError_ = 0;
                return files_.create(
                    HostFile{archive, {}, 0, false, entry->offset, entry->size, true});
            }
            if (archive) {
                std::fclose(archive);
            }
        }
    }

    if (baseName(*normalized) == "console.bin" && fileMode.find('r') != std::string::npos) {
        lastError_ = 0;
        return files_.create(HostFile{nullptr, {}, 0, true});
    }

    lastError_ = ENOENT;
    return 0;
}

GuestHandle S3eFileSystem::openMemory(std::vector<uint8_t> data) {
    lastError_ = 0;
    return files_.create(HostFile{nullptr, std::move(data), 0, true});
}

bool S3eFileSystem::close(GuestHandle handle) {
    auto *f = files_.get(handle);
    if (!f) {
        lastError_ = EBADF;
        return false;
    }
    if (f->fp && std::fclose(f->fp) != 0) {
        recordError(EIO);
        files_.release(handle);
        return false;
    }
    files_.release(handle);
    lastError_ = 0;
    return true;
}

uint32_t S3eFileSystem::read(uint8_t *out, uint32_t n, GuestHandle handle) {
    auto *f = files_.get(handle);
    if (!f) {
        lastError_ = EBADF;
        return 0;
    }
    if (n == 0) {
        return 0;
    }
    if (f->isMem) {
        uint32_t avail = f->mem.size() - f->memPos;
        uint32_t toRead = std::min(n, avail);
        std::memcpy(out, f->mem.data() + f->memPos, toRead);
        f->memPos += toRead;
        lastError_ = 0;
        return toRead;
    }
    if (f->fp) {
        if (f->isArchiveEntry) {
            const uint32_t available = f->archiveSize - f->memPos;
            n = std::min(n, available);
        }
        const uint32_t read = static_cast<uint32_t>(std::fread(out, 1, n, f->fp));
        if (f->isArchiveEntry) {
            f->memPos += read;
        }
        if (read < n && std::ferror(f->fp)) {
            recordError(EIO);
        } else {
            lastError_ = 0;
        }
        return read;
    }
    lastError_ = EBADF;
    return 0;
}

uint32_t S3eFileSystem::write(const uint8_t *data, uint32_t n, GuestHandle handle) {
    auto *f = files_.get(handle);
    if (!f || !f->fp || f->isArchiveEntry) {
        lastError_ = EBADF;
        return 0;
    }
    const uint32_t written = static_cast<uint32_t>(std::fwrite(data, 1, n, f->fp));
    if (written != n) {
        recordError(EIO);
    } else {
        lastError_ = 0;
    }
    return written;
}

bool S3eFileSystem::seek(GuestHandle handle, int32_t offset, int32_t origin) {
    auto *f = files_.get(handle);
    if (!f) {
        lastError_ = EBADF;
        return false;
    }
    if (f->isMem) {
        int32_t base;
        switch (origin) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = static_cast<int32_t>(f->memPos);
            break;
        case SEEK_END:
            base = static_cast<int32_t>(f->mem.size());
            break;
        default:
            return false;
        }
        int32_t newPos = base + offset;
        if (newPos < 0 || newPos > static_cast<int32_t>(f->mem.size())) {
            return false;
        }
        f->memPos = static_cast<uint32_t>(newPos);
        lastError_ = 0;
        return true;
    }
    if (f->fp) {
        if (f->isArchiveEntry) {
            int64_t base = 0;
            switch (origin) {
            case SEEK_SET:
                break;
            case SEEK_CUR:
                base = f->memPos;
                break;
            case SEEK_END:
                base = f->archiveSize;
                break;
            default:
                return false;
            }
            const int64_t next = base + offset;
            if (next < 0 || next > f->archiveSize) {
                return false;
            }
            if (std::fseek(f->fp, static_cast<long>(f->archiveOffset + next), SEEK_SET) != 0) {
                return false;
            }
            f->memPos = static_cast<uint32_t>(next);
            lastError_ = 0;
            return true;
        }
        const bool ok = std::fseek(f->fp, offset, origin) == 0;
        if (ok) {
            lastError_ = 0;
        } else {
            recordError(EINVAL);
        }
        return ok;
    }
    lastError_ = EBADF;
    return false;
}

int32_t S3eFileSystem::tell(GuestHandle handle) {
    auto *f = files_.get(handle);
    if (!f) {
        lastError_ = EBADF;
        return -1;
    }
    if (f->isMem) {
        return static_cast<int32_t>(f->memPos);
    }
    if (f->fp) {
        if (f->isArchiveEntry) {
            return static_cast<int32_t>(f->memPos);
        }
        const long result = std::ftell(f->fp);
        if (result < 0 || result > std::numeric_limits<int32_t>::max()) {
            recordError(EOVERFLOW);
            return -1;
        }
        lastError_ = 0;
        return static_cast<int32_t>(result);
    }
    lastError_ = EBADF;
    return -1;
}

int32_t S3eFileSystem::size(GuestHandle handle) {
    auto *f = files_.get(handle);
    if (!f) {
        lastError_ = EBADF;
        return -1;
    }
    if (f->isMem) {
        return static_cast<int32_t>(f->mem.size());
    }
    if (f->fp) {
        if (f->isArchiveEntry) {
            return static_cast<int32_t>(f->archiveSize);
        }
        long cur = std::ftell(f->fp);
        if (cur < 0 || std::fseek(f->fp, 0, SEEK_END) != 0) {
            recordError(EIO);
            return -1;
        }
        long end = std::ftell(f->fp);
        if (end < 0 || end > std::numeric_limits<int32_t>::max() ||
            std::fseek(f->fp, cur, SEEK_SET) != 0) {
            recordError(EOVERFLOW);
            return -1;
        }
        lastError_ = 0;
        return static_cast<int32_t>(end);
    }
    lastError_ = EBADF;
    return -1;
}

bool S3eFileSystem::flush(GuestHandle handle) {
    auto *file = files_.get(handle);
    if (!file || !file->fp || file->isArchiveEntry) {
        lastError_ = EBADF;
        return false;
    }
    if (std::fflush(file->fp) != 0) {
        recordError(EIO);
        return false;
    }
    lastError_ = 0;
    return true;
}

bool S3eFileSystem::makeDirectory(const std::string &name) {
    const auto normalized = normalize(name);
    if (!normalized) {
        lastError_ = EACCES;
        return false;
    }
    std::error_code error;
    const bool created = std::filesystem::create_directories(savePath(*normalized), error);
    if (error) {
        lastError_ = EIO;
        return false;
    }
    lastError_ = 0;
    return created || std::filesystem::is_directory(savePath(*normalized));
}

bool S3eFileSystem::remove(const std::string &name) {
    const auto normalized = normalize(name);
    if (!normalized) {
        lastError_ = EACCES;
        return false;
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(savePath(*normalized), error);
    if (error || !removed) {
        lastError_ = error ? EIO : ENOENT;
        return false;
    }
    lastError_ = 0;
    return true;
}

bool S3eFileSystem::rename(const std::string &oldName, const std::string &newName) {
    const auto oldPath = normalize(oldName);
    const auto newPath = normalize(newName);
    if (!oldPath || !newPath) {
        lastError_ = EACCES;
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(savePath(*newPath)).parent_path(),
                                        error);
    if (!error) {
        std::filesystem::rename(savePath(*oldPath), savePath(*newPath), error);
    }
    if (error) {
        lastError_ = EIO;
        return false;
    }
    lastError_ = 0;
    return true;
}

int S3eFileSystem::error() const {
    return lastError_;
}

std::string S3eFileSystem::errorString() const {
    return std::strerror(lastError_);
}

std::string S3eFileSystem::resourceStyle() {
    tryLoadDtrz();
    if (!dtrz_.loaded()) {
        return "gles1";
    }
    const std::string selected = baseName(dtrz_.path());
    if (equalIgnoreCase(selected, "blackops_etc.dz")) {
        return "etc";
    }
    if (equalIgnoreCase(selected, "blackops_atitc.dz")) {
        return "atitc";
    }
    if (equalIgnoreCase(selected, "blackops_dxt.dz")) {
        return "dxt";
    }
    return "gles1";
}

void S3eFileSystem::recordError(int fallback) {
    lastError_ = errno == 0 ? fallback : errno;
}

} // namespace codboz
