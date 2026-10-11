#include "formats/movie.hpp"

#include "formats/locale.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace os2 {

// ---------------------------------------------------------------------------
// Bit reader

uint32_t MadBitReader::peek(int n) const {
    uint32_t v = 0;
    size_t p = pos_;
    for (int i = 0; i < n; ++i, ++p)
        v = (v << 1) | ((byte(p >> 3) >> (7 - (p & 7))) & 1);
    return v;
}

uint32_t MadBitReader::bits(int n) {
    const uint32_t v = peek(n);
    pos_ += size_t(n);
    return v;
}

int32_t MadBitReader::signedBits(int n) {
    const uint32_t v = bits(n);
    return int32_t(v << (32 - n)) >> (32 - n);
}

// ---------------------------------------------------------------------------
// MPEG-1 tables (ISO/IEC 11172-2): the AC run/level codes, the default intra
// matrix, the zigzag scan; and the AAN IDCT's inverse scale factors.

namespace {

struct AcEntry {
    uint16_t code;
    uint8_t length, run, level;
};

// Table B.14 of ISO/IEC 11172-2 without the first-coefficient "1s" form:
// {code, length, run, level}.
constexpr AcEntry kAcTable[] = {
    {0x3, 2, 0, 1}, {0x4, 4, 0, 2}, {0x5, 5, 0, 3}, {0x6, 7, 0, 4}, {0x26, 8, 0, 5}, {0x21, 8, 0, 6},
    {0xa, 10, 0, 7}, {0x1d, 12, 0, 8}, {0x18, 12, 0, 9}, {0x13, 12, 0, 10}, {0x10, 12, 0, 11},
    {0x1a, 13, 0, 12}, {0x19, 13, 0, 13}, {0x18, 13, 0, 14}, {0x17, 13, 0, 15}, {0x1f, 14, 0, 16},
    {0x1e, 14, 0, 17}, {0x1d, 14, 0, 18}, {0x1c, 14, 0, 19}, {0x1b, 14, 0, 20}, {0x1a, 14, 0, 21},
    {0x19, 14, 0, 22}, {0x18, 14, 0, 23}, {0x17, 14, 0, 24}, {0x16, 14, 0, 25}, {0x15, 14, 0, 26},
    {0x14, 14, 0, 27}, {0x13, 14, 0, 28}, {0x12, 14, 0, 29}, {0x11, 14, 0, 30}, {0x10, 14, 0, 31},
    {0x18, 15, 0, 32}, {0x17, 15, 0, 33}, {0x16, 15, 0, 34}, {0x15, 15, 0, 35}, {0x14, 15, 0, 36},
    {0x13, 15, 0, 37}, {0x12, 15, 0, 38}, {0x11, 15, 0, 39}, {0x10, 15, 0, 40},
    {0x3, 3, 1, 1}, {0x6, 6, 1, 2}, {0x25, 8, 1, 3}, {0xc, 10, 1, 4}, {0x1b, 12, 1, 5}, {0x16, 13, 1, 6},
    {0x15, 13, 1, 7}, {0x1f, 15, 1, 8}, {0x1e, 15, 1, 9}, {0x1d, 15, 1, 10}, {0x1c, 15, 1, 11},
    {0x1b, 15, 1, 12}, {0x1a, 15, 1, 13}, {0x19, 15, 1, 14}, {0x13, 16, 1, 15}, {0x12, 16, 1, 16},
    {0x11, 16, 1, 17}, {0x10, 16, 1, 18},
    {0x5, 4, 2, 1}, {0x4, 7, 2, 2}, {0xb, 10, 2, 3}, {0x14, 12, 2, 4}, {0x14, 13, 2, 5},
    {0x7, 5, 3, 1}, {0x24, 8, 3, 2}, {0x1c, 12, 3, 3}, {0x13, 13, 3, 4},
    {0x6, 5, 4, 1}, {0xf, 10, 4, 2}, {0x12, 12, 4, 3},
    {0x7, 6, 5, 1}, {0x9, 10, 5, 2}, {0x12, 13, 5, 3},
    {0x5, 6, 6, 1}, {0x1e, 12, 6, 2}, {0x14, 16, 6, 3},
    {0x4, 6, 7, 1}, {0x15, 12, 7, 2}, {0x7, 7, 8, 1}, {0x11, 12, 8, 2}, {0x5, 7, 9, 1}, {0x11, 13, 9, 2},
    {0x27, 8, 10, 1}, {0x10, 13, 10, 2}, {0x23, 8, 11, 1}, {0x1a, 16, 11, 2}, {0x22, 8, 12, 1},
    {0x19, 16, 12, 2}, {0x20, 8, 13, 1}, {0x18, 16, 13, 2}, {0xe, 10, 14, 1}, {0x17, 16, 14, 2},
    {0xd, 10, 15, 1}, {0x16, 16, 15, 2}, {0x8, 10, 16, 1}, {0x15, 16, 16, 2},
    {0x1f, 12, 17, 1}, {0x1a, 12, 18, 1}, {0x19, 12, 19, 1}, {0x17, 12, 20, 1}, {0x16, 12, 21, 1},
    {0x1f, 13, 22, 1}, {0x1e, 13, 23, 1}, {0x1d, 13, 24, 1}, {0x1c, 13, 25, 1}, {0x1b, 13, 26, 1},
    {0x1f, 16, 27, 1}, {0x1e, 16, 28, 1}, {0x1d, 16, 29, 1}, {0x1c, 16, 30, 1}, {0x1b, 16, 31, 1},
};
constexpr uint16_t kEscapeCode = 0x1, kEscapeLength = 6; // 0000 01
constexpr uint16_t kEobCode = 0x2, kEobLength = 2;       // 10

// A 16-bit lookup: every code is at most 16 bits.
struct AcLookup {
    std::vector<MadAcCode> table;
    AcLookup() : table(1 << 16) {
        auto fill = [&](uint16_t code, int length, MadAcCode c) {
            c.length = uint8_t(length);
            const uint32_t first = uint32_t(code) << (16 - length), count = 1u << (16 - length);
            for (uint32_t i = 0; i < count; ++i)
                table[first + i] = c;
        };
        for (const auto& e : kAcTable)
            fill(e.code, e.length, {MadAcCode::Coefficient, uint8_t(e.run + 1), e.level, 0});
        fill(kEscapeCode, kEscapeLength, {MadAcCode::Escape, 0, 0, 0});
        fill(kEobCode, kEobLength, {MadAcCode::EndOfBlock, 0, 0, 0});
    }
};

const AcLookup& acLookup() {
    static const AcLookup lookup;
    return lookup;
}

constexpr uint8_t kIntraMatrix[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

constexpr uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// 2^12 / (AAN scale of row * AAN scale of column), rounded: 4096 / (s(u) s(v))
// with s(0) = 1 and s(k) = sqrt(2) cos(k pi / 16).
std::array<uint16_t, 64> makeInvAanScales() {
    std::array<uint16_t, 64> t{};
    auto s = [](int k) { return k == 0 ? 1.0 : std::sqrt(2.0) * std::cos(k * M_PI / 16); };
    for (int i = 0; i < 64; ++i)
        t[size_t(i)] = uint16_t(std::lround(4096.0 / (s(i / 8) * s(i % 8))));
    return t;
}

} // namespace

MadAcCode readMadAcCode(MadBitReader& br) {
    MadAcCode c = acLookup().table[br.peek(16)];
    if (c.kind != MadAcCode::Invalid)
        br.skip(c.length);
    return c;
}

// ---------------------------------------------------------------------------
// IDCT: a separable AAN-style integer transform, columns then rows; the
// result is >> 4 (the quantiser's scale) and clamped. Intermediates are kept
// in 16 bits between the passes, as the original does.

namespace {

constexpr int kSqrtHalf = 181; // (1/sqrt 2) << 8
constexpr int kA4 = 669;       // cos(pi/8) sqrt 2 << 9
constexpr int kA2 = 277;       // sin(pi/8) sqrt 2 << 9
constexpr int kA5 = 196;       // sin(pi/8) << 9

template <typename In, typename Out>
inline void idct8(const In* s, int step, Out&& out) {
    const int a1 = s[1 * step] + s[7 * step], a7 = s[1 * step] - s[7 * step];
    const int a5 = s[5 * step] + s[3 * step], a3 = s[5 * step] - s[3 * step];
    const int a2 = s[2 * step] + s[6 * step], a6 = (kSqrtHalf * (s[2 * step] - s[6 * step])) >> 8;
    const int a0 = s[0] + s[4 * step], a4 = s[0] - s[4 * step];
    const int odd = ((kA4 - kA5) * a7 - kA5 * a3) >> 9;
    const int even = ((kA2 + kA5) * a3 + kA5 * a7) >> 9;
    const int mid = (kSqrtHalf * (a1 - a5)) >> 8;
    const int b0 = odd + a1 + a5, b1 = odd + mid, b2 = even + mid, b3 = even;
    out(0, a0 + a2 + a6 + b0);
    out(1, a4 + a6 + b1);
    out(2, a4 - a6 + b2);
    out(3, a0 - a2 - a6 + b3);
    out(4, a0 - a2 - a6 - b3);
    out(5, a4 - a6 - b2);
    out(6, a4 + a6 - b1);
    out(7, a0 + a2 + a6 - b0);
}

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

} // namespace

void madIdct(int16_t block[64], uint8_t* dst, ptrdiff_t stride) {
    int16_t temp[64];
    block[0] = int16_t(block[0] + 4);
    for (int c = 0; c < 8; ++c) {
        const int16_t* s = block + c;
        if ((s[8] | s[16] | s[24] | s[32] | s[40] | s[48] | s[56]) == 0) {
            for (int r = 0; r < 8; ++r)
                temp[r * 8 + c] = s[0];
        } else {
            idct8(s, 8, [&](int r, int v) { temp[r * 8 + c] = int16_t(v); });
        }
    }
    for (int r = 0; r < 8; ++r) {
        uint8_t* d = dst + r * stride;
        idct8(temp + r * 8, 1, [&](int i, int v) { d[i] = clamp8(v >> 4); });
    }
}

// ---------------------------------------------------------------------------
// MAD frames

void MadDecoder::resize(int w, int h) {
    width_ = w;
    height_ = h;
    stride_ = (w + 15) / 16 * 16;
    rows_ = (h + 15) / 16 * 16;
    const size_t luma = size_t(stride_) * size_t(rows_);
    for (auto& f : frames_) {
        f.assign(luma + luma / 2, 0);
        std::fill(f.begin() + ptrdiff_t(luma), f.end(), uint8_t(0x80)); // grey chroma
    }
    haveRef_ = false;
}

uint8_t* MadDecoder::planePtr(int frame, int p) {
    const size_t luma = size_t(stride_) * size_t(rows_);
    uint8_t* base = frames_[frame].data();
    return p == 0 ? base : base + luma + (p == 2 ? luma / 4 : 0);
}

const uint8_t* MadDecoder::plane(int p) const {
    return const_cast<MadDecoder*>(this)->planePtr(out_, p);
}

namespace {

int readMotion(MadBitReader& br) {
    if (!br.bits(1))
        return 0;
    int v = br.bits(1) ? -17 : 0;
    return v + int(br.bits(4)) + 1;
}

// One intra block: DC (8 bits signed, +128), then AC run/levels in zigzag
// order until the end-of-block code. Values are kept in 16 bits.
bool readIntraBlock(MadBitReader& br, const uint16_t quant[64], int16_t block[64]) {
    std::memset(block, 0, 64 * sizeof(int16_t));
    const auto q = [&](int i) { return int(int16_t(quant[i])); };
    block[0] = int16_t((128 + br.signedBits(8)) * q(0));
    int i = 0;
    for (;;) {
        const MadAcCode c = readMadAcCode(br);
        int level;
        if (c.kind == MadAcCode::EndOfBlock)
            break;
        if (c.kind == MadAcCode::Invalid || br.overrun())
            return false;
        if (c.kind == MadAcCode::Coefficient) {
            i += c.run;
            if (i > 63)
                return false;
            const int j = kZigzag[i];
            level = (c.level * q(j)) >> 4;
            level = (level - 1) | 1;
            if (br.bits(1))
                level = -level;
            block[j] = int16_t(level);
        } else { // escape: 10-bit signed level, 6-bit run - 1
            level = br.signedBits(10);
            i += int(br.bits(6)) + 1;
            if (i > 63)
                return false;
            const int j = kZigzag[i];
            const bool neg = level < 0;
            level = ((neg ? -level : level) * q(j)) >> 4;
            level = (level - 1) | 1;
            block[j] = int16_t(neg ? -level : level);
        }
    }
    return true;
}

// Copies an 8x8 block from the reference, adding `add` to every pixel.
void copyBlock(uint8_t* dst, const uint8_t* src, int stride, int add) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            dst[y * stride + x] = clamp8(src[y * stride + x] + add);
}

} // namespace

bool MadDecoder::decode(std::span<const uint8_t> chunk) {
    if (chunk.size() < 26 || std::memcmp(chunk.data(), "MAD", 3) != 0)
        return false;
    const char kind = char(chunk[3]);
    const bool inter = kind == 'm' || kind == 'e';
    auto u16 = [&](size_t o) { return int(chunk[o] | chunk[o + 1] << 8); };
    const uint32_t ms = uint32_t(chunk[12] | chunk[13] << 8 | chunk[14] << 16 | uint32_t(chunk[15]) << 24);
    const int w = u16(16), h = u16(18);
    if (w < 16 || h < 16 || w > 4096 || h > 4096)
        return false;
    if (w != width_ || h != height_)
        resize(w, h);
    frameMs_ = ms / 65536.0;

    const int qscale = chunk[21];
    static const std::array<uint16_t, 64> inv = makeInvAanScales();
    quant_[0] = uint16_t((inv[0] * kIntraMatrix[0]) >> 11);
    for (int i = 1; i < 64; ++i)
        quant_[i] = uint16_t((uint32_t(inv[size_t(i)]) * kIntraMatrix[i] * uint32_t(qscale) + 32) >> 10);

    // Builds into the scratch picture; MADk/MADm then become the reference.
    const int target = ref_ == 0 ? 1 : 0;
    MadBitReader br(chunk.subspan(24));
    const int mbw = stride_ / 16, mbh = rows_ / 16;
    int16_t block[64];
    bool ok = true;
    for (int my = 0; my < mbh && ok; ++my) {
        for (int mx = 0; mx < mbw && ok; ++mx) {
            int map = 0, mvx = 0, mvy = 0;
            if (inter) {
                int v;
                if (br.bits(1))
                    v = 0;
                else
                    v = 2 - int(br.bits(1));
                if (v < 2) {
                    map = v ? int(br.bits(6)) : 63;
                    mvx = readMotion(br);
                    mvy = readMotion(br);
                }
            }
            for (int j = 0; j < 6; ++j) {
                const int p = j < 4 ? 0 : j - 3;
                const int s = stride(p);
                const int bx = p ? mx * 8 : mx * 16 + (j & 1) * 8;
                const int by = p ? my * 8 : my * 16 + (j & 2) * 4;
                uint8_t* d = planePtr(target, p) + by * s + bx;
                if (map & (1 << j)) {
                    const int add = 2 * readMotion(br);
                    if (!haveRef_)
                        continue;
                    // Chroma moves by half the vector (rounded toward zero).
                    const int sx = bx + (p ? mvx / 2 : mvx), sy = by + (p ? mvy / 2 : mvy);
                    const int ph = p ? rows_ / 2 : rows_;
                    if (sx < 0 || sy < 0 || sx > s - 8 || sy > ph - 8)
                        continue; // out of the picture: the block is left as it was
                    copyBlock(d, planePtr(ref_, p) + sy * s + sx, s, add);
                } else {
                    if (!readIntraBlock(br, quant_, block)) {
                        ok = false;
                        break;
                    }
                    madIdct(block, d, s);
                }
            }
        }
    }
    out_ = target;
    if (kind != 'e') {
        ref_ = target;
        haveRef_ = true;
    }
    return ok;
}

void MadDecoder::toRgba(RgbaImage& out) const {
    out.width = uint32_t(width_);
    out.height = uint32_t(height_);
    out.pixels.resize(size_t(width_) * size_t(height_) * 4);
    const uint8_t* Y = plane(0);
    const uint8_t* U = plane(1);
    const uint8_t* V = plane(2);
    const int cs = stride_ / 2;
    uint8_t* o = out.pixels.data();
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const int c = 298 * (Y[y * stride_ + x] - 16);
            const int u = U[(y / 2) * cs + x / 2] - 128, v = V[(y / 2) * cs + x / 2] - 128;
            *o++ = clamp8((c + 409 * v + 128) >> 8);
            *o++ = clamp8((c - 100 * u - 208 * v + 128) >> 8);
            *o++ = clamp8((c + 516 * u + 128) >> 8);
            *o++ = 255;
        }
    }
}

// ---------------------------------------------------------------------------
// Sound

MovieSoundHeader parseSoundHeader(std::span<const uint8_t> p) {
    MovieSoundHeader h;
    size_t o = 0;
    if (p.size() >= 4 && p[0] == 'P' && p[1] == 'T')
        o = 4;
    auto value = [&]() -> uint32_t { // a length byte, then that many bytes, big-endian
        if (o >= p.size())
            return 0;
        const size_t n = p[o++];
        uint32_t v = 0;
        for (size_t i = 0; i < n && o < p.size(); ++i)
            v = (v << 8) | p[o++];
        return v;
    };
    bool header = true;
    while (header && o < p.size()) {
        const uint8_t tag = p[o++];
        if (tag == 0xFF)
            break;
        if (tag != 0xFD) {
            value();
            continue;
        }
        while (o < p.size()) { // the sub-header
            const uint8_t sub = p[o++];
            if (sub == 0xFF) {
                header = false;
                break;
            }
            const uint32_t v = value();
            switch (sub) {
            case 0x80: h.revision = int(v); break;
            case 0x82: h.channels = int(v); break;
            case 0x83: h.compression = int(v); break;
            case 0x84: h.sampleRate = int(v); break;
            case 0x85: h.samples = v; break;
            default: break; // 0x8C and others: not needed
            }
        }
    }
    if (h.sampleRate < 0)
        h.sampleRate = h.revision == 3 ? 48000 : 22050;
    return h;
}

void decodeEaAdpcmR1(std::span<const uint8_t> p, int channels, std::vector<int16_t>& out) {
    static constexpr int kCoeff[8] = {0, 240, 460, 392, 0, 0, -208, -220};
    if (channels < 1 || channels > 6 || p.size() < size_t(4 + 4 * channels))
        return;
    auto le32 = [&](size_t o) { return uint32_t(p[o] | p[o + 1] << 8 | p[o + 2] << 16 | uint32_t(p[o + 3]) << 24); };
    const uint32_t declared = le32(0);
    const uint32_t frames = declared / 28;
    const size_t base = out.size();
    out.resize(base + size_t(frames) * 28 * size_t(channels), 0);
    for (int ch = 0; ch < channels; ++ch) {
        size_t o = size_t(le32(4 + 4 * size_t(ch))) + size_t(channels + 1) * 4;
        if (o + 4 > p.size())
            continue;
        int cur = int16_t(p[o] | p[o + 1] << 8);
        int prev = int16_t(p[o + 2] | p[o + 3] << 8);
        o += 4;
        int16_t* dst = out.data() + base + size_t(ch);
        for (uint32_t f = 0; f < frames && o < p.size(); ++f) {
            const uint8_t head = p[o++];
            const int c1 = kCoeff[head >> 4 & 7], c2 = kCoeff[(head >> 4 & 7) + 4];
            const int shift = 20 - (head & 0x0F);
            uint8_t byte = 0;
            for (int s = 0; s < 28; ++s) {
                int nibble;
                if (s & 1) {
                    nibble = int8_t(byte << 4) >> 4;
                } else {
                    byte = o < p.size() ? p[o++] : 0;
                    nibble = int8_t(byte) >> 4;
                }
                int next = int(unsigned(nibble) << shift);
                next += cur * c1 + prev * c2;
                next = std::clamp(next >> 8, -32768, 32767);
                prev = cur;
                cur = next;
                *dst = int16_t(cur);
                dst += channels;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The file

Movie::Movie(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {
    size_t o = 0;
    while (o + 8 <= bytes_.size()) {
        const uint8_t* c = bytes_.data() + o;
        const size_t size = size_t(c[4] | c[5] << 8 | c[6] << 16 | size_t(c[7]) << 24);
        if (size < 8 || o + size > bytes_.size())
            break;
        if (std::memcmp(c, "MAD", 3) == 0 && (c[3] == 'k' || c[3] == 'm' || c[3] == 'e') && size >= 26) {
            if (frames_.empty()) {
                width_ = c[16] | c[17] << 8;
                height_ = c[18] | c[19] << 8;
                frameMs_ = (c[12] | c[13] << 8 | c[14] << 16 | uint32_t(c[15]) << 24) / 65536.0;
            }
            frames_.push_back({o, size, char(c[3])});
        } else if (std::memcmp(c, "SCHl", 4) == 0) {
            sound_ = parseSoundHeader({c + 8, size - 8});
        } else if (std::memcmp(c, "SCDl", 4) == 0) {
            soundChunks_.push_back({o + 8, size - 8});
        }
        o += size;
    }
    if (frames_.empty() && soundChunks_.empty())
        throw std::runtime_error("not an EA movie (no MAD or SCDl chunks)");
    if (frameMs_ <= 0)
        frameMs_ = 1000.0 / 15;
}

Movie Movie::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Movie(std::move(bytes));
}

Pcm Movie::decodeSound() const {
    Pcm pcm;
    if (soundChunks_.empty())
        return pcm;
    pcm.sampleRate = uint32_t(sound_.sampleRate);
    pcm.channels = uint16_t(sound_.channels);
    if (sound_.samples)
        pcm.samples.reserve(size_t(sound_.samples) * pcm.channels);
    for (const auto& [o, n] : soundChunks_)
        decodeEaAdpcmR1({bytes_.data() + o, n}, sound_.channels, pcm.samples);
    return pcm;
}

size_t MovieDecoder::frameAt(double seconds) const {
    if (movie_.frames().empty())
        return 0;
    const double f = std::floor(std::max(0.0, seconds) / movie_.frameSeconds() + 1e-6);
    return std::min(size_t(f), movie_.frames().size() - 1);
}

bool MovieDecoder::seek(double seconds) {
    if (movie_.frames().empty())
        return false;
    const long want = long(frameAt(seconds));
    if (want == next_ - 1)
        return false;
    if (want < next_ - 1)
        next_ = 0;
    // From far behind: start at the last intra frame at or before `want`.
    for (long k = want; k > next_; --k)
        if (movie_.frames()[size_t(k)].kind == 'k') {
            next_ = k;
            break;
        }
    for (; next_ <= want; ++next_)
        mad_.decode(movie_.chunk(movie_.frames()[size_t(next_)]));
    return true;
}

std::filesystem::path findMovie(const std::filesystem::path& resDir, const std::string& name) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(name, ec))
        return name;
    std::string rel = name;
    if (rel.size() < 6 || rel.compare(rel.size() - 6, 6, ".movie") != 0)
        rel += ".movie";
    const fs::path relPath(rel);
    const fs::path localized = localizedFile(resDir, fs::path("Movies") / relPath, defaultLocale());
    if (fs::is_regular_file(localized, ec))
        return localized;
    // By file name anywhere under Movies.
    const std::string leaf = relPath.filename().string();
    auto lower = [](std::string s) {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string want = lower(leaf);
    if (fs::is_directory(resDir / "Movies", ec))
        for (const auto& e : fs::recursive_directory_iterator(resDir / "Movies", ec))
            if (e.is_regular_file(ec) && lower(e.path().filename().string()) == want)
                return e.path();
    return {};
}

} // namespace os2
