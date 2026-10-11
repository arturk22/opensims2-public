#pragma once

// Draws a movie frame fullscreen: black, with the picture scaled to fit the
// target and centred (letterboxed), in a pass of its own (its own command
// queue), for the app's window or an offscreen capture. Shaders are
// compiled at runtime, like the UI layer's.

#include <cstdint>

namespace MTL {
class CommandQueue;
class DepthStencilState;
class Device;
class RenderCommandEncoder;
class RenderPassDescriptor;
class RenderPipelineState;
class SamplerState;
class Texture;
} // namespace MTL
namespace CA {
class MetalDrawable;
}

namespace os2 {

struct RgbaImage;

class MovieLayer {
public:
    MovieLayer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    ~MovieLayer();
    MovieLayer(const MovieLayer&) = delete;
    MovieLayer& operator=(const MovieLayer&) = delete;

    // The picture to show (copied into the texture).
    void setFrame(const RgbaImage& img);
    // Encodes the black background and the picture into a pass.
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height);
    // Black bars over the top and bottom, each `share` of the height (a
    // cinematic's letterbox; 0.5 covers everything).
    void encodeLetterbox(MTL::RenderCommandEncoder* enc, float share);
    // A whole frame into a drawable (clears to black, presents).
    void draw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable);
    // Offscreen, read back as RGBA.
    RgbaImage capture(uint32_t width, uint32_t height);

private:
    MTL::Device* device_;
    MTL::CommandQueue* queue_ = nullptr;
    MTL::RenderPipelineState* pipeline_ = nullptr;
    MTL::DepthStencilState* noDepth_ = nullptr;
    MTL::SamplerState* sampler_ = nullptr;
    MTL::Texture* texture_ = nullptr;
    MTL::Texture* black_ = nullptr;
    uint32_t colorFormat_, depthFormat_;
    uint32_t frameW_ = 0, frameH_ = 0;
};

} // namespace os2
