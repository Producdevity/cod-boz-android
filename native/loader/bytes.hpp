#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>

namespace codboz {

inline uint16_t readLe16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t readLe64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

// Bounds-checked sequential reader over a fixed byte span.
class Reader {
  public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    std::size_t size() const {
        return data_.size();
    }
    std::size_t remaining() const {
        return data_.size() - pos_;
    }
    std::size_t position() const {
        return pos_;
    }

    void seek(std::size_t pos) {
        if (pos > data_.size()) {
            throw std::out_of_range("seek past end");
        }
        pos_ = pos;
    }

    uint8_t readU8() {
        need(1);
        return data_[pos_++];
    }

    uint16_t readLe16() {
        need(2);
        auto v = codboz::readLe16(data_.data() + pos_);
        pos_ += 2;
        return v;
    }

    uint32_t readLe32() {
        need(4);
        auto v = codboz::readLe32(data_.data() + pos_);
        pos_ += 4;
        return v;
    }

    uint64_t readLe64() {
        need(8);
        auto v = codboz::readLe64(data_.data() + pos_);
        pos_ += 8;
        return v;
    }

    std::string_view readCString(std::size_t maxLen) {
        need(1);
        std::size_t start = pos_;
        std::size_t end = start;
        while (end < data_.size() && data_[end] != 0) {
            ++end;
            if (end - start > maxLen) {
                throw std::out_of_range("c-string exceeds max length");
            }
        }
        if (end >= data_.size()) {
            throw std::out_of_range("unterminated c-string");
        }
        std::string_view s(reinterpret_cast<const char *>(data_.data() + start), end - start);
        pos_ = end + 1;
        return s;
    }

    std::span<const uint8_t> readBytes(std::size_t n) {
        need(n);
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

    // Span of [pos, pos+n) without advancing; throws if out of range.
    std::span<const uint8_t> viewAt(std::size_t offset, std::size_t n) const {
        if (offset > data_.size() || n > data_.size() - offset) {
            throw std::out_of_range("view past end");
        }
        return data_.subspan(offset, n);
    }

  private:
    void need(std::size_t n) const {
        if (n > remaining()) {
            throw std::out_of_range("read past end");
        }
    }

    std::span<const uint8_t> data_;
    std::size_t pos_ = 0;
};

} // namespace codboz
