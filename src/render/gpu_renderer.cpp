#include "render/gpu_renderer.hpp"

#include "formats/image.hpp"
#include "formats/lighting_config.hpp"
#include "formats/resindex.hpp"
#include "render/particle_system.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2 {

namespace {

constexpr uint32_t kTypeTxtr = 0x1C4A276C;
constexpr uint32_t kLighting = 1;
constexpr uint32_t kAlphaTest = 2;
constexpr uint32_t kMasked = 8;
constexpr uint32_t kVertexColor = 16384;
constexpr uint32_t kLightMapped = 65536;
constexpr uint32_t kWater = 524288;
constexpr uint32_t kTwoSidedLight = 1u << 25;
constexpr uint32_t kWallMask = 1u << 27;
constexpr uint32_t kMirror = 1u << 28;
constexpr uint32_t kShadowCaster = 1u << 29;
constexpr int kShadowResolution = 128;
constexpr int kShadowTiles = 8;
constexpr int kShadowAtlasSize = kShadowResolution * kShadowTiles;
constexpr int kMaxShadowCasters = 16;
constexpr int kCensorBlocks = 8;
constexpr int kMaxCensorBoxes = 16;

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    float uv2[2];
    uint32_t color = 0xFFFFFFFF;
};

struct SpriteVertex {
    float pixel[2];
    float uv[2];
    float color[4];
};

struct DrawUniforms {
    float viewProj[16];
    float model[16];
    float color[4];
    float uv0[4];
    float uv1[4];
    float maskU[4];
    float maskV[4];
    uint32_t flags = 0;
    float alphaRef = 0.5f;
    float lightGain = 2.f;
    float groundClip = -1e9f;
};

struct FragmentUniforms {
    DrawUniforms draw;
    float eye[4]{};
    float specular[4]{};
    float emissive[4]{};
    float ambient[4]{};
    float shadowCount = 0;
    float viewportX = 1, viewportY = 1, clipOn = 0;
    float clipPlane[4]{};
    float lightMapMix[4]{};
};

struct GpuShadow {
    float u[4]{};
    float v[4]{};
    float rect[4]{};
    float misc[4]{};
    float color[4]{1, 1, 1, 1};
};

struct GpuLight {
    float direction[4]{};
    float color[4]{};
    float params[4]{};
    float spot[4]{0, 0, -1, -2};
};

static_assert(sizeof(Vertex) == 44, "vertex pitch");
static_assert(sizeof(DrawUniforms) % 16 == 0, "vertex uniform alignment");
static_assert(sizeof(FragmentUniforms) == sizeof(DrawUniforms) + 112, "fragment uniform layout");
static_assert(sizeof(GpuLight) == 64, "light layout");

constexpr size_t kMaxLights = 16;

// A light set as the lot shader reads it: the ambient (w: light count) and
// the directional lights, then the point and spot lights.
struct GpuLightSet {
    float ambient[4]{};
    GpuLight lights[kMaxLights]{};
};
static_assert(sizeof(GpuShadow) == 80, "shadow layout");

std::runtime_error sdlError(const char* what) {
    return std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

void storeMatrix(const simd_float4x4& m, float* out) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out[c * 4 + r] = m.columns[c][r];
}

void copyInto(SDL_GPUDevice* device, SDL_GPUTransferBuffer* buffer, const void* data, size_t size) {
    void* mapped = SDL_MapGPUTransferBuffer(device, buffer, false);
    if (!mapped)
        throw sdlError("GPU upload");
    std::memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(device, buffer);
}

} // namespace

struct GpuRenderer::Gpu {
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;
    SDL_GPUGraphicsPipeline* opaqueBack = nullptr;
    SDL_GPUGraphicsPipeline* opaqueFront = nullptr;
    SDL_GPUGraphicsPipeline* opaqueNone = nullptr;
    // By StandardMaterial::Blend (1..5) and cull: back, front (reflected), none.
    static constexpr int kBlendModes = 6;
    SDL_GPUGraphicsPipeline* blended[kBlendModes][3][2]{}; // [blend][cull][depth write]
    SDL_GPUGraphicsPipeline* ui = nullptr;
    SDL_GPUGraphicsPipeline* shadow = nullptr;
    SDL_GPUGraphicsPipeline* mirrorOverlay = nullptr;
    SDL_GPUGraphicsPipeline* particle[3][3]{}; // [alpha, additive, modulate][depth test, none, inverted]
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUSampler* clampSampler = nullptr;
    SDL_GPUSampler* pointSampler = nullptr;
    SDL_GPUTexture* white = nullptr;
    SDL_GPUTexture* lightMap = nullptr;     // slot 0
    SDL_GPUTexture* lightMapNext = nullptr; // slot 1
    float lightMapMix = 0;
    simd_float3 environment{1, 1, 1};
    SDL_FColor clearColor{0.55f, 0.72f, 0.86f, 1.f};
    bool ownsWindow = true;
    bool overlays = true; // the UI image (off for offscreen captures)
    SDL_GPUTexture* depth = nullptr;
    SDL_GPUTexture* shadowAtlas = nullptr;
    SDL_GPUTexture* shadowDepth = nullptr;
    SDL_GPUTexture* reflection = nullptr;
    SDL_GPUTexture* reflectionDepth = nullptr;
    SDL_GPUTexture* censorColor = nullptr;
    SDL_GPUTexture* censorDepth = nullptr;
    unsigned reflectionW = 0, reflectionH = 0;
    bool sawMirror = false;
    float mirrorAt[3]{};
    float mirrorNormal[3]{0, 0, 1};
    bool mirrorInFrame = false;
    unsigned depthW = 0, depthH = 0;
    SDL_GPUBuffer* uiVertices = nullptr;
    unsigned uiCapacity = 0;
    float groundClip = -1e9f;
    RgbaImage uiImage;
    float uiX = 0, uiY = 0, uiW = 0, uiH = 0;
    SDL_GPUTexture* uiTexture = nullptr;
    bool uiDirty = false;

    struct Part {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        uint32_t indexCount = 0;
        SDL_GPUTexture* texture = nullptr;
        SDL_GPUTexture* mask = nullptr;
        uint32_t flags = 0;
        float alphaRef = 0.5f;
        float color[4]{1, 1, 1, 1};
        float maskU[4]{};
        float maskV[4]{};
        float specular[4]{};
        float emissive[4]{};
        bool blend = false;
        int blendMode = 0; // StandardMaterial::Blend
        int layer = 0;     // draw order (StandardMaterial::layer)
        int alphaOrder = 0;
        bool depthWrite = true; // blended parts: StandardMaterial::depthWrite
        bool twoSided = false;
        bool mirror = false;
        float center[3]{};
        float normal[3]{0, 0, 1};
        float lo[3]{0, 0, 0};
        float hi[3]{0, 0, 0};
        bool dynamic = false;
        bool dirty = false;
        std::vector<Vertex> cpu;
    };
    struct Model {
        std::vector<Part> parts;
    };
    struct Instance {
        int model = -1;
        int lightSet = 0;
        float transform[16]{};
    };

    std::vector<Model> models;
    std::vector<Instance> instances;
    std::unordered_map<std::string, SDL_GPUTexture*> textures;
    std::vector<LightSet> lightSets;
    std::vector<GpuLightSet> packedSets{2};
    const GpuLightSet& lightSetOf(int set) const {
        return set >= 0 && size_t(set) < packedSets.size() ? packedSets[size_t(set)] : packedSets[0];
    }
    float eye[4]{};
    struct Caster {
        int instance = -1;
        float feet[3]{};
        float heading = 0;
        float direction[3]{0, 0, -1};
        bool outside = false;
        float color[3]{0.5f, 0.5f, 0.55f};
    };
    std::vector<Caster> casters;
    GpuShadow shadowRecords[kMaxShadowCasters]{};
    int shadowCount = 0;
    struct CensorBox {
        float lo[3]{};
        float hi[3]{};
    };
    std::vector<CensorBox> censorBoxes;
    struct CensorTile {
        float crop[16]{};
        int index = 0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    std::vector<CensorTile> censorTiles;
    SDL_GPUBuffer* censorVertices = nullptr;
    int censorQuads = 0;
    unsigned censorPreparedW = 0, censorPreparedH = 0;
    struct ParticleSprite {
        fx::ParticleQuad q;
        std::string texture;
    };
    std::vector<ParticleSprite> particles;
    ResourceIndex* particleIndex = nullptr;
    std::unordered_map<std::string, SDL_GPUTexture*> particleTextures; // by effect name (registered images too)
    std::vector<SDL_GPUTexture*> particleImages;                       // the registered images (owned)
    struct ParticleMesh {
        std::vector<simd_float3> positions; // model space (z up), by triangle corner
        std::vector<simd_float2> uvs;
        SDL_GPUTexture* texture = nullptr;
        simd_float3 tint{1, 1, 1}; // the material's diffuse when untextured
    };
    std::unordered_map<std::string, std::unique_ptr<ParticleMesh>> particleMeshes;
    simd_float4x4 view = matrix_identity_float4x4;
    struct ParticleBatch {
        SDL_GPUTexture* texture = nullptr;
        int pipeline = 0, depth = 0;
        uint32_t first = 0, count = 0;
    };
    std::vector<ParticleBatch> particleBatches;
    SDL_GPUBuffer* particleVertices = nullptr;
    SDL_GPUTexture* particleTexture(const std::string& name);
    const ParticleMesh* particleMesh(const std::string& name);
    void buildParticles(const simd_float4x4& view);

    SDL_GPUBuffer* uploadBuffer(const void* data, size_t bytes, SDL_GPUBufferUsageFlags usage);
    SDL_GPUTexture* uploadImage(const RgbaImage& image, bool clamp);
    SDL_GPUTexture* textureNamed(const std::string& name, ResourceIndex& index);
    void fillPart(Part& part, const ObjectModel::Part& source, const ObjectModel& model, ResourceIndex& index, bool dynamic);
    void flushDynamic();
    void uploadPartVertices(Part& part);
    SDL_GPUGraphicsPipeline* pipeline(int blend, bool twoSided, bool front = false, bool depthWrite = false) const;
    bool render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height, const simd_float4x4& viewProj);
    void renderShadows(SDL_GPUCommandBuffer* cmd);
    void ensureDepth(unsigned width, unsigned height);
    void ensureShadowTargets();
    void prepareCensor(const simd_float4x4& viewProj, unsigned width, unsigned height);
};

SDL_GPUBuffer* GpuRenderer::Gpu::uploadBuffer(const void* data, size_t bytes, SDL_GPUBufferUsageFlags usage) {
    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = static_cast<Uint32>(std::max<size_t>(bytes, 4));
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device, &info);
    if (!buffer)
        throw sdlError("GPU buffer");
    if (bytes == 0)
        return buffer;
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    copyInto(device, transfer, data, bytes);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{transfer, 0};
    SDL_GPUBufferRegion to{buffer, 0, static_cast<Uint32>(bytes)};
    SDL_UploadToGPUBuffer(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return buffer;
}

SDL_GPUTexture* GpuRenderer::Gpu::uploadImage(const RgbaImage& image, bool clamp) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() != size_t(image.width) * image.height * 4)
        return white;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = image.width;
    info.height = image.height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    if (!texture)
        throw sdlError("GPU texture");
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(image.pixels.size());
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    copyInto(device, transfer, image.pixels.data(), image.pixels.size());
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo from{};
    from.transfer_buffer = transfer;
    from.pixels_per_row = image.width;
    from.rows_per_layer = image.height;
    SDL_GPUTextureRegion to{};
    to.texture = texture;
    to.w = image.width;
    to.h = image.height;
    to.d = 1;
    SDL_UploadToGPUTexture(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    (void)clamp;
    return texture;
}

SDL_GPUTexture* GpuRenderer::Gpu::textureNamed(const std::string& name, ResourceIndex& index) {
    if (name.empty())
        return white;
    if (auto it = textures.find(name); it != textures.end())
        return it->second ? it->second : white;
    SDL_GPUTexture* texture = white;
    try {
        auto loc = index.findByName(name, kTypeTxtr);
        if (!loc)
            throw std::runtime_error("not found");
        texture = uploadImage(loadTexture(index.read(*loc), &index), false);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: texture %s: %s\n", name.c_str(), e.what());
        texture = white;
    }
    textures[name] = texture;
    return texture;
}

void GpuRenderer::Gpu::fillPart(Part& part, const ObjectModel::Part& source, const ObjectModel& model, ResourceIndex& index,
                                bool dynamic) {
    const MeshGroup& mesh = source.mesh;
    StandardMaterial sm;
    bool have = false;
    if (auto it = model.materials.find(source.material); it != model.materials.end()) {
        sm = standardMaterial(it->second);
        have = true;
        if (!sm.visible)
            return;
    }
    part.texture = white;
    part.color[0] = part.color[1] = part.color[2] = 0.8f;
    part.color[3] = 1;
    if (have) {
        if (!sm.baseTexture.empty())
            part.texture = textureNamed(sm.baseTexture, index);
        const float alpha = sm.alphaMultiplier * (sm.baseTexture.empty() ? sm.untexturedAlpha : 1.f);
        part.color[0] = sm.diffuse[0];
        part.color[1] = sm.diffuse[1];
        part.color[2] = sm.diffuse[2];
        part.color[3] = alpha;
        if (sm.shading == StandardMaterial::Shading::Sim) {
            part.color[0] *= 0.8f;
            part.color[1] *= 0.8f;
            part.color[2] *= 0.8f;
        }
        part.flags = (sm.lighting ? kLighting : 0) | (sm.lightMapped ? kLightMapped : 0) | (sm.alphaTest ? kAlphaTest : 0);
        if (sm.shading == StandardMaterial::Shading::VertexColor)
            part.flags |= kVertexColor;
        part.alphaRef = sm.alphaRef;
        part.blend = sm.blend != StandardMaterial::Blend::None;
        part.blendMode = int(sm.blend);
        part.depthWrite = sm.depthWrite;
        part.layer = sm.layer;
        part.alphaOrder = sm.alphaOrder;
        part.twoSided = sm.cull == StandardMaterial::Cull::None;
        const float specSum = sm.specular[0] + sm.specular[1] + sm.specular[2];
        part.specular[0] = sm.specular[0];
        part.specular[1] = sm.specular[1];
        part.specular[2] = sm.specular[2];
        part.specular[3] = sm.specularPower > 1.f && specSum > 0.3f ? sm.specularPower * 0.4f : 0.f;
        part.emissive[0] = sm.emissive[0];
        part.emissive[1] = sm.emissive[1];
        part.emissive[2] = sm.emissive[2];
        if (sm.hair)
            part.flags |= kTwoSidedLight;
        if (sm.shading == StandardMaterial::Shading::Water)
            part.flags |= kWater;
        if (sm.mirror) {
            part.mirror = true;
            part.twoSided = true;
            part.flags &= ~kLighting;
            double cx = 0, cy = 0, cz = 0, nx = 0, ny = 0, nz = 0;
            for (const auto& p : mesh.positions) {
                cx += p[0];
                cy += p[1];
                cz += p[2];
            }
            if (!mesh.positions.empty()) {
                const double n = double(mesh.positions.size());
                part.center[0] = float(cx / n);
                part.center[1] = float(cy / n);
                part.center[2] = float(cz / n);
                part.lo[0] = part.lo[1] = part.lo[2] = 1e9f;
                part.hi[0] = part.hi[1] = part.hi[2] = -1e9f;
                for (const auto& p : mesh.positions)
                    for (int k = 0; k < 3; ++k) {
                        part.lo[k] = std::min(part.lo[k], p[size_t(k)]);
                        part.hi[k] = std::max(part.hi[k], p[size_t(k)]);
                    }
            }
            for (const auto& nrm : mesh.normals) {
                nx += nrm[0];
                ny += nrm[1];
                nz += nrm[2];
            }
            const float len = float(std::sqrt(nx * nx + ny * ny + nz * nz));
            if (len > 1e-6f) {
                part.normal[0] = float(nx / len);
                part.normal[1] = float(ny / len);
                part.normal[2] = float(nz / len);
            }
        }
    }
    if (source.mask >= 0 && size_t(source.mask) < model.masks.size()) {
        const auto& mask = model.masks[size_t(source.mask)];
        if (mask.width && mask.height && mask.values.size() == size_t(mask.width) * mask.height) {
            RgbaImage image;
            image.width = mask.width;
            image.height = mask.height;
            image.pixels.resize(mask.values.size() * 4);
            for (size_t i = 0; i < mask.values.size(); ++i) {
                image.pixels[i * 4 + 0] = 255;
                image.pixels[i * 4 + 1] = 255;
                image.pixels[i * 4 + 2] = 255;
                image.pixels[i * 4 + 3] = mask.values[i];
            }
            part.mask = uploadImage(image, true);
            if (mask.planar) {
                part.flags |= kWallMask;
                for (int k = 0; k < 4; ++k) {
                    part.maskU[k] = mask.planeU[size_t(k)];
                    part.maskV[k] = mask.planeV[size_t(k)];
                }
            } else {
                part.flags |= kMasked;
                const float sx = 1.f / (mask.spacing * float(mask.width));
                const float sy = 1.f / (mask.spacing * float(mask.height));
                part.maskU[0] = sx;
                part.maskU[1] = sy;
                part.maskU[2] = 0.5f / float(mask.width) - mask.originX * sx;
                part.maskU[3] = 0.5f / float(mask.height) - mask.originY * sy;
            }
        }
    }
    if ((part.flags & kLightMapped) && mesh.uvs2.size() != mesh.positions.size())
        part.flags &= ~kLighting;
    part.cpu.resize(mesh.positions.size());
    for (size_t i = 0; i < part.cpu.size(); ++i) {
        Vertex& v = part.cpu[i];
        if (i < mesh.colors.size())
            v.color = mesh.colors[i];
        for (int k = 0; k < 3; ++k)
            v.position[k] = mesh.positions[i][size_t(k)];
        if (i < mesh.normals.size()) {
            for (int k = 0; k < 3; ++k)
                v.normal[k] = mesh.normals[i][size_t(k)];
        } else {
            v.normal[2] = 1;
        }
        if (i < mesh.uvs.size()) {
            v.uv[0] = mesh.uvs[i][0];
            v.uv[1] = mesh.uvs[i][1];
        }
        if (i < mesh.uvs2.size()) {
            v.uv2[0] = mesh.uvs2[i][0];
            v.uv2[1] = mesh.uvs2[i][1];
        }
    }
    part.dynamic = dynamic;
    part.vertices = uploadBuffer(part.cpu.data(), part.cpu.size() * sizeof(Vertex), SDL_GPU_BUFFERUSAGE_VERTEX);
    part.indices = uploadBuffer(mesh.triangles.data(), mesh.triangles.size() * sizeof(uint32_t), SDL_GPU_BUFFERUSAGE_INDEX);
    part.indexCount = uint32_t(mesh.triangles.size());
}

void GpuRenderer::Gpu::uploadPartVertices(Part& part) {
    if (!part.dynamic || part.cpu.empty())
        return;
    const size_t bytes = part.cpu.size() * sizeof(Vertex);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    copyInto(device, transfer, part.cpu.data(), bytes);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{transfer, 0};
    SDL_GPUBufferRegion to{part.vertices, 0, static_cast<Uint32>(bytes)};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    part.dirty = false;
}

SDL_GPUGraphicsPipeline* GpuRenderer::Gpu::pipeline(int blend, bool twoSided, bool front, bool depthWrite) const {
    const int cull = twoSided ? 2 : front ? 1 : 0;
    if (blend > 0 && blend < kBlendModes)
        return blended[blend][cull][depthWrite ? 1 : 0];
    return cull == 2 ? opaqueNone : cull == 1 ? opaqueFront : opaqueBack;
}

void GpuRenderer::Gpu::ensureDepth(unsigned width, unsigned height) {
    if (depth && depthW == width && depthH == height)
        return;
    if (depth)
        SDL_ReleaseGPUTexture(device, depth);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    depth = SDL_CreateGPUTexture(device, &info);
    depthW = width;
    depthH = height;
}

void GpuRenderer::Gpu::ensureShadowTargets() {
    if (shadowAtlas)
        return;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = kShadowAtlasSize;
    info.height = kShadowAtlasSize;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    shadowAtlas = SDL_CreateGPUTexture(device, &info);
    info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    shadowDepth = SDL_CreateGPUTexture(device, &info);
}

void GpuRenderer::Gpu::renderShadows(SDL_GPUCommandBuffer* cmd) {
    shadowCount = 0;
    if (casters.empty() || !shadow)
        return;
    ensureShadowTargets();
    if (!shadowAtlas || !shadowDepth)
        return;
    SDL_GPUColorTargetInfo color{};
    color.texture = shadowAtlas;
    color.clear_color = {1, 1, 1, 1};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    depthInfo.texture = shadowDepth;
    depthInfo.clear_depth = 1;
    depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depthInfo);
    if (!pass)
        return;
    SDL_BindGPUGraphicsPipeline(pass, shadow);
    for (const auto& caster : casters) {
        if (shadowCount >= kMaxShadowCasters || caster.instance < 0 || size_t(caster.instance) >= instances.size())
            continue;
        const Instance& inst = instances[size_t(caster.instance)];
        if (inst.model < 0 || size_t(inst.model) >= models.size())
            continue;
        float fx = caster.direction[0], fy = caster.direction[1], fz = caster.direction[2];
        float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl < 1e-4f) {
            fx = 0;
            fy = 0;
            fz = -1;
            fl = 1;
        }
        fx /= fl;
        fy /= fl;
        fz /= fl;
        const float hx0 = std::cos(caster.heading), hx1 = std::sin(caster.heading);
        const float hy0 = -hx1, hy1 = hx0;
        auto cross = [](float ax, float ay, float az, float bx, float by, float bz, float& ox, float& oy, float& oz) {
            ox = ay * bz - az * by;
            oy = az * bx - ax * bz;
            oz = ax * by - ay * bx;
        };
        float rx, ry, rz;
        cross(fx, fy, fz, hy0, hy1, 0, rx, ry, rz);
        float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rl < 1e-3f)
            cross(fx, fy, fz, hx0, hx1, 0, rx, ry, rz), rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rl < 1e-6f)
            continue;
        rx /= rl;
        ry /= rl;
        rz /= rl;
        float ux, uy, uz;
        cross(rx, ry, rz, fx, fy, fz, ux, uy, uz);
        constexpr float w0 = 0.7f, w1 = 0.7f, h0 = -1.f, h1 = 2.f;
        float a0 = 1e9f, a1 = -1e9f, b0 = 1e9f, b1 = -1e9f, d0 = 1e9f, d1 = -1e9f;
        for (int k = 0; k < 8; ++k) {
            const float qx = (k & 1 ? w0 : -w0) * hx0 + (k & 2 ? w1 : -w1) * hy0;
            const float qy = (k & 1 ? w0 : -w0) * hx1 + (k & 2 ? w1 : -w1) * hy1;
            const float qz = k & 4 ? h1 : h0;
            a0 = std::min(a0, qx * rx + qy * ry + qz * rz);
            a1 = std::max(a1, qx * rx + qy * ry + qz * rz);
            b0 = std::min(b0, qx * ux + qy * uy + qz * uz);
            b1 = std::max(b1, qx * ux + qy * uy + qz * uz);
            d0 = std::min(d0, qx * fx + qy * fy + qz * fz);
            d1 = std::max(d1, qx * fx + qy * fy + qz * fz);
        }
        d0 -= 1;
        d1 += 1;
        const int blur = caster.outside ? 2 : 3;
        const float inner = float(kShadowResolution - 2 * blur) / float(kShadowResolution);
        const float ma = (a1 - a0) * (1.f / inner - 1.f) * 0.5f;
        const float mb = (b1 - b0) * (1.f / inner - 1.f) * 0.5f;
        const float A0 = a0 - ma, Aw = (a1 - a0) + 2 * ma;
        const float B0 = b0 - mb, Bh = (b1 - b0) + 2 * mb, Dd = std::max(d1 - d0, 1e-3f);
        const float px = caster.feet[0], py = caster.feet[1], pz = caster.feet[2];
        const float rd = rx * px + ry * py + rz * pz;
        const float ud = ux * px + uy * py + uz * pz;
        const float fd = fx * px + fy * py + fz * pz;
        const simd_float4x4 vp = simd_matrix_from_rows(
            simd_make_float4(2 * rx / Aw, 2 * ry / Aw, 2 * rz / Aw, -2 * (rd + A0) / Aw - 1),
            simd_make_float4(2 * ux / Bh, 2 * uy / Bh, 2 * uz / Bh, -2 * (ud + B0) / Bh - 1),
            simd_make_float4(fx / Dd, fy / Dd, fz / Dd, -(fd + d0) / Dd), simd_make_float4(0, 0, 0, 1));
        const int tx = shadowCount % kShadowTiles, ty = shadowCount / kShadowTiles;
        const SDL_GPUViewport viewport{float(tx * kShadowResolution), float(ty * kShadowResolution), float(kShadowResolution),
                                       float(kShadowResolution), 0, 1};
        SDL_SetGPUViewport(pass, &viewport);
        for (const auto& part : models[size_t(inst.model)].parts) {
            if (!part.vertices || part.indexCount == 0)
                continue;
            DrawUniforms draw{};
            storeMatrix(vp, draw.viewProj);
            std::memcpy(draw.model, inst.transform, sizeof draw.model);
            draw.color[0] = 0.63f;
            draw.color[1] = 0.63f;
            draw.color[2] = 0.85f;
            draw.color[3] = 1;
            draw.uv0[0] = 1;
            draw.uv1[1] = 1;
            const bool cut = part.blend || (part.flags & kAlphaTest);
            draw.flags = kShadowCaster | (cut ? kAlphaTest : 0u);
            draw.alphaRef = part.blend ? 0.5f : part.alphaRef;
            FragmentUniforms fragment{};
            fragment.draw = draw;
            SDL_PushGPUVertexUniformData(cmd, 0, &draw, sizeof draw);
            SDL_PushGPUFragmentUniformData(cmd, 0, &fragment, sizeof fragment);
            SDL_PushGPUFragmentUniformData(cmd, 1, lightSetOf(0).lights, sizeof GpuLightSet::lights);
            SDL_PushGPUFragmentUniformData(cmd, 2, shadowRecords, sizeof shadowRecords);
            SDL_GPUTextureSamplerBinding bindings[6] = {
                {part.texture ? part.texture : white, sampler},
                {white, sampler},
                {part.mask ? part.mask : white, clampSampler},
                {white, clampSampler},
                {white, clampSampler},
                {white, sampler},
            };
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, 6);
            SDL_GPUBufferBinding vertices{part.vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_GPUBufferBinding indices{part.indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_DrawGPUIndexedPrimitives(pass, part.indexCount, 1, 0, 0, 0);
        }
        const float scale = float(kShadowResolution) / float(kShadowAtlasSize);
        GpuShadow& rec = shadowRecords[shadowCount];
        rec = {};
        rec.u[0] = rx * (scale / Aw);
        rec.u[1] = ry * (scale / Aw);
        rec.u[2] = rz * (scale / Aw);
        rec.u[3] = float(tx) * scale - scale * (rd + A0) / Aw;
        rec.v[0] = -ux * (scale / Bh);
        rec.v[1] = -uy * (scale / Bh);
        rec.v[2] = -uz * (scale / Bh);
        rec.v[3] = float(ty + 1) * scale + scale * (ud + B0) / Bh;
        const float inset = 0.5f / float(kShadowAtlasSize);
        rec.rect[0] = float(tx) * scale + inset;
        rec.rect[1] = float(ty) * scale + inset;
        rec.rect[2] = float(tx + 1) * scale - inset;
        rec.rect[3] = float(ty + 1) * scale - inset;
        rec.misc[0] = float(blur) / float(kShadowAtlasSize);
        rec.misc[1] = caster.outside ? 1.f : 0.f;
        rec.misc[2] = pz;
        rec.color[0] = caster.color[0];
        rec.color[1] = caster.color[1];
        rec.color[2] = caster.color[2];
        rec.color[3] = 1;
        ++shadowCount;
    }
    SDL_EndGPURenderPass(pass);
}

void GpuRenderer::Gpu::flushDynamic() {
    for (auto& model : models)
        for (auto& part : model.parts)
            if (part.dirty)
                uploadPartVertices(part);
}

void GpuRenderer::Gpu::prepareCensor(const simd_float4x4& viewProj, unsigned width, unsigned height) {
    if (censorVertices) {
        SDL_ReleaseGPUBuffer(device, censorVertices);
        censorVertices = nullptr;
    }
    censorTiles.clear();
    censorQuads = 0;
    censorPreparedW = width;
    censorPreparedH = height;
    if (censorBoxes.empty() || width == 0 || height == 0 || !pointSampler)
        return;
    std::vector<SpriteVertex> quads;
    quads.reserve(censorBoxes.size() * 6);
    for (const auto& box : censorBoxes) {
        if (censorTiles.size() >= size_t(kMaxCensorBoxes))
            break;
        if (!(box.lo[0] <= box.hi[0] && box.lo[1] <= box.hi[1] && box.lo[2] <= box.hi[2]))
            continue;
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        bool behind = false;
        for (int k = 0; k < 8 && !behind; ++k) {
            const simd_float4 c = simd_mul(viewProj, simd_make_float4(k & 1 ? box.hi[0] : box.lo[0], k & 2 ? box.hi[1] : box.lo[1],
                                                                       k & 4 ? box.hi[2] : box.lo[2], 1));
            if (c.w <= 1e-4f) {
                behind = true;
                break;
            }
            const float px = (c.x / c.w * 0.5f + 0.5f) * float(width);
            const float py = (0.5f - c.y / c.w * 0.5f) * float(height);
            x0 = std::min(x0, px);
            x1 = std::max(x1, px);
            y0 = std::min(y0, py);
            y1 = std::max(y1, py);
        }
        if (behind)
            continue;
        const int ix0 = std::max(0, int(std::floor(x0)));
        const int iy0 = std::max(0, int(std::floor(y0)));
        const int ix1 = std::min(int(width), int(std::ceil(x1)));
        const int iy1 = std::min(int(height), int(std::ceil(y1)));
        if (ix1 - ix0 < 1 || iy1 - iy0 < 1)
            continue;
        const float nx0 = float(ix0) / float(width) * 2.f - 1.f;
        const float nx1 = float(ix1) / float(width) * 2.f - 1.f;
        const float ny0 = 1.f - float(iy1) / float(height) * 2.f;
        const float ny1 = 1.f - float(iy0) / float(height) * 2.f;
        if (std::fabs(nx1 - nx0) < 1e-5f || std::fabs(ny1 - ny0) < 1e-5f)
            continue;
        CensorTile tile;
        tile.index = int(censorTiles.size());
        tile.x0 = float(ix0);
        tile.y0 = float(iy0);
        tile.x1 = float(ix1);
        tile.y1 = float(iy1);
        simd_float4x4 crop = matrix_identity_float4x4;
        crop.columns[0][0] = 2.f / (nx1 - nx0);
        crop.columns[1][1] = 2.f / (ny1 - ny0);
        crop.columns[3][0] = -(nx1 + nx0) / (nx1 - nx0);
        crop.columns[3][1] = -(ny1 + ny0) / (ny1 - ny0);
        storeMatrix(simd_mul(crop, viewProj), tile.crop);
        const float u0 = float(tile.index) / float(kMaxCensorBoxes);
        const float u1 = float(tile.index + 1) / float(kMaxCensorBoxes);
        const float x = tile.x0, y = tile.y0, w = tile.x1 - tile.x0, h = tile.y1 - tile.y0;
        const SpriteVertex quad[6] = {
            {{x, y}, {u0, 0}, {1, 1, 1, 1}},     {{x + w, y}, {u1, 0}, {1, 1, 1, 1}},
            {{x + w, y + h}, {u1, 1}, {1, 1, 1, 1}}, {{x, y}, {u0, 0}, {1, 1, 1, 1}},
            {{x + w, y + h}, {u1, 1}, {1, 1, 1, 1}}, {{x, y + h}, {u0, 1}, {1, 1, 1, 1}},
        };
        quads.insert(quads.end(), quad, quad + 6);
        censorTiles.push_back(tile);
    }
    if (quads.empty())
        return;
    censorVertices = uploadBuffer(quads.data(), quads.size() * sizeof(SpriteVertex), SDL_GPU_BUFFERUSAGE_VERTEX);
    censorQuads = int(quads.size());
}

bool GpuRenderer::Gpu::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height,
                              const simd_float4x4& viewProj) {
    ensureDepth(width, height);
    if (!depth)
        return false;
    renderShadows(cmd);

    float mainVP[16];
    storeMatrix(viewProj, mainVP);
    const Part* chosen = nullptr;
    float chosenNormal[3]{0, 0, 1};
    float plane[4]{0, 0, 1, 0};
    float best = 1e30f;
    sawMirror = false;
    mirrorInFrame = false;
    bool savedAnchor = false;
    for (const auto& instance : instances) {
        if (instance.model < 0 || size_t(instance.model) >= models.size())
            continue;
        const float* m = instance.transform;
        for (const auto& part : models[size_t(instance.model)].parts) {
            if (!part.mirror)
                continue;
            sawMirror = true;
            const float c0 = m[0] * part.center[0] + m[4] * part.center[1] + m[8] * part.center[2] + m[12];
            const float c1 = m[1] * part.center[0] + m[5] * part.center[1] + m[9] * part.center[2] + m[13];
            const float c2 = m[2] * part.center[0] + m[6] * part.center[1] + m[10] * part.center[2] + m[14];
            float n0 = m[0] * part.normal[0] + m[4] * part.normal[1] + m[8] * part.normal[2];
            float n1 = m[1] * part.normal[0] + m[5] * part.normal[1] + m[9] * part.normal[2];
            float n2 = m[2] * part.normal[0] + m[6] * part.normal[1] + m[10] * part.normal[2];
            const float nl = std::sqrt(n0 * n0 + n1 * n1 + n2 * n2);
            if (nl < 1e-6f)
                continue;
            n0 /= nl;
            n1 /= nl;
            n2 /= nl;
            const float ex = eye[0] - c0, ey = eye[1] - c1, ez = eye[2] - c2;
            if (ex * n0 + ey * n1 + ez * n2 < 0) {
                n0 = -n0;
                n1 = -n1;
                n2 = -n2;
            }
            if (!savedAnchor) {
                savedAnchor = true;
                mirrorAt[0] = c0;
                mirrorAt[1] = c1;
                mirrorAt[2] = c2;
                mirrorNormal[0] = n0;
                mirrorNormal[1] = n1;
                mirrorNormal[2] = n2;
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    const float sx = part.hi[0] - part.lo[0], sy = part.hi[1] - part.lo[1], sz = part.hi[2] - part.lo[2];
                    const float wx = std::fabs(m[0]) * sx + std::fabs(m[4]) * sy + std::fabs(m[8]) * sz;
                    const float wy = std::fabs(m[1]) * sx + std::fabs(m[5]) * sy + std::fabs(m[9]) * sz;
                    const float wz = std::fabs(m[2]) * sx + std::fabs(m[6]) * sy + std::fabs(m[10]) * sz;
                    std::fprintf(stderr, "opensims2: mirror size world %.2f x %.2f x %.2f at (%.2f, %.2f, %.2f)\n", wx, wy, wz, c0,
                                 c1, c2);
                }
            }
            const float clipX = mainVP[0] * c0 + mainVP[4] * c1 + mainVP[8] * c2 + mainVP[12];
            const float clipY = mainVP[1] * c0 + mainVP[5] * c1 + mainVP[9] * c2 + mainVP[13];
            const float clipW = mainVP[3] * c0 + mainVP[7] * c1 + mainVP[11] * c2 + mainVP[15];
            if (clipW <= 0 || std::fabs(clipX) > clipW * 1.3f || std::fabs(clipY) > clipW * 1.3f)
                continue;
            const float d2 = ex * ex + ey * ey + ez * ez;
            if (d2 < best) {
                best = d2;
                chosen = &part;
                chosenNormal[0] = n0;
                chosenNormal[1] = n1;
                chosenNormal[2] = n2;
                plane[0] = n0;
                plane[1] = n1;
                plane[2] = n2;
                plane[3] = -(n0 * c0 + n1 * c1 + n2 * c2);
                mirrorAt[0] = c0;
                mirrorAt[1] = c1;
                mirrorAt[2] = c2;
                mirrorNormal[0] = n0;
                mirrorNormal[1] = n1;
                mirrorNormal[2] = n2;
            }
        }
    }
    mirrorInFrame = chosen != nullptr;

    auto drawScene = [&](SDL_GPURenderPass* pass, const float* vp, const float* passEye, bool reflected, SDL_GPUTexture* reflectionTex) {
        // Draw order (as the Mac renderer): by material layer, opaque parts first in
        // submission order, blended ones back to front by their centre's distance
        // from the eye (a hairdo's layers by the instance's origin, then alpha order).
        struct Item {
            int layer;
            bool blended;
            float distance;
            int alphaOrder;
            const Instance* instance;
            const Part* part;
        };
        std::vector<Item> items;
        for (const auto& instance : instances) {
            if (instance.model < 0 || size_t(instance.model) >= models.size())
                continue;
            const float* m = instance.transform;
            for (const auto& part : models[size_t(instance.model)].parts) {
                if (part.indexCount == 0 || (reflected && part.mirror))
                    continue;
                float distance = 0;
                if (part.blend) {
                    const float* c = part.center;
                    float at[3] = {m[12], m[13], m[14]};
                    if (!part.alphaOrder)
                        for (int k = 0; k < 3; ++k)
                            at[k] = m[k] * c[0] + m[4 + k] * c[1] + m[8 + k] * c[2] + m[12 + k];
                    for (int k = 0; k < 3; ++k)
                        distance += (at[k] - passEye[k]) * (at[k] - passEye[k]);
                }
                items.push_back({part.layer, part.blend, distance, part.alphaOrder, &instance, &part});
            }
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.layer != b.layer)
                return a.layer < b.layer;
            if (a.blended != b.blended)
                return !a.blended;
            if (a.blended && a.distance != b.distance)
                return a.distance > b.distance;
            return a.blended && a.alphaOrder < b.alphaOrder;
        });
        for (const Item& item : items) {
            const Instance& instance = *item.instance;
            const Part& part = *item.part;
            DrawUniforms u{};
            std::memcpy(u.viewProj, vp, sizeof u.viewProj);
            std::memcpy(u.model, instance.transform, sizeof u.model);
            std::memcpy(u.color, part.color, sizeof u.color);
            u.uv0[0] = 1;
            u.uv1[1] = 1;
            std::memcpy(u.maskU, part.maskU, sizeof u.maskU);
            std::memcpy(u.maskV, part.maskV, sizeof u.maskV);
            u.flags = part.flags;
            if (!reflected && &part == chosen) {
                u.flags |= kMirror;
                // The glass sits on the wall. A few centimetres toward the
                // eye keeps the quad in front of the wallpaper.
                u.model[12] += chosenNormal[0] * 0.05f;
                u.model[13] += chosenNormal[1] * 0.05f;
                u.model[14] += chosenNormal[2] * 0.05f;
            }
            u.alphaRef = part.alphaRef;
            u.lightGain = 2.f;
            u.groundClip = groundClip;
            FragmentUniforms fragment{};
            fragment.draw = u;
            std::memcpy(fragment.eye, passEye, sizeof fragment.eye);
            std::memcpy(fragment.specular, part.specular, sizeof fragment.specular);
            std::memcpy(fragment.emissive, part.emissive, sizeof fragment.emissive);
            const GpuLightSet& lights = lightSetOf(instance.lightSet);
            std::memcpy(fragment.ambient, lights.ambient, sizeof fragment.ambient);
            fragment.lightMapMix[0] = lightMapNext ? std::clamp(lightMapMix, 0.f, 1.f) : 0.f;
            fragment.shadowCount = float(shadowCount);
            fragment.viewportX = float(width);
            fragment.viewportY = float(height);
            fragment.clipOn = reflected ? 1.f : 0.f;
            if (reflected)
                std::memcpy(fragment.clipPlane, plane, sizeof fragment.clipPlane);
            SDL_BindGPUGraphicsPipeline(pass, (!reflected && &part == chosen && mirrorOverlay) ? mirrorOverlay
                                                                                         : pipeline(part.blendMode, part.twoSided, reflected, part.depthWrite));
            SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
            SDL_PushGPUFragmentUniformData(cmd, 0, &fragment, sizeof fragment);
            SDL_PushGPUFragmentUniformData(cmd, 1, lights.lights, sizeof lights.lights);
            SDL_PushGPUFragmentUniformData(cmd, 2, shadowRecords, sizeof shadowRecords);
            SDL_GPUTextureSamplerBinding bindings[6] = {
                {part.texture ? part.texture : white, sampler},
                {lightMap ? lightMap : white, sampler},
                {part.mask ? part.mask : white, clampSampler},
                {shadowCount && shadowAtlas ? shadowAtlas : white, clampSampler},
                {reflectionTex ? reflectionTex : white, clampSampler},
                {lightMapNext ? lightMapNext : white, sampler},
            };
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, 6);
            SDL_GPUBufferBinding vertices{part.vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_GPUBufferBinding indices{part.indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_DrawGPUIndexedPrimitives(pass, part.indexCount, 1, 0, 0, 0);
        }
    };

    if (chosen) {
        if (!reflection || reflectionW != width || reflectionH != height) {
            if (reflection)
                SDL_ReleaseGPUTexture(device, reflection);
            if (reflectionDepth)
                SDL_ReleaseGPUTexture(device, reflectionDepth);
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GetGPUSwapchainTextureFormat(device, window);
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = width;
            info.height = height;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            reflection = SDL_CreateGPUTexture(device, &info);
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
            reflectionDepth = SDL_CreateGPUTexture(device, &info);
            reflectionW = width;
            reflectionH = height;
        }
        float reflectM[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                reflectM[c * 4 + k] -= 2.f * chosenNormal[k] * chosenNormal[c];
        reflectM[12] = -2.f * plane[3] * chosenNormal[0];
        reflectM[13] = -2.f * plane[3] * chosenNormal[1];
        reflectM[14] = -2.f * plane[3] * chosenNormal[2];
        auto columns = [](const float* a) {
            return simd_matrix(simd_make_float4(a[0], a[1], a[2], a[3]), simd_make_float4(a[4], a[5], a[6], a[7]),
                               simd_make_float4(a[8], a[9], a[10], a[11]), simd_make_float4(a[12], a[13], a[14], a[15]));
        };
        float reflectedVP[16];
        storeMatrix(simd_mul(columns(mainVP), columns(reflectM)), reflectedVP);
        float reflectedEye[4] = {
            reflectM[0] * eye[0] + reflectM[4] * eye[1] + reflectM[8] * eye[2] + reflectM[12],
            reflectM[1] * eye[0] + reflectM[5] * eye[1] + reflectM[9] * eye[2] + reflectM[13],
            reflectM[2] * eye[0] + reflectM[6] * eye[1] + reflectM[10] * eye[2] + reflectM[14],
            1};
        SDL_GPUColorTargetInfo reflectedColor{};
        reflectedColor.texture = reflection;
        reflectedColor.clear_color = clearColor;
        reflectedColor.load_op = SDL_GPU_LOADOP_CLEAR;
        reflectedColor.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo reflectedDepth{};
        reflectedDepth.texture = reflectionDepth;
        reflectedDepth.clear_depth = 1;
        reflectedDepth.load_op = SDL_GPU_LOADOP_CLEAR;
        reflectedDepth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        if (SDL_GPURenderPass* reflectedPass = SDL_BeginGPURenderPass(cmd, &reflectedColor, 1, &reflectedDepth)) {
            drawScene(reflectedPass, reflectedVP, reflectedEye, true, white);
            SDL_EndGPURenderPass(reflectedPass);
        } else {
            std::fprintf(stderr, "opensims2: reflection pass failed: %s\n", SDL_GetError());
        }
    }

    const bool censorNow = !censorTiles.empty() && censorVertices && censorQuads > 0 && width == censorPreparedW && height == censorPreparedH;
    if (censorNow) {
        if (!censorColor) {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GetGPUSwapchainTextureFormat(device, window);
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = kCensorBlocks * kMaxCensorBoxes;
            info.height = kCensorBlocks;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            censorColor = SDL_CreateGPUTexture(device, &info);
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
            censorDepth = SDL_CreateGPUTexture(device, &info);
        }
        if (censorColor && censorDepth) {
            SDL_GPUColorTargetInfo censorTarget{};
            censorTarget.texture = censorColor;
            censorTarget.clear_color = clearColor;
            censorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
            censorTarget.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo censorDepthInfo{};
            censorDepthInfo.texture = censorDepth;
            censorDepthInfo.clear_depth = 1;
            censorDepthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
            censorDepthInfo.store_op = SDL_GPU_STOREOP_DONT_CARE;
            if (SDL_GPURenderPass* censorPass = SDL_BeginGPURenderPass(cmd, &censorTarget, 1, &censorDepthInfo)) {
                for (const auto& tile : censorTiles) {
                    const SDL_GPUViewport viewport{float(tile.index * kCensorBlocks), 0, float(kCensorBlocks), float(kCensorBlocks), 0, 1};
                    SDL_SetGPUViewport(censorPass, &viewport);
                    drawScene(censorPass, tile.crop, eye, false, white);
                }
                SDL_EndGPURenderPass(censorPass);
            }
        }
    }

    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = clearColor;
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    depthInfo.texture = depth;
    depthInfo.clear_depth = 1;
    depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depthInfo);
    if (!pass)
        return false;
    drawScene(pass, mainVP, eye, false, chosen ? reflection : white);

    if (censorNow && censorColor && pointSampler) {
        const SDL_GPUViewport full{0, 0, float(width), float(height), 0, 1};
        SDL_SetGPUViewport(pass, &full);
        float screen[4] = {float(width), float(height), 0, 0};
        SDL_BindGPUGraphicsPipeline(pass, ui);
        SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
        SDL_GPUTextureSamplerBinding binding{censorColor, pointSampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_GPUBufferBinding vertices{censorVertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        SDL_DrawGPUPrimitives(pass, uint32_t(censorQuads), 1, 0, 0);
    }

    if (particleVertices && !particleBatches.empty()) {
        float viewProjFloats[16];
        std::memcpy(viewProjFloats, mainVP, sizeof viewProjFloats);
        SDL_GPUBufferBinding vertices{particleVertices, 0};
        for (const auto& batch : particleBatches) {
            SDL_GPUGraphicsPipeline* pipe = particle[batch.pipeline][batch.depth];
            if (!pipe || batch.count == 0)
                continue;
            SDL_BindGPUGraphicsPipeline(pass, pipe);
            SDL_PushGPUVertexUniformData(cmd, 0, viewProjFloats, sizeof viewProjFloats);
            SDL_GPUTextureSamplerBinding binding{batch.texture ? batch.texture : white, clampSampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_DrawGPUPrimitives(pass, batch.count, 1, batch.first, 0);
        }
    }

    if (overlays && uiTexture && uiVertices && uiW > 1 && uiH > 1) {
        float screen[4] = {float(width), float(height), 0, 0};
        SDL_BindGPUGraphicsPipeline(pass, ui);
        SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
        SDL_GPUTextureSamplerBinding binding{uiTexture, clampSampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_GPUBufferBinding vertices{uiVertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

namespace {

// `blend` is a StandardMaterial::Blend, with the Mac renderer's factors:
// Alpha srcÎ± + (1 âˆ’ srcÎ±), Additive srcÎ± + one, AddNoSrcAlpha one + one,
// Premultiplied one + (1 âˆ’ srcÎ±), Multiply dstÂ·src + (1 âˆ’ srcÎ±).
SDL_GPUGraphicsPipeline* makePipeline(SDL_GPUDevice* device, SDL_GPUTextureFormat format, SDL_GPUShader* vertex, SDL_GPUShader* fragment,
                                      int blend, bool twoSided, bool front = false, bool depth = true, bool blendDepthWrite = false) {
    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(Vertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[5]{};
    attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
    attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12};
    attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24};
    attributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 32};
    attributes[4] = {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 40};
    SDL_GPUColorTargetDescription color{};
    color.format = format;
    using Blend = os2::StandardMaterial::Blend;
    const Blend mode = Blend(blend);
    if (mode != Blend::None) {
        const SDL_GPUBlendFactor src = mode == Blend::AddNoSrcAlpha || mode == Blend::Premultiplied ? SDL_GPU_BLENDFACTOR_ONE
                                       : mode == Blend::Multiply                                   ? SDL_GPU_BLENDFACTOR_DST_COLOR
                                                                                                   : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        const SDL_GPUBlendFactor dst = mode == Blend::Alpha || mode == Blend::Premultiplied || mode == Blend::Multiply
                                           ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
                                           : SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = src;
        color.blend_state.dst_color_blendfactor = dst;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = mode == Blend::Alpha ? SDL_GPU_BLENDFACTOR_ONE
                                                  : mode == Blend::Multiply ? SDL_GPU_BLENDFACTOR_ZERO
                                                                            : src;
        color.blend_state.dst_alpha_blendfactor = dst;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 5;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = twoSided ? SDL_GPU_CULLMODE_NONE : front ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.depth_stencil_state.enable_depth_test = depth;
    info.depth_stencil_state.enable_depth_write = depth && (mode == Blend::None || blendDepthWrite);
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipeline)
        throw sdlError("pipeline");
    return pipeline;
}

SDL_GPUShader* loadShader(SDL_GPUDevice* device, const std::filesystem::path& file, SDL_GPUShaderStage stage, Uint32 samplers,
                          Uint32 uniforms) {
    SDL_IOStream* io = SDL_IOFromFile(file.string().c_str(), "rb");
    if (!io)
        throw sdlError(file.string().c_str());
    const Sint64 size = SDL_GetIOSize(io);
    std::vector<uint8_t> code(size > 0 ? size_t(size) : 0);
    if (SDL_ReadIO(io, code.data(), code.size()) != code.size()) {
        SDL_CloseIO(io);
        throw sdlError(file.string().c_str());
    }
    SDL_CloseIO(io);
    SDL_GPUShaderCreateInfo info{};
    info.code_size = code.size();
    info.code = code.data();
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_DXIL;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniforms;
    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader)
        throw sdlError(file.string().c_str());
    return shader;
}

} // namespace

GpuRenderer::GpuRenderer(const std::filesystem::path& shaderFolder, int width, int height, SDL_Window* window)
    : gpu_(new Gpu) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        throw sdlError("SDL_Init");
    gpu_->ownsWindow = window == nullptr;
    gpu_->window = window ? window : SDL_CreateWindow("opensims2", width, height, SDL_WINDOW_RESIZABLE);
    if (!gpu_->window)
        throw sdlError("window");
    gpu_->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, false, nullptr);
    if (!gpu_->device)
        throw sdlError("GPU device");
    if (!SDL_ClaimWindowForGPUDevice(gpu_->device, gpu_->window))
        throw sdlError("claim window");

    const SDL_GPUTextureFormat swap = SDL_GetGPUSwapchainTextureFormat(gpu_->device, gpu_->window);
    SDL_GPUShader* vertex = loadShader(gpu_->device, shaderFolder / "lot.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* fragment = loadShader(gpu_->device, shaderFolder / "lot.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 6, 3);
    SDL_GPUShader* uiVertex = loadShader(gpu_->device, shaderFolder / "ui.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* uiFragment = loadShader(gpu_->device, shaderFolder / "ui.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    gpu_->opaqueBack = makePipeline(gpu_->device, swap, vertex, fragment, false, false);
    gpu_->opaqueFront = makePipeline(gpu_->device, swap, vertex, fragment, false, false, true);
    gpu_->opaqueNone = makePipeline(gpu_->device, swap, vertex, fragment, false, true);
    for (int mode = 1; mode < Gpu::kBlendModes; ++mode) {
        for (int write = 0; write < 2; ++write) {
            gpu_->blended[mode][0][write] = makePipeline(gpu_->device, swap, vertex, fragment, mode, false, false, true, write);
            gpu_->blended[mode][1][write] = makePipeline(gpu_->device, swap, vertex, fragment, mode, false, true, true, write);
            gpu_->blended[mode][2][write] = makePipeline(gpu_->device, swap, vertex, fragment, mode, true, false, true, write);
        }
    }
    gpu_->shadow = makePipeline(gpu_->device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, vertex, fragment, false, true);
    gpu_->mirrorOverlay = makePipeline(gpu_->device, swap, vertex, fragment, false, true, false, false);
    SDL_GPUShader* particleVertex = loadShader(gpu_->device, shaderFolder / "particle.vert.dxil", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* particleFragment = loadShader(gpu_->device, shaderFolder / "particle.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    SDL_GPUShader* particleModulate =
        loadShader(gpu_->device, shaderFolder / "particle_modulate.frag.dxil", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    // Blending as ParticleLayer's: alpha, additive (source alpha + one),
    // modulate (destination x source); the destination's alpha kept. Depth:
    // tested, ignored, inverted (in front of nothing); never written.
    auto makeParticles = [&](int blend, int depth) {
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = 36;
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attributes[3]{};
        attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
        attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12};
        attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 20};
        SDL_GPUColorTargetDescription color{};
        color.format = swap;
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = blend == 2 ? SDL_GPU_BLENDFACTOR_DST_COLOR : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = blend == 2   ? SDL_GPU_BLENDFACTOR_ZERO
                                                  : blend == 1 ? SDL_GPU_BLENDFACTOR_ONE
                                                               : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = particleVertex;
        info.fragment_shader = blend == 2 ? particleModulate : particleFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = false;
        info.depth_stencil_state.compare_op = depth == 1   ? SDL_GPU_COMPAREOP_ALWAYS
                                              : depth == 2 ? SDL_GPU_COMPAREOP_GREATER
                                                           : SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(gpu_->device, &info);
        if (!pipeline)
            throw sdlError("particle pipeline");
        return pipeline;
    };
    for (int blend = 0; blend < 3; ++blend)
        for (int depth = 0; depth < 3; ++depth)
            gpu_->particle[blend][depth] = makeParticles(blend, depth);

    SDL_GPUVertexBufferDescription uiBuffer{};
    uiBuffer.slot = 0;
    uiBuffer.pitch = sizeof(SpriteVertex);
    uiBuffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute uiAttributes[3]{};
    uiAttributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0};
    uiAttributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8};
    uiAttributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16};
    SDL_GPUColorTargetDescription uiColor{};
    uiColor.format = SDL_GetGPUSwapchainTextureFormat(gpu_->device, gpu_->window);
    uiColor.blend_state.enable_blend = true;
    uiColor.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    uiColor.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    uiColor.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    uiColor.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    uiColor.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    uiColor.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo uiInfo{};
    uiInfo.vertex_shader = uiVertex;
    uiInfo.fragment_shader = uiFragment;
    uiInfo.vertex_input_state.vertex_buffer_descriptions = &uiBuffer;
    uiInfo.vertex_input_state.num_vertex_buffers = 1;
    uiInfo.vertex_input_state.vertex_attributes = uiAttributes;
    uiInfo.vertex_input_state.num_vertex_attributes = 3;
    uiInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    uiInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    uiInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    uiInfo.target_info.color_target_descriptions = &uiColor;
    uiInfo.target_info.num_color_targets = 1;
    gpu_->ui = SDL_CreateGPUGraphicsPipeline(gpu_->device, &uiInfo);
    if (!gpu_->ui)
        throw sdlError("ui pipeline");
    SDL_ReleaseGPUShader(gpu_->device, vertex);
    SDL_ReleaseGPUShader(gpu_->device, fragment);
    SDL_ReleaseGPUShader(gpu_->device, uiVertex);
    SDL_ReleaseGPUShader(gpu_->device, uiFragment);

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    gpu_->sampler = SDL_CreateGPUSampler(gpu_->device, &sampler);
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    gpu_->clampSampler = SDL_CreateGPUSampler(gpu_->device, &sampler);
    sampler.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    gpu_->pointSampler = SDL_CreateGPUSampler(gpu_->device, &sampler);

    RgbaImage white;
    white.width = white.height = 1;
    white.pixels = {255, 255, 255, 255};
    gpu_->white = gpu_->uploadImage(white, true);
}

GpuRenderer::~GpuRenderer() {
    if (!gpu_)
        return;
    for (auto& model : gpu_->models) {
        for (auto& part : model.parts) {
            if (part.vertices)
                SDL_ReleaseGPUBuffer(gpu_->device, part.vertices);
            if (part.indices)
                SDL_ReleaseGPUBuffer(gpu_->device, part.indices);
            if (part.mask && part.mask != gpu_->white)
                SDL_ReleaseGPUTexture(gpu_->device, part.mask);
        }
    }
    for (auto& [name, texture] : gpu_->textures)
        if (texture && texture != gpu_->white)
            SDL_ReleaseGPUTexture(gpu_->device, texture);
    if (gpu_->uiTexture && gpu_->uiTexture != gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->uiTexture);
    if (gpu_->lightMap && gpu_->lightMap != gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->lightMap);
    if (gpu_->lightMapNext && gpu_->lightMapNext != gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->lightMapNext);
    if (gpu_->depth)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->depth);
    if (gpu_->shadowAtlas)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->shadowAtlas);
    if (gpu_->shadowDepth)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->shadowDepth);
    if (gpu_->reflection)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->reflection);
    if (gpu_->reflectionDepth)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->reflectionDepth);
    if (gpu_->censorColor)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->censorColor);
    if (gpu_->censorDepth)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->censorDepth);
    if (gpu_->censorVertices)
        SDL_ReleaseGPUBuffer(gpu_->device, gpu_->censorVertices);
    if (gpu_->particleVertices)
        SDL_ReleaseGPUBuffer(gpu_->device, gpu_->particleVertices);
    for (SDL_GPUTexture* texture : gpu_->particleImages)
        SDL_ReleaseGPUTexture(gpu_->device, texture);
    for (auto& blends : gpu_->particle)
        for (SDL_GPUGraphicsPipeline* pipeline : blends)
            if (pipeline)
                SDL_ReleaseGPUGraphicsPipeline(gpu_->device, pipeline);
    if (gpu_->uiVertices)
        SDL_ReleaseGPUBuffer(gpu_->device, gpu_->uiVertices);
    if (gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->white);
    if (gpu_->sampler)
        SDL_ReleaseGPUSampler(gpu_->device, gpu_->sampler);
    if (gpu_->clampSampler)
        SDL_ReleaseGPUSampler(gpu_->device, gpu_->clampSampler);
    if (gpu_->pointSampler)
        SDL_ReleaseGPUSampler(gpu_->device, gpu_->pointSampler);
    for (SDL_GPUGraphicsPipeline* pipeline : {gpu_->opaqueBack, gpu_->opaqueFront, gpu_->opaqueNone, gpu_->shadow, gpu_->mirrorOverlay,
                                              gpu_->ui})
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(gpu_->device, pipeline);
    for (auto& modes : gpu_->blended)
        for (auto& culls : modes)
            for (SDL_GPUGraphicsPipeline* pipeline : culls)
                if (pipeline)
                    SDL_ReleaseGPUGraphicsPipeline(gpu_->device, pipeline);
    if (gpu_->device && gpu_->window)
        SDL_ReleaseWindowFromGPUDevice(gpu_->device, gpu_->window);
    if (gpu_->device)
        SDL_DestroyGPUDevice(gpu_->device);
    if (gpu_->window && gpu_->ownsWindow)
        SDL_DestroyWindow(gpu_->window);
    delete gpu_;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int GpuRenderer::addObject(const ObjectModel& model, ResourceIndex& index, bool dynamic) {
    Gpu::Model stored;
    for (const auto& part : model.parts) {
        if (part.mesh.positions.empty() || part.mesh.triangles.empty() || part.shadowPlane)
            continue;
        Gpu::Part gpuPart;
        gpu_->fillPart(gpuPart, part, model, index, dynamic);
        if (gpuPart.indexCount == 0)
            continue;
        stored.parts.push_back(std::move(gpuPart));
    }
    gpu_->models.push_back(std::move(stored));
    return int(gpu_->models.size() - 1);
}

void GpuRenderer::replaceObject(int model, const ObjectModel& replacement, ResourceIndex& index) {
    if (model < 0 || size_t(model) >= gpu_->models.size())
        return;
    Gpu::Model stored;
    for (const auto& part : replacement.parts) {
        if (part.mesh.positions.empty() || part.mesh.triangles.empty() || part.shadowPlane)
            continue;
        Gpu::Part gpuPart;
        gpu_->fillPart(gpuPart, part, replacement, index, false);
        if (gpuPart.indexCount == 0)
            continue;
        stored.parts.push_back(std::move(gpuPart));
    }
    // SDL_GPU frees released buffers once the frames using them are done.
    for (auto& part : gpu_->models[size_t(model)].parts) {
        if (part.vertices)
            SDL_ReleaseGPUBuffer(gpu_->device, part.vertices);
        if (part.indices)
            SDL_ReleaseGPUBuffer(gpu_->device, part.indices);
        if (part.mask && part.mask != gpu_->white)
            SDL_ReleaseGPUTexture(gpu_->device, part.mask);
    }
    gpu_->models[size_t(model)] = std::move(stored);
}

void GpuRenderer::updateObject(int model, const ObjectModel& posed) {
    if (model < 0 || size_t(model) >= gpu_->models.size())
        return;
    auto& stored = gpu_->models[size_t(model)];
    size_t slot = 0;
    for (const auto& part : posed.parts) {
        if (part.mesh.positions.empty() || part.mesh.triangles.empty() || part.shadowPlane)
            continue;
        if (slot >= stored.parts.size())
            break;
        auto& gpuPart = stored.parts[slot++];
        if (!gpuPart.dynamic || gpuPart.cpu.size() != part.mesh.positions.size())
            continue;
        for (size_t i = 0; i < gpuPart.cpu.size(); ++i) {
            for (int k = 0; k < 3; ++k) {
                gpuPart.cpu[i].position[k] = part.mesh.positions[i][size_t(k)];
                if (i < part.mesh.normals.size())
                    gpuPart.cpu[i].normal[k] = part.mesh.normals[i][size_t(k)];
            }
        }
        gpuPart.dirty = true;
    }
}

int GpuRenderer::addInstance(int model, const simd_float4x4& transform) {
    Gpu::Instance instance;
    instance.model = model;
    storeMatrix(transform, instance.transform);
    gpu_->instances.push_back(instance);
    return int(gpu_->instances.size() - 1);
}

void GpuRenderer::setInstanceTransform(int instance, const simd_float4x4& transform) {
    if (instance >= 0 && size_t(instance) < gpu_->instances.size())
        storeMatrix(transform, gpu_->instances[size_t(instance)].transform);
}

void GpuRenderer::setInstanceModel(int instance, int model) {
    if (instance >= 0 && size_t(instance) < gpu_->instances.size())
        gpu_->instances[size_t(instance)].model = model;
}

int GpuRenderer::instanceModel(int instance) const {
    if (instance >= 0 && size_t(instance) < gpu_->instances.size())
        return gpu_->instances[size_t(instance)].model;
    return -1;
}

void GpuRenderer::retargetInstances(int from, int to) {
    for (auto& instance : gpu_->instances)
        if (instance.model == from)
            instance.model = to;
}

void GpuRenderer::setLightMap(const RgbaImage& image) {
    setLightMap(0, image);
    if (gpu_->lightMapNext && gpu_->lightMapNext != gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->lightMapNext);
    gpu_->lightMapNext = nullptr;
}

void GpuRenderer::setLightMap(int slot, const RgbaImage& image) {
    if (slot < 0 || slot > 1 || !image.width || !image.height || image.pixels.size() < size_t(image.width) * image.height * 4)
        return;
    SDL_GPUTexture*& target = slot == 0 ? gpu_->lightMap : gpu_->lightMapNext;
    if (target && target != gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, target);
    target = gpu_->uploadImage(image, true);
}

void GpuRenderer::setLightMapMix(float mix) { gpu_->lightMapMix = mix; }

void GpuRenderer::setEnvironmentLight(simd_float3 rgb) { gpu_->environment = rgb; }

GpuRenderer::LightSet GpuRenderer::outdoorLights(const LightingState& state, bool sims) {
    auto lower = [](std::string s) {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    LightSet set;
    const simd_float3 sky = simd_make_float3(state.skyColor[0], state.skyColor[1], state.skyColor[2]) * state.skyStrength;
    set.ambient = sky;
    bool simFill = false;
    for (const auto& l : state.lights)
        simFill = simFill || lower(l.name).starts_with("simfill");
    for (const auto& l : state.lights) {
        const std::string name = lower(l.name);
        if (name.starts_with("simfill") ? !sims : sims && simFill && (name == "fill1" || name == "fill2"))
            continue;
        const simd_float3 dir = simd_make_float3(l.direction[0], l.direction[1], l.direction[2]);
        if (simd_length(dir) == 0)
            continue;
        const simd_float3 color = simd_max(simd_make_float3(l.color[0], l.color[1], l.color[2]) - sky, simd_make_float3(0, 0, 0));
        set.lights.push_back({simd_normalize(dir), color * l.intensity});
    }
    return set;
}

void GpuRenderer::setLightSets(std::vector<LightSet> sets) {
    if (sets.size() > kMaxLightSets)
        sets.resize(kMaxLightSets);
    gpu_->lightSets = std::move(sets);
    gpu_->packedSets.assign(std::max<size_t>(gpu_->lightSets.size(), 1), GpuLightSet{});
    for (size_t i = 0; i < gpu_->lightSets.size(); ++i) {
        const LightSet& ls = gpu_->lightSets[i];
        GpuLightSet& out = gpu_->packedSets[i];
        const size_t n = std::min(ls.lights.size(), kMaxLights);
        const size_t np = std::min(ls.points.size(), kMaxLights - n);
        out.ambient[0] = ls.ambient.x, out.ambient[1] = ls.ambient.y, out.ambient[2] = ls.ambient.z;
        out.ambient[3] = float(n + np);
        for (size_t k = 0; k < n; ++k) {
            const auto& d = ls.lights[k];
            GpuLight& g = out.lights[k];
            g = {};
            g.direction[0] = d.direction.x, g.direction[1] = d.direction.y, g.direction[2] = d.direction.z;
            g.color[0] = d.color.x, g.color[1] = d.color.y, g.color[2] = d.color.z, g.color[3] = d.specular;
        }
        for (size_t k = 0; k < np; ++k) {
            const auto& p = ls.points[k];
            GpuLight& g = out.lights[n + k];
            const simd_float3 spot = simd_length(p.spotDirection) > 0 ? simd_normalize(p.spotDirection) : simd_make_float3(0, 0, -1);
            g.direction[0] = p.position.x, g.direction[1] = p.position.y, g.direction[2] = p.position.z, g.direction[3] = 1;
            g.color[0] = p.color.x, g.color[1] = p.color.y, g.color[2] = p.color.z, g.color[3] = p.specular;
            g.params[0] = p.falloff, g.params[1] = p.range, g.params[2] = 1, g.params[3] = p.cosInner;
            g.spot[0] = spot.x, g.spot[1] = spot.y, g.spot[2] = spot.z, g.spot[3] = p.cosOuter;
        }
    }
}

const std::vector<GpuRenderer::LightSet>& GpuRenderer::lightSets() const { return gpu_->lightSets; }

int GpuRenderer::instanceOf(int model) const {
    for (size_t i = 0; i < gpu_->instances.size(); ++i)
        if (gpu_->instances[i].model == model)
            return int(i);
    return -1;
}

void GpuRenderer::setGroundClip(float z) { gpu_->groundClip = z; }

void GpuRenderer::setEye(simd_float3 eye) {
    gpu_->eye[0] = eye.x;
    gpu_->eye[1] = eye.y;
    gpu_->eye[2] = eye.z;
    gpu_->eye[3] = 1;
}

void GpuRenderer::setInstanceLightSet(int instance, int set) {
    if (instance >= 0 && size_t(instance) < gpu_->instances.size())
        gpu_->instances[size_t(instance)].lightSet = std::max(0, set);
}

void GpuRenderer::setLighting(const LightingState& state) {
    gpu_->environment = simd_make_float3(state.environment[0], state.environment[1], state.environment[2]);
    std::vector<LightSet> sets = gpu_->lightSets;
    if (sets.size() < 2)
        sets.resize(2);
    sets[0] = outdoorLights(state, false);
    sets[1] = outdoorLights(state, true);
    setLightSets(std::move(sets));
}

int GpuRenderer::objectLightCount() const { return int(gpu_->lightSetOf(0).ambient[3]); }

int GpuRenderer::simLightCount() const { return gpu_->packedSets.size() > 1 ? int(gpu_->packedSets[1].ambient[3]) : 0; }

void GpuRenderer::setShadowCasters(const std::vector<ShadowCaster>& casters) {
    gpu_->casters.clear();
    gpu_->casters.reserve(casters.size());
    for (const auto& caster : casters) {
        GpuRenderer::Gpu::Caster stored;
        stored.instance = caster.instance;
        stored.feet[0] = caster.feet.x;
        stored.feet[1] = caster.feet.y;
        stored.feet[2] = caster.feet.z;
        stored.heading = caster.heading;
        stored.direction[0] = caster.direction.x;
        stored.direction[1] = caster.direction.y;
        stored.direction[2] = caster.direction.z;
        stored.outside = caster.outside;
        stored.color[0] = caster.color.x;
        stored.color[1] = caster.color.y;
        stored.color[2] = caster.color.z;
        gpu_->casters.push_back(stored);
    }
}

int GpuRenderer::shadowCount() const { return gpu_->shadowCount; }

void GpuRenderer::setCensorBoxes(const std::vector<CensorBox>& boxes) {
    gpu_->censorBoxes.clear();
    gpu_->censorBoxes.reserve(boxes.size());
    for (const auto& box : boxes) {
        Gpu::CensorBox stored;
        stored.lo[0] = box.lo.x;
        stored.lo[1] = box.lo.y;
        stored.lo[2] = box.lo.z;
        stored.hi[0] = box.hi.x;
        stored.hi[1] = box.hi.y;
        stored.hi[2] = box.hi.z;
        gpu_->censorBoxes.push_back(stored);
    }
}

int GpuRenderer::censorCount() const { return int(gpu_->censorTiles.size()); }

bool GpuRenderer::mirrorAnchor(simd_float3& center, simd_float3& normal) const {
    if (!gpu_->sawMirror)
        return false;
    center = simd_make_float3(gpu_->mirrorAt[0], gpu_->mirrorAt[1], gpu_->mirrorAt[2]);
    normal = simd_make_float3(gpu_->mirrorNormal[0], gpu_->mirrorNormal[1], gpu_->mirrorNormal[2]);
    return true;
}

SDL_GPUTexture* GpuRenderer::Gpu::particleTexture(const std::string& name) {
    if (name.empty())
        return white;
    if (auto it = particleTextures.find(name); it != particleTextures.end())
        return it->second;
    SDL_GPUTexture* texture = particleIndex ? textureNamed(name + "_txtr", *particleIndex) : white;
    particleTextures[name] = texture;
    return texture;
}

const GpuRenderer::Gpu::ParticleMesh* GpuRenderer::Gpu::particleMesh(const std::string& name) {
    static constexpr std::string_view kShape = "_shpe";
    if (name.size() <= kShape.size() || !particleIndex)
        return nullptr;
    std::string lower = name;
    for (char& c : lower)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!lower.ends_with(kShape))
        return nullptr;
    if (auto it = particleMeshes.find(name); it != particleMeshes.end())
        return it->second.get();
    std::unique_ptr<ParticleMesh> out;
    // The CRES is the name up to one of its underscores, the longest that
    // exists ("lightning_root_rot_shpe" -> "lightning").
    const std::string stem = name.substr(0, name.size() - kShape.size());
    for (size_t cut = stem.rfind('_'); cut != std::string::npos && cut > 0 && !out; cut = stem.rfind('_', cut - 1)) {
        try {
            const ObjectModel om = loadObjectModel(*particleIndex, stem.substr(0, cut));
            auto m = std::make_unique<ParticleMesh>();
            auto shapeOf = [](const ObjectModel::Part& p) {
                std::string shape = p.shape;
                for (char& c : shape)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                return shape;
            };
            bool named = false;
            for (const auto& p : om.parts)
                named = named || shapeOf(p) == lower;
            for (const auto& p : om.parts) {
                if (named && shapeOf(p) != lower)
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
                            m->texture = particleTexture(tex);
                        else
                            m->tint = simd_make_float3(sm.diffuse[0], sm.diffuse[1], sm.diffuse[2]);
                    }
            }
            if (!m->texture)
                m->texture = white;
            if (!m->positions.empty())
                out = std::move(m);
        } catch (const std::exception&) {
        }
    }
    if (!out)
        std::fprintf(stderr, "opensims2: effect shape %s: not found\n", name.c_str());
    return (particleMeshes[name] = std::move(out)).get();
}

namespace {

int particlePipeline(fx::DrawType d) {
    switch (d) {
    case fx::DrawType::Additive:
    case fx::DrawType::AdditiveInvertDepth:
    case fx::DrawType::AdditiveIgnoreDepth: return 1;
    case fx::DrawType::Modulate: return 2;
    default: return 0;
    }
}

int particleDepth(fx::DrawType d) {
    switch (d) {
    case fx::DrawType::DecalIgnoreDepth:
    case fx::DrawType::AdditiveIgnoreDepth: return 1;
    case fx::DrawType::DecalInvertDepth:
    case fx::DrawType::AdditiveInvertDepth: return 2;
    default: return 0;
    }
}

} // namespace

void GpuRenderer::Gpu::buildParticles(const simd_float4x4& viewMatrix) {
    if (particleVertices)
        SDL_ReleaseGPUBuffer(device, particleVertices);
    particleVertices = nullptr;
    particleBatches.clear();
    if (particles.empty())
        return;
    // Camera axes in world space: the view matrix's rows.
    const simd_float3 right = simd_make_float3(viewMatrix.columns[0].x, viewMatrix.columns[1].x, viewMatrix.columns[2].x);
    const simd_float3 up = simd_make_float3(viewMatrix.columns[0].y, viewMatrix.columns[1].y, viewMatrix.columns[2].y);
    const simd_float3 back = simd_make_float3(viewMatrix.columns[0].z, viewMatrix.columns[1].z, viewMatrix.columns[2].z);
    const simd_float4 eyeColumn = simd_inverse(viewMatrix).columns[3];
    const simd_float3 eyeAt = simd_make_float3(eyeColumn.x, eyeColumn.y, eyeColumn.z);

    // Back to front (by layer first), so blended particles composite.
    std::vector<std::pair<float, const ParticleSprite*>> order;
    order.reserve(particles.size());
    for (const auto& s : particles) {
        const auto& sp = s.q.sortPosition;
        order.push_back({simd_length_squared(simd_make_float3(sp[0], sp[1], sp[2]) - eyeAt) - s.q.layer * 1e-3f, &s});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second->q.sortOrder < b.second->q.sortOrder;
    });

    struct Corner {
        float position[3];
        float uv[2];
        float color[4];
    };
    std::vector<Corner> corners;
    corners.reserve(order.size() * 6);
    for (const auto& [d, sprite] : order) {
        const fx::ParticleQuad& q = sprite->q;
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
            const simd_float3 v = simd_make_float3(q.velocity[0], q.velocity[1], q.velocity[2]);
            if (simd_length(v) > 1e-4f) {
                ay = simd_normalize(v);
                const simd_float3 x = simd_cross(ay, back);
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
        const ParticleMesh* shape = particleMesh(sprite->texture);
        SDL_GPUTexture* texture = shape ? shape->texture : particleTexture(sprite->texture);
        const int pipe = particlePipeline(q.draw), dep = particleDepth(q.draw);
        if (particleBatches.empty() || particleBatches.back().texture != texture || particleBatches.back().pipeline != pipe ||
            particleBatches.back().depth != dep)
            particleBatches.push_back({texture, pipe, dep, uint32_t(corners.size()), 0});
        if (shape) {
            // The mesh at the particle: its size as the scale, its rotation
            // about the vertical, the model's axes otherwise.
            const float cs = std::cos(q.rotation), sn = std::sin(q.rotation), k = q.width;
            for (size_t i = 0; i < shape->positions.size(); ++i) {
                const simd_float3 m = shape->positions[i] * k;
                const simd_float3 p = c + simd_make_float3(m.x * cs - m.y * sn, m.x * sn + m.y * cs, m.z);
                corners.push_back({{p.x, p.y, p.z},
                                   {shape->uvs[i].x, shape->uvs[i].y},
                                   {q.color[0] * shape->tint.x, q.color[1] * shape->tint.y, q.color[2] * shape->tint.z, q.color[3]}});
            }
            particleBatches.back().count += uint32_t(shape->positions.size());
            continue;
        }
        auto corner = [&](simd_float3 p, float u, float v) {
            corners.push_back({{p.x, p.y, p.z}, {u, v}, {q.color[0], q.color[1], q.color[2], q.color[3]}});
        };
        const auto& uv = q.uv;
        corner(c - hx - hy, uv[0], uv[3]);
        corner(c + hx - hy, uv[2], uv[3]);
        corner(c + hx + hy, uv[2], uv[1]);
        corner(c - hx - hy, uv[0], uv[3]);
        corner(c + hx + hy, uv[2], uv[1]);
        corner(c - hx + hy, uv[0], uv[1]);
        particleBatches.back().count += 6;
    }
    if (!corners.empty())
        particleVertices = uploadBuffer(corners.data(), corners.size() * sizeof(Corner), SDL_GPU_BUFFERUSAGE_VERTEX);
}

void GpuRenderer::setParticles(const std::vector<fx::ParticleQuad>& quads, ResourceIndex* index) {
    gpu_->particleIndex = index;
    gpu_->particles.clear();
    gpu_->particles.reserve(quads.size());
    for (const auto& q : quads)
        gpu_->particles.push_back({q, q.texture ? *q.texture : std::string()});
}

void GpuRenderer::setParticleImage(const std::string& name, const RgbaImage& image) {
    if (gpu_->particleTextures.count(name) || !image.width || !image.height)
        return;
    try {
        SDL_GPUTexture* texture = gpu_->uploadImage(image, true);
        gpu_->particleImages.push_back(texture);
        gpu_->particleTextures[name] = texture;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: effect icon %s: %s\n", name.c_str(), e.what());
        gpu_->particleTextures[name] = gpu_->white;
    }
}

bool GpuRenderer::hasParticleImage(const std::string& name) const { return gpu_->particleTextures.count(name) != 0; }

size_t GpuRenderer::particleCount() const { return gpu_->particles.size(); }

void GpuRenderer::setView(const simd_float4x4& view) { gpu_->view = view; }

void GpuRenderer::setUiImage(const RgbaImage& image, float x, float y, float w, float h) {
    if (gpu_->uiTexture && gpu_->uiTexture != gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, gpu_->uiTexture);
    gpu_->uiTexture = gpu_->uploadImage(image, true);
    gpu_->uiX = x;
    gpu_->uiY = y;
    gpu_->uiW = w;
    gpu_->uiH = h;
    SpriteVertex quad[6] = {
        {{x, y}, {0, 0}, {1, 1, 1, 1}},       {{x + w, y}, {1, 0}, {1, 1, 1, 1}},
        {{x + w, y + h}, {1, 1}, {1, 1, 1, 1}}, {{x, y}, {0, 0}, {1, 1, 1, 1}},
        {{x + w, y + h}, {1, 1}, {1, 1, 1, 1}}, {{x, y + h}, {0, 1}, {1, 1, 1, 1}},
    };
    if (gpu_->uiVertices)
        SDL_ReleaseGPUBuffer(gpu_->device, gpu_->uiVertices);
    gpu_->uiVertices = gpu_->uploadBuffer(quad, sizeof quad, SDL_GPU_BUFFERUSAGE_VERTEX);
}

void GpuRenderer::setClearColor(simd_float4 color) { gpu_->clearColor = {color.x, color.y, color.z, color.w}; }

SDL_Window* GpuRenderer::window() const { return gpu_->window; }

bool GpuRenderer::draw(const simd_float4x4& viewProj) {
    SDL_Event event;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_EVENT_QUIT)
            return false;
    gpu_->flushDynamic();
    gpu_->buildParticles(gpu_->view);
    int pixelW = 0, pixelH = 0;
    SDL_GetWindowSizeInPixels(gpu_->window, &pixelW, &pixelH);
    gpu_->prepareCensor(viewProj, unsigned(std::max(pixelW, 0)), unsigned(std::max(pixelH, 0)));
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_->device);
    if (!cmd)
        return true;
    SDL_GPUTexture* swap = nullptr;
    uint32_t width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gpu_->window, &swap, &width, &height) || !swap) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return true;
    }
    gpu_->render(cmd, swap, width, height, viewProj);
    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
}

bool GpuRenderer::capture(const std::filesystem::path& path, const simd_float4x4& viewProj) {
    int width = 0, height = 0;
    SDL_GetWindowSize(gpu_->window, &width, &height);
    if (width <= 0 || height <= 0)
        return false;
    gpu_->flushDynamic();
    gpu_->buildParticles(gpu_->view);
    gpu_->prepareCensor(viewProj, unsigned(width), unsigned(height));
    const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(gpu_->device, gpu_->window);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = uint32_t(width);
    info.height = uint32_t(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(gpu_->device, &info);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = uint32_t(width * height * 4);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(gpu_->device, &transferInfo);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_->device);
    if (!target || !transfer || !cmd || !gpu_->render(cmd, target, uint32_t(width), uint32_t(height), viewProj)) {
        if (cmd)
            SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }
    SDL_GPUTransferBuffer* reflectionTransfer = nullptr;
    if (gpu_->reflection && path.filename().string().find("mirror") != std::string::npos)
        reflectionTransfer = SDL_CreateGPUTransferBuffer(gpu_->device, &transferInfo);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion from{};
    from.texture = target;
    from.w = uint32_t(width);
    from.h = uint32_t(height);
    from.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = transfer;
    to.pixels_per_row = uint32_t(width);
    to.rows_per_layer = uint32_t(height);
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    if (reflectionTransfer) {
        SDL_GPUTextureRegion reflectionFrom{};
        reflectionFrom.texture = gpu_->reflection;
        reflectionFrom.w = uint32_t(width);
        reflectionFrom.h = uint32_t(height);
        reflectionFrom.d = 1;
        SDL_GPUTextureTransferInfo reflectionTo{};
        reflectionTo.transfer_buffer = reflectionTransfer;
        reflectionTo.pixels_per_row = uint32_t(width);
        reflectionTo.rows_per_layer = uint32_t(height);
        SDL_DownloadFromGPUTexture(copy, &reflectionFrom, &reflectionTo);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence)
        return false;
    SDL_WaitForGPUFences(gpu_->device, true, &fence, 1);
    SDL_ReleaseGPUFence(gpu_->device, fence);
    const auto* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(gpu_->device, transfer, false));
    if (!pixels)
        return false;
    RgbaImage image;
    image.width = uint32_t(width);
    image.height = uint32_t(height);
    image.pixels.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
    // The window is opaque: blended passes leave the target's alpha below 1
    // (multiply writes 0), which the screen ignores.
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
            std::swap(image.pixels[i], image.pixels[i + 2]);
        image.pixels[i + 3] = 255;
    }
    SDL_UnmapGPUTransferBuffer(gpu_->device, transfer);
    SDL_ReleaseGPUTransferBuffer(gpu_->device, transfer);
    SDL_ReleaseGPUTexture(gpu_->device, target);
    writePng(path, image);
    if (reflectionTransfer) {
        const auto* reflected = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(gpu_->device, reflectionTransfer, false));
        if (reflected) {
            RgbaImage reflectionImage;
            reflectionImage.width = uint32_t(width);
            reflectionImage.height = uint32_t(height);
            reflectionImage.pixels.assign(reflected, reflected + size_t(width) * size_t(height) * 4);
            if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) {
                for (size_t i = 0; i + 3 < reflectionImage.pixels.size(); i += 4)
                    std::swap(reflectionImage.pixels[i], reflectionImage.pixels[i + 2]);
            }
            SDL_UnmapGPUTransferBuffer(gpu_->device, reflectionTransfer);
            writePng(path.parent_path() / "frame_reflection.png", reflectionImage);
        }
        SDL_ReleaseGPUTransferBuffer(gpu_->device, reflectionTransfer);
    }
    return true;
}

RgbaImage GpuRenderer::capture(uint32_t width, uint32_t height, const FrameView& view, simd_float4 clearColor) {
    RgbaImage image;
    if (width == 0 || height == 0)
        return image;
    const simd_float4x4 viewProj = simd_mul(view.projection, view.view);
    gpu_->flushDynamic();
    gpu_->buildParticles(view.view);
    gpu_->prepareCensor(viewProj, width, height);
    const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(gpu_->device, gpu_->window);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(gpu_->device, &info);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = width * height * 4;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(gpu_->device, &transferInfo);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_->device);
    const SDL_FColor savedClear = gpu_->clearColor;
    gpu_->clearColor = {clearColor.x, clearColor.y, clearColor.z, clearColor.w};
    gpu_->overlays = false;
    const bool rendered = target && transfer && cmd && gpu_->render(cmd, target, width, height, viewProj);
    gpu_->clearColor = savedClear;
    gpu_->overlays = true;
    auto release = [&] {
        if (transfer)
            SDL_ReleaseGPUTransferBuffer(gpu_->device, transfer);
        if (target)
            SDL_ReleaseGPUTexture(gpu_->device, target);
    };
    if (!rendered) {
        if (cmd)
            SDL_CancelGPUCommandBuffer(cmd);
        release();
        return image;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion from{};
    from.texture = target;
    from.w = width;
    from.h = height;
    from.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = transfer;
    to.pixels_per_row = width;
    to.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd)) {
        SDL_WaitForGPUFences(gpu_->device, true, &fence, 1);
        SDL_ReleaseGPUFence(gpu_->device, fence);
        if (const auto* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(gpu_->device, transfer, false))) {
            image.width = width;
            image.height = height;
            image.pixels.assign(pixels, pixels + size_t(width) * height * 4);
            if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
                for (size_t i = 0; i + 3 < image.pixels.size(); i += 4)
                    std::swap(image.pixels[i], image.pixels[i + 2]);
            SDL_UnmapGPUTransferBuffer(gpu_->device, transfer);
        }
    }
    release();
    return image;
}

bool GpuRenderer::drawMovie(const RgbaImage& frame) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_->device);
    if (!cmd)
        return false;
    SDL_GPUTexture* swap = nullptr;
    uint32_t width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gpu_->window, &swap, &width, &height) || !swap) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    SDL_GPUTexture* picture = frame.width && frame.height ? gpu_->uploadImage(frame, true) : nullptr;
    SDL_GPUBuffer* vertices = nullptr;
    if (picture) {
        const float fit = std::min(float(width) / float(frame.width), float(height) / float(frame.height));
        const float w = float(frame.width) * fit, h = float(frame.height) * fit;
        const float x = (float(width) - w) * 0.5f, y = (float(height) - h) * 0.5f;
        const SpriteVertex quad[6] = {
            {{x, y}, {0, 0}, {1, 1, 1, 1}},         {{x + w, y}, {1, 0}, {1, 1, 1, 1}},
            {{x + w, y + h}, {1, 1}, {1, 1, 1, 1}}, {{x, y}, {0, 0}, {1, 1, 1, 1}},
            {{x + w, y + h}, {1, 1}, {1, 1, 1, 1}}, {{x, y + h}, {0, 1}, {1, 1, 1, 1}},
        };
        vertices = gpu_->uploadBuffer(quad, sizeof quad, SDL_GPU_BUFFERUSAGE_VERTEX);
    }
    SDL_GPUColorTargetInfo color{};
    color.texture = swap;
    color.clear_color = {0, 0, 0, 1};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr)) {
        if (picture && vertices) {
            float screen[4] = {float(width), float(height), 0, 0};
            SDL_BindGPUGraphicsPipeline(pass, gpu_->ui);
            SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
            SDL_GPUTextureSamplerBinding binding{picture, gpu_->clampSampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_GPUBufferBinding bindingVertices{vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &bindingVertices, 1);
            SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    // (Released once the GPU is done with them.)
    if (vertices)
        SDL_ReleaseGPUBuffer(gpu_->device, vertices);
    if (picture && picture != gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, picture);
    return true;
}

bool GpuRenderer::captureMovie(const std::filesystem::path& path, const RgbaImage& frame) {
    int width = 0, height = 0;
    SDL_GetWindowSize(gpu_->window, &width, &height);
    if (width <= 0 || height <= 0 || frame.width == 0 || frame.height == 0)
        return false;
    SDL_GPUTexture* picture = gpu_->uploadImage(frame, true);
    const float fit = std::min(float(width) / float(frame.width), float(height) / float(frame.height));
    const float w = float(frame.width) * fit;
    const float h = float(frame.height) * fit;
    const float x = (float(width) - w) * 0.5f;
    const float y = (float(height) - h) * 0.5f;
    const SpriteVertex quad[6] = {
        {{x, y}, {0, 0}, {1, 1, 1, 1}},         {{x + w, y}, {1, 0}, {1, 1, 1, 1}},
        {{x + w, y + h}, {1, 1}, {1, 1, 1, 1}}, {{x, y}, {0, 0}, {1, 1, 1, 1}},
        {{x + w, y + h}, {1, 1}, {1, 1, 1, 1}}, {{x, y + h}, {0, 1}, {1, 1, 1, 1}},
    };
    SDL_GPUBuffer* vertices = gpu_->uploadBuffer(quad, sizeof quad, SDL_GPU_BUFFERUSAGE_VERTEX);
    const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(gpu_->device, gpu_->window);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = uint32_t(width);
    info.height = uint32_t(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(gpu_->device, &info);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = uint32_t(width * height * 4);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(gpu_->device, &transferInfo);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_->device);
    bool drew = false;
    if (target && transfer && cmd && picture && vertices) {
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.clear_color = {0, 0, 0, 1};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr)) {
            float screen[4] = {float(width), float(height), 0, 0};
            SDL_BindGPUGraphicsPipeline(pass, gpu_->ui);
            SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
            SDL_GPUTextureSamplerBinding binding{picture, gpu_->clampSampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_GPUBufferBinding bindingVertices{vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &bindingVertices, 1);
            SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
            drew = true;
        }
    }
    if (!drew) {
        if (cmd)
            SDL_CancelGPUCommandBuffer(cmd);
        if (transfer)
            SDL_ReleaseGPUTransferBuffer(gpu_->device, transfer);
        if (target)
            SDL_ReleaseGPUTexture(gpu_->device, target);
        if (vertices)
            SDL_ReleaseGPUBuffer(gpu_->device, vertices);
        if (picture && picture != gpu_->white)
            SDL_ReleaseGPUTexture(gpu_->device, picture);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion from{};
    from.texture = target;
    from.w = uint32_t(width);
    from.h = uint32_t(height);
    from.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = transfer;
    to.pixels_per_row = uint32_t(width);
    to.rows_per_layer = uint32_t(height);
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (picture && picture != gpu_->white)
        SDL_ReleaseGPUTexture(gpu_->device, picture);
    SDL_ReleaseGPUBuffer(gpu_->device, vertices);
    if (!fence) {
        SDL_ReleaseGPUTransferBuffer(gpu_->device, transfer);
        SDL_ReleaseGPUTexture(gpu_->device, target);
        return false;
    }
    SDL_WaitForGPUFences(gpu_->device, true, &fence, 1);
    SDL_ReleaseGPUFence(gpu_->device, fence);
    const auto* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(gpu_->device, transfer, false));
    if (!pixels) {
        SDL_ReleaseGPUTransferBuffer(gpu_->device, transfer);
        SDL_ReleaseGPUTexture(gpu_->device, target);
        return false;
    }
    RgbaImage image;
    image.width = uint32_t(width);
    image.height = uint32_t(height);
    image.pixels.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
    // The window is opaque: blended passes leave the target's alpha below 1
    // (multiply writes 0), which the screen ignores.
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
            std::swap(image.pixels[i], image.pixels[i + 2]);
        image.pixels[i + 3] = 255;
    }
    SDL_UnmapGPUTransferBuffer(gpu_->device, transfer);
    SDL_ReleaseGPUTransferBuffer(gpu_->device, transfer);
    SDL_ReleaseGPUTexture(gpu_->device, target);
    writePng(path, image);
    return true;
}

std::string GpuRenderer::backend() const {
    const char* name = SDL_GetGPUDeviceDriver(gpu_->device);
    return name ? name : "";
}

} // namespace os2
