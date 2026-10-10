#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace os2 {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// "0x1A2B" style hexadecimal for error messages.
inline std::string hexString(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    std::string s;
    do {
        s.insert(s.begin(), digits[v & 15]);
        v >>= 4;
    } while (v);
    return "0x" + s;
}

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Bounds-checked little-endian cursor over a resource's bytes.
class BinReader {
public:
    explicit BinReader(std::span<const uint8_t> data) : data_(data) {}

    size_t pos() const { return pos_; }
    size_t size() const { return data_.size(); }
    size_t remaining() const { return data_.size() - pos_; }
    bool atEnd() const { return pos_ == data_.size(); }
    std::span<const uint8_t> data() const { return data_; }

    void seek(size_t p) {
        if (p > data_.size())
            throw FormatError("seek past end");
        pos_ = p;
    }
    void skip(size_t n) { seek(need(n) + n); }

    uint8_t u8() { auto p = need(1); ++pos_; return data_[p]; }
    uint16_t u16() { auto p = need(2); pos_ += 2; return le16(&data_[p]); }
    uint32_t u32() { auto p = need(4); pos_ += 4; return le32(&data_[p]); }
    int16_t i16() { return int16_t(u16()); }
    int32_t i32() { return int32_t(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }

    std::span<const uint8_t> bytes(size_t n) {
        auto p = need(n);
        pos_ += n;
        return data_.subspan(p, n);
    }

    // .NET BinaryWriter string: 7-bit varint length, then bytes.
    std::string str7() {
        uint32_t len = 0;
        for (int shift = 0;; shift += 7) {
            if (shift > 28)
                throw FormatError("bad string length");
            const uint8_t b = u8();
            len |= uint32_t(b & 0x7F) << shift;
            if (!(b & 0x80))
                break;
        }
        return chars(len);
    }
    // One length byte, then bytes.
    std::string str8() { return chars(u8()); }
    // Zero-terminated.
    std::string cstr() {
        const auto* begin = data_.data() + pos_;
        const void* z = std::memchr(begin, 0, remaining());
        if (!z)
            throw FormatError("unterminated string");
        const size_t n = size_t(static_cast<const uint8_t*>(z) - begin);
        std::string s(reinterpret_cast<const char*>(begin), n);
        pos_ += n + 1;
        return s;
    }
    // Fixed-size field, trimmed at the first NUL.
    std::string fixed(size_t n) {
        auto b = bytes(n);
        std::string s(reinterpret_cast<const char*>(b.data()), n);
        if (auto z = s.find('\0'); z != std::string::npos)
            s.resize(z);
        return s;
    }

    void expectEnd(const char* what) const {
        if (!atEnd())
            throw FormatError(std::string(what) + ": " + std::to_string(remaining()) + " trailing bytes");
    }
    // Like expectEnd, but accepts padding made of one repeated byte (the game's
    // tools leave 0x00 or 0xA3 fill after some resources).
    void expectEndOrFill(const char* what) {
        if (atEnd())
            return;
        const auto rest = bytes(remaining());
        for (auto b : rest)
            if (b != rest[0])
                throw FormatError(std::string(what) + ": " + std::to_string(rest.size()) + " trailing bytes");
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;

    size_t need(size_t n) const {
        if (n > remaining())
            throw FormatError("read past end at offset " + std::to_string(pos_));
        return pos_;
    }
    std::string chars(size_t n) {
        auto b = bytes(n);
        return std::string(reinterpret_cast<const char*>(b.data()), n);
    }
};

} // namespace os2
