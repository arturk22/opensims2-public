#pragma once

// Draws EffectSystem particles (render/particle_system.hpp) in the 3D pass:
// textured quads with the effect's blending (alpha, additive, modulate) and
// depth mode. Each emitter's particles are a group the renderer sorts among
// its blended parts (by key and depth, as the game's display list does) and
// draws with drawGroup. Shaders are compiled at runtime.

#include "render/particle_system.hpp"

#include <simd/simd.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
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
    // A group to sort: its key (layer x 8 + 7) and depth, the z / w of the
    // centre of its particles' box (cDisplayList::AddGeometry with the flag
    // 0x40, BV 0x86ed4).
    struct Group {
        int layer = 7;
        float depth = 0;
        int id = 0;
    };
    // Builds this frame's vertices (`view`: the camera's view matrix, the
    // billboards face it) and lists the groups.
    void prepare(const simd_float4x4& view, const simd_float4x4& viewProj, std::vector<Group>& out);
    // Encodes one prepared group.
    void drawGroup(MTL::RenderCommandEncoder* enc, int id);

    // A texture by effect name, cached (white when it cannot be loaded).
    MTL::Texture* texture(const std::string& name);
    // A particle "texture" that names a shape ("<cres>_<shape>_shpe": the
    // clothes-change swirl, the hearts, the steam puffs) draws that SHPE's
    // mesh at each particle instead of a quad; null for other names or a
    // shape that cannot be loaded (cached).
    struct Mesh {
        std::vector<simd_float3> positions; // model space (z up), by triangle corner
        std::vector<simd_float2> uvs;
        MTL::Texture* texture = nullptr;
        simd_float3 tint{1, 1, 1}; // the material's diffuse when untextured
    };
    const Mesh* mesh(const std::string& name);
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
    std::map<std::string, std::unique_ptr<Mesh>> meshes_;
    std::vector<Quad> quads_;
    // This frame's batches, by group.
    struct Batch {
        MTL::Texture* tex;
        int pipeline, depth;
        uint32_t first, count;
    };
    std::vector<Batch> batches_;
    std::vector<std::pair<size_t, size_t>> groupBatches_; // first, count in batches_
    MTL::Buffer* frameBuffer_ = nullptr;
    simd_float4x4 viewProj_{};
};

} // namespace os2
