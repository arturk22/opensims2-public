#include "renderer.hpp"

#include "shaders_src.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <dispatch/dispatch.h>
#include <simd/simd.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace os2 {

namespace {

struct Vertex {
    float position[3];
    float normal[3];
};

struct Instance {
    simd_float4x4 model;
    simd_float4 tint;
};

struct Frame {
    simd_float4x4 viewProj;
    simd_float4 lightDir;
};

// Unit cube [0,1]^3 scaled to `size` with its base centered on `base`.
simd_float4x4 box(simd_float3 base, simd_float3 size) {
    return simd_matrix(simd_make_float4(size.x, 0, 0, 0), simd_make_float4(0, size.y, 0, 0),
                       simd_make_float4(0, 0, size.z, 0),
                       simd_make_float4(base.x - size.x * 0.5f, base.y - size.y * 0.5f, base.z, 1));
}

std::vector<Instance> testScene() {
    std::vector<Instance> s;
    struct V3 {
        float x, y, z;
    };
    auto add = [&](V3 base, V3 size, V3 rgb, bool checker = false) {
        s.push_back({box(simd_make_float3(base.x, base.y, base.z), simd_make_float3(size.x, size.y, size.z)),
                     simd_make_float4(rgb.x, rgb.y, rgb.z, checker ? 1.0f : 0.0f)});
    };
    // 32x32 tiles of floor, top surface at z = 0.
    add({0, 0, -0.05f}, {32, 32, 0.05f}, {0.55f, 0.62f, 0.45f}, true);
    // Axis markers: +X red, +Y green, origin white.
    for (int i = 1; i <= 4; ++i) {
        add({float(i) + 0.5f, 0.5f, 0}, {0.6f, 0.6f, 0.15f}, {0.85f, 0.25f, 0.2f});
        add({0.5f, float(i) + 0.5f, 0}, {0.6f, 0.6f, 0.15f}, {0.25f, 0.7f, 0.3f});
    }
    add({0.5f, 0.5f, 0}, {0.6f, 0.6f, 0.6f}, {0.95f, 0.95f, 0.95f});
    // Blocks of 1, 2 and 3 m (wall height is 3 tiles) to judge scale and depth.
    add({-4.5f, -3.5f, 0}, {1, 1, 1}, {0.3f, 0.45f, 0.85f});
    add({-6.5f, 2.5f, 0}, {1, 2, 2}, {0.85f, 0.7f, 0.25f});
    add({5.5f, -6.5f, 0}, {4, 0.2f, 3}, {0.8f, 0.78f, 0.72f});
    return s;
}

std::string errorText(NS::Error* error) {
    if (!error || !error->localizedDescription())
        return "unknown error";
    return error->localizedDescription()->utf8String();
}

// Unit cube [0,1]^3, four vertices per face so normals stay flat.
void buildCube(std::array<Vertex, 24>& v, std::array<uint16_t, 36>& idx) {
    static const float faces[6][4][3] = {
        {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}, // +X
        {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}}, // -X
        {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}, // +Y
        {{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}, // -Y
        {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}, // +Z
        {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}, // -Z
    };
    static const float normals[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        for (int c = 0; c < 4; ++c) {
            Vertex& out = v[f * 4 + c];
            for (int k = 0; k < 3; ++k) {
                out.position[k] = faces[f][c][k];
                out.normal[k] = normals[f][k];
            }
        }
        const uint16_t base = uint16_t(f * 4);
        const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i)
            idx[f * 6 + i] = uint16_t(base + quad[i]);
    }
}

dispatch_semaphore_t sema(void* p) { return static_cast<dispatch_semaphore_t>(p); }

} // namespace

Renderer::Renderer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat)
    : device_(device->retain()), inFlight_(dispatch_semaphore_create(kFramesInFlight)) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    queue_ = device_->newCommandQueue();

    NS::Error* error = nullptr;
    MTL::Library* library = device_->newLibrary(
        NS::String::string(kShaderSource, NS::UTF8StringEncoding), nullptr, &error);
    if (!library) {
        std::string msg = "shader compilation failed: " + errorText(error);
        pool->release();
        throw std::runtime_error(msg);
    }
    MTL::Function* vs = library->newFunction(NS::String::string("vertex_main", NS::UTF8StringEncoding));
    MTL::Function* fs = library->newFunction(NS::String::string("fragment_main", NS::UTF8StringEncoding));

    MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vs);
    desc->setFragmentFunction(fs);
    desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormat(colorFormat));
    desc->setDepthAttachmentPixelFormat(MTL::PixelFormat(depthFormat));
    pipeline_ = device_->newRenderPipelineState(desc, &error);
    desc->release();
    vs->release();
    fs->release();
    library->release();
    if (!pipeline_) {
        std::string msg = "pipeline creation failed: " + errorText(error);
        pool->release();
        throw std::runtime_error(msg);
    }

    MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
    dsd->setDepthCompareFunction(MTL::CompareFunctionLess);
    dsd->setDepthWriteEnabled(true);
    depth_ = device_->newDepthStencilState(dsd);
    dsd->release();

    std::array<Vertex, 24> verts;
    std::array<uint16_t, 36> idx;
    buildCube(verts, idx);
    vertices_ = device_->newBuffer(verts.data(), sizeof(verts), MTL::ResourceStorageModeShared);
    indices_ = device_->newBuffer(idx.data(), sizeof(idx), MTL::ResourceStorageModeShared);
    std::vector<Instance> scene = testScene();
    instanceCount_ = uint32_t(scene.size());
    instances_ = device_->newBuffer(scene.data(), scene.size() * sizeof(Instance), MTL::ResourceStorageModeShared);
    for (auto& f : frames_)
        f = device_->newBuffer(sizeof(Frame), MTL::ResourceStorageModeShared);

    pool->release();
}

Renderer::~Renderer() {
    // Let in-flight frames finish before their buffers go away.
    for (int i = 0; i < kFramesInFlight; ++i)
        dispatch_semaphore_wait(sema(inFlight_), DISPATCH_TIME_FOREVER);
    // libdispatch traps if a semaphore is freed below its initial value.
    for (int i = 0; i < kFramesInFlight; ++i)
        dispatch_semaphore_signal(sema(inFlight_));
    for (auto* f : frames_)
        f->release();
    instances_->release();
    indices_->release();
    vertices_->release();
    depth_->release();
    pipeline_->release();
    queue_->release();
    device_->release();
    dispatch_release(sema(inFlight_));
}

void Renderer::draw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable, const FrameView& view) {
    if (!pass || !drawable)
        return;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    dispatch_semaphore_wait(sema(inFlight_), DISPATCH_TIME_FOREVER);
    frame_ = (frame_ + 1) % kFramesInFlight;

    auto* f = static_cast<Frame*>(frames_[frame_]->contents());
    f->viewProj = simd_mul(view.projection, view.view);
    simd_float3 light = simd_normalize(simd_make_float3(-0.4f, -0.6f, -1.0f));
    f->lightDir = simd_make_float4(light.x, light.y, light.z, 0);

    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    dispatch_semaphore_t sem = sema(inFlight_);
    cmd->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });

    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setRenderPipelineState(pipeline_);
    enc->setDepthStencilState(depth_);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
    enc->setCullMode(MTL::CullModeBack);
    enc->setVertexBuffer(vertices_, 0, 0);
    enc->setVertexBuffer(instances_, 0, 1);
    enc->setVertexBuffer(frames_[frame_], 0, 2);
    enc->setFragmentBuffer(frames_[frame_], 0, 2);
    enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(36), MTL::IndexTypeUInt16, indices_,
                               NS::UInteger(0), NS::UInteger(instanceCount_));
    enc->endEncoding();

    cmd->presentDrawable(drawable);
    cmd->commit();

    pool->release();
}

} // namespace os2
