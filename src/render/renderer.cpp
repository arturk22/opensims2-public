#include "renderer.hpp"

#include "shaders_src.hpp"
#include "texture.hpp"

#include "formats/image.hpp"
#include "formats/lighting_config.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace os2 {

namespace {

constexpr uint32_t kTypeTxtr = 0x1C4A276C;

// Must match shaders.metal.
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

enum : uint32_t { kLighting = 1, kAlphaTest = 2, kChecker = 4 };

struct Draw {
    simd_float4x4 model;
    simd_float4 color;
    simd_float4 specular;
    simd_float4 emissive;
    float alphaRef;
    uint32_t flags;
};

constexpr size_t kMaxLights = 8;

struct Light {
    simd_float4 direction;
    simd_float4 color;
};

struct Frame {
    simd_float4x4 viewProj;
    simd_float4 eye;
    simd_float4 ambient;
    Light lights[kMaxLights];
    uint32_t lightCount;
};

simd_float4x4 identity() { return matrix_identity_float4x4; }

// Unit cube [0,1]^3 scaled to `size` with its base centered on `base`.
simd_float4x4 box(simd_float3 base, simd_float3 size) {
    return simd_matrix(simd_make_float4(size.x, 0, 0, 0), simd_make_float4(0, size.y, 0, 0),
                       simd_make_float4(0, 0, size.z, 0),
                       simd_make_float4(base.x - size.x * 0.5f, base.y - size.y * 0.5f, base.z, 1));
}

// Unit cube [0,1]^3, four vertices per face so normals stay flat.
void buildCube(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    static const float faces[6][4][3] = {
        {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}, // +X
        {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}}, // -X
        {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}, // +Y
        {{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}, // -Y
        {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}, // +Z
        {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}, // -Z
    };
    static const float normals[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const float uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = uint32_t(v.size());
        for (int c = 0; c < 4; ++c) {
            Vertex out{};
            for (int k = 0; k < 3; ++k) {
                out.position[k] = faces[f][c][k];
                out.normal[k] = normals[f][k];
            }
            out.uv[0] = uvs[c][0];
            out.uv[1] = uvs[c][1];
            v.push_back(out);
        }
        for (uint32_t q : {0u, 1u, 2u, 0u, 2u, 3u})
            idx.push_back(base + q);
    }
}

std::string errorText(NS::Error* error) {
    if (!error || !error->localizedDescription())
        return "unknown error";
    return error->localizedDescription()->utf8String();
}

NS::String* str(const char* s) { return NS::String::string(s, NS::UTF8StringEncoding); }

dispatch_semaphore_t sema(void* p) { return static_cast<dispatch_semaphore_t>(p); }

} // namespace

struct Renderer::Material {
    MTL::Texture* texture = nullptr; // not owned (cache or white)
    simd_float4 color{1, 1, 1, 1};
    simd_float4 specular{0, 0, 0, 1}; // power in w
    simd_float4 emissive{0, 0, 0, 0};
    float alphaRef = 0.5f;
    uint32_t flags = kLighting;
    StandardMaterial::Blend blend = StandardMaterial::Blend::None;
    MTL::CullMode cull = MTL::CullModeBack;
};

struct Renderer::Part {
    MTL::Buffer* vertices = nullptr;
    MTL::Buffer* indices = nullptr;
    uint32_t indexCount = 0;
    Material material;
};

struct Renderer::Model {
    std::string name;
    std::vector<Part> parts;
    ~Model() {
        for (auto& p : parts) {
            p.vertices->release();
            p.indices->release();
        }
    }
};

Renderer::Renderer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat)
    : device_(device->retain()), colorFormat_(colorFormat), depthFormat_(depthFormat), inFlight_(dispatch_semaphore_create(kFramesInFlight)) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    queue_ = device_->newCommandQueue();
    // Until setLighting: soft grey ambient and one light from above.
    lights_.push_back({simd_normalize(simd_make_float3(0.4f, 0.6f, 1.0f)), simd_make_float3(0.55f, 0.55f, 0.55f)});

    NS::Error* error = nullptr;
    MTL::Library* library = device_->newLibrary(str(kShaderSource), nullptr, &error);
    if (!library) {
        std::string msg = "shader compilation failed: " + errorText(error);
        pool->release();
        throw std::runtime_error(msg);
    }
    MTL::Function* vs = library->newFunction(str("vertex_main"));
    MTL::Function* fs = library->newFunction(str("fragment_main"));

    auto pipeline = [&](StandardMaterial::Blend blend) {
        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vs);
        desc->setFragmentFunction(fs);
        auto* color = desc->colorAttachments()->object(0);
        color->setPixelFormat(MTL::PixelFormat(colorFormat));
        if (blend != StandardMaterial::Blend::None) {
            color->setBlendingEnabled(true);
            color->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
            color->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
            const auto dst = blend == StandardMaterial::Blend::Additive ? MTL::BlendFactorOne
                                                                        : MTL::BlendFactorOneMinusSourceAlpha;
            color->setDestinationRGBBlendFactor(dst);
            color->setDestinationAlphaBlendFactor(dst);
        }
        desc->setDepthAttachmentPixelFormat(MTL::PixelFormat(depthFormat));
        MTL::RenderPipelineState* state = device_->newRenderPipelineState(desc, &error);
        desc->release();
        if (!state)
            throw std::runtime_error("pipeline creation failed: " + errorText(error));
        return state;
    };
    try {
        opaque_ = pipeline(StandardMaterial::Blend::None);
        alphaBlend_ = pipeline(StandardMaterial::Blend::Alpha);
        additive_ = pipeline(StandardMaterial::Blend::Additive);
    } catch (...) {
        vs->release();
        fs->release();
        library->release();
        pool->release();
        throw;
    }
    vs->release();
    fs->release();
    library->release();

    MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
    dsd->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
    dsd->setDepthWriteEnabled(true);
    depthWrite_ = device_->newDepthStencilState(dsd);
    dsd->setDepthWriteEnabled(false);
    depthRead_ = device_->newDepthStencilState(dsd);
    dsd->release();

    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMipFilter(MTL::SamplerMipFilterLinear);
    sd->setSAddressMode(MTL::SamplerAddressModeRepeat);
    sd->setTAddressMode(MTL::SamplerAddressModeRepeat);
    sd->setMaxAnisotropy(8);
    sampler_ = device_->newSamplerState(sd);
    sd->release();

    white_ = whiteTexture(device_);
    for (auto& f : frames_)
        f = device_->newBuffer(sizeof(Frame), MTL::ResourceStorageModeShared);

    // Test floor (32x32 tiles, top at z = 0) and axis markers: +X red, +Y green, origin white.
    addInstance(addBox({0, 0, -0.05f}, {32, 32, 0.05f}, {0.55f, 0.62f, 0.45f}, kLighting | kChecker), identity());
    for (int i = 1; i <= 4; ++i) {
        addInstance(addBox({float(i) + 0.5f, 0.5f, 0}, {0.6f, 0.6f, 0.15f}, {0.85f, 0.25f, 0.2f}, kLighting),
                    identity());
        addInstance(addBox({0.5f, float(i) + 0.5f, 0}, {0.6f, 0.6f, 0.15f}, {0.25f, 0.7f, 0.3f}, kLighting),
                    identity());
    }
    addInstance(addBox({0.5f, 0.5f, 0}, {0.6f, 0.6f, 0.6f}, {0.95f, 0.95f, 0.95f}, kLighting), identity());

    pool->release();
}

Renderer::~Renderer() {
    // Let in-flight frames finish before their buffers go away.
    for (int i = 0; i < kFramesInFlight; ++i)
        dispatch_semaphore_wait(sema(inFlight_), DISPATCH_TIME_FOREVER);
    // libdispatch traps if a semaphore is freed below its initial value.
    for (int i = 0; i < kFramesInFlight; ++i)
        dispatch_semaphore_signal(sema(inFlight_));
    models_.clear();
    for (auto& [name, tex] : textures_)
        if (tex)
            tex->release();
    white_->release();
    for (auto* f : frames_)
        f->release();
    sampler_->release();
    depthRead_->release();
    depthWrite_->release();
    additive_->release();
    alphaBlend_->release();
    opaque_->release();
    queue_->release();
    device_->release();
    dispatch_release(sema(inFlight_));
}

int Renderer::addBox(simd_float3 base, simd_float3 size, simd_float3 rgb, uint32_t flags) {
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    buildCube(v, idx);
    const simd_float4x4 m = box(base, size);
    for (auto& vert : v) {
        simd_float4 p = simd_mul(m, simd_make_float4(vert.position[0], vert.position[1], vert.position[2], 1));
        vert.position[0] = p.x, vert.position[1] = p.y, vert.position[2] = p.z;
    }
    auto model = std::make_unique<Model>();
    model->name = "box";
    Part part;
    part.vertices = device_->newBuffer(v.data(), v.size() * sizeof(Vertex), MTL::ResourceStorageModeShared);
    part.indices = device_->newBuffer(idx.data(), idx.size() * sizeof(uint32_t), MTL::ResourceStorageModeShared);
    part.indexCount = uint32_t(idx.size());
    part.material.texture = white_;
    part.material.color = simd_make_float4(rgb.x, rgb.y, rgb.z, 1);
    part.material.flags = flags;
    model->parts.push_back(part);
    models_.push_back(std::move(model));
    return int(models_.size() - 1);
}

MTL::Texture* Renderer::texture(const std::string& name, ResourceIndex& index) {
    if (auto it = textures_.find(name); it != textures_.end())
        return it->second;
    MTL::Texture* tex = nullptr;
    try {
        auto loc = index.findByName(name, kTypeTxtr);
        if (!loc)
            throw std::runtime_error("not found");
        tex = uploadTexture(device_, loadTextureLevels(index.read(*loc), &index));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: texture %s: %s\n", name.c_str(), e.what());
    }
    textures_[name] = tex;
    return tex;
}

int Renderer::addObject(const ObjectModel& model, ResourceIndex& index) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    auto out = std::make_unique<Model>();
    out->name = model.name;
    for (const auto& part : model.parts) {
        const MeshGroup& mesh = part.mesh;
        if (mesh.positions.empty() || mesh.triangles.empty())
            continue;

        Material mat;
        mat.texture = white_;
        mat.color = simd_make_float4(0.8f, 0.8f, 0.8f, 1); // no material: plain grey
        if (auto it = model.materials.find(part.material); it != model.materials.end()) {
            const StandardMaterial sm = standardMaterial(it->second);
            if (!sm.visible)
                continue;
            if (!sm.baseTexture.empty())
                if (MTL::Texture* t = texture(sm.baseTexture, index))
                    mat.texture = t;
            // Texture times diffuse coefficient: ground shadows are a black
            // diffuse over an alpha texture.
            const float alpha = sm.alphaMultiplier * (sm.baseTexture.empty() ? sm.untexturedAlpha : 1.0f);
            mat.color = simd_make_float4(sm.diffuse[0], sm.diffuse[1], sm.diffuse[2], alpha);
            mat.specular = simd_make_float4(sm.specular[0], sm.specular[1], sm.specular[2], sm.specularPower);
            mat.emissive = simd_make_float4(sm.emissive[0], sm.emissive[1], sm.emissive[2], 0);
            mat.blend = sm.blend;
            mat.alphaRef = sm.alphaRef;
            mat.flags = (sm.lighting ? kLighting : 0) | (sm.alphaTest ? kAlphaTest : 0);
            mat.cull = sm.cull == StandardMaterial::Cull::None             ? MTL::CullModeNone
                       : sm.cull == StandardMaterial::Cull::CounterClockwise ? MTL::CullModeFront
                                                                             : MTL::CullModeBack;
        }

        std::vector<Vertex> v(mesh.positions.size());
        for (size_t i = 0; i < v.size(); ++i) {
            for (int k = 0; k < 3; ++k)
                v[i].position[k] = mesh.positions[i][k];
            if (i < mesh.normals.size())
                for (int k = 0; k < 3; ++k)
                    v[i].normal[k] = mesh.normals[i][k];
            else
                v[i].normal[2] = 1;
            if (i < mesh.uvs.size())
                v[i].uv[0] = mesh.uvs[i][0], v[i].uv[1] = mesh.uvs[i][1];
        }
        Part p;
        p.vertices = device_->newBuffer(v.data(), v.size() * sizeof(Vertex), MTL::ResourceStorageModeShared);
        p.indices = device_->newBuffer(mesh.triangles.data(), mesh.triangles.size() * sizeof(uint32_t),
                                       MTL::ResourceStorageModeShared);
        p.indexCount = uint32_t(mesh.triangles.size());
        p.material = mat;
        out->parts.push_back(p);
    }
    models_.push_back(std::move(out));
    pool->release();
    return int(models_.size() - 1);
}

void Renderer::setLighting(const LightingState& state) {
    ambient_ = simd_make_float3(state.skyColor[0], state.skyColor[1], state.skyColor[2]) * state.skyStrength;
    lights_.clear();
    for (const auto& l : state.objectLights()) {
        const simd_float3 dir = simd_make_float3(l.direction[0], l.direction[1], l.direction[2]);
        if (simd_length(dir) == 0)
            continue;
        lights_.push_back({simd_normalize(dir), simd_make_float3(l.color[0], l.color[1], l.color[2]) * l.intensity});
    }
    if (lights_.size() > kMaxLights)
        std::fprintf(stderr, "opensims2: lighting %s: only the first %zu of %zu lights are used\n",
                     state.name.c_str(), kMaxLights, lights_.size());
}

void Renderer::addInstance(int model, const simd_float4x4& transform) {
    if (model >= 0 && size_t(model) < models_.size())
        instances_.push_back({model, transform});
}

MTL::CommandBuffer* Renderer::encodeFrame(MTL::RenderPassDescriptor* pass, const FrameView& view) {
    dispatch_semaphore_wait(sema(inFlight_), DISPATCH_TIME_FOREVER);
    frame_ = (frame_ + 1) % kFramesInFlight;

    auto* f = static_cast<Frame*>(frames_[frame_]->contents());
    f->viewProj = simd_mul(view.projection, view.view);
    f->eye = simd_inverse(view.view).columns[3];
    f->ambient = simd_make_float4(ambient_, 0);
    f->lightCount = uint32_t(std::min(lights_.size(), kMaxLights));
    for (uint32_t i = 0; i < f->lightCount; ++i)
        f->lights[i] = {simd_make_float4(lights_[i].direction, 0), simd_make_float4(lights_[i].color, 0)};

    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    dispatch_semaphore_t sem = sema(inFlight_);
    cmd->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });

    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
    enc->setVertexBuffer(frames_[frame_], 0, 2);
    enc->setFragmentBuffer(frames_[frame_], 0, 2);
    enc->setFragmentSamplerState(sampler_, 0);

    // Opaque parts first, then blended ones over them without writing depth.
    for (int blended = 0; blended < 2; ++blended) {
        enc->setDepthStencilState(blended ? depthRead_ : depthWrite_);
        for (const auto& inst : instances_) {
            for (const auto& part : models_[size_t(inst.model)]->parts) {
                const Material& m = part.material;
                if ((m.blend != StandardMaterial::Blend::None) != bool(blended))
                    continue;
                enc->setRenderPipelineState(m.blend == StandardMaterial::Blend::Alpha      ? alphaBlend_
                                            : m.blend == StandardMaterial::Blend::Additive ? additive_
                                                                                           : opaque_);
                enc->setCullMode(m.cull);
                Draw d{inst.transform, m.color, m.specular, m.emissive, m.alphaRef, m.flags};
                enc->setVertexBytes(&d, sizeof d, 1);
                enc->setFragmentBytes(&d, sizeof d, 1);
                enc->setVertexBuffer(part.vertices, 0, 0);
                enc->setFragmentTexture(m.texture, 0);
                enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(part.indexCount),
                                           MTL::IndexTypeUInt32, part.indices, NS::UInteger(0));
            }
        }
    }
    enc->endEncoding();
    return cmd;
}

void Renderer::draw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable, const FrameView& view) {
    if (!pass || !drawable)
        return;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* cmd = encodeFrame(pass, view);
    cmd->presentDrawable(drawable);
    cmd->commit();
    pool->release();
}

RgbaImage Renderer::capture(uint32_t width, uint32_t height, const FrameView& view, simd_float4 clearColor) {
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
    ca->setClearColor(MTL::ClearColor::Make(clearColor.x, clearColor.y, clearColor.z, clearColor.w));
    pass->depthAttachment()->setTexture(depth);
    pass->depthAttachment()->setLoadAction(MTL::LoadActionClear);
    pass->depthAttachment()->setStoreAction(MTL::StoreActionDontCare);
    pass->depthAttachment()->setClearDepth(1.0);

    MTL::CommandBuffer* cmd = encodeFrame(pass, view);
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
