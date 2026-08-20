#pragma once

#include <cstdint>
#include <string>

namespace codboz {

struct ImportOutcome {
    bool ok = false;
    std::string sha256Hex;
    bool hashMatched = false;
    std::string sourceVersion;
    uint32_t entryCount = 0;
    uint32_t assetCount = 0;
    uint32_t s3eSymbols = 0;
    uint32_t s3eBaseAddr = 0;
    uint32_t s3eCodeMemSize = 0;
    uint32_t s3eUnpackedSize = 0;
    std::string resourceBaseUrl;
    std::string error;
};

// Import a source APK into gameDir: verify the hash, extract bundled STORED
// assets, decompress boz.s3e, validate the S3E header, and write an import
// manifest. No game code is executed.
ImportOutcome importApk(const std::string &apkPath, const std::string &gameDir);

// Validates the supported guest payload and recovers an interrupted replacement.
bool isImported(const std::string &gameDir);
bool isPrepared(const std::string &gameDir);

std::string manifestPath(const std::string &gameDir);
std::string toJson(const ImportOutcome &o);

} // namespace codboz
