#include "jpeg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace os2 {

namespace {

constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Huffman {
    // Canonical code tables (JPEG spec F.2.2.3).
    std::array<int32_t, 18> maxcode{};
    std::array<int32_t, 17> valptr{};
    std::array<int32_t, 17> mincode{};
    std::vector<uint8_t> values;
    bool defined = false;

    void build(const uint8_t counts[16], std::vector<uint8_t> vals) {
        values = std::move(vals);
        int32_t code = 0, k = 0;
        for (int len = 1; len <= 16; ++len) {
            valptr[size_t(len)] = k;
            mincode[size_t(len)] = code;
            code += counts[len - 1];
            k += counts[len - 1];
            maxcode[size_t(len)] = counts[len - 1] ? code - 1 : -1;
            code <<= 1;
        }
        maxcode[17] = 0x7FFFFFFF;
        defined = true;
    }
};

struct Component {
    uint8_t id = 0, h = 1, v = 1, tq = 0;
    uint8_t td = 0, ta = 0; // scan tables
    int dcPred = 0;
    int bw = 0, bh = 0; // blocks across/down in the padded plane
    std::vector<uint8_t> plane;
};

class BitReader {
public:
    BitReader(std::span<const uint8_t> d, size_t pos) : d_(d), pos_(pos) {}

    int bit() {
        if (count_ == 0)
            fill();
        --count_;
        return int(acc_ >> count_) & 1;
    }
    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | bit();
        return v;
    }
    // Skips to the next RSTn marker and resets the bit buffer.
    void restart() {
        count_ = 0;
        while (pos_ + 1 < d_.size() && !(d_[pos_] == 0xFF && d_[pos_ + 1] >= 0xD0 && d_[pos_ + 1] <= 0xD7))
            ++pos_;
        if (pos_ + 1 < d_.size())
            pos_ += 2;
        marker_ = false;
    }
    size_t pos() const { return pos_; }

private:
    void fill() {
        // At a marker (or the end), feed zeros, as decoders conventionally do.
        uint8_t b = 0;
        if (!marker_ && pos_ < d_.size()) {
            b = d_[pos_];
            if (b == 0xFF) {
                const uint8_t n = pos_ + 1 < d_.size() ? d_[pos_ + 1] : 0xD9;
                if (n == 0x00)
                    pos_ += 2;
                else {
                    marker_ = true;
                    b = 0;
                }
            } else {
                ++pos_;
            }
        }
        acc_ = (acc_ << 8) | b;
        count_ = 8;
    }

    std::span<const uint8_t> d_;
    size_t pos_;
    uint32_t acc_ = 0;
    int count_ = 0;
    bool marker_ = false;
};

int decodeHuff(BitReader& br, const Huffman& h) {
    if (!h.defined)
        throw JpegError("undefined huffman table");
    int32_t code = br.bit();
    int len = 1;
    while (code > h.maxcode[size_t(len)]) {
        code = (code << 1) | br.bit();
        if (++len > 16)
            throw JpegError("bad huffman code");
    }
    const size_t idx = size_t(h.valptr[size_t(len)] + code - h.mincode[size_t(len)]);
    if (idx >= h.values.size())
        throw JpegError("bad huffman code");
    return h.values[idx];
}

int extend(int v, int t) { return t == 0 ? 0 : v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

// Separable float IDCT of a dequantized block (natural order), level shifted and clamped.
void idct(const float in[64], uint8_t* out, int stride) {
    static float c[8][8];
    static bool init = false;
    if (!init) {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                c[x][u] = float((u == 0 ? std::sqrt(0.5) : 1.0) * std::cos((2 * x + 1) * u * M_PI / 16) / 2);
        init = true;
    }
    float tmp[64];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u)
                s += c[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            float s = 0;
            for (int v = 0; v < 8; ++v)
                s += c[y][v] * tmp[v * 8 + x];
            out[y * stride + x] = uint8_t(std::clamp(int(std::lround(s + 128)), 0, 255));
        }
}

std::vector<uint8_t> decodeAlfa(std::span<const uint8_t> seg, size_t pixels) {
    std::vector<uint8_t> a;
    a.reserve(pixels);
    for (size_t i = 0; i < seg.size();) {
        const int8_t n = int8_t(seg[i++]);
        if (n < 0) {
            if (i >= seg.size())
                break;
            a.insert(a.end(), size_t(-n) + 1, seg[i++]);
        } else {
            for (int k = 0; k <= n && i < seg.size(); ++k)
                a.push_back(seg[i++]);
        }
    }
    return a;
}

} // namespace

RgbaImage decodeJpeg(std::span<const uint8_t> d) {
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8)
        throw JpegError("not a JPEG");
    std::array<std::array<uint16_t, 64>, 4> qt{};
    std::array<Huffman, 4> dc, ac;
    std::vector<Component> comps;
    std::vector<uint8_t> alpha;
    std::vector<uint8_t> alfa; // ALFA segments, concatenated
    uint32_t width = 0, height = 0;
    int hmax = 1, vmax = 1, mcusX = 0, mcusY = 0;
    unsigned restartInterval = 0;
    bool frame = false, scanned = false;
    size_t pos = 2;

    auto u16 = [&](size_t o) {
        if (o + 2 > d.size())
            throw JpegError("truncated");
        return unsigned(d[o] << 8 | d[o + 1]);
    };

    while (pos + 4 <= d.size()) {
        if (d[pos] != 0xFF)
            throw JpegError("expected a marker");
        const uint8_t m = d[pos + 1];
        if (m == 0xFF) {
            ++pos;
            continue;
        }
        if (m == 0xD9)
            break;
        const unsigned len = u16(pos + 2);
        const size_t body = pos + 4, end = pos + 2 + len;
        if (end > d.size() || len < 2)
            throw JpegError("truncated segment");
        switch (m) {
        case 0xE0:
            if (len >= 6 && std::memcmp(&d[body], "ALFA", 4) == 0)
                alfa.insert(alfa.end(), d.begin() + long(body + 4), d.begin() + long(end));
            break;
        case 0xDB:
            for (size_t p = body; p < end;) {
                const uint8_t pq = d[p] >> 4, tq = d[p] & 3;
                ++p;
                for (int i = 0; i < 64; ++i) {
                    qt[tq][kZigzag[i]] = pq ? uint16_t(u16(p)) : d[p];
                    p += pq ? 2 : 1;
                }
            }
            break;
        case 0xC4:
            for (size_t p = body; p + 17 <= end;) {
                const uint8_t tc = d[p] >> 4, th = d[p] & 3;
                uint8_t counts[16];
                std::memcpy(counts, &d[p + 1], 16);
                size_t total = 0;
                for (auto c : counts)
                    total += c;
                p += 17;
                if (p + total > end)
                    throw JpegError("truncated DHT");
                (tc ? ac : dc)[th].build(counts, std::vector<uint8_t>(d.begin() + long(p), d.begin() + long(p + total)));
                p += total;
            }
            break;
        case 0xDD:
            restartInterval = u16(body);
            break;
        case 0xC0:
        case 0xC1: {
            if (d[body] != 8)
                throw JpegError("unsupported precision");
            height = u16(body + 1);
            width = u16(body + 3);
            const int n = d[body + 5];
            if (width == 0 || height == 0 || (n != 1 && n != 3))
                throw JpegError("unsupported frame");
            for (int i = 0; i < n; ++i) {
                Component c;
                c.id = d[body + 6 + size_t(i) * 3];
                c.h = d[body + 7 + size_t(i) * 3] >> 4;
                c.v = d[body + 7 + size_t(i) * 3] & 15;
                c.tq = d[body + 8 + size_t(i) * 3] & 3;
                if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4)
                    throw JpegError("bad sampling factors");
                hmax = std::max(hmax, int(c.h));
                vmax = std::max(vmax, int(c.v));
                comps.push_back(c);
            }
            mcusX = int((width + unsigned(hmax) * 8 - 1) / (unsigned(hmax) * 8));
            mcusY = int((height + unsigned(vmax) * 8 - 1) / (unsigned(vmax) * 8));
            for (auto& c : comps) {
                c.bw = mcusX * c.h;
                c.bh = mcusY * c.v;
                c.plane.assign(size_t(c.bw) * 8 * size_t(c.bh) * 8, 0);
            }
            frame = true;
            break;
        }
        case 0xC2:
        case 0xC3:
        case 0xC5:
        case 0xC6:
        case 0xC7:
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCD:
        case 0xCE:
        case 0xCF:
            throw JpegError("unsupported JPEG process SOF" + std::to_string(m - 0xC0));
        case 0xDA: {
            if (!frame)
                throw JpegError("scan before frame");
            const int n = d[body];
            std::vector<Component*> sc;
            for (int i = 0; i < n; ++i) {
                const uint8_t id = d[body + 1 + size_t(i) * 2], t = d[body + 2 + size_t(i) * 2];
                auto it = std::find_if(comps.begin(), comps.end(), [&](const Component& c) { return c.id == id; });
                if (it == comps.end())
                    throw JpegError("scan of an unknown component");
                it->td = t >> 4;
                it->ta = t & 3;
                it->dcPred = 0;
                sc.push_back(&*it);
            }
            BitReader br(d, end);
            float block[64];
            auto decodeBlock = [&](Component& c, int bx, int by) {
                int coef[64] = {};
                const int t = decodeHuff(br, dc[c.td]);
                c.dcPred += extend(br.bits(t), t);
                coef[0] = c.dcPred;
                for (int k = 1; k < 64;) {
                    const int rs = decodeHuff(br, ac[c.ta]);
                    const int r = rs >> 4, s = rs & 15;
                    if (s == 0) {
                        if (r != 15)
                            break;
                        k += 16;
                        continue;
                    }
                    k += r;
                    if (k > 63)
                        throw JpegError("bad AC run");
                    coef[kZigzag[k]] = extend(br.bits(s), s);
                    ++k;
                }
                for (int i = 0; i < 64; ++i)
                    block[i] = float(coef[i] * qt[c.tq][size_t(i)]);
                const size_t stride = size_t(c.bw) * 8;
                if (bx < c.bw && by < c.bh)
                    idct(block, &c.plane[size_t(by) * 8 * stride + size_t(bx) * 8], int(stride));
            };
            unsigned todo = restartInterval;
            auto maybeRestart = [&]() {
                if (!restartInterval)
                    return;
                if (--todo == 0) {
                    br.restart();
                    todo = restartInterval;
                    for (auto* c : sc)
                        c->dcPred = 0;
                }
            };
            if (sc.size() == 1) {
                Component& c = *sc[0];
                const int bw = int((width * c.h / unsigned(hmax) + 7) / 8), bh = int((height * c.v / unsigned(vmax) + 7) / 8);
                for (int by = 0; by < bh; ++by)
                    for (int bx = 0; bx < bw; ++bx) {
                        decodeBlock(c, bx, by);
                        maybeRestart();
                    }
            } else {
                for (int my = 0; my < mcusY; ++my)
                    for (int mx = 0; mx < mcusX; ++mx) {
                        for (auto* c : sc)
                            for (int v = 0; v < c->v; ++v)
                                for (int h = 0; h < c->h; ++h)
                                    decodeBlock(*c, mx * c->h + h, my * c->v + v);
                        maybeRestart();
                    }
            }
            scanned = true;
            // Continue after the entropy-coded data: find the next marker that is not RSTn.
            size_t p = br.pos();
            while (p + 1 < d.size() && !(d[p] == 0xFF && d[p + 1] != 0 && !(d[p + 1] >= 0xD0 && d[p + 1] <= 0xD7)))
                ++p;
            pos = p;
            continue;
        }
        default:
            break;
        }
        pos = end;
    }
    if (!scanned)
        throw JpegError("no image data");

    RgbaImage img;
    img.width = width;
    img.height = height;
    img.pixels.assign(size_t(width) * height * 4, 255);
    auto sample = [&](const Component& c, uint32_t x, uint32_t y) {
        const size_t sx = x * c.h / unsigned(hmax), sy = y * c.v / unsigned(vmax);
        return float(c.plane[sy * size_t(c.bw) * 8 + sx]);
    };
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* o = &img.pixels[(size_t(y) * width + x) * 4];
            const float Y = sample(comps[0], x, y);
            if (comps.size() == 1) {
                o[0] = o[1] = o[2] = uint8_t(Y);
                continue;
            }
            const float cb = sample(comps[1], x, y) - 128, cr = sample(comps[2], x, y) - 128;
            o[0] = uint8_t(std::clamp(std::lround(Y + 1.402f * cr), 0l, 255l));
            o[1] = uint8_t(std::clamp(std::lround(Y - 0.344136f * cb - 0.714136f * cr), 0l, 255l));
            o[2] = uint8_t(std::clamp(std::lround(Y + 1.772f * cb), 0l, 255l));
        }
    if (!alfa.empty()) {
        alpha = decodeAlfa(alfa, size_t(width) * height);
        const size_t n = std::min(alpha.size(), size_t(width) * height);
        for (size_t i = 0; i < n; ++i)
            img.pixels[i * 4 + 3] = alpha[i];
    }
    return img;
}

} // namespace os2
