#pragma once

#include "rcol.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

// Pixel formats of cImageData (TXTR), numbered as stored in the file.
enum class TexFormat : uint32_t {
    Raw32 = 1,    // B,G,R,A
    Raw24 = 2,    // B,G,R
    Alpha8 = 3,   // 8-bit alpha (shadow and cut-out masks)
    Dxt1 = 4,
    Dxt3 = 5,
    Raw8 = 6,     // 8-bit luminance (bump maps)
    ExtRaw24 = 7, // B,G,R
    Dxt5 = 8,
    ExtRaw32 = 9, // B,G,R,A
};

const char* texFormatName(TexFormat f);

// Bytes a mip level of the given size occupies.
size_t texDataSize(TexFormat f, uint32_t width, uint32_t height);

struct RgbaImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels; // RGBA8, rows top to bottom
};

// Decodes one mip level to RGBA8. Throws if `data` has the wrong size.
RgbaImage decodeTexture(TexFormat f, uint32_t width, uint32_t height, std::span<const uint8_t> data);

// cImageData (TXTR). Usually one texture, six for cube maps. Mip levels are
// stored smallest first; each is either inline data or the name of a
// cLevelInfo (LIFO) resource holding the bytes.
struct ImageData {
    struct Mip {
        std::vector<uint8_t> data; // inline bytes, empty when lifoName is set
        std::string lifoName;
    };
    struct Texture {
        std::vector<Mip> mips;
        uint32_t creator = 0;
        float unknown = 0; // version >= 8
    };

    uint32_t version = 0;
    SgResource resource;
    uint32_t width = 0, height = 0;
    TexFormat format = TexFormat::Raw32;
    uint32_t mipLevels = 0;
    float unknown0 = 0;
    uint32_t unknown1 = 0;
    std::string fileNameRepeat; // version 9
    std::vector<Texture> textures;

    static ImageData read(BinReader& r);
    static ImageData parse(std::span<const uint8_t> body); // whole block body

    // Size of mip `i` of `count` (the last one is full size).
    uint32_t mipWidth(size_t i, size_t count) const;
    uint32_t mipHeight(size_t i, size_t count) const;
};

// cLevelInfo (LIFO): one mip level stored on its own.
struct LevelInfo {
    uint32_t version = 0;
    SgResource resource;
    uint32_t width = 0, height = 0;
    uint32_t zLevel = 0;
    std::vector<uint8_t> data;

    static LevelInfo read(BinReader& r);
    static LevelInfo parse(std::span<const uint8_t> body);
};

// Mip chain of a TXTR resource's first texture, as stored (still compressed),
// largest level first. LIFO references are resolved through `index` (when
// null, only textures with inline data can be loaded). A few LIFOs do not ship
// with the game; the chain then starts at the largest level that does.
struct TextureLevels {
    TexFormat format = TexFormat::Raw32;
    struct Level {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> data;
    };
    std::vector<Level> levels;
};
TextureLevels loadTextureLevels(std::span<const uint8_t> txtrResource, ResourceIndex* index);
// Every texture of a TXTR (six for an env cube: +X, -X, +Y, -Y, +Z, -Z in
// Direct3D order), each loaded like loadTextureLevels.
std::vector<TextureLevels> loadAllTextureLevels(std::span<const uint8_t> txtrResource, ResourceIndex* index);

// Largest loadable level of a TXTR resource, decoded.
RgbaImage loadTexture(std::span<const uint8_t> txtrResource, ResourceIndex* index);

// The image as PNG bytes (8-bit RGBA).
std::vector<uint8_t> encodePng(const RgbaImage& img);
void writePng(const std::filesystem::path& path, const RgbaImage& img);

} // namespace os2
