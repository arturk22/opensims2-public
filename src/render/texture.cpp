#include "texture.hpp"

#include <Metal/Metal.hpp>

namespace os2 {

MTL::Texture* uploadTexture(MTL::Device* device, const TextureLevels& t) {
    MTL::PixelFormat pf = MTL::PixelFormatRGBA8Unorm;
    size_t blockBytes = 0; // 0: uncompressed RGBA8
    if (device->supportsBCTextureCompression()) {
        switch (t.format) {
        case TexFormat::Dxt1: pf = MTL::PixelFormatBC1_RGBA, blockBytes = 8; break;
        case TexFormat::Dxt3: pf = MTL::PixelFormatBC2_RGBA, blockBytes = 16; break;
        case TexFormat::Dxt5: pf = MTL::PixelFormatBC3_RGBA, blockBytes = 16; break;
        default: break;
        }
    }

    const auto& top = t.levels.front();
    MTL::TextureDescriptor* desc =
        MTL::TextureDescriptor::texture2DDescriptor(pf, top.width, top.height, t.levels.size() > 1);
    desc->setMipmapLevelCount(t.levels.size());
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* tex = device->newTexture(desc);

    for (size_t i = 0; i < t.levels.size(); ++i) {
        const auto& l = t.levels[i];
        MTL::Region region = MTL::Region::Make2D(0, 0, l.width, l.height);
        if (blockBytes) {
            tex->replaceRegion(region, i, l.data.data(), ((l.width + 3) / 4) * blockBytes);
        } else {
            RgbaImage img = decodeTexture(t.format, l.width, l.height, l.data);
            tex->replaceRegion(region, i, img.pixels.data(), size_t(l.width) * 4);
        }
    }
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

} // namespace os2
