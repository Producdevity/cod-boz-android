#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace codboz {

constexpr uint32_t kS3eMagic = 0x55334558u;

struct S3eHeader {
    uint32_t ident = 0;
    uint32_t version = 0;
    uint16_t flags = 0;
    uint16_t arch = 0;
    uint32_t fixupOffset = 0;
    uint32_t fixupSize = 0;
    uint32_t codeOffset = 0;
    uint32_t codeFileSize = 0;
    uint32_t codeMemSize = 0;
    uint32_t sigOffset = 0;
    uint32_t sigSize = 0;
    uint32_t entryOffset = 0;
    uint32_t configOffset = 0;
    uint32_t configSize = 0;
    uint32_t baseAddrOrig = 0;
    uint32_t extraOffset = 0;
    uint32_t extraSize = 0;
    uint32_t extHeaderSize = 0;
    uint32_t dataOffset = 0;
    uint32_t isJuice = 0;
};

// Fixup block kinds found in the fixup region.
enum class FixupType : uint32_t {
    Symbols = 0,
    InternalRelocs = 1,
    ExternalRelocs = 2,
};

// One external relocation: patch the 32-bit slot at (image_base + offset) with
// the resolved address of symbols[symbolIndex].
struct ExternalReloc {
    uint32_t offset;
    uint16_t symbolIndex;
    uint32_t type;
};

struct FixupSummary {
    std::vector<std::string> symbols;
    uint32_t internalRelocCount = 0;
    uint32_t externalRelocCount = 0;
    uint32_t externalImportSymbols = 0; // distinct symbol indices referenced
    std::map<uint32_t, uint32_t> externalRelocTypes;
};

// Read-only S3E image: parses and validates the header, symbol table, and
// relocation records. It never executes or copies code; relocation application
// is handled by the guest loader once guest memory exists.
class S3eImage {
  public:
    explicit S3eImage(std::span<const uint8_t> data);

    const S3eHeader &header() const {
        return header_;
    }
    FixupSummary parseFixups() const;

    const std::vector<std::string> &symbols() const {
        return symbols_;
    }
    std::span<const uint32_t> internalRelocs() const {
        return internalRelocs_;
    }
    std::span<const ExternalReloc> externalRelocs() const {
        return externalRelocs_;
    }

    std::span<const uint8_t> code() const;
    std::span<const uint8_t> config() const;
    std::span<const uint8_t> rawData() const {
        return data_;
    }

  private:
    void parseHeader();
    void parseFixupRegion();
    static void validateRanges(const S3eHeader &h, std::size_t fileSize);

    std::span<const uint8_t> data_;
    S3eHeader header_;
    std::vector<std::string> symbols_;
    std::vector<uint32_t> internalRelocs_;
    std::vector<ExternalReloc> externalRelocs_;
};

} // namespace codboz
