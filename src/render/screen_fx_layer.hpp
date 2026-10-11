#pragma once

// Full-screen effects drawn over the scene at the end of a frame (plain C++
// header; Metal in the .cpp, shaders compiled at runtime): what a
// cinematic's "postprocessingmaterial" and the effects' screen blocks ask
// for (docs/cinematics-mac.md, docs/effects-mac.md):
//
// - a snapshot crossfade (screen mode 4, "crossFadeSnap*": the last frame of
//   the previous shot over the new one, fading out),
// - a flat colour over everything (the other screen effects: flashes, fades),
// - the letterbox bars, a vignette, film grain and a cheap bloom (the scene
//   scaled by the bloom's w plus its rgb as a brightening; the game blurs,
//   inferred and simplified).

#include <array>
#include <cstdint>

namespace MTL {
class DepthStencilState;
class Device;
class RenderCommandEncoder;
class RenderPipelineState;
class SamplerState;
class Texture;
} // namespace MTL

namespace os2 {

struct RgbaImage;

class ScreenFxLayer {
public:
    ScreenFxLayer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    ~ScreenFxLayer();
    ScreenFxLayer(const ScreenFxLayer&) = delete;
    ScreenFxLayer& operator=(const ScreenFxLayer&) = delete;

    struct Params {
        float letterbox = 0;                   // each bar's share of the height
        float snapshotAlpha = 0;               // the snapshot over the scene
        std::array<float, 4> flash{0, 0, 0, 0}; // rgb, alpha
        std::array<float, 3> vignette{0.5f, 0.5f, 0}; // centre x, y, strength
        float grain = 0;                       // noise amplitude
        std::array<float, 4> bloom{0, 0, 0, 1}; // rgb added, w scales the scene
        float time = 0;                        // seconds (grain animation)
    };
    // The frame to crossfade from (copied).
    void setSnapshot(const RgbaImage& image);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, const Params& p);

private:
    MTL::Device* device_;
    MTL::RenderPipelineState* blend_ = nullptr;    // alpha blend
    MTL::RenderPipelineState* multiply_ = nullptr; // dst * src
    MTL::RenderPipelineState* screen_ = nullptr;   // dst + src * dst (brighten)
    MTL::DepthStencilState* noDepth_ = nullptr;
    MTL::SamplerState* sampler_ = nullptr;
    MTL::Texture* snapshot_ = nullptr;
    MTL::Texture* white_ = nullptr;
    uint32_t snapW_ = 0, snapH_ = 0;
};

} // namespace os2
