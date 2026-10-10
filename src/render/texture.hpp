#pragma once

#include "formats/image.hpp"

#include <vector>

namespace MTL {
class Device;
class Texture;
} // namespace MTL

namespace os2 {

// Creates a GPU texture with the whole mip chain. DXT1/3/5 go up as BC1/2/3
// blocks when the device supports them; everything else is decoded to RGBA8.
// Textures are not sRGB: the game lit and blended in gamma space, so the
// renderer does too. Returns a retained texture.
MTL::Texture* uploadTexture(MTL::Device* device, const TextureLevels& levels);

// Cube map from the six textures of an env cube TXTR (+X, -X, +Y, -Y, +Z, -Z;
// Metal samples cube maps with Direct3D's face layout). Returns a retained texture.
MTL::Texture* uploadCubeTexture(MTL::Device* device, const std::vector<TextureLevels>& faces);

// 1x1 opaque white, for untextured materials.
MTL::Texture* whiteTexture(MTL::Device* device);
MTL::Texture* whiteCubeTexture(MTL::Device* device);

} // namespace os2
