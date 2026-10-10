#include "texture.hpp"

#include <Metal/Metal.hpp>

#include <algorithm>

namespace os2 {

namespace {

// Pixel format for `f` and its BC block size in bytes (0: decoded to RGBA8).
std::pair<MTL::PixelFormat, size_t> gpuFormat(MTL::Device* device, TexFormat f) {
    if (device->supportsBCTextureCompression()) {
        switch (f) {
        case TexFormat::Dxt1: return {MTL::PixelFormatBC1_RGBA, 8};
        case TexFormat::Dxt3: return {MTL::PixelFormatBC2_RGBA, 16};
        case TexFormat::Dxt5: return {MTL::PixelFormatBC3_RGBA, 16};
        default: break;
        }
    }
    return {MTL::PixelFormatRGBA8Unorm, 0};
}

void uploadLevels(MTL::Texture* tex, const TextureLevels& t, size_t blockBytes, size_t slice, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const auto& l = t.levels[i];
        MTL::Region region = MTL::Region::Make2D(0, 0, l.width, l.height);
        if (blockBytes) {
            const size_t row = ((l.width + 3) / 4) * blockBytes;
            tex->replaceRegion(region, i, slice, l.data.data(), row, row * ((l.height + 3) / 4));
        } else {
            RgbaImage img = decodeTexture(t.format, l.width, l.height, l.data);
            tex->replaceRegion(region, i, slice, img.pixels.data(), size_t(l.width) * 4, img.pixels.size());
        }
    }
}

} // namespace

MTL::Texture* uploadTexture(MTL::Device* device, const TextureLevels& t) {
    const auto [pf, blockBytes] = gpuFormat(device, t.format);
    const auto& top = t.levels.front();
    MTL::TextureDescriptor* desc =
        MTL::TextureDescriptor::texture2DDescriptor(pf, top.width, top.height, t.levels.size() > 1);
    desc->setMipmapLevelCount(t.levels.size());
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* tex = device->newTexture(desc);
    uploadLevels(tex, t, blockBytes, 0, t.levels.size());
    return tex;
}

MTL::Texture* uploadCubeTexture(MTL::Device* device, const std::vector<TextureLevels>& faces) {
    const auto [pf, blockBytes] = gpuFormat(device, faces.front().format);
    // Faces share one size and level count: keep the levels all of them have.
    size_t levels = faces.front().levels.size();
    for (const auto& f : faces)
        levels = std::min(levels, f.levels.size());
    const auto& top = faces.front().levels.front();
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::textureCubeDescriptor(pf, top.width, levels > 1);
    desc->setMipmapLevelCount(levels);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* tex = device->newTexture(desc);
    for (size_t i = 0; i < 6 && i < faces.size(); ++i)
        uploadLevels(tex, faces[i], gpuFormat(device, faces[i].format).second, i, levels);
    return tex;
}

MTL::Texture* whiteTexture(MTL::Device* device) {
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, 1, 1, false);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* tex = device->newTexture(desc);
    const uint8_t white[4] = {255, 255, 255, 255};
    tex->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, white, 4);
    return tex;
}

MTL::Texture* whiteCubeTexture(MTL::Device* device) {
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::textureCubeDescriptor(MTL::PixelFormatRGBA8Unorm, 1, false);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* tex = device->newTexture(desc);
    const uint8_t white[4] = {255, 255, 255, 255};
    for (NS::UInteger face = 0; face < 6; ++face)
        tex->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, face, white, 4, 4);
    return tex;
}

} // namespace os2
