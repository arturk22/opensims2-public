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

namespace os2 {

namespace {

struct Vertex {
    float position[3];
    float color[3];
};

struct Uniforms {
    simd_float4x4 mvp;
};

// Right-handed, looking down -Z, Metal clip depth in [0, 1].
simd_float4x4 perspective(float fovY, float aspect, float near, float far) {
    float ys = 1.0f / std::tan(fovY * 0.5f);
    float xs = ys / aspect;
    float zs = far / (near - far);
    return simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                       simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * near, 0));
}

simd_float4x4 translation(float x, float y, float z) {
    return simd_matrix(simd_make_float4(1, 0, 0, 0), simd_make_float4(0, 1, 0, 0),
                       simd_make_float4(0, 0, 1, 0), simd_make_float4(x, y, z, 1));
}

simd_float4x4 rotation(float angle, simd_float3 axis) {
    simd_quatf q = simd_quaternion(angle, simd_normalize(axis));
    return simd_matrix4x4(q);
}

std::string errorText(NS::Error* error) {
    if (!error || !error->localizedDescription())
        return "unknown error";
    return error->localizedDescription()->utf8String();
}

// One color per face, four vertices per face so faces stay flat-colored.
void buildCube(std::array<Vertex, 24>& v, std::array<uint16_t, 36>& idx) {
    static const float faces[6][4][3] = {
        {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},         // +X
        {{-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}},     // -X
        {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},         // +Y
        {{-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}},     // -Y
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},         // +Z
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},     // -Z
    };
    static const float colors[6][3] = {
        {0.90f, 0.30f, 0.25f}, {0.25f, 0.70f, 0.35f}, {0.30f, 0.45f, 0.90f},
        {0.95f, 0.80f, 0.25f}, {0.70f, 0.35f, 0.85f}, {0.25f, 0.80f, 0.85f},
    };
    for (int f = 0; f < 6; ++f) {
        for (int c = 0; c < 4; ++c) {
            Vertex& out = v[f * 4 + c];
            for (int k = 0; k < 3; ++k) {
                out.position[k] = faces[f][c][k] * 0.5f;
                out.color[k] = colors[f][k];
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
    for (auto& u : uniforms_)
        u = device_->newBuffer(sizeof(Uniforms), MTL::ResourceStorageModeShared);

    pool->release();
}

Renderer::~Renderer() {
    // Let in-flight frames finish before their buffers go away.
    for (int i = 0; i < kFramesInFlight; ++i)
        dispatch_semaphore_wait(sema(inFlight_), DISPATCH_TIME_FOREVER);
    // libdispatch traps if a semaphore is freed below its initial value.
    for (int i = 0; i < kFramesInFlight; ++i)
        dispatch_semaphore_signal(sema(inFlight_));
    for (auto* u : uniforms_)
        u->release();
    indices_->release();
    vertices_->release();
    depth_->release();
    pipeline_->release();
    queue_->release();
    device_->release();
    dispatch_release(sema(inFlight_));
}

void Renderer::resize(double width, double height) {
    if (width > 0 && height > 0)
        aspect_ = float(width / height);
}

void Renderer::draw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable, double dt) {
    if (!pass || !drawable)
        return;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    dispatch_semaphore_wait(sema(inFlight_), DISPATCH_TIME_FOREVER);
    frame_ = (frame_ + 1) % kFramesInFlight;
    time_ += dt;

    const float t = float(time_);
    simd_float4x4 model = simd_mul(rotation(t, simd_make_float3(0, 1, 0)),
                                   rotation(t * 0.6f, simd_make_float3(1, 0, 0)));
    simd_float4x4 view = translation(0, 0, -3.0f);
    simd_float4x4 proj = perspective(float(M_PI) / 3.0f, aspect_, 0.1f, 100.0f);
    auto* u = static_cast<Uniforms*>(uniforms_[frame_]->contents());
    u->mvp = simd_mul(proj, simd_mul(view, model));

    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    dispatch_semaphore_t sem = sema(inFlight_);
    cmd->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });

    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setRenderPipelineState(pipeline_);
    enc->setDepthStencilState(depth_);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
    enc->setCullMode(MTL::CullModeBack);
    enc->setVertexBuffer(vertices_, 0, 0);
    enc->setVertexBuffer(uniforms_[frame_], 0, 1);
    enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(36), MTL::IndexTypeUInt16,
                               indices_, NS::UInteger(0));
    enc->endEncoding();

    cmd->presentDrawable(drawable);
    cmd->commit();

    pool->release();
}

} // namespace os2
