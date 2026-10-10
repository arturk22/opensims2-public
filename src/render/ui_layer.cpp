#include "ui_layer.hpp"

#include "formats/image.hpp"
#include "formats/ui_screen.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace os2 {

namespace {

// Compiled at runtime with MTL::Device::newLibrary, like shaders.metal.
constexpr const char* kUiShader = R"(
#include <metal_stdlib>
using namespace metal;

struct UiVertex {
    float2 pos;   // canvas pixels
    float2 uv;
    float4 color;
};

struct UiView {
    float2 scale; // canvas pixels -> NDC
    float2 offset;
};

struct UiOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};

vertex UiOut ui_vertex(uint vid [[vertex_id]], const device UiVertex* v [[buffer(0)]], constant UiView& view [[buffer(1)]]) {
    UiOut out;
    out.position = float4(v[vid].pos * view.scale + view.offset, 0.0, 1.0);
    out.uv = v[vid].uv;
    out.color = v[vid].color;
    return out;
}

fragment float4 ui_fragment(UiOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv) * in.color;
}
)";

struct Vertex {
    float pos[2];
    float uv[2];
    float color[4];
};

struct View {
    float scale[2];
    float offset[2];
};

NS::String* str(const char* s) { return NS::String::string(s, NS::UTF8StringEncoding); }

std::string errorText(NS::Error* error) {
    if (!error || !error->localizedDescription())
        return "unknown error";
    return error->localizedDescription()->utf8String();
}

} // namespace

UiLayer::UiLayer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) : device_(device->retain()) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    NS::Error* error = nullptr;
    MTL::Library* lib = device_->newLibrary(str(kUiShader), nullptr, &error);
    if (!lib) {
        std::string msg = "UI shader compilation failed: " + errorText(error);
        pool->release();
        throw std::runtime_error(msg);
    }
    MTL::Function* vs = lib->newFunction(str("ui_vertex"));
    MTL::Function* fs = lib->newFunction(str("ui_fragment"));
    MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vs);
    desc->setFragmentFunction(fs);
    auto* color = desc->colorAttachments()->object(0);
    color->setPixelFormat(MTL::PixelFormat(colorFormat));
    color->setBlendingEnabled(true);
    color->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
    color->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    color->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
    color->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    desc->setDepthAttachmentPixelFormat(MTL::PixelFormat(depthFormat));
    pipeline_ = device_->newRenderPipelineState(desc, &error);
    desc->release();
    vs->release();
    fs->release();
    lib->release();
    if (!pipeline_) {
        std::string msg = "UI pipeline creation failed: " + errorText(error);
        pool->release();
        throw std::runtime_error(msg);
    }

    MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
    dsd->setDepthCompareFunction(MTL::CompareFunctionAlways);
    dsd->setDepthWriteEnabled(false);
    noDepth_ = device_->newDepthStencilState(dsd);
    dsd->release();

    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    sampler_ = device_->newSamplerState(sd);
    sd->release();

    RgbaImage white;
    white.width = white.height = 1;
    white.pixels = {255, 255, 255, 255};
    white_ = upload(white);
    pool->release();
}

UiLayer::~UiLayer() {
    clear();
    if (white_)
        white_->release();
    sampler_->release();
    noDepth_->release();
    pipeline_->release();
    device_->release();
}

void UiLayer::clear() {
    for (auto& [img, tex] : textures_)
        tex->release();
    textures_.clear();
    if (vertices_)
        vertices_->release();
    vertices_ = nullptr;
    batches_.clear();
}

MTL::Texture* UiLayer::upload(const RgbaImage& img) {
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, img.width, img.height, false);
    td->setUsage(MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* tex = device_->newTexture(td);
    tex->replaceRegion(MTL::Region::Make2D(0, 0, img.width, img.height), 0, img.pixels.data(), img.width * 4);
    return tex;
}

void UiLayer::setScreen(const UiScreen& screen) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    clear();
    canvasWidth_ = float(screen.width);
    canvasHeight_ = float(screen.height);
    std::vector<Vertex> verts;
    verts.reserve(screen.quads.size() * 6);
    for (const auto& q : screen.quads) {
        MTL::Texture* tex = white_;
        if (q.image && q.image->width && q.image->height) {
            auto it = textures_.find(q.image);
            if (it == textures_.end())
                it = textures_.emplace(q.image, upload(*q.image)).first;
            tex = it->second;
        }
        if (batches_.empty() || batches_.back().texture != tex)
            batches_.push_back({tex, uint32_t(verts.size()), 0});
        auto v = [&](float x, float y, float u, float t) {
            verts.push_back({{x, y}, {u, t}, {q.color[0], q.color[1], q.color[2], q.color[3]}});
        };
        v(q.x0, q.y0, q.u0, q.v0);
        v(q.x1, q.y0, q.u1, q.v0);
        v(q.x1, q.y1, q.u1, q.v1);
        v(q.x0, q.y0, q.u0, q.v0);
        v(q.x1, q.y1, q.u1, q.v1);
        v(q.x0, q.y1, q.u0, q.v1);
        batches_.back().count += 6;
    }
    if (!verts.empty())
        vertices_ = device_->newBuffer(verts.data(), verts.size() * sizeof(Vertex), MTL::ResourceStorageModeShared);
    pool->release();
}

void UiLayer::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height) {
    if (!vertices_ || batches_.empty() || width == 0 || height == 0 || canvasWidth_ <= 0 || canvasHeight_ <= 0)
        return;
    const float w = float(width), h = float(height);
    const float s = std::min(w / canvasWidth_, h / canvasHeight_);
    const float ox = (w - canvasWidth_ * s) / 2, oy = (h - canvasHeight_ * s) / 2;
    const View view{{2 * s / w, -2 * s / h}, {2 * ox / w - 1, 1 - 2 * oy / h}};
    enc->setRenderPipelineState(pipeline_);
    enc->setDepthStencilState(noDepth_);
    enc->setCullMode(MTL::CullModeNone);
    enc->setVertexBuffer(vertices_, 0, 0);
    enc->setVertexBytes(&view, sizeof view, 1);
    enc->setFragmentSamplerState(sampler_, 0);
    for (const auto& b : batches_) {
        enc->setFragmentTexture(b.texture, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(b.first), NS::UInteger(b.count));
    }
}

} // namespace os2
