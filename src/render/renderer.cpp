#include "renderer.hpp"
#include "formats/graphics_options.hpp"

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
#include <chrono>
#include <cmath>
#include <cstring>
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
    float uv2[2]; // light map (lot surfaces)
    uint32_t color = 0xFFFFFFFF; // D3DCOLOR
};

enum : uint32_t {
    kLighting = 1,
    kAlphaTest = 2,
    kChecker = 4,
    kMasked = 8,
    kUnderlay = 16,
    kOverlay1 = 32,
    kOverlay2 = 64,
    kEnvCube = 128,
    kEnvLocked = 256,
    kNormalMap = 512,
    kAlphaReplicate = 1024,
    kAlphaOnly = 2048,
    kCelShaded = 4096,
    kCelEdge = 8192,
    kVertexColor = 16384,
    kOverlay3 = 32768,
    kLightMapped = 65536,
    kDetail = 131072,
    kMaskAlphaOnly = 262144,
    kWater = 524288,
    kOverlayMore = 1u << 20, // bits 20-24: textures 13-17 drawn over the base by their alpha, after overlay 3
    kTwoSidedLight = 1u << 25, // lit from both sides (Sim hair: the inside of a hairdo's shell)
    kUnderlayFlat = 1u << 26,  // the underlay's smallest mip (its average colour)
    kWallMask = 1u << 27,      // walls: alpha test against a mask over the face (planes mask / maskV)
    kMirror = 1u << 28,        // the mirror reflecting this frame: the reflection texture (18) at the pixel
    kShadowCaster = 1u << 29,  // a shadow silhouette: the draw's colour (alpha-tested by the texture)
    kPoolWater = 1u << 30,     // RegularWavesHLSL: waves (misc), env cube x (1 - N.V) x 0.5 + highlight, added
    kShadowDecal = 1u << 30,   // a Sim's shadow on the floor: the silhouette (texture 19) at mask/maskV
};

// Sims' shadows (nTSSG::cShadow, docs/lighting-mac.md 9): the shadow
// manager's settings (BV 0x6f2840 reads them, the defaults are set in
// 0x6f3d30), all in tiles and texels.
constexpr float kShadowW0 = 0.7f, kShadowW1 = 0.7f, kShadowH0 = -1.0f, kShadowH1 = 2.0f;
constexpr int kShadowResolution = 128, kIndoorShadowBlur = 3, kOutdoorShadowBlur = 2;
constexpr int kShadowAtlasTiles = 8; // ours: the casters' textures in one atlas
constexpr int kShadowAtlas = kShadowResolution * kShadowAtlasTiles;
constexpr size_t kMaxShadowCasters = size_t(kShadowAtlasTiles * kShadowAtlasTiles);
const simd_float3 kStdMatShadowColor{0.63f, 0.63f, 0.85f}; // sims.matShad: setc stdMatShadowColor

// Must match shaders.metal: a caster's silhouette as the floors see it.
struct GpuShadow {
    simd_float4 u, v;   // world -> atlas uv: dot(xyz, p) + w
    simd_float4 rect;   // the tile in the atlas (uv min, max)
    simd_float4 misc;   // x: blur (uv), y: outside, z: the floor under the caster (z)
    simd_float4 color;  // outside: the shadow colour
};
constexpr size_t kMaxOverlays = 8; // a face's composited layers: eyes, brows, makeup, beard...

struct Draw {
    simd_float4x4 model;
    simd_float4 color;
    simd_float4 specular;
    simd_float4 emissive;
    simd_float4 mask;
    simd_float4 env;
    simd_float4 uv0, uv1;
    simd_float4 misc;
    simd_float4 maskV;
    float alphaRef;
    uint32_t flags;
    uint32_t lightSet;
};

// Height map to normal: height difference per texel times this (inferred;
// the game's conversion strength is not known).
constexpr float kBumpStrength = 2.0f;

constexpr size_t kMaxLights = 16;

struct Light {
    simd_float4 direction;
    simd_float4 color;
    simd_float4 params; // point lights: falloff rate, range, 1, a spot's inner cone cosine
    simd_float4 spot;   // a spot: where it shines, the outer cone cosine (< -1: not a spot)
};

struct GpuLightSet {
    simd_float4 ambient; // rgb, w: light count (as float)
    Light lights[kMaxLights];
};

struct Frame {
    simd_float4x4 viewProj;
    simd_float4x4 view;
    simd_float4 eye;
    simd_float4 params;      // x: mix of light map slot 1 over slot 0, y: time (s)
    simd_float4 environment; // rgb: the environment light (env cubes)
    simd_float4 clip;        // a plane (xyz normal, w offset) that keeps what is on its positive side, when viewport.z is 1
    simd_float4 viewport;    // xy: the target's size in pixels, z: clip on, w: ground clip height
};
constexpr size_t kFrameStride = 256; // the frame buffer holds the frame, then the reflected one at this offset

simd_float4x4 identity() { return matrix_identity_float4x4; }

std::string lowerAsciiName(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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

// 0..1 over each cycle of a texture animation waveform.
float wave(StandardMaterial::Waveform w, double t) {
    const float phase = float(t - std::floor(t));
    switch (w) {
    case StandardMaterial::Waveform::Sawtooth: return phase;
    case StandardMaterial::Waveform::Sine: return 0.5f - 0.5f * std::cos(phase * 2 * float(M_PI));
    case StandardMaterial::Waveform::Triangular: break;
    }
    return 1 - std::fabs(2 * phase - 1);
}

// Value of an animated component at time t; `off` while its speed is 0.
simd_float2 waveValue(const StandardMaterial::Wave& w, double t, simd_float2 off) {
    if (w.speed == 0)
        return off;
    const float k = wave(w.waveform, t * w.speed);
    return simd_make_float2(w.start[0] + (w.end[0] - w.start[0]) * k, w.start[1] + (w.end[1] - w.start[1]) * k);
}

// Base texture coordinate transform (rows of a 2x3 matrix) of a material's
// texture coordinate animation at time t:
// - tile: the texture is a grid of tiles x[0] by x[1] shown one after the
//   other, row by row, `tileSpeed` per second;
// - transform: rotation (turns) and scale about the origin, then translation.
// The composition order is inferred (the game builds the matrix in code).
std::pair<simd_float4, simd_float4> uvTransform(const StandardMaterial& m, double t) {
    if (m.texAnim == StandardMaterial::TexAnim::Tile) {
        const int nu = std::max(1, int(std::lround(m.tiles[0]))), nv = std::max(1, int(std::lround(m.tiles[1])));
        const long frame = long(std::floor(t * m.tileSpeed)) % long(nu * nv);
        const float col = float(frame % nu), row = float(frame / nu);
        return {simd_make_float4(1.0f / float(nu), 0, col / float(nu), 0),
                simd_make_float4(0, 1.0f / float(nv), row / float(nv), 0)};
    }
    if (m.texAnim == StandardMaterial::TexAnim::Transform) {
        const float a = waveValue(m.rotation, t, simd_make_float2(0, 0)).x * 2 * float(M_PI);
        const simd_float2 s = waveValue(m.scale, t, simd_make_float2(1, 1));
        const simd_float2 tr = waveValue(m.translation, t, simd_make_float2(0, 0));
        const float c = std::cos(a), sn = std::sin(a), ox = m.animOrigin[0], oy = m.animOrigin[1];
        // uv' = R S (uv - o) + o + tr
        const float m00 = c * s.x, m01 = -sn * s.y, m10 = sn * s.x, m11 = c * s.y;
        return {simd_make_float4(m00, m01, ox + tr.x - m00 * ox - m01 * oy, 0),
                simd_make_float4(m10, m11, oy + tr.y - m10 * ox - m11 * oy, 0)};
    }
    return {simd_make_float4(1, 0, 0, 0), simd_make_float4(0, 1, 0, 0)};
}

} // namespace

struct Renderer::Material {
    MTL::Texture* texture = nullptr; // not owned (cache or white)
    std::array<MTL::Texture*, 1 + kMaxOverlays> layers{}; // underlay, overlays 1-8 (not owned, null if unused)
    MTL::Texture* height = nullptr;   // bump height map (not owned, null if unused)
    MTL::Texture* envCube = nullptr;  // not owned, null if unused
    MTL::Texture* ramp = nullptr;     // cel shading ramps (not owned, null if unused)
    MTL::Texture* edgeRamp = nullptr;
    MTL::Texture* detail = nullptr;   // terrain detail texture (not owned, null if unused)
    float uvScale = 1;                // base texture coordinates scale (terrain)
    simd_float4 color{1, 1, 1, 1};
    simd_float4 specular{0, 0, 0, 0}; // power in w
    simd_float4 emissive{0, 0, 0, 0};
    simd_float4 maskTransform{0, 0, 0, 0}; // model xy -> mask uv: xy scale, zw offset
    simd_float4 env{0, 0, 0, 0};           // coefficient rgb, mip bias
    simd_float4 misc{0, 0, 0, 0};          // cel edge offset, bump strength, detail uv scale
    simd_float4 maskV{0, 0, 0, 0};         // kWallMask: model xyz -> mask v (maskTransform: -> u)
    float alphaRef = 0.5f;
    uint32_t flags = kLighting;
    StandardMaterial::Blend blend = StandardMaterial::Blend::None;
    bool depthWrite = true;
    bool clamp = false; // base texture clamped instead of tiled
    int layer = 0;
    int alphaOrder = 0; // Sim hair layers (StandardMaterial::alphaOrder)
    bool mirror = false; // StandardMaterial::mirror
    MTL::CullMode cull = MTL::CullModeBack;
    StandardMaterial anim; // texture coordinate animation (texAnim and its parameters)
};

struct Renderer::Part {
    MTL::Buffer* vertices = nullptr;
    // Dynamic parts: one buffer per frame slot (vertices is the first), the
    // part of the source model, and vertices waiting to be copied in.
    std::array<MTL::Buffer*, kFramesInFlight> slots{};
    size_t source = 0;
    bool shadowPlane = false; // skipped while graphicsOptions().guob is off
    std::vector<Vertex> pending;
    std::array<bool, kFramesInFlight> stale{};
    MTL::Buffer* indices = nullptr;
    uint32_t indexCount = 0;
    simd_float3 center{0, 0, 0}; // bounding box centre in model space (blended parts are sorted by it)
    simd_float3 lo{0, 0, 0}, hi{0, 0, 0}; // bounding box in model space
    bool cullable = false; // static vertices (lo/hi stay true): left out of a pass whose frustum misses the box
    simd_float3 normal{0, 0, 1}; // a mirror's: its vertices' mean normal, model space
    Material material;
    MTL::Texture* mask = nullptr; // owned, with kMasked
};

struct Renderer::Model {
    std::string name;
    std::vector<Part> parts;
    ~Model() {
        for (auto& p : parts) {
            for (auto* b : p.slots)
                if (b && b != p.vertices)
                    b->release();
            p.vertices->release();
            p.indices->release();
            if (p.mask)
                p.mask->release();
        }
    }
};

Renderer::Renderer(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat)
    : device_(device->retain()), colorFormat_(colorFormat), depthFormat_(depthFormat), inFlight_(dispatch_semaphore_create(kFramesInFlight)) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    queue_ = device_->newCommandQueue();
    // Until setLighting: soft grey ambient and one light from above.
    LightSet initial;
    initial.ambient = simd_make_float3(0.45f, 0.45f, 0.45f);
    initial.lights.push_back({simd_normalize(simd_make_float3(0.4f, 0.6f, 1.0f)), simd_make_float3(0.55f, 0.55f, 0.55f)});
    lightSets_ = {initial, initial};

    NS::Error* error = nullptr;
    MTL::Library* library = device_->newLibrary(str(kShaderSource), nullptr, &error);
    if (!library) {
        std::string msg = "shader compilation failed: " + errorText(error);
        pool->release();
        throw std::runtime_error(msg);
    }
    MTL::Function* vs = library->newFunction(str("vertex_main"));
    MTL::Function* fs = library->newFunction(str("fragment_main"));
    {
        MTL::Function* cvs = library->newFunction(str("censor_vs"));
        MTL::Function* cfs = library->newFunction(str("censor_fs"));
        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(cvs);
        desc->setFragmentFunction(cfs);
        desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormat(colorFormat));
        desc->setDepthAttachmentPixelFormat(MTL::PixelFormat(depthFormat));
        censor_ = device_->newRenderPipelineState(desc, &error);
        desc->release();
        cvs->release();
        cfs->release();
        if (!censor_) {
            std::string msg = "censor pipeline creation failed: " + errorText(error);
            vs->release();
            fs->release();
            library->release();
            pool->release();
            throw std::runtime_error(msg);
        }
    }

    auto pipeline = [&](StandardMaterial::Blend blend) {
        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vs);
        desc->setFragmentFunction(fs);
        auto* color = desc->colorAttachments()->object(0);
        color->setPixelFormat(MTL::PixelFormat(colorFormat));
        if (blend != StandardMaterial::Blend::None) {
            color->setBlendingEnabled(true);
            const auto src = blend == StandardMaterial::Blend::AddNoSrcAlpha || blend == StandardMaterial::Blend::Premultiplied
                                 ? MTL::BlendFactorOne
                             : blend == StandardMaterial::Blend::Multiply ? MTL::BlendFactorDestinationColor
                                                                          : MTL::BlendFactorSourceAlpha;
            color->setSourceRGBBlendFactor(src);
            color->setSourceAlphaBlendFactor(blend == StandardMaterial::Blend::Multiply ? MTL::BlendFactorZero : src);
            const auto dst = blend == StandardMaterial::Blend::Alpha || blend == StandardMaterial::Blend::Premultiplied ||
                                     blend == StandardMaterial::Blend::Multiply
                                 ? MTL::BlendFactorOneMinusSourceAlpha
                                 : MTL::BlendFactorOne;
            color->setDestinationRGBBlendFactor(dst);
            // Multiply (destColor + invSrcAlpha: the lot imposters' terrainLmap)
            // leaves the target's alpha as it is: with invSrcAlpha it went to 0
            // under every lot, which a screenshot (RGBA) shows as white.
            color->setDestinationAlphaBlendFactor(blend == StandardMaterial::Blend::Multiply ? MTL::BlendFactorOne : dst);
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
        addOne_ = pipeline(StandardMaterial::Blend::AddNoSrcAlpha);
        premultiplied_ = pipeline(StandardMaterial::Blend::Premultiplied);
        multiply_ = pipeline(StandardMaterial::Blend::Multiply);
    } catch (...) { // any type: the Metal objects are released, then it goes on (throw;)
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
    sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setMaxAnisotropy(8);
    clampSampler_ = device_->newSamplerState(sd);
    sd->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setMaxAnisotropy(1);
    cubeSampler_ = device_->newSamplerState(sd);
    sd->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
    maskSampler_ = device_->newSamplerState(sd);
    sd->setMinFilter(MTL::SamplerMinMagFilterNearest);
    sd->setMagFilter(MTL::SamplerMinMagFilterNearest);
    pointSampler_ = device_->newSamplerState(sd);
    sd->release();

    white_ = whiteTexture(device_);
    whiteCube_ = whiteCubeTexture(device_);
    grey_ = rgbaTexture(RgbaImage{1, 1, {128, 128, 128, 255}});
    for (auto& f : frames_)
        f = device_->newBuffer((2 + 2 * kMaxCensorBoxes) * kFrameStride, MTL::ResourceStorageModeShared);
    for (auto& b : lightBuffers_)
        b = device_->newBuffer(sizeof(GpuLightSet) * kMaxLightSets, MTL::ResourceStorageModeShared);

    // Test floor (32x32 tiles, top at z = 0) and axis markers: +X red, +Y green, origin white.
    addInstance(addBox({0, 0, -0.05f}, {32, 32, 0.05f}, {0.55f, 0.62f, 0.45f}, kLighting | kChecker), identity());
    for (int i = 1; i <= 4; ++i) {
        addInstance(addBox({float(i) + 0.5f, 0.5f, 0}, {0.6f, 0.6f, 0.15f}, {0.85f, 0.25f, 0.2f}, kLighting),
                    identity());
        addInstance(addBox({0.5f, float(i) + 0.5f, 0}, {0.6f, 0.6f, 0.15f}, {0.25f, 0.7f, 0.3f}, kLighting),
                    identity());
    }
    addInstance(addBox({0.5f, 0.5f, 0}, {0.6f, 0.6f, 0.6f}, {0.95f, 0.95f, 0.95f}, kLighting), identity());
    testInstances_ = instances_.size();

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
    if (shadowAtlas_)
        shadowAtlas_->release();
    if (shadowDepth_)
        shadowDepth_->release();
    for (auto* b : shadowBuffers_)
        if (b)
            b->release();
    if (reflectionColor_)
        reflectionColor_->release();
    if (reflectionDepth_)
        reflectionDepth_->release();
    if (censorColor_)
        censorColor_->release();
    if (censorDepth_)
        censorDepth_->release();
    censor_->release();
    for (auto& [name, tex] : textures_)
        if (tex)
            tex->release();
    white_->release();
    whiteCube_->release();
    grey_->release();
    for (auto* t : lightMaps_)
        if (t)
            t->release();
    for (auto& [t, frame] : retired_)
        t->release();
    for (auto* f : frames_)
        f->release();
    for (auto* b : lightBuffers_)
        b->release();
    pointSampler_->release();
    cubeSampler_->release();
    clampSampler_->release();
    maskSampler_->release();
    sampler_->release();
    depthRead_->release();
    depthWrite_->release();
    additive_->release();
    addOne_->release();
    premultiplied_->release();
    multiply_->release();
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

MTL::Texture* Renderer::texture(const std::string& name, ResourceIndex& index, bool cube) {
    const std::string key = cube ? "cube:" + name : name;
    if (auto it = textures_.find(key); it != textures_.end())
        return it->second;
    MTL::Texture* tex = nullptr;
    try {
        auto loc = index.findByName(name, kTypeTxtr);
        if (!loc)
            throw std::runtime_error("not found");
        if (cube) {
            auto faces = loadAllTextureLevels(index.read(*loc), &index);
            if (faces.size() != 6)
                throw std::runtime_error("not a cube map (" + std::to_string(faces.size()) + " textures)");
            tex = uploadCubeTexture(device_, faces);
        } else {
            tex = uploadTexture(device_, loadTextureLevels(index.read(*loc), &index));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: texture %s: %s\n", name.c_str(), e.what());
    }
    textures_[key] = tex;
    return tex;
}

Renderer::Material Renderer::material(const StandardMaterial& sm, ResourceIndex& index) {
    Material mat;
    mat.texture = white_;
    if (!sm.baseTexture.empty())
        if (MTL::Texture* t = texture(sm.baseTexture, index))
            mat.texture = t;
    // Texture times diffuse coefficient: ground shadows are a black
    // diffuse over an alpha texture.
    const float alpha = sm.alphaMultiplier * (sm.baseTexture.empty() ? sm.untexturedAlpha : 1.0f);
    simd_float3 diffuse = simd_make_float3(sm.diffuse[0], sm.diffuse[1], sm.diffuse[2]);
    // Hairdos darken their inner layer (diffuse 0.4 on "_l1"), which came out
    // near black inside the shell (the sides of a bob in Create-a-Sim): kept
    // darker, but lifted most of the way to the outer layers.
    // (Only those: the outer layers keep their own, 0.8 or 1.)
    if (sm.hair && simd_reduce_max(diffuse) < 0.6f)
        diffuse = simd_mix(diffuse, simd_make_float3(1, 1, 1), simd_make_float3(0.6f, 0.6f, 0.6f));
    if (sm.shading == StandardMaterial::Shading::Sim)
        diffuse *= 0.8f; // simMaterialLightingScale
    if (sm.shading == StandardMaterial::Shading::VertexColor)
        diffuse = simd_make_float3(1, 1, 1);
    mat.color = simd_make_float4(diffuse, alpha);
    // The specular pass runs only for a power over 1 and coefficients over
    // 0.3 in all, with Phong's R.L to the power x 0.4 (docs/shaders-mac.md 4.3).
    // LightingQuality $Low has no specHighlights (Graphics Rules.sgr).
    const bool specular = graphicsOptions().specHighlights && sm.specularPower > 1 &&
                          sm.specular[0] + sm.specular[1] + sm.specular[2] > 0.3f;
    mat.specular = simd_make_float4(sm.specular[0], sm.specular[1], sm.specular[2], specular ? sm.specularPower * 0.4f : 0);
    mat.emissive = simd_make_float4(sm.emissive[0], sm.emissive[1], sm.emissive[2], 0);
    if (sm.shading == StandardMaterial::Shading::Sim || sm.shading == StandardMaterial::Shading::Water)
        mat.emissive = simd_make_float4(0, 0, 0, 0); // their fixed-function light has no emissive
    mat.blend = sm.blend;
    mat.depthWrite = sm.depthWrite;
    mat.layer = sm.layer;
    mat.mirror = sm.mirror;
    mat.alphaOrder = sm.alphaOrder;
    mat.clamp = sm.clampU || sm.clampV;
    mat.alphaRef = sm.alphaRef;
    mat.flags = (sm.lighting ? kLighting : 0) | (sm.lightMapped ? kLightMapped : 0) | (sm.alphaTest ? kAlphaTest : 0) |
                (sm.alphaReplicate ? kAlphaReplicate : 0) | (sm.textureColor ? 0 : kAlphaOnly) |
                (sm.maskAlphaOnly ? kMaskAlphaOnly : 0);
    mat.uvScale = sm.textureScale;
    if (sm.shading == StandardMaterial::Shading::Water) {
        mat.flags |= kWater;
        mat.misc.w = sm.waterSpeed;
    }
    if (sm.shading == StandardMaterial::Shading::PoolWater) {
        mat.flags |= kPoolWater;
        mat.misc = simd_make_float4(sm.poolWaves[0], sm.poolWaves[1], sm.poolWaves[2], sm.poolWaves[3]);
    }
    if (!sm.detailTexture.empty())
        if (MTL::Texture* t = texture(sm.detailTexture, index)) {
            mat.detail = t;
            mat.misc.z = sm.detailScale;
            mat.flags |= kDetail;
        }
    mat.cull = sm.cull == StandardMaterial::Cull::None             ? MTL::CullModeNone
               : sm.cull == StandardMaterial::Cull::CounterClockwise ? MTL::CullModeFront
                                                                     : MTL::CullModeBack;
    if (sm.shading == StandardMaterial::Shading::VertexColor || sm.shading == StandardMaterial::Shading::TerrainWater)
        mat.flags |= kVertexColor;
    if (sm.hair)
        mat.flags |= kTwoSidedLight;
    if (!sm.envCube.empty())
        if (MTL::Texture* t = texture(sm.envCube, index, true)) {
            mat.envCube = t;
            mat.env = simd_make_float4(sm.envCubeCoef[0], sm.envCubeCoef[1], sm.envCubeCoef[2], sm.envCubeBlur);
            mat.flags |= kEnvCube | (sm.envCubeLockedToCamera ? kEnvLocked : 0);
        }
    if (!sm.normalMap.empty())
        if (MTL::Texture* t = texture(sm.normalMap, index)) {
            mat.height = t;
            mat.misc.y = kBumpStrength;
            mat.flags |= kNormalMap;
        }
    if (sm.shading == StandardMaterial::Shading::CelShaded) {
        if (MTL::Texture* t = texture(sm.celRamp, index)) {
            mat.ramp = t;
            mat.flags |= kCelShaded;
            if (!sm.celEdgeRamp.empty())
                if (MTL::Texture* e = texture(sm.celEdgeRamp, index)) {
                    mat.edgeRamp = e;
                    mat.misc.x = sm.celEdgeOffset;
                    mat.flags |= kCelEdge;
                }
        }
    }
    mat.anim.texAnim = sm.texAnim;
    if (sm.texAnim != StandardMaterial::TexAnim::None) {
        mat.anim = sm;
        mat.anim.baseTexture.clear();
    }
    return mat;
}

void Renderer::replaceObject(int id, const ObjectModel& model, ResourceIndex& index) {
    if (id < 0 || size_t(id) >= models_.size())
        return;
    addObject(model, index);
    retiredModels_.push_back({std::move(models_[size_t(id)]), kFramesInFlight + 1});
    models_[size_t(id)] = std::move(models_.back());
    models_.pop_back();
}

int Renderer::addObject(const ObjectModel& model, ResourceIndex& index, bool dynamic) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    auto out = std::make_unique<Model>();
    out->name = model.name;
    for (size_t source = 0; source < model.parts.size(); ++source) {
        const auto& part = model.parts[source];
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
            mat = material(sm, index);
        }

        if (!part.underlay.empty())
            if (MTL::Texture* t = texture(part.underlay, index)) {
                mat.layers[0] = t;
                mat.flags |= kUnderlay | (part.underlayFlat ? kUnderlayFlat : 0);
            }
        // Every composited layer (the game bakes them into one texture; a
        // face with makeup has more than three).
        for (size_t i = 0, slot = 0; i < part.overlays.size() && slot < kMaxOverlays; ++i)
            if (MTL::Texture* t = texture(part.overlays[i], index)) {
                mat.layers[1 + slot] = t;
                mat.flags |= slot == 0 ? kOverlay1 : slot == 1 ? kOverlay2 : slot == 2 ? kOverlay3 : (kOverlayMore << (slot - 3));
                ++slot;
            }

        std::vector<Vertex> v(mesh.positions.size());
        simd_float3 lo = simd_make_float3(INFINITY, INFINITY, INFINITY), hi = -lo;
        for (size_t i = 0; i < v.size(); ++i) {
            const simd_float3 p = simd_make_float3(mesh.positions[i][0], mesh.positions[i][1], mesh.positions[i][2]);
            lo = simd_min(lo, p);
            hi = simd_max(hi, p);
            if (i < mesh.colors.size())
                v[i].color = mesh.colors[i];
            for (int k = 0; k < 3; ++k)
                v[i].position[k] = mesh.positions[i][k];
            if (i < mesh.normals.size())
                for (int k = 0; k < 3; ++k)
                    v[i].normal[k] = mesh.normals[i][k];
            else
                v[i].normal[2] = 1;
            if (i < mesh.uvs.size())
                v[i].uv[0] = mesh.uvs[i][0], v[i].uv[1] = mesh.uvs[i][1];
            if (i < mesh.uvs2.size())
                v[i].uv2[0] = mesh.uvs2[i][0], v[i].uv2[1] = mesh.uvs2[i][1];
        }
        // Light-mapped materials outside the lot have no light-map page: the
        // game then shows the texture as it is (docs/shaders-mac.md 5-6).
        if ((mat.flags & kLightMapped) && mesh.uvs2.size() != mesh.positions.size())
            mat.flags &= ~uint32_t(kLighting);
        Part p;
        if (part.mask >= 0 && size_t(part.mask) < model.masks.size()) {
            const auto& m = model.masks[size_t(part.mask)];
            if (m.width && m.height && m.values.size() == size_t(m.width) * m.height) {
                p.mask = uploadMask(m);
                if (m.planar) {
                    mat.flags |= kWallMask;
                    mat.maskTransform = simd_make_float4(m.planeU[0], m.planeU[1], m.planeU[2], m.planeU[3]);
                    mat.maskV = simd_make_float4(m.planeV[0], m.planeV[1], m.planeV[2], m.planeV[3]);
                } else {
                    mat.flags |= kMasked;
                    // Value i sits at i * spacing: sample texel centres.
                    const float sx = 1.0f / (m.spacing * float(m.width)), sy = 1.0f / (m.spacing * float(m.height));
                    mat.maskTransform = simd_make_float4(sx, sy, 0.5f / float(m.width) - m.originX * sx,
                                                         0.5f / float(m.height) - m.originY * sy);
                }
            }
        }
        p.vertices = device_->newBuffer(v.data(), v.size() * sizeof(Vertex), MTL::ResourceStorageModeShared);
        p.source = source;
        p.shadowPlane = part.shadowPlane;
        if (dynamic) {
            p.slots[0] = p.vertices;
            for (size_t i = 1; i < p.slots.size(); ++i)
                p.slots[i] = device_->newBuffer(v.data(), v.size() * sizeof(Vertex), MTL::ResourceStorageModeShared);
        }
        p.indices = device_->newBuffer(mesh.triangles.data(), mesh.triangles.size() * sizeof(uint32_t),
                                       MTL::ResourceStorageModeShared);
        p.indexCount = uint32_t(mesh.triangles.size());
        p.center = (lo + hi) * 0.5f;
        p.lo = lo, p.hi = hi;
        p.cullable = !dynamic && !v.empty();
        if (mat.mirror) {
            simd_float3 n = 0;
            for (const auto& vn : mesh.normals)
                n += simd_make_float3(vn[0], vn[1], vn[2]);
            if (simd_length(n) < 1e-6f && mesh.triangles.size() >= 3) { // no normals: the first triangle's
                const auto& a = mesh.positions[mesh.triangles[0]];
                const auto& b = mesh.positions[mesh.triangles[1]];
                const auto& c = mesh.positions[mesh.triangles[2]];
                n = simd_cross(simd_make_float3(b[0] - a[0], b[1] - a[1], b[2] - a[2]),
                               simd_make_float3(c[0] - a[0], c[1] - a[1], c[2] - a[2]));
            }
            p.normal = simd_length(n) > 1e-6f ? simd_normalize(n) : simd_make_float3(0, 0, 1);
        }
        p.material = mat;
        out->parts.push_back(p);
    }
    models_.push_back(std::move(out));
    pool->release();
    return int(models_.size() - 1);
}

void Renderer::updateObject(int model, const ObjectModel& posed) {
    if (model < 0 || size_t(model) >= models_.size())
        return;
    for (auto& p : models_[size_t(model)]->parts) {
        if (!p.slots[0] || p.source >= posed.parts.size())
            continue;
        const MeshGroup& mesh = posed.parts[p.source].mesh;
        const size_t n = p.vertices->length() / sizeof(Vertex);
        if (mesh.positions.size() != n)
            continue;
        if (p.pending.size() != n)
            p.pending.assign(static_cast<const Vertex*>(p.vertices->contents()),
                             static_cast<const Vertex*>(p.vertices->contents()) + n);
        simd_float3 lo = simd_make_float3(INFINITY, INFINITY, INFINITY), hi = -lo;
        for (size_t i = 0; i < n; ++i) {
            for (int k = 0; k < 3; ++k)
                p.pending[i].position[k] = mesh.positions[i][size_t(k)];
            if (i < mesh.normals.size())
                for (int k = 0; k < 3; ++k)
                    p.pending[i].normal[k] = mesh.normals[i][size_t(k)];
            const simd_float3 q = simd_make_float3(mesh.positions[i][0], mesh.positions[i][1], mesh.positions[i][2]);
            lo = simd_min(lo, q), hi = simd_max(hi, q);
        }
        // The posed box: the blended parts sort by it (an animated
        // object's, the PlumbBob's, was its bind pose's).
        if (n) {
            p.lo = lo, p.hi = hi;
            p.center = (lo + hi) * 0.5f;
        }
        p.stale.fill(true);
    }
}

void Renderer::setDynamicTexture(const std::string& name, const RgbaImage& image) {
    if (!image.width || !image.height || image.pixels.size() < size_t(image.width) * image.height * 4)
        return;
    MTL::Texture*& tex = textures_[name];
    if (!tex) {
        MTL::TextureDescriptor* d = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, image.width,
                                                                                image.height, false);
        d->setUsage(MTL::TextureUsageShaderRead);
        d->setStorageMode(MTL::StorageModeShared);
        tex = device_->newTexture(d);
    }
    if (tex->width() != image.width || tex->height() != image.height)
        return; // the materials hold this texture: its size stays
    tex->replaceRegion(MTL::Region::Make2D(0, 0, image.width, image.height), 0, image.pixels.data(),
                       size_t(image.width) * 4);
}

MTL::Texture* Renderer::rgbaTexture(const RgbaImage& image) {
    MTL::TextureDescriptor* d = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, image.width,
                                                                            image.height, false);
    d->setUsage(MTL::TextureUsageShaderRead);
    MTL::Texture* tex = device_->newTexture(d);
    tex->replaceRegion(MTL::Region::Make2D(0, 0, image.width, image.height), 0, image.pixels.data(),
                       size_t(image.width) * 4);
    return tex;
}

void Renderer::setLightMap(int slot, const RgbaImage& image) {
    if (slot < 0 || slot > 1 || !image.width || !image.height || image.pixels.size() < size_t(image.width) * image.height * 4)
        return;
    if (lightMaps_[size_t(slot)])
        retired_.push_back({lightMaps_[size_t(slot)], kFramesInFlight + 1});
    lightMaps_[size_t(slot)] = rgbaTexture(image);
}

MTL::Texture* Renderer::uploadMask(const ObjectModel::AlphaMask& m) {
    MTL::TextureDescriptor* d = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatR8Unorm, m.width,
                                                                            m.height, false);
    d->setUsage(MTL::TextureUsageShaderRead);
    MTL::Texture* tex = device_->newTexture(d);
    tex->replaceRegion(MTL::Region::Make2D(0, 0, m.width, m.height), 0, m.values.data(), m.width);
    return tex;
}

void Renderer::hideTestScene() {
    instances_.erase(instances_.begin(), instances_.begin() + std::ptrdiff_t(testInstances_));
    testInstances_ = 0;
}

Renderer::LightSet Renderer::outdoorLights(const LightingState& state, bool sims) {
    LightSet set;
    const simd_float3 sky = simd_make_float3(state.skyColor[0], state.skyColor[1], state.skyColor[2]) * state.skyStrength;
    set.ambient = sky;
    bool simFill = false;
    for (const auto& l : state.lights)
        simFill = simFill || lowerAsciiName(l.name).starts_with("simfill");
    for (const auto& l : state.lights) {
        const std::string name = lowerAsciiName(l.name);
        if (name.starts_with("simfill") ? !sims : sims && simFill && (name == "fill1" || name == "fill2"))
            continue;
        const simd_float3 dir = simd_make_float3(l.direction[0], l.direction[1], l.direction[2]);
        if (simd_length(dir) == 0)
            continue;
        const simd_float3 color = simd_max(simd_make_float3(l.color[0], l.color[1], l.color[2]) - sky, 0);
        set.lights.push_back({simd_normalize(dir), color * l.intensity});
    }
    return set;
}

void Renderer::setLighting(const LightingState& state) {
    environment_ = simd_make_float3(state.environment[0], state.environment[1], state.environment[2]);
    if (lightSets_.size() < 2)
        lightSets_.resize(2);
    lightSets_[0] = outdoorLights(state, false);
    lightSets_[1] = outdoorLights(state, true);
}

void Renderer::setLightSets(std::vector<LightSet> sets) {
    if (sets.size() > kMaxLightSets)
        sets.resize(kMaxLightSets);
    lightSets_ = std::move(sets);
}

void Renderer::setInstanceLightSet(int instance, int set) {
    if (instance >= 0 && size_t(instance) < instances_.size())
        instances_[size_t(instance)].lightSet = std::max(0, set);
}

void Renderer::setInstanceAlpha(int instance, float alpha) {
    if (instance >= 0 && size_t(instance) < instances_.size())
        instances_[size_t(instance)].alpha = std::clamp(alpha, 0.0f, 1.0f);
}

std::optional<std::pair<simd_float3, simd_float3>> Renderer::instanceBounds(int instance) const {
    if (instance < 0 || size_t(instance) >= instances_.size())
        return std::nullopt;
    const Instance& inst = instances_[size_t(instance)];
    if (inst.model < 0 || size_t(inst.model) >= models_.size() || models_[size_t(inst.model)]->parts.empty())
        return std::nullopt;
    simd_float3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
    for (const Part& p : models_[size_t(inst.model)]->parts)
        for (int k = 0; k < 8; ++k) {
            const simd_float3 corner = simd_make_float3(k & 1 ? p.hi.x : p.lo.x, k & 2 ? p.hi.y : p.lo.y, k & 4 ? p.hi.z : p.lo.z);
            const simd_float3 w = simd_make_float3(simd_mul(inst.transform, simd_make_float4(corner, 1)));
            lo = simd_min(lo, w), hi = simd_max(hi, w);
        }
    return std::pair{lo, hi};
}

int Renderer::instanceOf(int model) const {
    for (size_t i = 0; i < instances_.size(); ++i)
        if (instances_[i].model == model)
            return int(i);
    return -1;
}

int Renderer::addInstance(int model, const simd_float4x4& transform) {
    if (model < 0 || size_t(model) >= models_.size())
        return -1;
    instances_.push_back({model, transform});
    return int(instances_.size() - 1);
}

void Renderer::setInstanceTransform(int instance, const simd_float4x4& transform) {
    if (instance >= 0 && size_t(instance) < instances_.size())
        instances_[size_t(instance)].transform = transform;
}

simd_float4x4 Renderer::instanceTransform(int instance) const {
    return instance >= 0 && size_t(instance) < instances_.size() ? instances_[size_t(instance)].transform : identity();
}

int Renderer::instanceModel(int instance) const {
    return instance >= 0 && size_t(instance) < instances_.size() ? instances_[size_t(instance)].model : -1;
}

void Renderer::setInstanceModel(int instance, int model) {
    if (instance >= 0 && size_t(instance) < instances_.size() && model >= 0 && size_t(model) < models_.size())
        instances_[size_t(instance)].model = model;
}

void Renderer::retargetInstances(int from, int to) {
    if (to < 0 || size_t(to) >= models_.size())
        return;
    for (auto& i : instances_)
        if (i.model == from)
            i.model = to;
}

// Whether a part's model-space box is wholly outside the clip volume of
// `viewProj * model` (Metal's: -w <= x, y <= w, 0 <= z <= w). The clip volume
// is an intersection of half-spaces and the map is linear, so when all 8
// corners are outside the same one, so is every triangle of the part: it
// would draw nothing. A tiny margin keeps boxes that only graze an edge.
static bool outsideFrustum(const simd_float4x4& viewProj, const simd_float4x4& model, simd_float3 lo, simd_float3 hi) {
    const simd_float4x4 m = simd_mul(viewProj, model);
    int outside[6] = {0, 0, 0, 0, 0, 0};
    for (int c = 0; c < 8; ++c) {
        const simd_float4 p = simd_mul(m, simd_make_float4(c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2], 1));
        const float e = 1e-4f * (std::abs(p.w) + 1.0f);
        outside[0] += p.x + p.w < -e;
        outside[1] += p.w - p.x < -e;
        outside[2] += p.y + p.w < -e;
        outside[3] += p.w - p.y < -e;
        outside[4] += p.z < -e;
        outside[5] += p.w - p.z < -e;
    }
    for (int k = 0; k < 6; ++k)
        if (outside[k] == 8)
            return true;
    return false;
}

MTL::CommandBuffer* Renderer::encodeFrame(MTL::RenderPassDescriptor* pass, const FrameView& view) {
    dispatch_semaphore_wait(sema(inFlight_), DISPATCH_TIME_FOREVER);
    frame_ = (frame_ + 1) % kFramesInFlight;

    auto* f = static_cast<Frame*>(frames_[frame_]->contents());
    f->viewProj = simd_mul(view.projection, view.view);
    f->view = view.view;
    f->eye = simd_inverse(view.view).columns[3];
    auto* sets = static_cast<GpuLightSet*>(lightBuffers_[size_t(frame_)]->contents());
    for (size_t i = 0; i < lightSets_.size() && i < kMaxLightSets; ++i) {
        const LightSet& ls = lightSets_[i];
        const size_t n = std::min(ls.lights.size(), kMaxLights);
        const size_t np = std::min(ls.points.size(), kMaxLights - n);
        sets[i].ambient = simd_make_float4(ls.ambient, float(n + np));
        for (size_t k = 0; k < n; ++k)
            sets[i].lights[k] = {simd_make_float4(ls.lights[k].direction, 0), simd_make_float4(ls.lights[k].color, ls.lights[k].specular),
                                 simd_make_float4(0, 0, 0, 0), simd_make_float4(0, 0, -1, -2)};
        for (size_t k = 0; k < np; ++k) {
            const auto& p = ls.points[k];
            sets[i].lights[n + k] = {simd_make_float4(p.position, 1), simd_make_float4(p.color, p.specular),
                                     simd_make_float4(p.falloff, p.range, 1, p.cosInner),
                                     simd_make_float4(simd_normalize(p.spotDirection), p.cosOuter)};
        }
    }
    const double now = time_ >= 0 ? time_ : std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    f->params = simd_make_float4(std::clamp(lightMapMix_, 0.0f, 1.0f), float(std::fmod(now, 3600.0)), 0, 0);
    f->environment = simd_make_float4(environment_, 1);
    // Light maps replaced a few frames ago are no longer read.
    for (auto& r : retired_)
        if (--r.second <= 0)
            r.first->release();
    std::erase_if(retired_, [](const auto& r) { return r.second <= 0; });
    for (auto& r : retiredModels_)
        --r.second;
    std::erase_if(retiredModels_, [](const auto& r) { return r.second <= 0; });
    MTL::Texture* lightMap0 = lightMaps_[0] ? lightMaps_[0] : lightMaps_[1] ? lightMaps_[1] : grey_;
    MTL::Texture* lightMap1 = lightMaps_[1] ? lightMaps_[1] : lightMap0;

    // The GPU is done with this slot's buffers: bring dynamic vertices up to date.
    for (auto& m : models_)
        for (auto& p : m->parts)
            if (p.slots[0] && p.stale[size_t(frame_)]) {
                std::memcpy(p.slots[size_t(frame_)]->contents(), p.pending.data(), p.pending.size() * sizeof(Vertex));
                p.stale[size_t(frame_)] = false;
            }

    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    dispatch_semaphore_t sem = sema(inFlight_);
    cmd->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });

    // Draw order (cDisplayList::AddGeometry BV 0xca8598, cLessGeomPartInfo):
    // by key (stdMatLayer x 8, +7 for blended parts), opaque parts in
    // submission order, blended ones by depth, the farthest first: a part's
    // depth is the z / w of the nearest corner of its box (BV 0x86d4c), a
    // particle group's that of its box's centre (0x86ed4; external items).
    struct Item {
        int layer;
        bool blended;
        float depth;
        int alphaOrder;
        const Instance* inst;
        const Part* part;
        int external = -1;
    };
    std::vector<Item> items;
    const simd_float3 eye = simd_make_float3(f->eye); // (the mirror below)
    auto ndcDepth = [&](simd_float4 p) { return p.w > 1e-5f ? p.z / p.w : 0.0f; };
    for (const auto& inst : instances_)
        for (const auto& part : models_[size_t(inst.model)]->parts) {
            if (part.shadowPlane && !graphicsOptions().guob)
                continue;
            const Material& m = part.material;
            const bool blended = m.blend != StandardMaterial::Blend::None || inst.alpha < 1;
            float depth = 0;
            if (blended) {
                if (m.alphaOrder) {
                    // A hairdo's layers sort together, by the instance's
                    // origin, then in their alpha order (the game keeps one
                    // geometry's parts in order with a 1e-5 step; inferred here).
                    depth = ndcDepth(simd_mul(f->viewProj, inst.transform.columns[3]));
                } else {
                    const simd_float4x4 mvp = simd_mul(f->viewProj, inst.transform);
                    depth = 1e6f;
                    for (int c = 0; c < 8; ++c)
                        depth = std::min(depth, ndcDepth(simd_mul(mvp, simd_make_float4(c & 1 ? part.hi[0] : part.lo[0],
                                                                                         c & 2 ? part.hi[1] : part.lo[1],
                                                                                         c & 4 ? part.hi[2] : part.lo[2], 1))));
                }
            }
            items.push_back({m.layer, blended, depth, m.alphaOrder, &inst, &part});
        }
    if (externalItems_) {
        std::vector<ExternalItem> external;
        externalItems_(view.view, f->viewProj, external);
        for (const auto& e : external)
            items.push_back({e.layer, true, e.depth, 0, nullptr, nullptr, e.id});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.layer != b.layer)
            return a.layer < b.layer;
        if (a.blended != b.blended)
            return !a.blended;
        if (a.blended && a.depth != b.depth)
            return a.depth > b.depth;
        return a.blended && a.alphaOrder < b.alphaOrder;
    });

    MTL::Texture* target = pass->colorAttachments()->object(0)->texture();
    const uint32_t targetW = target ? uint32_t(target->width()) : 1, targetH = target ? uint32_t(target->height()) : 1;
    const float ground = std::isfinite(groundClip_) ? groundClip_ : -1e30f;
    f->viewport = simd_make_float4(float(targetW), float(targetH), 0, ground);
    f->clip = simd_make_float4(0, 0, 1, 0);

    // The mirror that reflects this frame: the nearest one in view, its
    // plane turned towards the eye (MirrorReflectionMaterial's camera is
    // reflected in it and clipped to its front).
    const Item* mirror = nullptr;
    simd_float4 plane{0, 0, 1, 0};
    if (reflections_ && target) {
        float best = INFINITY;
        for (const auto& item : items) {
            if (!item.part || !item.part->material.mirror)
                continue;
            const simd_float4x4& m = item.inst->transform;
            const simd_float3 c = simd_make_float3(simd_mul(m, simd_make_float4(item.part->center, 1)));
            const simd_float4 clipC = simd_mul(f->viewProj, simd_make_float4(c, 1));
            if (clipC.w <= 0 || std::abs(clipC.x) > clipC.w * 1.3f || std::abs(clipC.y) > clipC.w * 1.3f)
                continue;
            simd_float3 n = simd_normalize(simd_make_float3(simd_mul(m, simd_make_float4(item.part->normal, 0))));
            if (simd_dot(n, eye - c) < 0)
                n = -n;
            const float d2 = simd_length_squared(c - eye);
            if (d2 < best)
                best = d2, mirror = &item, plane = simd_make_float4(n, -simd_dot(n, c));
        }
    }

    // Sims' shadows: each caster's silhouette into its tile of the atlas,
    // and its quad on the floor (drawn after the opaque parts).
    size_t shadowCount = 0;
    if (!casters_.empty()) {
        if (!shadowAtlas_) {
            MTL::TextureDescriptor* cd =
                MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormat(colorFormat_), kShadowAtlas, kShadowAtlas, false);
            cd->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            cd->setStorageMode(MTL::StorageModePrivate);
            shadowAtlas_ = device_->newTexture(cd);
            MTL::TextureDescriptor* dd =
                MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormat(depthFormat_), kShadowAtlas, kShadowAtlas, false);
            dd->setUsage(MTL::TextureUsageRenderTarget);
            dd->setStorageMode(MTL::StorageModePrivate);
            shadowDepth_ = device_->newTexture(dd);
            for (auto& b : shadowBuffers_)
                b = device_->newBuffer(sizeof(GpuShadow) * kMaxShadowCasters, MTL::ResourceStorageModeShared);
        }
        auto* gpu = static_cast<GpuShadow*>(shadowBuffers_[size_t(frame_)]->contents());
        MTL::RenderPassDescriptor* sp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* sa = sp->colorAttachments()->object(0);
        sa->setTexture(shadowAtlas_);
        sa->setLoadAction(MTL::LoadActionClear);
        sa->setStoreAction(MTL::StoreActionStore);
        sa->setClearColor(MTL::ClearColor::Make(1, 1, 1, 1));
        sp->depthAttachment()->setTexture(shadowDepth_);
        sp->depthAttachment()->setLoadAction(MTL::LoadActionClear);
        sp->depthAttachment()->setStoreAction(MTL::StoreActionDontCare);
        sp->depthAttachment()->setClearDepth(1.0);
        MTL::RenderCommandEncoder* senc = cmd->renderCommandEncoder(sp);
        senc->setRenderPipelineState(opaque_);
        senc->setDepthStencilState(depthWrite_);
        senc->setCullMode(MTL::CullModeNone);
        senc->setFragmentBuffer(lightBuffers_[size_t(frame_)], 0, 3);
        senc->setFragmentSamplerState(sampler_, 0);
        senc->setFragmentSamplerState(maskSampler_, 1);
        senc->setFragmentSamplerState(cubeSampler_, 2);
        senc->setFragmentSamplerState(pointSampler_, 3);
        for (NS::UInteger k = 1; k <= 19; ++k)
            if (k != 6)
                senc->setFragmentTexture(white_, k);
        senc->setFragmentTexture(whiteCube_, 6);
        size_t n = 0;
        for (const auto& c : casters_) {
            if (n >= kMaxShadowCasters || c.instance < 0 || size_t(c.instance) >= instances_.size())
                continue;
            const Instance& inst = instances_[size_t(c.instance)];
            // The viewer's frame: looking along the light, turned with the Sim.
            const simd_float3 fwd = simd_normalize(c.direction);
            const simd_float3 hx = simd_make_float3(std::cos(c.heading), std::sin(c.heading), 0);
            const simd_float3 hy = simd_make_float3(-hx.y, hx.x, 0);
            simd_float3 r = simd_cross(fwd, hy);
            if (simd_length_squared(r) < 1e-6f)
                r = simd_cross(fwd, hx);
            r = simd_normalize(r);
            const simd_float3 u = simd_cross(r, fwd);
            // UpdateInsideOutsideSettings: the box's extent seen from the light.
            float a0 = INFINITY, a1 = -INFINITY, b0 = INFINITY, b1 = -INFINITY, d0 = INFINITY, d1 = -INFINITY;
            for (int k = 0; k < 8; ++k) {
                const simd_float3 q = (k & 1 ? kShadowW0 : -kShadowW0) * hx + (k & 2 ? kShadowW1 : -kShadowW1) * hy +
                                      simd_make_float3(0, 0, k & 4 ? kShadowH1 : kShadowH0);
                a0 = std::min(a0, simd_dot(q, r)), a1 = std::max(a1, simd_dot(q, r));
                b0 = std::min(b0, simd_dot(q, u)), b1 = std::max(b1, simd_dot(q, u));
                d0 = std::min(d0, simd_dot(q, fwd)), d1 = std::max(d1, simd_dot(q, fwd));
            }
            d0 -= 1, d1 += 1;
            // CreateShadow: the quad shows the inner (resolution - 2 blur) /
            // resolution of the texture, the blur's margin around it.
            const int blur = c.outside ? kOutdoorShadowBlur : kIndoorShadowBlur;
            const float inner = float(kShadowResolution - 2 * blur) / float(kShadowResolution);
            const float ma = (a1 - a0) * (1 / inner - 1) / 2, mb = (b1 - b0) * (1 / inner - 1) / 2;
            const float A0 = a0 - ma, Aw = (a1 - a0) + 2 * ma, B0 = b0 - mb, Bh = (b1 - b0) + 2 * mb, Dd = d1 - d0;
            const simd_float3 p = c.feet;
            const simd_float4x4 vp = simd_matrix_from_rows(
                simd_make_float4(2 * r / Aw, -2 * (simd_dot(r, p) + A0) / Aw - 1),
                simd_make_float4(2 * u / Bh, -2 * (simd_dot(u, p) + B0) / Bh - 1),
                simd_make_float4(fwd / Dd, -(simd_dot(fwd, p) + d0) / Dd), simd_make_float4(0, 0, 0, 1));
            const int tx = int(n) % kShadowAtlasTiles, ty = int(n) / kShadowAtlasTiles;
            MTL::Viewport vpt{double(tx * kShadowResolution), double(ty * kShadowResolution), double(kShadowResolution),
                              double(kShadowResolution), 0.0, 1.0};
            senc->setViewport(vpt);
            Frame sf = *f;
            sf.viewProj = vp;
            sf.viewport = simd_make_float4(kShadowResolution, kShadowResolution, 0, f->viewport.w);
            senc->setVertexBytes(&sf, sizeof sf, 2);
            senc->setFragmentBytes(&sf, sizeof sf, 2);
            for (const auto& part : models_[size_t(inst.model)]->parts) {
                if (part.shadowPlane)
                    continue;
                const Material& m = part.material;
                const bool cut = m.blend != StandardMaterial::Blend::None || (m.flags & kAlphaTest);
                Draw d{inst.transform, simd_make_float4(kStdMatShadowColor, 1), {}, {}, m.maskTransform, {},
                       simd_make_float4(1, 0, 0, 0), simd_make_float4(0, 1, 0, 0), {}, {},
                       m.blend != StandardMaterial::Blend::None ? 0.5f : m.alphaRef,
                       uint32_t(kShadowCaster) | (cut ? uint32_t(kAlphaTest) : 0u), 0};
                senc->setVertexBytes(&d, sizeof d, 1);
                senc->setFragmentBytes(&d, sizeof d, 1);
                senc->setVertexBuffer(part.slots[0] ? part.slots[size_t(frame_)] : part.vertices, 0, 0);
                senc->setFragmentTexture(m.texture, 0);
                senc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(part.indexCount), MTL::IndexTypeUInt32,
                                            part.indices, NS::UInteger(0));
            }
            // World -> atlas uv (the viewer's x and y, into the tile; v down).
            const float S = float(kShadowResolution) / float(kShadowAtlas);
            const simd_float4 mu = simd_make_float4(r * (S / Aw), float(tx) * S - S * (simd_dot(r, p) + A0) / Aw);
            const simd_float4 mv = simd_make_float4(-u * (S / Bh), float(ty + 1) * S + S * (simd_dot(u, p) + B0) / Bh);
            const float h = 0.5f / float(kShadowAtlas);
            const simd_float4 rect = simd_make_float4(float(tx) * S + h, float(ty) * S + h, float(tx + 1) * S - h, float(ty + 1) * S - h);
            gpu[n] = {mu, mv, rect, simd_make_float4(float(blur) / float(kShadowAtlas), c.outside ? 1.0f : 0.0f, p.z, 0),
                      simd_make_float4(c.color, 1)};
            ++n;
        }
        senc->endEncoding();
        shadowCount = n;
    }
    f->params.z = float(shadowCount);

    const double t = time_ >= 0 ? time_ : std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    // `reflected`: the pass into the reflection texture (mirrors left out).
    // `frameSlot`: the Frame used (0 the view, 1 the reflected view, 2 + i
    // the censor's box i); off the view (slot > 0) no mirror reflects.
    auto drawItems = [&](MTL::RenderCommandEncoder* enc, bool reflected, const simd_float4x4& passViewProj, size_t frameSlot) {
        enc->setVertexBuffer(frames_[frame_], frameSlot * kFrameStride, 2);
        enc->setFragmentBuffer(frames_[frame_], frameSlot * kFrameStride, 2);
        enc->setFragmentBuffer(lightBuffers_[size_t(frame_)], 0, 3);
        enc->setFragmentSamplerState(sampler_, 0);
        enc->setFragmentSamplerState(maskSampler_, 1);
        enc->setFragmentSamplerState(cubeSampler_, 2);
        enc->setFragmentSamplerState(pointSampler_, 3);
        enc->setFragmentTexture(lightMap0, 10);
        enc->setFragmentTexture(lightMap1, 11);
        enc->setFragmentTexture(mirror && !reflected ? reflectionColor_ : white_, 18);
        enc->setFragmentTexture(shadowCount ? shadowAtlas_ : white_, 19);
        enc->setFragmentBuffer(shadowCount ? shadowBuffers_[size_t(frame_)] : lightBuffers_[size_t(frame_)], 0, 4);
        for (const auto& item : items) {
            if (item.external >= 0) {
                // A particle group: only in the view itself (not the mirror's
                // nor the censor's renders).
                if (frameSlot == 0 && externalDraw_)
                    externalDraw_(enc, item.external);
                continue;
            }
            const Part& part = *item.part;
            const Material& m = part.material;
            if (reflected && m.mirror)
                continue;
            if (part.cullable && outsideFrustum(passViewProj, item.inst->transform, part.lo, part.hi))
                continue;
            const bool faded = item.inst->alpha < 1;
            enc->setDepthStencilState((m.blend == StandardMaterial::Blend::None && !faded) || m.depthWrite ? depthWrite_ : depthRead_);
            enc->setRenderPipelineState(m.blend == StandardMaterial::Blend::Alpha || (faded && m.blend == StandardMaterial::Blend::None)
                                            ? alphaBlend_
                                        : m.blend == StandardMaterial::Blend::Additive ? additive_
                                        : m.blend == StandardMaterial::Blend::AddNoSrcAlpha ? addOne_
                                        : m.blend == StandardMaterial::Blend::Premultiplied ? premultiplied_
                                        : m.blend == StandardMaterial::Blend::Multiply ? multiply_
                                                                                       : opaque_);
            enc->setCullMode(m.cull);
            auto [uv0, uv1] = uvTransform(m.anim, t);
            uv0 *= m.uvScale, uv1 *= m.uvScale;
            const int set = item.inst->lightSet < int(std::min(lightSets_.size(), kMaxLightSets)) ? item.inst->lightSet : 0;
            const uint32_t flags = m.flags | (frameSlot == 0 && &item == mirror ? uint32_t(kMirror) : 0u);
            simd_float4 color = m.color;
            color.w *= item.inst->alpha;
            Draw d{item.inst->transform, color, m.specular, m.emissive, m.maskTransform, m.env, uv0, uv1, m.misc,
                   m.maskV, m.alphaRef, flags, uint32_t(set)};
            enc->setVertexBytes(&d, sizeof d, 1);
            enc->setFragmentBytes(&d, sizeof d, 1);
            enc->setVertexBuffer(part.slots[0] ? part.slots[size_t(frame_)] : part.vertices, 0, 0);
            enc->setFragmentSamplerState(m.clamp ? clampSampler_ : sampler_, 0);
            enc->setFragmentTexture(m.texture, 0);
            enc->setFragmentTexture(part.mask ? part.mask : white_, 1);
            for (size_t l = 0; l < m.layers.size(); ++l) // textures 2-4, overlay 3 at 9, overlays 4-8 at 13-17
                enc->setFragmentTexture(m.layers[l] ? m.layers[l] : white_, NS::UInteger(l < 3 ? 2 + l : l == 3 ? 9 : 9 + l));
            enc->setFragmentTexture(m.height ? m.height : white_, 5);
            enc->setFragmentTexture(m.envCube ? m.envCube : whiteCube_, 6);
            enc->setFragmentTexture(m.ramp ? m.ramp : white_, 7);
            enc->setFragmentTexture(m.edgeRamp ? m.edgeRamp : white_, 8);
            enc->setFragmentTexture(m.detail ? m.detail : white_, 12);
            enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(part.indexCount), MTL::IndexTypeUInt32,
                                       part.indices, NS::UInteger(0));
        }
    };

    // The censor: each box's rectangle rendered again into its tile of
    // kCensorBlocks x kCensorBlocks texels (RenderPartPtrs ->
    // SelectAndClearSurfaces, RenderGeometryForCensor). Run for the view and,
    // when a mirror reflects, for the reflected view too (kRenderTypeCensorInMirror:
    // the mirror's reflection of a nude Sim is pixelated the same way).
    using Censored = std::vector<std::pair<CensorRect, int>>;
    auto censorRects = [&](const simd_float4x4& viewProj) {
        Censored out;
        if (target)
            for (const CensorBox& box : censorBoxes_) {
                if (out.size() >= size_t(kMaxCensorBoxes))
                    break;
                if (auto r = censorRect(box, viewProj, targetW, targetH))
                    out.push_back({*r, int(out.size())});
            }
        return out;
    };
    // `src`: the Frame of the view (0 or 1); the tiles use frame slots from `firstSlot`.
    auto censorTiles = [&](const Censored& list, const Frame& src, size_t firstSlot, MTL::Winding winding) {
        if (list.empty())
            return;
        if (!censorColor_) {
            MTL::TextureDescriptor* cd = MTL::TextureDescriptor::texture2DDescriptor(
                MTL::PixelFormat(colorFormat_), kCensorBlocks * kMaxCensorBoxes, kCensorBlocks, false);
            cd->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            cd->setStorageMode(MTL::StorageModePrivate);
            censorColor_ = device_->newTexture(cd);
            MTL::TextureDescriptor* dd = MTL::TextureDescriptor::texture2DDescriptor(
                MTL::PixelFormat(depthFormat_), kCensorBlocks * kMaxCensorBoxes, kCensorBlocks, false);
            dd->setUsage(MTL::TextureUsageRenderTarget);
            dd->setStorageMode(MTL::StorageModePrivate);
            censorDepth_ = device_->newTexture(dd);
        }
        MTL::RenderPassDescriptor* cp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* ca = cp->colorAttachments()->object(0);
        ca->setTexture(censorColor_);
        ca->setLoadAction(MTL::LoadActionClear);
        ca->setStoreAction(MTL::StoreActionStore);
        ca->setClearColor(pass->colorAttachments()->object(0)->clearColor());
        cp->depthAttachment()->setTexture(censorDepth_);
        cp->depthAttachment()->setLoadAction(MTL::LoadActionClear);
        cp->depthAttachment()->setStoreAction(MTL::StoreActionDontCare);
        cp->depthAttachment()->setClearDepth(1.0);
        MTL::RenderCommandEncoder* cenc = cmd->renderCommandEncoder(cp);
        cenc->setFrontFacingWinding(winding);
        for (const auto& [r, i] : list) {
            // Clip space of the view -> the rectangle filling the tile.
            const float sx = 2 / (r.nx1 - r.nx0), sy = 2 / (r.ny1 - r.ny0);
            simd_float4x4 crop = matrix_identity_float4x4;
            crop.columns[0][0] = sx;
            crop.columns[1][1] = sy;
            crop.columns[3][0] = -(r.nx1 + r.nx0) / (r.nx1 - r.nx0);
            crop.columns[3][1] = -(r.ny1 + r.ny0) / (r.ny1 - r.ny0);
            auto* cf = reinterpret_cast<Frame*>(static_cast<char*>(frames_[frame_]->contents()) + (firstSlot + size_t(i)) * kFrameStride);
            *cf = src;
            cf->viewProj = simd_mul(crop, src.viewProj);
            cf->viewport = simd_make_float4(kCensorBlocks, kCensorBlocks, src.viewport.z, src.viewport.w);
            cenc->setViewport(MTL::Viewport{double(i * kCensorBlocks), 0.0, double(kCensorBlocks), double(kCensorBlocks), 0.0, 1.0});
            drawItems(cenc, src.viewport.z > 0.5f, cf->viewProj, firstSlot + size_t(i));
        }
        cenc->endEncoding();
    };
    // The compositing quads (CensorMaterial, after the transparent parts;
    // layer -1 keeps it out of its own render).
    auto censorComposite = [&](MTL::RenderCommandEncoder* e, const Censored& list) {
        e->setRenderPipelineState(censor_);
        e->setDepthStencilState(depthRead_);
        e->setCullMode(MTL::CullModeNone);
        e->setFragmentTexture(censorColor_, 0);
        e->setFragmentSamplerState(pointSampler_, 0);
        const float tile = 1.0f / float(kMaxCensorBoxes);
        for (const auto& [r, i] : list) {
            struct {
                simd_float4 rect, uvRect, target;
            } q{simd_make_float4(float(r.x0), float(r.y0), float(r.x1), float(r.y1)),
                simd_make_float4(float(i) * tile, 0, float(i + 1) * tile, 1),
                simd_make_float4(float(targetW), float(targetH), r.depth, 0)};
            e->setVertexBytes(&q, sizeof q, 0);
            e->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
        }
    };

    if (mirror) {
        // The reflected frame: world -> mirrored world -> the camera.
        const simd_float3 n = simd_make_float3(plane);
        const float dd = plane.w;
        simd_float4x4 r = matrix_identity_float4x4;
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                r.columns[c][k] -= 2 * n[k] * n[c];
        r.columns[3] = simd_make_float4(-2 * dd * n, 1);
        auto* rf = reinterpret_cast<Frame*>(static_cast<char*>(frames_[frame_]->contents()) + kFrameStride);
        *rf = *f;
        rf->view = simd_mul(view.view, r);
        rf->viewProj = simd_mul(view.projection, rf->view);
        rf->eye = simd_mul(r, f->eye);
        rf->clip = plane;
        rf->viewport.z = 1;
        if (!reflectionColor_ || reflectionColor_->width() != targetW || reflectionColor_->height() != targetH) {
            if (reflectionColor_)
                retired_.push_back({reflectionColor_, kFramesInFlight + 1});
            if (reflectionDepth_)
                retired_.push_back({reflectionDepth_, kFramesInFlight + 1});
            MTL::TextureDescriptor* cd =
                MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormat(colorFormat_), targetW, targetH, false);
            cd->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            cd->setStorageMode(MTL::StorageModePrivate);
            reflectionColor_ = device_->newTexture(cd);
            MTL::TextureDescriptor* dd2 =
                MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormat(depthFormat_), targetW, targetH, false);
            dd2->setUsage(MTL::TextureUsageRenderTarget);
            dd2->setStorageMode(MTL::StorageModePrivate);
            reflectionDepth_ = device_->newTexture(dd2);
        }
        MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* ca = rp->colorAttachments()->object(0);
        ca->setTexture(reflectionColor_);
        ca->setLoadAction(MTL::LoadActionClear);
        ca->setStoreAction(MTL::StoreActionStore);
        ca->setClearColor(pass->colorAttachments()->object(0)->clearColor());
        rp->depthAttachment()->setTexture(reflectionDepth_);
        rp->depthAttachment()->setLoadAction(MTL::LoadActionClear);
        rp->depthAttachment()->setStoreAction(censorBoxes_.empty() ? MTL::StoreActionDontCare : MTL::StoreActionStore);
        rp->depthAttachment()->setClearDepth(1.0);
        MTL::RenderCommandEncoder* renc = cmd->renderCommandEncoder(rp);
        renc->setFrontFacingWinding(MTL::WindingClockwise); // the reflection turns the windings around
        drawItems(renc, true, rf->viewProj, 1);
        renc->endEncoding();
        // The reflection of a nude Sim is censored in the reflected view.
        const Censored inMirror = std::getenv("OS2_NO_MIRROR_CENSOR") ? Censored{} : censorRects(rf->viewProj);
        if (!inMirror.empty()) {
            censorTiles(inMirror, *rf, size_t(2 + kMaxCensorBoxes), MTL::WindingClockwise);
            MTL::RenderPassDescriptor* mp = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* mc = mp->colorAttachments()->object(0);
            mc->setTexture(reflectionColor_);
            mc->setLoadAction(MTL::LoadActionLoad);
            mc->setStoreAction(MTL::StoreActionStore);
            mp->depthAttachment()->setTexture(reflectionDepth_);
            mp->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
            mp->depthAttachment()->setStoreAction(MTL::StoreActionDontCare);
            MTL::RenderCommandEncoder* menc = cmd->renderCommandEncoder(mp);
            censorComposite(menc, inMirror);
            menc->endEncoding();
        }
    }

    const Censored censored = censorRects(f->viewProj);
    censorTiles(censored, *f, 2, MTL::WindingCounterClockwise);

    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
    drawItems(enc, false, f->viewProj, 0);
    if (!censored.empty())
        censorComposite(enc, censored);
    if (sceneHook_)
        sceneHook_(enc, view.view, f->viewProj);
    if (overlay_ && target)
        overlay_(enc, targetW, targetH);
    enc->endEncoding();
    return cmd;
}

std::optional<Renderer::CensorRect> Renderer::censorRect(const CensorBox& box, const simd_float4x4& viewProj, uint32_t width,
                                                        uint32_t height) {
    if (!(box.lo.x <= box.hi.x && box.lo.y <= box.hi.y && box.lo.z <= box.hi.z) || width == 0 || height == 0)
        return std::nullopt;
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY, z0 = INFINITY;
    for (int k = 0; k < 8; ++k) {
        const simd_float4 c = simd_mul(viewProj, simd_make_float4(k & 1 ? box.hi.x : box.lo.x, k & 2 ? box.hi.y : box.lo.y,
                                                                   k & 4 ? box.hi.z : box.lo.z, 1));
        if (c.w <= 1e-4f)
            return std::nullopt; // reaches behind the eye (inferred: the game projects it anyway)
        const float px = (c.x / c.w * 0.5f + 0.5f) * float(width), py = (0.5f - c.y / c.w * 0.5f) * float(height);
        x0 = std::min(x0, px), x1 = std::max(x1, px), y0 = std::min(y0, py), y1 = std::max(y1, py);
        z0 = std::min(z0, c.z / c.w);
    }
    // AddCompositingPassGeometries: rounded outwards to pixels, clipped to
    // the viewport, dropped when under 0.01 pixel either way.
    CensorRect r;
    r.x0 = std::max(0, int(std::floor(x0))), r.y0 = std::max(0, int(std::floor(y0)));
    r.x1 = std::min(int(width), int(std::ceil(x1))), r.y1 = std::min(int(height), int(std::ceil(y1)));
    if (r.x1 - r.x0 < 1 || r.y1 - r.y0 < 1)
        return std::nullopt;
    r.depth = std::clamp(z0, 0.0f, 1.0f);
    r.nx0 = float(r.x0) / float(width) * 2 - 1, r.nx1 = float(r.x1) / float(width) * 2 - 1;
    r.ny0 = 1 - float(r.y1) / float(height) * 2, r.ny1 = 1 - float(r.y0) / float(height) * 2;
    return r;
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
