#include "image.hpp"

#include "resindex.hpp"

#include <algorithm>
#include <fstream>
#include <zlib.h>

namespace os2 {

namespace {

constexpr uint32_t kTypeLifo = 0xED534136;
constexpr uint32_t kMaxDim = 8192;

bool isDxt(TexFormat f) { return f == TexFormat::Dxt1 || f == TexFormat::Dxt3 || f == TexFormat::Dxt5; }

void rgb565(uint16_t c, uint8_t out[4]) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = uint8_t((r << 3) | (r >> 2));
    out[1] = uint8_t((g << 2) | (g >> 4));
    out[2] = uint8_t((b << 3) | (b >> 2));
    out[3] = 255;
}

// Colour part of a DXT block (8 bytes) into a 4x4 RGBA block.
void decodeColorBlock(const uint8_t* p, bool allowPunchThrough, uint8_t out[16][4]) {
    const uint16_t c0 = le16(p), c1 = le16(p + 2);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1 || !allowPunchThrough) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (c0 > c1 || !allowPunchThrough) ? 255 : 0;
    const uint32_t bits = le32(p + 4);
    for (int i = 0; i < 16; ++i)
        std::copy_n(pal[(bits >> (2 * i)) & 3], 4, out[i]);
}

void decodeDxt5Alpha(const uint8_t* p, uint8_t out[16][4]) {
    uint8_t a[8];
    a[0] = p[0];
    a[1] = p[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; ++i)
            a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
    } else {
        for (int i = 1; i < 5; ++i)
            a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= uint64_t(p[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i][3] = a[(bits >> (3 * i)) & 7];
}

void decodeDxt(TexFormat f, RgbaImage& img, std::span<const uint8_t> data) {
    const uint32_t bw = (img.width + 3) / 4, bh = (img.height + 3) / 4;
    const size_t blockSize = f == TexFormat::Dxt1 ? 8 : 16;
    uint8_t block[16][4];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* p = data.data() + (size_t(by) * bw + bx) * blockSize;
            if (f == TexFormat::Dxt1) {
                decodeColorBlock(p, true, block);
            } else {
                decodeColorBlock(p + 8, false, block);
                if (f == TexFormat::Dxt3) {
                    for (int i = 0; i < 16; ++i) {
                        const uint8_t nib = (p[i / 2] >> (4 * (i & 1))) & 15;
                        block[i][3] = uint8_t(nib * 17);
                    }
                } else {
                    decodeDxt5Alpha(p, block);
                }
            }
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < img.width && y < img.height)
                    std::copy_n(block[i], 4, &img.pixels[(size_t(y) * img.width + x) * 4]);
            }
        }
}

} // namespace

const char* texFormatName(TexFormat f) {
    switch (f) {
    case TexFormat::Raw32: return "Raw32";
    case TexFormat::Raw24: return "Raw24";
    case TexFormat::Alpha8: return "Alpha8";
    case TexFormat::Dxt1: return "DXT1";
    case TexFormat::Dxt3: return "DXT3";
    case TexFormat::Raw8: return "Raw8";
    case TexFormat::ExtRaw24: return "ExtRaw24";
    case TexFormat::Dxt5: return "DXT5";
    case TexFormat::ExtRaw32: return "ExtRaw32";
    }
    return "?";
}

size_t texDataSize(TexFormat f, uint32_t width, uint32_t height) {
    const size_t px = size_t(width) * height;
    switch (f) {
    case TexFormat::Raw32:
    case TexFormat::ExtRaw32: return px * 4;
    case TexFormat::Raw24:
    case TexFormat::ExtRaw24: return px * 3;
    case TexFormat::Raw8:
    case TexFormat::Alpha8: return px;
    case TexFormat::Dxt1: return size_t((width + 3) / 4) * ((height + 3) / 4) * 8;
    case TexFormat::Dxt3:
    case TexFormat::Dxt5: return size_t((width + 3) / 4) * ((height + 3) / 4) * 16;
    }
    throw FormatError("unknown texture format " + std::to_string(uint32_t(f)));
}

RgbaImage decodeTexture(TexFormat f, uint32_t width, uint32_t height, std::span<const uint8_t> data) {
    const size_t expected = texDataSize(f, width, height);
    if (data.size() != expected)
        throw FormatError(std::string(texFormatName(f)) + " " + std::to_string(width) + "x" + std::to_string(height) +
                          ": expected " + std::to_string(expected) + " bytes, got " + std::to_string(data.size()));
    RgbaImage img;
    img.width = width;
    img.height = height;
    img.pixels.resize(size_t(width) * height * 4);
    if (isDxt(f)) {
        decodeDxt(f, img, data);
        return img;
    }
    const size_t n = size_t(width) * height;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* o = &img.pixels[i * 4];
        switch (f) {
        case TexFormat::Raw32:
        case TexFormat::ExtRaw32:
            o[0] = data[i * 4 + 2], o[1] = data[i * 4 + 1], o[2] = data[i * 4], o[3] = data[i * 4 + 3];
            break;
        case TexFormat::Raw24:
        case TexFormat::ExtRaw24:
            o[0] = data[i * 3 + 2], o[1] = data[i * 3 + 1], o[2] = data[i * 3], o[3] = 255;
            break;
        case TexFormat::Alpha8:
            o[0] = o[1] = o[2] = 255, o[3] = data[i];
            break;
        default: // 8-bit luminance
            o[0] = o[1] = o[2] = data[i], o[3] = 255;
            break;
        }
    }
    return img;
}

ImageData ImageData::parse(std::span<const uint8_t> body) {
    BinReader r(body);
    auto d = read(r);
    r.expectEnd("cImageData");
    return d;
}

ImageData ImageData::read(BinReader& r) {
    ImageData d;
    d.version = r.u32();
    if (d.version < 7 || d.version > 9)
        throw FormatError("cImageData: unsupported version " + std::to_string(d.version));
    d.resource = SgResource::read(r);
    d.width = r.u32();
    d.height = r.u32();
    d.format = TexFormat(r.u32());
    d.mipLevels = r.u32();
    d.unknown0 = r.f32();
    const uint32_t count = r.u32();
    d.unknown1 = r.u32();
    if (d.version == 9)
        d.fileNameRepeat = r.str7();
    if (d.width > kMaxDim || d.height > kMaxDim || count > 64)
        throw FormatError("cImageData: implausible header");
    for (uint32_t t = 0; t < count; ++t) {
        // Only version 9 stores the per-texture mip count.
        const uint32_t mips = d.version == 9 ? r.u32() : d.mipLevels;
        if (mips > 16)
            throw FormatError("cImageData: bad mip count");
        Texture tex;
        tex.mips.resize(mips);
        for (auto& m : tex.mips) {
            const uint8_t kind = r.u8();
            if (kind == 0) {
                auto b = r.bytes(r.u32());
                m.data.assign(b.begin(), b.end());
            } else if (kind == 1) {
                m.lifoName = r.str7();
            } else {
                throw FormatError("cImageData: bad mip kind " + std::to_string(kind));
            }
        }
        tex.creator = r.u32();
        if (d.version >= 8)
            tex.unknown = r.f32();
        d.textures.push_back(std::move(tex));
    }

    // Inline mips must match the declared format.
    for (const auto& tex : d.textures)
        for (size_t i = 0, n = tex.mips.size(); i < n; ++i)
            if (tex.mips[i].lifoName.empty()) {
                const size_t want = texDataSize(d.format, d.mipWidth(i, n), d.mipHeight(i, n));
                if (tex.mips[i].data.size() != want)
                    throw FormatError("cImageData: mip " + std::to_string(i) + " has " +
                                      std::to_string(tex.mips[i].data.size()) + " bytes, expected " +
                                      std::to_string(want));
            }
    return d;
}

uint32_t ImageData::mipWidth(size_t i, size_t count) const { return std::max<uint32_t>(1, width >> (count - 1 - i)); }
uint32_t ImageData::mipHeight(size_t i, size_t count) const { return std::max<uint32_t>(1, height >> (count - 1 - i)); }

LevelInfo LevelInfo::parse(std::span<const uint8_t> body) {
    BinReader r(body);
    auto l = read(r);
    r.expectEnd("cLevelInfo");
    return l;
}

LevelInfo LevelInfo::read(BinReader& r) {
    LevelInfo l;
    l.version = r.u32();
    l.resource = SgResource::read(r);
    l.width = r.u32();
    l.height = r.u32();
    l.zLevel = r.u32();
    auto b = r.bytes(r.u32());
    l.data.assign(b.begin(), b.end());
    return l;
}

RgbaImage loadTexture(std::span<const uint8_t> txtrResource, ResourceIndex* index) {
    Rcol rc(txtrResource);
    const auto* block = rc.find("cImageData");
    if (!block)
        throw FormatError("TXTR without cImageData");
    const auto img = ImageData::parse(block->body);
    if (img.textures.empty() || img.textures[0].mips.empty())
        throw FormatError("TXTR without mip levels");
    // Largest mip that can be loaded: a few LIFO references point to resources
    // that do not ship with the game, so fall back to smaller levels.
    const auto& levels = img.textures[0].mips;
    std::string missing;
    for (size_t i = levels.size(); i-- > 0;) {
        const auto& m = levels[i];
        const uint32_t w = img.mipWidth(i, levels.size()), h = img.mipHeight(i, levels.size());
        if (m.lifoName.empty())
            return decodeTexture(img.format, w, h, m.data);
        if (!index)
            throw FormatError("TXTR references LIFO '" + m.lifoName + "' but no resource index was given");
        auto loc = index->findByName(m.lifoName, kTypeLifo);
        if (!loc) {
            if (missing.empty())
                missing = m.lifoName;
            continue;
        }
        const auto bytes = index->read(*loc);
        Rcol lrc(bytes);
        const auto* lb = lrc.find("cLevelInfo");
        if (!lb)
            throw FormatError("LIFO without cLevelInfo");
        const auto lifo = LevelInfo::parse(lb->body);
        return decodeTexture(img.format, lifo.width, lifo.height, lifo.data);
    }
    throw FormatError("no loadable mip level (missing LIFO " + missing + ")");
}

void writePng(const std::filesystem::path& path, const RgbaImage& img) {
    auto be32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int s = 24; s >= 0; s -= 8)
            v.push_back(uint8_t(x >> s));
    };
    std::vector<uint8_t> raw;
    raw.reserve((size_t(img.width) * 4 + 1) * img.height);
    for (uint32_t y = 0; y < img.height; ++y) {
        raw.push_back(0); // filter: none
        const auto* row = &img.pixels[size_t(y) * img.width * 4];
        raw.insert(raw.end(), row, row + size_t(img.width) * 4);
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK)
        throw FormatError("PNG: zlib compression failed");
    z.resize(zlen);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* tag, const std::vector<uint8_t>& payload) {
        be32(out, uint32_t(payload.size()));
        const size_t start = out.size();
        out.insert(out.end(), tag, tag + 4);
        out.insert(out.end(), payload.begin(), payload.end());
        be32(out, uint32_t(crc32(0, &out[start], uInt(out.size() - start))));
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, img.width);
    be32(ihdr, img.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no interlace
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw FormatError("cannot write " + path.string());
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}

} // namespace os2
