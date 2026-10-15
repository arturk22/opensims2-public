#pragma once

// SDL_GPU (Direct3D 12 on Windows) backend for the lot the Mac renderer draws.
// The vertex layout and material flags follow src/render/renderer.cpp.

#include "formats/image.hpp"
#include "formats/lighting_config.hpp"
#include "formats/model.hpp"

#include "render/particle_system.hpp"

#include <filesystem>
#include <simd/simd.h>

#include <cstdint>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

class GpuRenderer {
public:
    GpuRenderer(const std::filesystem::path& shaderFolder, int width, int height);
    ~GpuRenderer();
    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;

    int addObject(const ObjectModel& model, ResourceIndex& index, bool dynamic = false);
    void updateObject(int model, const ObjectModel& posed);
    int addInstance(int model, const simd_float4x4& transform);
    void setInstanceTransform(int instance, const simd_float4x4& transform);
    void setLightMap(const RgbaImage& image);
    void setGroundClip(float z);
    // The game's outdoor lights (Renderer::outdoorLights): set 0 for objects,
    // set 1 for Sims. Directions point toward the light.
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

    // Returns false when the window was closed.
    bool draw(const simd_float4x4& viewProj);
    bool capture(const std::filesystem::path& path, const simd_float4x4& viewProj);
    // A .movie frame the way MovieLayer draws it: black, fitted, centred.
    bool captureMovie(const std::filesystem::path& path, const RgbaImage& frame);

    std::string backend() const;

private:
    struct Gpu;

    Gpu* gpu_ = nullptr;
};

} // namespace os2
