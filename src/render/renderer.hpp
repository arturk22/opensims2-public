#pragma once

#include <simd/simd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "formats/model.hpp"

namespace MTL {
class Buffer;
class CommandBuffer;
class CommandQueue;
class DepthStencilState;
class Device;
class RenderPassDescriptor;
class RenderPipelineState;
class SamplerState;
class Texture;
} // namespace MTL
namespace CA {
class MetalDrawable;
}

namespace os2 {

class ResourceIndex;
struct LightingState;
struct RgbaImage;

struct FrameView {
    simd_float4x4 view;
    simd_float4x4 projection;
};

// Metal renderer behind the app's MTKView, in game space (Z up, 1 unit per
// tile). Objects are uploaded once (meshes, materials, textures) and drawn as
// instances; a checkered test floor with axis markers is always present.
class Renderer {
public:
    // Pixel formats are MTL::PixelFormat values, matching the view's.
    Renderer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Uploads an object's parts; base textures are loaded through `index`.
    // Returns the model id for addInstance.
    // `dynamic` models keep one vertex buffer per frame in flight so that
    // updateObject can replace their vertices every frame (skinned Sims).
    int addObject(const ObjectModel& model, ResourceIndex& index, bool dynamic = false);
    // New positions and normals for a dynamic model: `posed` must have the
    // parts (and vertex counts) of the model it was added from. Takes effect
    // from the next frame drawn.
    void updateObject(int model, const ObjectModel& posed);
    void addInstance(int model, const simd_float4x4& transform);
    // Removes the checkered test floor and axis markers (e.g. when a lot is shown).
    void hideTestScene();

    // Outdoor direct lighting from one of the game's lighting states:
    // skylight as ambient plus its object lights (see LightingState).
    void setLighting(const LightingState& state);

    // Clock for texture coordinate animations, in seconds; until it is set
    // (or after setting it negative) the renderer uses its own running time.
    void setTime(double seconds) { time_ = seconds; }

    // Encodes and commits one frame; `pass` and `drawable` come from the view.
    void draw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable, const FrameView& view);

    // Renders one frame offscreen and reads it back (waits for the GPU).
    RgbaImage capture(uint32_t width, uint32_t height, const FrameView& view, simd_float4 clearColor);

private:
    struct Material;
    struct Part;
    struct Model;

    // A TXTR by name, cached; `cube` loads its six textures as a cube map.
    MTL::Texture* texture(const std::string& name, ResourceIndex& index, bool cube = false);
    Material material(const StandardMaterial& sm, ResourceIndex& index);
    MTL::Texture* uploadMask(const ObjectModel::AlphaMask& mask);
    // Waits for a frame slot, encodes the scene into `pass` and returns the
    // (uncommitted) command buffer; the slot is freed when it completes.
    MTL::CommandBuffer* encodeFrame(MTL::RenderPassDescriptor* pass, const FrameView& view);
    int addBox(simd_float3 base, simd_float3 size, simd_float3 rgb, uint32_t flags);

    static constexpr int kFramesInFlight = 3;

    MTL::Device* device_;
    uint32_t colorFormat_, depthFormat_;
    MTL::CommandQueue* queue_ = nullptr;
    MTL::RenderPipelineState* opaque_ = nullptr;
    MTL::RenderPipelineState* alphaBlend_ = nullptr;
    MTL::RenderPipelineState* additive_ = nullptr;
    MTL::DepthStencilState* depthWrite_ = nullptr;
    MTL::DepthStencilState* depthRead_ = nullptr; // blended parts test but do not write
    MTL::SamplerState* sampler_ = nullptr;
    MTL::SamplerState* clampSampler_ = nullptr; // like sampler_, clamped
    MTL::SamplerState* maskSampler_ = nullptr;  // clamped, no mips
    MTL::SamplerState* cubeSampler_ = nullptr;  // clamped, mipmapped
    MTL::SamplerState* pointSampler_ = nullptr; // clamped, nearest (cel shading ramps)
    MTL::Texture* white_ = nullptr;
    MTL::Texture* whiteCube_ = nullptr;
    std::map<std::string, MTL::Texture*> textures_; // by TXTR name; null if it failed to load
    std::vector<std::unique_ptr<Model>> models_;
    struct Instance {
        int model;
        simd_float4x4 transform;
    };
    std::vector<Instance> instances_;
    size_t testInstances_ = 0; // the first instances_ are the test scene
    struct DirLight {
        simd_float3 direction; // unit, towards the light
        simd_float3 color;     // times intensity
    };
    simd_float3 ambient_{0.45f, 0.45f, 0.45f};
    std::vector<DirLight> lights_;
    std::array<MTL::Buffer*, kFramesInFlight> frames_{};
    void* inFlight_; // dispatch_semaphore_t, opaque so ARC in the app shell leaves it alone
    int frame_ = 0;
    double time_ = -1;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

} // namespace os2
