#include "ui_image.hpp"

#include "jpeg.hpp"

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace os2 {

namespace {

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

uint16_t le16(std::span<const uint8_t> d, size_t o) {
    if (o + 2 > d.size())
        throw Fail("truncated");
    return uint16_t(d[o] | d[o + 1] << 8);
}
uint32_t le32(std::span<const uint8_t> d, size_t o) {
    if (o + 4 > d.size())
        throw Fail("truncated");
    return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
}
uint32_t be32(std::span<const uint8_t> d, size_t o) {
    if (o + 4 > d.size())
        throw Fail("truncated");
    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | uint32_t(d[o + 3]);
}

RgbaImage blank(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > 8192 || h > 8192)
        throw Fail("bad size " + std::to_string(w) + "x" + std::to_string(h));
    RgbaImage img;
    img.width = w;
    img.height = h;
    img.pixels.assign(size_t(w) * h * 4, 255);
    return img;
}

RgbaImage decodeTga(std::span<const uint8_t> d) {
    if (d.size() < 18)
        throw Fail("truncated header");
    const uint8_t idLen = d[0], mapType = d[1], type = d[2], bpp = d[16], desc = d[17];
    if (mapType != 0 || (type != 2 && type != 10))
        throw Fail("unsupported TGA type " + std::to_string(type));
    if (bpp != 24 && bpp != 32)
        throw Fail("unsupported TGA depth " + std::to_string(bpp));
    const uint32_t w = le16(d, 12), h = le16(d, 14);
    RgbaImage img = blank(w, h);
    const size_t bytes = bpp / 8, count = size_t(w) * h;
    size_t pos = 18 + idLen;
    std::vector<uint8_t> px(count * bytes);
    if (type == 2) {
        if (pos + px.size() > d.size())
            throw Fail("truncated pixels");
        std::memcpy(px.data(), d.data() + pos, px.size());
    } else {
        size_t out = 0;
        while (out < count) {
            if (pos >= d.size())
                throw Fail("truncated RLE");
            const uint8_t hdr = d[pos++];
            const size_t n = (hdr & 0x7F) + 1;
            if (out + n > count)
                throw Fail("RLE overrun");
            if (hdr & 0x80) {
                if (pos + bytes > d.size())
                    throw Fail("truncated RLE");
                for (size_t i = 0; i < n; ++i)
                    std::memcpy(&px[(out + i) * bytes], d.data() + pos, bytes);
                pos += bytes;
            } else {
                if (pos + n * bytes > d.size())
                    throw Fail("truncated RLE");
                std::memcpy(&px[out * bytes], d.data() + pos, n * bytes);
                pos += n * bytes;
            }
            out += n;
        }
    }
    const bool topDown = desc & 0x20, rightToLeft = desc & 0x10;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* s = &px[(size_t(y) * w + x) * bytes];
            const uint32_t dy = topDown ? y : h - 1 - y, dx = rightToLeft ? w - 1 - x : x;
            uint8_t* o = &img.pixels[(size_t(dy) * w + dx) * 4];
            o[0] = s[2];
            o[1] = s[1];
            o[2] = s[0];
            o[3] = bytes == 4 ? s[3] : 255;
        }
    return img;
}

RgbaImage decodeBmp(std::span<const uint8_t> d) {
    const uint32_t offset = le32(d, 10), hdr = le32(d, 14);
    if (hdr < 40)
        throw Fail("unsupported BMP header");
    const int32_t w = int32_t(le32(d, 18)), hs = int32_t(le32(d, 22));
    const uint16_t bpp = le16(d, 28);
    const uint32_t comp = le32(d, 30);
    if (comp != 0 || (bpp != 24 && bpp != 32))
        throw Fail("unsupported BMP depth/compression " + std::to_string(bpp) + "/" + std::to_string(comp));
    const uint32_t h = uint32_t(hs < 0 ? -hs : hs);
    RgbaImage img = blank(uint32_t(w), h);
    const size_t stride = (size_t(w) * (bpp / 8) + 3) & ~size_t(3);
    if (offset + stride * h > d.size())
        throw Fail("truncated pixels");
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = d.data() + offset + stride * y;
        const uint32_t dy = hs < 0 ? y : h - 1 - y;
        for (int32_t x = 0; x < w; ++x) {
            const uint8_t* s = row + size_t(x) * (bpp / 8);
            uint8_t* o = &img.pixels[(size_t(dy) * uint32_t(w) + uint32_t(x)) * 4];
            o[0] = s[2];
            o[1] = s[1];
            o[2] = s[0];
            o[3] = 255; // the 4th byte of 32-bit BMPs is not alpha in the shipped one (all zero)
        }
    }
    return img;
}

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

RgbaImage decodePng(std::span<const uint8_t> d) {
    size_t pos = 8;
    uint32_t w = 0, h = 0;
    uint8_t depth = 0, color = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    while (pos + 8 <= d.size()) {
        const uint32_t len = be32(d, pos);
        const std::string type(reinterpret_cast<const char*>(d.data() + pos + 4), 4);
        const size_t body = pos + 8;
        if (body + len + 4 > d.size())
            throw Fail("truncated chunk " + type);
        if (type == "IHDR") {
            w = be32(d, body);
            h = be32(d, body + 4);
            depth = d[body + 8];
            color = d[body + 9];
            interlace = d[body + 12];
        } else if (type == "PLTE") {
            palette.assign(d.begin() + long(body), d.begin() + long(body + len));
        } else if (type == "tRNS") {
            trns.assign(d.begin() + long(body), d.begin() + long(body + len));
        } else if (type == "IDAT") {
            idat.insert(idat.end(), d.begin() + long(body), d.begin() + long(body + len));
        } else if (type == "IEND") {
            break;
        }
        pos = body + len + 4;
    }
    if (depth != 8 || interlace != 0)
        throw Fail("unsupported PNG depth/interlace " + std::to_string(depth) + "/" + std::to_string(interlace));
    size_t channels = 0;
    switch (color) {
    case 0: channels = 1; break; // grey
    case 2: channels = 3; break; // RGB
    case 3: channels = 1; break; // palette
    case 4: channels = 2; break; // grey + alpha
    case 6: channels = 4; break; // RGBA
    default: throw Fail("unsupported PNG colour type " + std::to_string(color));
    }
    RgbaImage img = blank(w, h);
    const size_t stride = size_t(w) * channels;
    std::vector<uint8_t> raw((stride + 1) * h);
    uLongf rawLen = uLongf(raw.size());
    if (uncompress(raw.data(), &rawLen, idat.data(), uLong(idat.size())) != Z_OK || rawLen != raw.size())
        throw Fail("bad PNG zlib stream");
    std::vector<uint8_t> prev(stride, 0), cur(stride);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= channels ? cur[i - channels] : 0, b = prev[i], c = i >= channels ? prev[i - channels] : 0;
            int v = src[i];
            switch (filter) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            default: throw Fail("bad PNG filter");
            }
            cur[i] = uint8_t(v);
        }
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* s = &cur[x * channels];
            uint8_t* o = &img.pixels[(size_t(y) * w + x) * 4];
            switch (color) {
            case 0: o[0] = o[1] = o[2] = s[0]; break;
            case 2: o[0] = s[0], o[1] = s[1], o[2] = s[2]; break;
            case 3:
                if (size_t(s[0]) * 3 + 2 >= palette.size())
                    throw Fail("bad palette index");
                o[0] = palette[s[0] * 3], o[1] = palette[s[0] * 3 + 1], o[2] = palette[s[0] * 3 + 2];
                o[3] = s[0] < trns.size() ? trns[s[0]] : 255;
                break;
            case 4: o[0] = o[1] = o[2] = s[0], o[3] = s[1]; break;
            case 6: std::memcpy(o, s, 4); break;
            }
        }
        std::swap(prev, cur);
    }
    return img;
}

} // namespace

const char* uiImageFormatName(UiImageFormat f) {
    switch (f) {
    case UiImageFormat::Tga: return "TGA";
    case UiImageFormat::Bmp: return "BMP";
    case UiImageFormat::Png: return "PNG";
    case UiImageFormat::Jpeg: return "JPEG";
    default: return "unknown";
    }
}

UiImageFormat detectUiImage(std::span<const uint8_t> d) {
    if (d.size() >= 8 && std::memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
        return UiImageFormat::Png;
    if (d.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return UiImageFormat::Jpeg;
    if (d.size() >= 54 && d[0] == 'B' && d[1] == 'M')
        return UiImageFormat::Bmp;
    // TGA has no magic: no colour map and an uncompressed or RLE true-colour type.
    if (d.size() >= 18 && d[1] == 0 && (d[2] == 2 || d[2] == 10) && (d[16] == 24 || d[16] == 32))
        return UiImageFormat::Tga;
    return UiImageFormat::Unknown;
}

std::optional<RgbaImage> decodeUiImage(std::span<const uint8_t> data, std::string* error) {
    try {
        switch (detectUiImage(data)) {
        case UiImageFormat::Tga: return decodeTga(data);
        case UiImageFormat::Bmp: return decodeBmp(data);
        case UiImageFormat::Png: return decodePng(data);
        case UiImageFormat::Jpeg: return decodeJpeg(data);
        default: throw Fail("unknown image format");
        }
    } catch (const JpegError& e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    } catch (const Fail& e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

} // namespace os2
