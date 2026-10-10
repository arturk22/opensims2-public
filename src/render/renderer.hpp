#pragma once

#include <array>
#include <cstdint>

namespace MTL {
class Buffer;
class CommandQueue;
class DepthStencilState;
class Device;
class RenderPassDescriptor;
class RenderPipelineState;
} // namespace MTL
namespace CA {
class MetalDrawable;
}

namespace os2 {

// Metal renderer behind the app's MTKView. For now it draws a spinning cube to
// prove the pipeline (runtime-compiled shaders, uniforms, depth); objects and
// lots will replace it.
class Renderer {
public:
    // Pixel formats are MTL::PixelFormat values, matching the view's.
    Renderer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void resize(double width, double height);
    // Encodes and commits one frame; `pass` and `drawable` come from the view.
    void draw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable, double dt);

private:
    static constexpr int kFramesInFlight = 3;

    MTL::Device* device_;
    MTL::CommandQueue* queue_ = nullptr;
    MTL::RenderPipelineState* pipeline_ = nullptr;
    MTL::DepthStencilState* depth_ = nullptr;
    MTL::Buffer* vertices_ = nullptr;
    MTL::Buffer* indices_ = nullptr;
    std::array<MTL::Buffer*, kFramesInFlight> uniforms_{};
    void* inFlight_; // dispatch_semaphore_t, opaque so ARC in the app shell leaves it alone
    int frame_ = 0;
    float aspect_ = 1.0f;
    double time_ = 0.0;
};

} // namespace os2
