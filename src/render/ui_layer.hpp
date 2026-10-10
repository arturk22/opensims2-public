#pragma once

#include <cstdint>
#include <map>
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

struct RgbaImage;
struct UiScreen;

// 2D overlay drawn after the 3D scene in the same render pass: textured,
// alpha-blended quads in an orthographic projection of the UI canvas (pixels,
// origin top-left), scaled to fit the drawable and centred (letterboxed).
// No depth test or writes. Shaders are compiled at runtime.
class UiLayer {
public:
    UiLayer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    ~UiLayer();
    UiLayer(const UiLayer&) = delete;
    UiLayer& operator=(const UiLayer&) = delete;

    // Uploads the screen's images and quads; the screen's images must stay
    // alive only for this call. Replaces the previous screen, so it must not
    // be called while frames using the old one are in flight.
    void setScreen(const UiScreen& screen);

    // Encodes the quads into a pass whose colour target is width x height.
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height);

private:
    struct Batch {
        MTL::Texture* texture; // not owned (textures_ or white_)
        uint32_t first, count; // vertices
    };

    MTL::Texture* upload(const RgbaImage& img);
    void clear();

    MTL::Device* device_;
    MTL::RenderPipelineState* pipeline_ = nullptr;
    MTL::DepthStencilState* noDepth_ = nullptr;
    MTL::SamplerState* sampler_ = nullptr;
    MTL::Texture* white_ = nullptr;
    MTL::Buffer* vertices_ = nullptr;
    std::map<const RgbaImage*, MTL::Texture*> textures_;
    std::vector<Batch> batches_;
    float canvasWidth_ = 0, canvasHeight_ = 0;
};

} // namespace os2
