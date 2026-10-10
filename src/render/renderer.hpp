#pragma once

#include <simd/simd.h>

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

struct FrameView {
    simd_float4x4 view;
    simd_float4x4 projection;
};

// Metal renderer behind the app's MTKView. For now it draws a test scene in
// game space (Z up, 1 unit per tile): a checkered floor, a few blocks and axis
// markers, to check the camera against; lots and objects will replace it.
class Renderer {
public:
    // Pixel formats are MTL::PixelFormat values, matching the view's.
    Renderer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Encodes and commits one frame; `pass` and `drawable` come from the view.
    void draw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable, const FrameView& view);

private:
    static constexpr int kFramesInFlight = 3;

    MTL::Device* device_;
    MTL::CommandQueue* queue_ = nullptr;
    MTL::RenderPipelineState* pipeline_ = nullptr;
    MTL::DepthStencilState* depth_ = nullptr;
    MTL::Buffer* vertices_ = nullptr;
    MTL::Buffer* indices_ = nullptr;
    MTL::Buffer* instances_ = nullptr;
    uint32_t instanceCount_ = 0;
    std::array<MTL::Buffer*, kFramesInFlight> frames_{};
    void* inFlight_; // dispatch_semaphore_t, opaque so ARC in the app shell leaves it alone
    int frame_ = 0;
};

} // namespace os2
