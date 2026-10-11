#pragma once

#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// Little-endian byte sink, the counterpart of BinReader.
class BinWriter {
public:
    std::vector<uint8_t>& bytes() { return out_; }
    const std::vector<uint8_t>& bytes() const { return out_; }
    size_t size() const { return out_.size(); }

    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) {
        out_.push_back(uint8_t(v));
        out_.push_back(uint8_t(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out_.push_back(uint8_t(v >> (8 * i)));
    }
    void i16(int16_t v) { u16(uint16_t(v)); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
    void raw(std::span<const uint8_t> b) { out_.insert(out_.end(), b.begin(), b.end()); }
    // .NET BinaryWriter string (BinReader::str7).
    void str7(const std::string& s) {
        uint32_t n = uint32_t(s.size());
        do {
            uint8_t b = n & 0x7F;
            n >>= 7;
            if (n)
                b |= 0x80;
            out_.push_back(b);
        } while (n);
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void shorts(std::span<const int16_t> v) {
        for (int16_t x : v)
            i16(x);
    }

private:
    std::vector<uint8_t> out_;
};

} // namespace os2
