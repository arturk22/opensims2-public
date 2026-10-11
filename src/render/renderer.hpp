#pragma once

#include <simd/simd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
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
class RenderCommandEncoder;
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
    // Returns the instance's index (for setInstanceTransform), or -1.
    int addInstance(int model, const simd_float4x4& transform);
    // Moves an instance (the plumbob over the selected Sim).
    void setInstanceTransform(int instance, const simd_float4x4& transform);
    // Draws an instance with another model (an object whose material the VM changed).
    void setInstanceModel(int instance, int model);
    // Points the instances of `from` at model `to` (a Sim changing clothes).
    void retargetInstances(int from, int to);
    // Removes the checkered test floor and axis markers (e.g. when a lot is shown).
    void hideTestScene();

    // Lights for groups of instances (light set 0 is the default): an
    // ambient colour and up to 8 directional lights, as the game gives each
    // object the lights of its room and group (docs/lighting-mac.md).
    struct LightSet {
        struct Directional {
            simd_float3 direction; // unit, towards the light
            simd_float3 color;     // times intensity
            float specular = 1;    // relative specular scale (roomSpecular for room lights)
        };
        simd_float3 ambient{0, 0, 0};
        std::vector<Directional> lights;
    };
    static constexpr size_t kMaxLightSets = 256;
    // Replaces every light set (set 0 included).
    void setLightSets(std::vector<LightSet> sets);
    void setInstanceLightSet(int instance, int set);
    // The first instance of a model, or -1 (a Sim's actor has one).
    int instanceOf(int model) const;

    // Outdoor direct lighting from one of the game's lighting states, as
    // light sets 0 (objects) and 1 (Sims): see outdoorLights.
    void setLighting(const LightingState& state);
    // The game's outdoor lights of a state (docs/lighting-mac.md 3): the
    // skylight (colour x strength) as ambient, and each light's colour
    // minus the skylight, times its intensity. simFill lights only light
    // Sims; when there are any, fill1/fill2 do not light Sims.
    static LightSet outdoorLights(const LightingState& state, bool sims);

    // The lot's light maps (an atlas laid out as LotLightMapLayout, RGBA8):
    // walls, floors and terrain are drawn as 2 x texture x light map. Two
    // slots crossfade on a lighting state change (the game's LerpLightMaps):
    // the shaders show mix(slot 0, slot 1, mix). Until set, the light map is
    // 0.5 everywhere (textures as they are).
    void setLightMap(int slot, const RgbaImage& image);
    void setLightMapMix(float mix) { lightMapMix_ = mix; }
    // The state's "environment" light (colour x strength): env cube
    // reflections are multiplied by it (allTextureLights), dimming them at night.
    void setEnvironmentLight(simd_float3 rgb) { environment_ = rgb; }

    // Clock for texture coordinate animations, in seconds; until it is set
    // (or after setting it negative) the renderer uses its own running time.
    void setTime(double seconds) { time_ = seconds; }
    // Called at the end of every frame's render pass, after the scene, with the
    // pass's encoder and colour target size: 2D overlays such as the UI.
    using Overlay = std::function<void(MTL::RenderCommandEncoder*, uint32_t width, uint32_t height)>;
    void setOverlay(Overlay overlay) { overlay_ = std::move(overlay); }

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
    MTL::Texture* rgbaTexture(const RgbaImage& image); // retained
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
    MTL::RenderPipelineState* addOne_ = nullptr; // one + one (addNoSrcAlphaScale)
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
        int lightSet = 0;
    };
    std::vector<Instance> instances_;
    size_t testInstances_ = 0; // the first instances_ are the test scene
    std::vector<LightSet> lightSets_;
    std::array<MTL::Buffer*, kFramesInFlight> lightBuffers_{};
    std::array<MTL::Texture*, 2> lightMaps_{}; // owned, null until set
    float lightMapMix_ = 0;
    simd_float3 environment_{1, 1, 1};
    // Light maps replaced while frames in flight may still read them: released
    // once kFramesInFlight frames have been encoded since.
    std::vector<std::pair<MTL::Texture*, int>> retired_;
    MTL::Texture* grey_ = nullptr; // 0.5 everywhere: the light map until one is set
    std::array<MTL::Buffer*, kFramesInFlight> frames_{};
    void* inFlight_; // dispatch_semaphore_t, opaque so ARC in the app shell leaves it alone
    int frame_ = 0;
    double time_ = -1;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    Overlay overlay_;
};

} // namespace os2
