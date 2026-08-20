#include "lzma.hpp"

#include "bytes.hpp"

#include <LzmaDec.h>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace codboz {

namespace {
constexpr uint32_t kLzmaHeaderSize = 13;
constexpr uint64_t kMaxOutputSize = 64u * 1024u * 1024u;

void *szAlloc(ISzAllocPtr, size_t size) {
    return malloc(size ? size : 1);
}
void szFree(ISzAllocPtr, void *addr) {
    free(addr);
}
} // namespace

std::vector<uint8_t> lzmaDecompress(std::span<const uint8_t> src) {
    if (src.size() < kLzmaHeaderSize) {
        throw std::runtime_error("LZMA stream is too small for the alone header");
    }
    uint64_t unpacked = readLe64(src.data() + 5);
    if (unpacked == UINT64_MAX || unpacked == 0 || unpacked > kMaxOutputSize) {
        throw std::runtime_error("LZMA stream has an unsupported unpacked size");
    }

    std::vector<uint8_t> out(unpacked);
    SizeT dstLen = static_cast<SizeT>(unpacked);
    SizeT srcLen = static_cast<SizeT>(src.size() - kLzmaHeaderSize);
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    ISzAlloc alloc{szAlloc, szFree};
    SRes res = LzmaDecode(out.data(), &dstLen, src.data() + kLzmaHeaderSize, &srcLen, src.data(),
                          LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &alloc);
    if (res != SZ_OK || dstLen != static_cast<SizeT>(unpacked)) {
        throw std::runtime_error("LZMA decode failed");
    }
    return out;
}

} // namespace codboz
