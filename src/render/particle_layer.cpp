#include "render/particle_layer.hpp"

#include "formats/image.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "render/texture.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace os2 {

namespace {

constexpr uint32_t kTypeTxtr = 0x1C4A276C;

constexpr const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;

struct PVertex { // packed: 36 bytes, as the C++ side writes them
    packed_float3 pos;
    packed_float2 uv;
    packed_float4 color;
};

struct POut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};

vertex POut particle_vertex(uint vid [[vertex_id]], const device PVertex* v [[buffer(0)]],
                            constant float4x4& viewProj [[buffer(1)]]) {
    POut out;
    out.position = viewProj * float4(float3(v[vid].pos), 1.0);
    out.uv = float2(v[vid].uv);
    out.color = float4(v[vid].color);
    return out;
}

fragment float4 particle_fragment(POut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv) * in.color;
}

// Modulate: the destination times the texture colour, faded to 1 by alpha.
fragment float4 particle_modulate(POut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    float4 c = tex.sample(smp, in.uv) * in.color;
    return float4(mix(float3(1.0), c.rgb, c.a), 1.0);
}
)";

struct Vertex {
    float pos[3];
    float uv[2];
    float color[4];
};

NS::String* str(const char* s) { return NS::String::string(s, NS::UTF8StringEncoding); }

std::string errorText(NS::Error* error) {
    if (!error || !error->localizedDescription())
        return "unknown error";
    return error->localizedDescription()->utf8String();
}

int pipelineFor(fx::DrawType d) {
    switch (d) {
    case fx::DrawType::Additive:
    case fx::DrawType::AdditiveInvertDepth:
    case fx::DrawType::AdditiveIgnoreDepth: return 1;
    case fx::DrawType::Modulate: return 2;
    default: return 0;
    }
}

int depthFor(fx::DrawType d) {
    switch (d) {
    case fx::DrawType::DecalIgnoreDepth:
    case fx::DrawType::AdditiveIgnoreDepth: return 1;
    case fx::DrawType::DecalInvertDepth:
    case fx::DrawType::AdditiveInvertDepth: return 2;
    default: return 0;
    }
}

} // namespace

ParticleLayer::ParticleLayer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat, ResourceIndex* index)
    : device_(device->retain()), index_(index) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    NS::Error* error = nullptr;
    MTL::Library* lib = device_->newLibrary(str(kShader), nullptr, &error);
    if (!lib) {
        std::string msg = "particle shader compilation failed: " + errorText(error);
        pool->release();
        throw std::runtime_error(msg);
    }
    MTL::Function* vs = lib->newFunction(str("particle_vertex"));
    MTL::Function* fs = lib->newFunction(str("particle_fragment"));
    MTL::Function* fm = lib->newFunction(str("particle_modulate"));
    for (int i = 0; i < 3; ++i) {
        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vs);
        desc->setFragmentFunction(i == 2 ? fm : fs);
        auto* color = desc->colorAttachments()->object(0);
        color->setPixelFormat(MTL::PixelFormat(colorFormat));
        color->setBlendingEnabled(true);
        if (i == 2) {
            color->setSourceRGBBlendFactor(MTL::BlendFactorDestinationColor);
            color->setDestinationRGBBlendFactor(MTL::BlendFactorZero);
            color->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
            color->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
        } else {
            color->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
            color->setDestinationRGBBlendFactor(i == 1 ? MTL::BlendFactorOne : MTL::BlendFactorOneMinusSourceAlpha);
            color->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
            color->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
        }
        desc->setDepthAttachmentPixelFormat(MTL::PixelFormat(depthFormat));
        pipelines_[size_t(i)] = device_->newRenderPipelineState(desc, &error);
        desc->release();
        if (!pipelines_[size_t(i)]) {
            std::string msg = "particle pipeline creation failed: " + errorText(error);
            pool->release();
            throw std::runtime_error(msg);
        }
    }
    vs->release();
    fs->release();
    fm->release();
    lib->release();

    const MTL::CompareFunction compares[3] = {MTL::CompareFunctionLessEqual, MTL::CompareFunctionAlways,
                                              MTL::CompareFunctionGreater};
    for (int i = 0; i < 3; ++i) {
        MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(compares[i]);
        dsd->setDepthWriteEnabled(false);
        depth_[size_t(i)] = device_->newDepthStencilState(dsd);
        dsd->release();
    }
    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMipFilter(MTL::SamplerMipFilterLinear);
    sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    sampler_ = device_->newSamplerState(sd);
    sd->release();
    white_ = whiteTexture(device_);
    pool->release();
}

ParticleLayer::~ParticleLayer() {
    for (auto& [n, t] : textures_)
        if (t && t != white_)
            t->release();
    for (auto* b : buffers_)
        if (b)
            b->release();
    white_->release();
    sampler_->release();
    for (auto* d : depth_)
        d->release();
    for (auto* p : pipelines_)
        p->release();
    device_->release();
}

MTL::Texture* ParticleLayer::texture(const std::string& name) {
    if (name.empty())
        return white_;
    if (auto it = textures_.find(name); it != textures_.end())
        return it->second;
    MTL::Texture* tex = white_;
    if (index_) {
        try {
            auto loc = index_->findByName(name + "_txtr", kTypeTxtr);
            if (!loc)
                throw std::runtime_error("not found");
            tex = uploadTexture(device_, loadTextureLevels(index_->read(*loc), index_));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: effect texture %s: %s\n", name.c_str(), e.what());
        }
    }
    textures_[name] = tex;
    return tex;
}

const ParticleLayer::Mesh* ParticleLayer::mesh(const std::string& name) {
    static constexpr std::string_view kShape = "_shpe";
    if (name.size() <= kShape.size() || !index_)
        return nullptr;
    std::string lower = name;
    for (char& c : lower)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!lower.ends_with(kShape))
        return nullptr;
    if (auto it = meshes_.find(name); it != meshes_.end())
        return it->second.get();
    std::unique_ptr<Mesh> out;
    // The CRES is the name up to one of its underscores, the longest that
    // exists ("neighborhood_plumbobShape_neighborhood_plumbobshape_shpe",
    // "lightning_root_rot_shpe" -> "lightning").
    const std::string stem = name.substr(0, name.size() - kShape.size());
    for (size_t cut = stem.rfind('_'); cut != std::string::npos && cut > 0 && !out; cut = stem.rfind('_', cut - 1)) {
        try {
            const ObjectModel om = loadObjectModel(*index_, stem.substr(0, cut));
            auto m = std::make_unique<Mesh>();
            bool named = false;
            for (const auto& p : om.parts) {
                std::string shape = p.shape;
                for (char& c : shape)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                named = named || shape == lower;
            }
            for (const auto& p : om.parts) {
                std::string shape = p.shape;
                for (char& c : shape)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                if (named && shape != lower)
                    continue;
                const auto& g = p.mesh;
                for (uint32_t i : g.triangles) {
                    if (i >= g.positions.size())
                        continue;
                    m->positions.push_back(simd_make_float3(g.positions[i][0], g.positions[i][1], g.positions[i][2]));
                    m->uvs.push_back(i < g.uvs.size() ? simd_make_float2(g.uvs[i][0], g.uvs[i][1]) : simd_make_float2(0, 0));
                }
                if (!m->texture)
                    if (auto mat = om.materials.find(p.material); mat != om.materials.end()) {
                        const StandardMaterial sm = standardMaterial(mat->second);
                        std::string tex = sm.baseTexture;
                        if (tex.ends_with("_txtr"))
                            tex.resize(tex.size() - 5);
                        if (!tex.empty())
                            m->texture = texture(tex);
                        else
                            m->tint = simd_make_float3(sm.diffuse[0], sm.diffuse[1], sm.diffuse[2]);
                    }
            }
            if (!m->texture)
                m->texture = white_;
            if (!m->positions.empty())
                out = std::move(m);
        } catch (const std::exception&) { // (the next, shorter name is tried; "not found" below if none loads)
        }
    }
    if (!out)
        std::fprintf(stderr, "opensims2: effect shape %s: not found\n", name.c_str());
    return (meshes_[name] = std::move(out)).get();
}

void ParticleLayer::setImage(const std::string& name, const RgbaImage& image) {
    if (textures_.count(name) || !image.width || !image.height)
        return;
    TextureLevels levels;
    levels.format = TexFormat::Raw32;
    levels.levels.push_back({image.width, image.height, image.pixels});
    auto& px = levels.levels[0].data; // RGBA -> the B,G,R,A of Raw32
    for (size_t i = 0; i + 3 < px.size(); i += 4)
        std::swap(px[i], px[i + 2]);
    try {
        textures_[name] = uploadTexture(device_, levels);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: effect icon %s: %s\n", name.c_str(), e.what());
        textures_[name] = white_;
    }
}

void ParticleLayer::setQuads(const std::vector<fx::ParticleQuad>& quads) {
    quads_.clear();
    quads_.reserve(quads.size());
    for (const auto& q : quads)
        quads_.push_back({q, q.texture ? *q.texture : std::string()});
}

void ParticleLayer::prepare(const simd_float4x4& view, const simd_float4x4& viewProj, std::vector<Group>& out) {
    batches_.clear();
    groupBatches_.clear();
    frameBuffer_ = nullptr;
    viewProj_ = viewProj;
    if (quads_.empty())
        return;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    // Camera axes in world space: the view matrix's rows.
    const simd_float3 right = simd_make_float3(view.columns[0].x, view.columns[1].x, view.columns[2].x);
    const simd_float3 up = simd_make_float3(view.columns[0].y, view.columns[1].y, view.columns[2].y);
    const simd_float3 back = simd_make_float3(view.columns[0].z, view.columns[1].z, view.columns[2].z);

    std::vector<Vertex> verts;
    verts.reserve(quads_.size() * 6);
    int currentGroup = -1;
    for (const auto& qp : quads_) {
        const fx::ParticleQuad& q = qp.q;
        if (q.group != currentGroup) {
            // A new group (an emitter): its sort key and depth.
            currentGroup = q.group;
            groupBatches_.push_back({batches_.size(), 0});
            const simd_float4 c = simd_mul(viewProj, simd_make_float4(q.groupCenter[0], q.groupCenter[1], q.groupCenter[2], 1));
            out.push_back({q.layerKey, c.w > 1e-5f ? c.z / c.w : 0.0f, int(groupBatches_.size() - 1)});
        }
        const simd_float3 c = simd_make_float3(q.position[0], q.position[1], q.position[2]);
        simd_float3 ax = right, ay = up;
        if (q.alignment == 1) { // flat on the ground
            ax = simd_make_float3(1, 0, 0), ay = simd_make_float3(0, 1, 0);
        } else if (q.alignment == 6) { // vertical, turned about Z towards the camera
            simd_float3 f = simd_make_float3(back.x, back.y, 0);
            const float l = simd_length(f);
            f = l > 1e-4f ? f / l : simd_make_float3(0, 1, 0);
            ax = simd_make_float3(-f.y, f.x, 0), ay = simd_make_float3(0, 0, 1);
        } else if (q.alignment == 2) { // along the velocity
            simd_float3 v = simd_make_float3(q.velocity[0], q.velocity[1], q.velocity[2]);
            if (simd_length(v) > 1e-4f) {
                ay = simd_normalize(v);
                simd_float3 x = simd_cross(ay, back);
                if (simd_length(x) > 1e-4f)
                    ax = simd_normalize(x);
            }
        }
        if (q.alignment != 2 && q.rotation != 0) {
            const float cs = std::cos(q.rotation), sn = std::sin(q.rotation);
            const simd_float3 rx = ax * cs + ay * sn, ry = ay * cs - ax * sn;
            ax = rx, ay = ry;
        }
        const simd_float3 hx = ax * (q.width * 0.5f), hy = ay * (q.height * 0.5f);
        const Mesh* shape = mesh(qp.texture);
        MTL::Texture* tex = shape ? shape->texture : texture(qp.texture);
        const int pipe = pipelineFor(q.draw), dep = depthFor(q.draw);
        auto& range = groupBatches_.back();
        if (range.second == 0 || batches_.back().tex != tex || batches_.back().pipeline != pipe || batches_.back().depth != dep) {
            batches_.push_back({tex, pipe, dep, uint32_t(verts.size()), 0});
            ++range.second;
        }
        if (shape) {
            // The mesh at the particle: its size as the scale, its rotation
            // about the vertical (inferred), the model's axes otherwise.
            const float cs = std::cos(q.rotation), sn = std::sin(q.rotation), k = q.width;
            const float col[4] = {q.color[0] * shape->tint.x, q.color[1] * shape->tint.y, q.color[2] * shape->tint.z,
                                  q.color[3]};
            for (size_t i = 0; i < shape->positions.size(); ++i) {
                const simd_float3 m = shape->positions[i] * k;
                const simd_float3 p = c + simd_make_float3(m.x * cs - m.y * sn, m.x * sn + m.y * cs, m.z);
                verts.push_back({{p.x, p.y, p.z}, {shape->uvs[i].x, shape->uvs[i].y}, {col[0], col[1], col[2], col[3]}});
            }
            batches_.back().count += uint32_t(shape->positions.size());
            continue;
        }
        auto v = [&](simd_float3 p, float u, float t) {
            verts.push_back({{p.x, p.y, p.z}, {u, t}, {q.color[0], q.color[1], q.color[2], q.color[3]}});
        };
        const auto& uv = q.uv;
        v(c - hx - hy, uv[0], uv[3]);
        v(c + hx - hy, uv[2], uv[3]);
        v(c + hx + hy, uv[2], uv[1]);
        v(c - hx - hy, uv[0], uv[3]);
        v(c + hx + hy, uv[2], uv[1]);
        v(c - hx + hy, uv[0], uv[1]);
        batches_.back().count += 6;
    }
    ring_ = (ring_ + 1) % kRing;
    MTL::Buffer*& buf = buffers_[size_t(ring_)];
    const size_t bytes = verts.size() * sizeof(Vertex);
    if (!buf || buf->length() < bytes) {
        if (buf)
            buf->release();
        buf = device_->newBuffer(std::max<size_t>(bytes * 2, 64 * 1024), MTL::ResourceStorageModeShared);
    }
    std::memcpy(buf->contents(), verts.data(), bytes);
    frameBuffer_ = buf;
    pool->release();
}

void ParticleLayer::drawGroup(MTL::RenderCommandEncoder* enc, int id) {
    if (!frameBuffer_ || id < 0 || size_t(id) >= groupBatches_.size())
        return;
    enc->setCullMode(MTL::CullModeNone);
    enc->setVertexBuffer(frameBuffer_, 0, 0);
    enc->setVertexBytes(&viewProj_, sizeof viewProj_, 1);
    enc->setFragmentSamplerState(sampler_, 0);
    const auto [first, count] = groupBatches_[size_t(id)];
    for (size_t i = first; i < first + count; ++i) {
        const Batch& b = batches_[i];
        enc->setRenderPipelineState(pipelines_[size_t(b.pipeline)]);
        enc->setDepthStencilState(depth_[size_t(b.depth)]);
        enc->setFragmentTexture(b.tex, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(b.first), NS::UInteger(b.count));
    }
}

} // namespace os2
