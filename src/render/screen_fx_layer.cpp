#include "screen_fx_layer.hpp"

#include "formats/image.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace os2 {

namespace {

constexpr const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;

struct FxQuad {
    float2 scale;  // NDC half-extent
    float2 offset; // NDC centre
    float4 tint;   // rgb, alpha
    float4 p;      // vignette centre x, y, strength; mode
    float4 q;      // time, width, height, -
};

struct FxOut {
    float4 position [[position]];
    float2 uv;
};

vertex FxOut fx_vertex(uint vid [[vertex_id]], constant FxQuad& u [[buffer(0)]]) {
    const float2 corners[6] = {float2(-1, 1), float2(1, 1), float2(1, -1), float2(-1, 1), float2(1, -1), float2(-1, -1)};
    FxOut out;
    const float2 c = corners[vid];
    out.position = float4(c * u.scale + u.offset, 0.0, 1.0);
    out.uv = float2((c.x + 1) * 0.5, (1 - c.y) * 0.5);
    return out;
}

float hash(float2 p) {
    p = fract(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

fragment float4 fx_fragment(FxOut in [[stage_in]], constant FxQuad& u [[buffer(0)]],
                            texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    const int mode = int(u.p.w + 0.5);
    if (mode == 1) { // vignette: black towards the corners
        const float2 d = (in.uv - u.p.xy) * float2(u.q.y / max(u.q.z, 1.0), 1.0);
        const float r = length(d) / length(float2(u.q.y / max(u.q.z, 1.0), 1.0) * 0.5);
        return float4(0, 0, 0, u.p.z * smoothstep(0.55, 1.15, r) * 0.85);
    }
    if (mode == 2) { // film grain
        const float2 px = floor(in.uv * u.q.yz);
        const float n = hash(px + floor(u.q.x * 24.0) * float2(17.0, 31.0));
        return float4(float3(n), u.tint.a);
    }
    const float4 c = tex.sample(smp, in.uv);
    return float4(c.rgb * u.tint.rgb, c.a * u.tint.a);
}
)";

NS::String* str(const char* s) { return NS::String::string(s, NS::UTF8StringEncoding); }

struct Quad {
    float scale[2];
    float offset[2];
    float tint[4];
    float p[4];
    float q[4];
};

MTL::Texture* newTexture(MTL::Device* device, uint32_t w, uint32_t h) {
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, w, h, false);
    td->setUsage(MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    return device->newTexture(td);
}

} // namespace

ScreenFxLayer::ScreenFxLayer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat)
    : device_(device->retain()) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    NS::Error* error = nullptr;
    MTL::Library* lib = device_->newLibrary(str(kShader), nullptr, &error);
    if (!lib) {
        std::string msg = "screen fx shader compilation failed";
        if (error && error->localizedDescription())
            msg += std::string(": ") + error->localizedDescription()->utf8String();
        pool->release();
        throw std::runtime_error(msg);
    }
    MTL::Function* vs = lib->newFunction(str("fx_vertex"));
    MTL::Function* fs = lib->newFunction(str("fx_fragment"));
    auto make = [&](MTL::BlendFactor srcRgb, MTL::BlendFactor dstRgb) {
        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vs);
        desc->setFragmentFunction(fs);
        auto* ca = desc->colorAttachments()->object(0);
        ca->setPixelFormat(MTL::PixelFormat(colorFormat));
        ca->setBlendingEnabled(true);
        ca->setSourceRGBBlendFactor(srcRgb);
        ca->setDestinationRGBBlendFactor(dstRgb);
        ca->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
        ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
        desc->setDepthAttachmentPixelFormat(MTL::PixelFormat(depthFormat));
        MTL::RenderPipelineState* ps = device_->newRenderPipelineState(desc, &error);
        desc->release();
        if (!ps)
            throw std::runtime_error("screen fx pipeline creation failed");
        return ps;
    };
    blend_ = make(MTL::BlendFactorSourceAlpha, MTL::BlendFactorOneMinusSourceAlpha);
    multiply_ = make(MTL::BlendFactorZero, MTL::BlendFactorSourceColor);
    screen_ = make(MTL::BlendFactorDestinationColor, MTL::BlendFactorOne);
    vs->release();
    fs->release();
    lib->release();
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
    white_ = newTexture(device_, 1, 1);
    const uint8_t px[4] = {255, 255, 255, 255};
    white_->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, px, 4);
    pool->release();
}

ScreenFxLayer::~ScreenFxLayer() {
    if (snapshot_)
        snapshot_->release();
    white_->release();
    sampler_->release();
    noDepth_->release();
    screen_->release();
    multiply_->release();
    blend_->release();
    device_->release();
}

void ScreenFxLayer::setSnapshot(const RgbaImage& img) {
    if (!img.width || !img.height)
        return;
    if (!snapshot_ || snapW_ != img.width || snapH_ != img.height) {
        if (snapshot_)
            snapshot_->release();
        snapshot_ = newTexture(device_, img.width, img.height);
        snapW_ = img.width;
        snapH_ = img.height;
    }
    snapshot_->replaceRegion(MTL::Region::Make2D(0, 0, img.width, img.height), 0, img.pixels.data(), img.width * 4);
}

void ScreenFxLayer::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, const Params& p) {
    if (!width || !height)
        return;
    enc->setDepthStencilState(noDepth_);
    enc->setCullMode(MTL::CullModeNone);
    enc->setFragmentSamplerState(sampler_, 0);
    auto draw = [&](MTL::RenderPipelineState* ps, MTL::Texture* tex, Quad q) {
        enc->setRenderPipelineState(ps);
        enc->setFragmentTexture(tex ? tex : white_, 0);
        q.q[0] = p.time;
        q.q[1] = float(width);
        q.q[2] = float(height);
        enc->setVertexBytes(&q, sizeof q, 0);
        enc->setFragmentBytes(&q, sizeof q, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));
    };
    const Quad full{{1, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    // Bloom: the scene times w (darkening when w < 1), then brightened by rgb.
    const float w = std::clamp(p.bloom[3], 0.0f, 2.0f);
    if (w < 0.999f) {
        Quad q = full;
        q.tint[0] = q.tint[1] = q.tint[2] = w;
        draw(multiply_, nullptr, q);
    }
    float add[3] = {std::max(0.0f, p.bloom[0]), std::max(0.0f, p.bloom[1]), std::max(0.0f, p.bloom[2])};
    if (w > 1.001f)
        for (float& a : add)
            a += w - 1;
    if (add[0] > 0.001f || add[1] > 0.001f || add[2] > 0.001f) {
        Quad q = full;
        for (int k = 0; k < 3; ++k)
            q.tint[k] = std::min(add[k], 1.0f);
        draw(screen_, nullptr, q);
    }
    if (p.snapshotAlpha > 0.001f && snapshot_) {
        Quad q = full;
        q.tint[3] = std::min(p.snapshotAlpha, 1.0f);
        draw(blend_, snapshot_, q);
    }
    if (p.vignette[2] > 0.001f) {
        Quad q = full;
        q.p[0] = p.vignette[0];
        q.p[1] = p.vignette[1];
        q.p[2] = std::min(p.vignette[2], 1.0f);
        q.p[3] = 1;
        draw(blend_, nullptr, q);
    }
    if (p.grain > 0.0005f) {
        Quad q = full;
        q.tint[3] = std::min(p.grain * 2.0f, 0.5f);
        q.p[3] = 2;
        draw(blend_, nullptr, q);
    }
    if (p.flash[3] > 0.001f) {
        Quad q = full;
        for (int k = 0; k < 4; ++k)
            q.tint[k] = p.flash[size_t(k)];
        draw(blend_, nullptr, q);
    }
    if (p.letterbox > 0) {
        const float share = std::min(p.letterbox, 0.5f);
        for (float side : {1.0f, -1.0f}) {
            Quad q{{1, share}, {0, side * (1 - share)}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}};
            draw(blend_, nullptr, q);
        }
    }
}

} // namespace os2
