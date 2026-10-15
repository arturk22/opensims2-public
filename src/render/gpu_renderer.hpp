#pragma once

// SDL_GPU (Direct3D 12 on Windows) backend for the lot the Mac renderer draws.
// The vertex layout and material flags follow src/render/renderer.cpp.

#include "formats/image.hpp"
#include "formats/lighting_config.hpp"
#include "formats/model.hpp"

#include "render/particle_system.hpp"

#include <filesystem>
#include <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace os2 {

class ResourceIndex;

// A camera's view and projection (as render/renderer.hpp's).
struct FrameView {
    simd_float4x4 view;
    simd_float4x4 projection;
};

class GpuRenderer {
public:
    // Draws into `window` when given (the host keeps it across scenes), else opens its own.
    GpuRenderer(const std::filesystem::path& shaderFolder, int width, int height, SDL_Window* window = nullptr);
    ~GpuRenderer();
    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;

    int addObject(const ObjectModel& model, ResourceIndex& index, bool dynamic = false);
    void updateObject(int model, const ObjectModel& posed);
    // Uploads `replacement` in place of a model (the lot after Build mode changed it).
    void replaceObject(int model, const ObjectModel& replacement, ResourceIndex& index);
    int addInstance(int model, const simd_float4x4& transform);
    void setInstanceTransform(int instance, const simd_float4x4& transform);
    void setInstanceModel(int instance, int model);
    int instanceModel(int instance) const;
    // Every instance of model `from` draws model `to` (a Sim's new outfit).
    void retargetInstances(int from, int to);
    // One light map in slot 0 (no crossfade).
    void setLightMap(const RgbaImage& image);
    // The lot's two light-map slots, drawn as mix(slot 0, slot 1, mix) (Renderer::setLightMap).
    void setLightMap(int slot, const RgbaImage& image);
    void setLightMapMix(float mix);
    void setEnvironmentLight(simd_float3 rgb);
    void setGroundClip(float z);
    // No test scene is drawn here (Renderer::hideTestScene).
    void hideTestScene() {}

    // Lights for groups of instances, as Renderer::LightSet: an ambient and
    // up to 16 directional, point and spot lights; set 0 is the default.
    struct LightSet {
        struct Directional {
            simd_float3 direction; // unit, towards the light
            simd_float3 color;     // times intensity
            float specular = 1;
        };
        struct Point {
            simd_float3 position;
            simd_float3 color;
            float falloff = 1, range = 0;
            float specular = 1;
            simd_float3 spotDirection{0, 0, -1};
            float cosOuter = -2, cosInner = -1; // cosOuter < -1: a point light
        };
        simd_float3 ambient{0, 0, 0};
        std::vector<Directional> lights;
        std::vector<Point> points;
    };
    static constexpr size_t kMaxLightSets = 256;
    static simd_float2 spotCosines(float angle, float penumbra) {
        angle = std::fabs(angle), penumbra = std::fabs(penumbra);
        const float outer = std::min(angle + penumbra, 3.14159265f);
        return simd_make_float2(std::cos(outer / 2), std::cos(std::min(angle, outer) / 2));
    }
    void setLightSets(std::vector<LightSet> sets);
    const std::vector<LightSet>& lightSets() const;
    static LightSet outdoorLights(const LightingState& state, bool sims);
    int instanceOf(int model) const;
    // The game's outdoor lights (outdoorLights): set 0 for objects, set 1
    // for Sims. Directions point toward the light.
    void setLighting(const LightingState& state);
    int objectLightCount() const;
    int simLightCount() const;
    void setInstanceLightSet(int instance, int set);
    void setEye(simd_float3 eye);
    // A Sim's shadow (nTSSG::cShadow): the silhouette is drawn into an atlas
    // from the light and multiplied onto upward floors.
    struct ShadowCaster {
        int instance = -1;
        simd_float3 feet{0, 0, 0};
        float heading = 0;
        simd_float3 direction{0, 0, -1};
        bool outside = true;
        simd_float3 color{0.5f, 0.5f, 0.55f};
    };
    void setShadowCasters(const std::vector<ShadowCaster>& casters);
    int shadowCount() const;
    // The first mirror in the scene, after a draw: its centre and the normal
    // facing the eye. False when the lot has none.
    bool mirrorAnchor(simd_float3& center, simd_float3& normal) const;
    // Nude-Sim mosaic (UpdateCensor boxes). Empty: nothing is pixelated.
    struct CensorBox {
        simd_float3 lo{0, 0, 0};
        simd_float3 hi{0, 0, 0};
    };
    void setCensorBoxes(const std::vector<CensorBox>& boxes);
    int censorCount() const;
    // Effect particles (fx::EffectSystem::collect), camera-facing quads.
    void setParticleQuads(const std::vector<fx::ParticleQuad>& quads, simd_float3 right, simd_float3 up, ResourceIndex* index);
    size_t particleCount() const;
    // A real UI image, in window pixels from the top left.
    void setUiImage(const RgbaImage& image, float x, float y, float w, float h);

    // The Mac renderer's UI overlay hook. Here the host composes the UI into setUiImage and
    // offscreen captures never draw it, so the hook is only kept for code that saves and restores it.
    using Overlay = std::function<void()>;
    Overlay overlay() const { return overlay_; }
    void setOverlay(Overlay overlay) { overlay_ = std::move(overlay); }
    // The window's background (the sky blue of the lot unless set).
    void setClearColor(simd_float4 color);
    SDL_Window* window() const;
    // Returns false when the window was closed.
    bool draw(const simd_float4x4& viewProj);
    bool capture(const std::filesystem::path& path, const simd_float4x4& viewProj);
    // Renders one frame offscreen without the UI and reads it back (Renderer::capture).
    RgbaImage capture(uint32_t width, uint32_t height, const FrameView& view, simd_float4 clearColor);
    // A .movie frame the way MovieLayer draws it: black, fitted, centred.
    bool captureMovie(const std::filesystem::path& path, const RgbaImage& frame);

    std::string backend() const;

private:
    struct Gpu;

    Gpu* gpu_ = nullptr;
    Overlay overlay_;
};

// The name the shared app modules (VmObjectView, BuyMode) take the renderer by.
class Renderer final : public GpuRenderer {
public:
    using GpuRenderer::GpuRenderer;
};

} // namespace os2
