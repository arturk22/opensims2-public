#pragma once

// Draws EffectSystem particles (render/particle_system.hpp) in the 3D pass,
// after the scene: textured quads with the effect's blending (alpha,
// additive, modulate) and depth mode, blended ones back to front. Shaders
// are compiled at runtime. The renderer calls encode from its scene hook.

#include "render/particle_system.hpp"

#include <simd/simd.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace MTL {
class Buffer;
class DepthStencilState;
class Device;
class RenderCommandEncoder;
class RenderPipelineState;
class SamplerState;
class Texture;
} // namespace MTL

namespace os2 {

class ResourceIndex;
struct RgbaImage;

class ParticleLayer {
public:
    // Textures are TXTR "<name>_txtr" resources found through `index`.
    ParticleLayer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat, ResourceIndex* index);
    ~ParticleLayer();
    ParticleLayer(const ParticleLayer&) = delete;
    ParticleLayer& operator=(const ParticleLayer&) = delete;

    // The particles to draw from the next frame on (copied).
    void setQuads(const std::vector<fx::ParticleQuad>& quads);
    // Encodes them; `view` is the camera's view matrix (billboards face it).
    void encode(MTL::RenderCommandEncoder* enc, const simd_float4x4& view, const simd_float4x4& viewProj);

    // A texture by effect name, cached (white when it cannot be loaded).
    MTL::Texture* texture(const std::string& name);
    // Registers an image under a name (balloon icons from object thumbnails).
    void setImage(const std::string& name, const RgbaImage& image);
    bool hasTexture(const std::string& name) const { return textures_.count(name) != 0; }

private:
    struct Quad {
        fx::ParticleQuad q;
        std::string texture;
    };
    static constexpr int kRing = 4; // > the renderer's frames in flight

    MTL::Device* device_;
    ResourceIndex* index_;
    std::array<MTL::RenderPipelineState*, 3> pipelines_{}; // alpha, additive, modulate
    std::array<MTL::DepthStencilState*, 3> depth_{};       // test, none, inverted
    MTL::SamplerState* sampler_ = nullptr;
    MTL::Texture* white_ = nullptr;
    std::array<MTL::Buffer*, kRing> buffers_{};
    int ring_ = 0;
    std::map<std::string, MTL::Texture*> textures_;
    std::vector<Quad> quads_;
};

} // namespace os2
