#include "movie_layer.hpp"

#include "formats/image.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace os2 {

namespace {

constexpr const char* kMovieShader = R"(
#include <metal_stdlib>
using namespace metal;

struct MovieQuad {
    float2 scale;  // NDC half-extent of the picture
};

struct MovieOut {
    float4 position [[position]];
    float2 uv;
};

vertex MovieOut movie_vertex(uint vid [[vertex_id]], constant MovieQuad& q [[buffer(0)]]) {
    const float2 corners[6] = {float2(-1, 1), float2(1, 1), float2(1, -1), float2(-1, 1), float2(1, -1), float2(-1, -1)};
    MovieOut out;
    const float2 c = corners[vid];
    out.position = float4(c * q.scale, 0.0, 1.0);
    out.uv = float2((c.x + 1) * 0.5, (1 - c.y) * 0.5);
    return out;
}

fragment float4 movie_fragment(MovieOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    return float4(tex.sample(smp, in.uv).rgb, 1.0);
}
)";

NS::String* str(const char* s) { return NS::String::string(s, NS::UTF8StringEncoding); }

std::string errorText(NS::Error* error) {
    if (!error || !error->localizedDescription())
        return "unknown error";
    return error->localizedDescription()->utf8String();
}

MTL::Texture* newTexture(MTL::Device* device, uint32_t w, uint32_t h) {
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, w, h, false);
    td->setUsage(MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    return device->newTexture(td);
}

} // namespace

MovieLayer::MovieLayer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat)
    : device_(device->retain()), colorFormat_(colorFormat), depthFormat_(depthFormat) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    NS::Error* error = nullptr;
    MTL::Library* lib = device_->newLibrary(str(kMovieShader), nullptr, &error);
    if (!lib) {
        std::string msg = "movie shader compilation failed: " + errorText(error);
        pool->release();
        throw std::runtime_error(msg);
    }
    MTL::Function* vs = lib->newFunction(str("movie_vertex"));
    MTL::Function* fs = lib->newFunction(str("movie_fragment"));
    MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vs);
    desc->setFragmentFunction(fs);
    desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormat(colorFormat));
    desc->setDepthAttachmentPixelFormat(MTL::PixelFormat(depthFormat));
    pipeline_ = device_->newRenderPipelineState(desc, &error);
    desc->release();
    vs->release();
    fs->release();
    lib->release();
    if (!pipeline_) {
        std::string msg = "movie pipeline creation failed: " + errorText(error);
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
    black_ = newTexture(device_, 1, 1);
    const uint8_t px[4] = {0, 0, 0, 255};
    black_->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, px, 4);
    queue_ = device_->newCommandQueue();
    pool->release();
}

MovieLayer::~MovieLayer() {
    if (texture_)
        texture_->release();
    black_->release();
    queue_->release();
    sampler_->release();
    noDepth_->release();
    pipeline_->release();
    device_->release();
}

void MovieLayer::setFrame(const RgbaImage& img) {
    if (!img.width || !img.height)
        return;
    if (!texture_ || frameW_ != img.width || frameH_ != img.height) {
        if (texture_)
            texture_->release();
        texture_ = newTexture(device_, img.width, img.height);
        frameW_ = img.width;
        frameH_ = img.height;
    }
    texture_->replaceRegion(MTL::Region::Make2D(0, 0, img.width, img.height), 0, img.pixels.data(), img.width * 4);
}

void MovieLayer::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height) {
    if (!width || !height)
        return;
    enc->setRenderPipelineState(pipeline_);
    enc->setDepthStencilState(noDepth_);
    enc->setCullMode(MTL::CullModeNone);
    enc->setFragmentSamplerState(sampler_, 0);
    float full[2] = {1, 1};
    enc->setVertexBytes(full, sizeof full, 0);
    enc->setFragmentTexture(black_, 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));
    if (!texture_)
        return;
    // Fit, keeping the picture's aspect.
    const float s = std::min(float(width) / float(frameW_), float(height) / float(frameH_));
    float scale[2] = {frameW_ * s / float(width), frameH_ * s / float(height)};
    enc->setVertexBytes(scale, sizeof scale, 0);
    enc->setFragmentTexture(texture_, 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));
}

void MovieLayer::draw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable) {
    if (!pass || !drawable)
        return;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    auto* ca = pass->colorAttachments()->object(0);
    ca->setLoadAction(MTL::LoadActionClear);
    ca->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    MTL::Texture* target = ca->texture();
    encode(enc, target ? uint32_t(target->width()) : 0, target ? uint32_t(target->height()) : 0);
    enc->endEncoding();
    cmd->presentDrawable(drawable);
    cmd->commit();
    pool->release();
}

RgbaImage MovieLayer::capture(uint32_t width, uint32_t height) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::TextureDescriptor* cd =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormat(colorFormat_), width, height, false);
    cd->setUsage(MTL::TextureUsageRenderTarget);
    cd->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* color = device_->newTexture(cd);
    MTL::TextureDescriptor* dd =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormat(depthFormat_), width, height, false);
    dd->setUsage(MTL::TextureUsageRenderTarget);
    dd->setStorageMode(MTL::StorageModePrivate);
    MTL::Texture* depth = device_->newTexture(dd);
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* ca = pass->colorAttachments()->object(0);
    ca->setTexture(color);
    ca->setLoadAction(MTL::LoadActionClear);
    ca->setStoreAction(MTL::StoreActionStore);
    ca->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
    pass->depthAttachment()->setTexture(depth);
    pass->depthAttachment()->setLoadAction(MTL::LoadActionClear);
    pass->depthAttachment()->setStoreAction(MTL::StoreActionDontCare);
    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    encode(enc, width, height);
    enc->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    RgbaImage img;
    img.width = width;
    img.height = height;
    img.pixels.resize(size_t(width) * height * 4);
    color->getBytes(img.pixels.data(), size_t(width) * 4, MTL::Region::Make2D(0, 0, width, height), 0);
    if (MTL::PixelFormat(colorFormat_) == MTL::PixelFormatBGRA8Unorm ||
        MTL::PixelFormat(colorFormat_) == MTL::PixelFormatBGRA8Unorm_sRGB)
        for (size_t i = 0; i < img.pixels.size(); i += 4)
            std::swap(img.pixels[i], img.pixels[i + 2]);
    depth->release();
    color->release();
    pool->release();
    return img;
}

} // namespace os2
