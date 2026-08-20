#include "s3e_image.hpp"

#include "bytes.hpp"

#include <set>
#include <stdexcept>

namespace codboz {

S3eImage::S3eImage(std::span<const uint8_t> data) : data_(data) {
    parseHeader();
    validateRanges(header_, data_.size());
    parseFixupRegion();
}

void S3eImage::parseHeader() {
    Reader r(data_);
    S3eHeader h;
    h.ident = r.readLe32();
    h.version = r.readLe32();
    h.flags = r.readLe16();
    h.arch = r.readLe16();
    h.fixupOffset = r.readLe32();
    h.fixupSize = r.readLe32();
    h.codeOffset = r.readLe32();
    h.codeFileSize = r.readLe32();
    h.codeMemSize = r.readLe32();
    h.sigOffset = r.readLe32();
    h.sigSize = r.readLe32();
    h.entryOffset = r.readLe32();
    h.configOffset = r.readLe32();
    h.configSize = r.readLe32();
    h.baseAddrOrig = r.readLe32();
    h.extraOffset = r.readLe32();
    h.extraSize = r.readLe32();
    h.extHeaderSize = r.readLe32();

    if (h.ident != kS3eMagic) {
        throw std::runtime_error("not an S3E image (bad magic)");
    }
    if (h.extHeaderSize == 0x0c) {
        h.dataOffset = r.readLe32();
        h.isJuice = r.readLe32();
    }
    header_ = h;
}

void S3eImage::validateRanges(const S3eHeader &h, std::size_t fileSize) {
    auto rangeOk = [&](uint32_t off, uint32_t len) {
        return off <= fileSize && len <= fileSize - off;
    };
    if (!rangeOk(h.fixupOffset, h.fixupSize) || !rangeOk(h.codeOffset, h.codeFileSize) ||
        h.codeFileSize > h.codeMemSize || h.entryOffset >= h.codeMemSize ||
        (h.extHeaderSize == 0x0c && h.dataOffset > h.codeFileSize)) {
        throw std::runtime_error("S3E header ranges are invalid");
    }
}

std::span<const uint8_t> S3eImage::code() const {
    return data_.subspan(header_.codeOffset, header_.codeFileSize);
}

std::span<const uint8_t> S3eImage::config() const {
    if (header_.configSize == 0) {
        return {};
    }
    if (header_.configOffset > data_.size() ||
        header_.configSize > data_.size() - header_.configOffset) {
        throw std::out_of_range("S3E config range is invalid");
    }
    return data_.subspan(header_.configOffset, header_.configSize);
}

void S3eImage::parseFixupRegion() {
    Reader r(data_);
    r.seek(header_.fixupOffset);
    const std::size_t end = static_cast<std::size_t>(header_.fixupOffset) + header_.fixupSize;

    while (r.position() < end) {
        auto type = r.readLe32();
        auto blockSize = r.readLe32();
        if (blockSize < 8) {
            throw std::runtime_error("invalid S3E fixup block size");
        }
        const std::size_t blockStart = r.position() - 8;
        if (blockSize > end - blockStart) {
            throw std::runtime_error("S3E fixup block overruns fixup region");
        }
        const std::size_t body = r.position();
        const std::size_t blockEnd = blockStart + blockSize;
        Reader b(data_.subspan(body, blockEnd - body));

        if (type == static_cast<uint32_t>(FixupType::Symbols)) {
            uint16_t count = b.readLe16();
            symbols_.reserve(count);
            for (uint16_t i = 0; i < count; ++i) {
                symbols_.emplace_back(b.readCString(256));
            }
        } else if (type == static_cast<uint32_t>(FixupType::InternalRelocs)) {
            uint32_t count = b.readLe32();
            internalRelocs_.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t offset = b.readLe32();
                if (offset > header_.codeMemSize - 4) {
                    throw std::runtime_error("internal reloc offset out of range");
                }
                internalRelocs_.push_back(offset);
            }
        } else if (type == 2 || type == 3 || type == 4) {
            uint32_t count = b.readLe32();
            externalRelocs_.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t hi = b.readLe16();
                uint32_t lo = b.readLe16();
                uint16_t symIdx = b.readLe16();
                uint32_t offset = (hi << 16) | lo;
                if (offset > header_.codeMemSize - 4 || symIdx >= symbols_.size()) {
                    throw std::runtime_error("external reloc out of range");
                }
                externalRelocs_.push_back({offset, symIdx, type});
            }
        }

        r.seek(blockEnd);
    }
}

FixupSummary S3eImage::parseFixups() const {
    FixupSummary out;
    out.symbols = symbols_;
    out.internalRelocCount = static_cast<uint32_t>(internalRelocs_.size());
    out.externalRelocCount = static_cast<uint32_t>(externalRelocs_.size());
    std::set<uint16_t> seen;
    for (const auto &er : externalRelocs_) {
        seen.insert(er.symbolIndex);
        ++out.externalRelocTypes[er.type];
    }
    out.externalImportSymbols = static_cast<uint32_t>(seen.size());
    return out;
}

} // namespace codboz
