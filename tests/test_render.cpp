// Offscreen rendering checks: materials (color, blending, culling) reach the
// framebuffer as expected. Needs a Metal device.
#include "formats/image.hpp"
#include "formats/lighting_config.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "render/camera.hpp"
#include "render/renderer.hpp"

#include <Metal/Metal.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// A 20x20 horizontal quad at height z; `clockwise` flips the winding seen from above.
static MeshGroup quad(float z, bool clockwise = false) {
    MeshGroup m;
    m.name = "quad";
    m.positions = {{-10, -10, z}, {10, -10, z}, {10, 10, z}, {-10, 10, z}};
    m.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    m.uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    m.triangles = clockwise ? std::vector<uint32_t>{0, 2, 1, 0, 3, 2} : std::vector<uint32_t>{0, 1, 2, 0, 2, 3};
    return m;
}

static MaterialDefinition material(const char* diffuse, const char* blend, const char* alpha) {
    MaterialDefinition m;
    m.materialType = "StandardMaterial";
    m.properties = {{"stdMatDiffCoef", diffuse},
                    {"stdMatAlphaBlendMode", blend},
                    {"stdMatAlphaMultiplier", alpha},
                    {"stdMatLightingEnabled", "0"},
                    {"stdMatCullMode", "cullClockwise"}};
    return m;
}

static const uint8_t* center(const RgbaImage& img) {
    return &img.pixels[(size_t(img.height / 2) * img.width + img.width / 2) * 4];
}

static bool near(int a, int b) { return std::abs(a - b) <= 2; }

int main() {
    {
        // Spot cones (cSpotLight): the portals' 70 + 10 degrees, the cinematic rig's 40 + 10.
        const float d = 3.14159265f / 180;
        const simd_float2 p = Renderer::spotCosines(70 * d, 10 * d), r = Renderer::spotCosines(-40 * d, 10 * d);
        check(std::fabs(p.x - std::cos(40 * d)) < 1e-4f && std::fabs(p.y - std::cos(35 * d)) < 1e-4f, "spot cone cosines");
        check(std::fabs(r.x - std::cos(25 * d)) < 1e-4f && std::fabs(r.y - std::cos(20 * d)) < 1e-4f, "spot cone, absolute angle");
        check(Renderer::spotCosines(3, 1).x == std::cos(3.14159265f / 2), "spot cone at most pi");
    }
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (!device) {
        std::printf("no Metal device, skipping\n");
        return 0;
    }
    auto empty = std::filesystem::temp_directory_path() / "os2_test_render_empty";
    std::filesystem::create_directories(empty);
    ResourceIndex index(empty);

    CameraDef def;
    def.distances = {20};
    def.fov = {45};
    def.pitchMin = {60};
    def.pitchMax = {60};
    LotCamera cam(def);
    const FrameView view{cam.view(), cam.projection(1.0f)};
    const simd_float4 clear = simd_make_float4(0, 0, 1, 1);

    Renderer r(device, uint32_t(MTL::PixelFormatBGRA8Unorm), uint32_t(MTL::PixelFormatDepth32Float));

    // Opaque red quad above the test floor.
    ObjectModel red;
    red.name = "red";
    red.parts.push_back({"n", "s", "red", quad(2)});
    red.materials["red"] = material("1,0,0", "none", "1");
    r.addInstance(r.addObject(red, index), matrix_identity_float4x4);
    auto img = r.capture(64, 64, view, clear);
    const uint8_t* p = center(img);
    check(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255, "opaque unlit red");

    // A clockwise quad in front of it is culled.
    ObjectModel back;
    back.name = "back";
    back.parts.push_back({"n", "s", "green", quad(3, true)});
    back.materials["green"] = material("0,1,0", "none", "1");
    r.addInstance(r.addObject(back, index), matrix_identity_float4x4);
    img = r.capture(64, 64, view, clear);
    p = center(img);
    check(p[0] == 255 && p[1] == 0, "clockwise faces are culled");

    // Black at 50% alpha blended over the red.
    ObjectModel shadow;
    shadow.name = "shadow";
    shadow.parts.push_back({"n", "s", "shadow", quad(4)});
    shadow.materials["shadow"] = material("0,0,0", "blend", "0.5");
    r.addInstance(r.addObject(shadow, index), matrix_identity_float4x4);
    img = r.capture(64, 64, view, clear);
    p = center(img);
    check(near(p[0], 128) && p[1] == 0 && p[2] == 0, "alpha blend over opaque");

    // "Null" materials are not drawn.
    ObjectModel hidden;
    hidden.name = "hidden";
    hidden.parts.push_back({"n", "s", "null", quad(5)});
    hidden.materials["null"] = material("1,1,1", "none", "1");
    hidden.materials["null"].materialType = "Null";
    r.addInstance(r.addObject(hidden, index), matrix_identity_float4x4);
    img = r.capture(64, 64, view, clear);
    p = center(img);
    check(near(p[0], 128) && p[1] == 0, "null material hidden");

    // Lighting: diffuse x (ambient + lights) saturates, then specular adds on top.
    {
        Renderer lit(device, uint32_t(MTL::PixelFormatBGRA8Unorm), uint32_t(MTL::PixelFormatDepth32Float));
        LightingState state;
        state.name = "test";
        state.skyColor = {1, 1, 1};
        state.skyStrength = 0.25f;
        state.lights.push_back({"sun", {0, 0, 1}, {1, 1, 1}, 0.25f});        // straight down
        state.lights.push_back({"simFill1", {0, 0, 1}, {1, 1, 1}, 10.0f});  // Sims only: ignored
        state.lights.push_back({"under", {0, 0, -1}, {1, 1, 1}, 10.0f});    // lights the underside only
        lit.setLighting(state);
        ObjectModel grey;
        grey.name = "grey";
        grey.parts.push_back({"n", "s", "grey", quad(2)});
        MaterialDefinition m = material("0.8,0.4,4", "none", "1");
        for (auto& [k, v] : m.properties)
            if (k == "stdMatLightingEnabled")
                v = "1";
        m.properties.push_back({"stdMatEmissiveCoef", "0,0.1,0"});
        grey.materials["grey"] = m;
        lit.addInstance(lit.addObject(grey, index), matrix_identity_float4x4);
        img = lit.capture(64, 64, view, clear);
        p = center(img);
        // The game's DX8 path (docs/shaders-mac.md 4.2, lighting-mac.md 3): the sun's colour less the
        // skylight, (1 - 0.25) * 0.25 = 0.1875, plus the skylight 0.25 = 0.4375; colour =
        // saturate(2 * tex * saturate(0.5 * (diffuse * light + emissive))): red 0.8 * 0.4375 = 0.35,
        // green 0.4 * 0.4375 + 0.1 = 0.275, blue 4 * 0.4375 = 1.75 saturates (at 2 inside, 1 out).
        check(near(p[0], 89) && near(p[1], 70) && p[2] == 255, "diffuse, emissive and saturation");
    }

    // Blended parts are drawn back to front whatever order they were added in.
    {
        Renderer sorted(device, uint32_t(MTL::PixelFormatBGRA8Unorm), uint32_t(MTL::PixelFormatDepth32Float));
        sorted.hideTestScene();
        ObjectModel glass;
        glass.name = "glass";
        glass.parts.push_back({"n", "s", "near", quad(3)});
        glass.parts.push_back({"n", "s", "far", quad(2)});
        glass.materials["near"] = material("0,1,0", "blend", "0.5");
        glass.materials["far"] = material("1,0,0", "blend", "0.5");
        sorted.addInstance(sorted.addObject(glass, index), matrix_identity_float4x4);
        img = sorted.capture(64, 64, view, clear);
        p = center(img);
        // far over blue: (128, 0, 128); near over that: (64, 128, 64).
        check(near(p[0], 64) && near(p[1], 128) && near(p[2], 64), "blended parts sorted back to front");
    }

    // Mirrors (docs/shaders-mac.md 13.1): two side by side both reflect the
    // red ground in front of them (each its own viewer, one target, the
    // stencil of its quad); with reflections off, the blank mirror colour.
    {
        Renderer mr(device, uint32_t(MTL::PixelFormatBGRA8Unorm), uint32_t(MTL::PixelFormatDepth32Float));
        mr.hideTestScene();
        ObjectModel ground;
        ground.name = "ground";
        ground.parts.push_back({"n", "s", "red", quad(0)});
        ground.materials["red"] = material("1,0,0", "none", "1");
        mr.addInstance(mr.addObject(ground, index), matrix_identity_float4x4);
        ObjectModel mirrors;
        mirrors.name = "mirrors";
        for (float x : {-3.5f, 1.0f}) { // upright in y = 0, facing -y (the eye)
            MeshGroup g;
            g.name = "mirror";
            g.positions = {{x, 0, 0.5f}, {x + 2.5f, 0, 0.5f}, {x + 2.5f, 0, 3}, {x, 0, 3}};
            g.normals = {{0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}};
            g.uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
            g.triangles = {0, 1, 2, 0, 2, 3};
            mirrors.parts.push_back({"n", "s", "glass", g});
        }
        MaterialDefinition glass;
        glass.materialType = "SimpleMirrorReflection";
        mirrors.materials["glass"] = glass;
        mr.addInstance(mr.addObject(mirrors, index), matrix_identity_float4x4);
        // An eye in front of the mirrors, looking at them.
        const simd_float3 eye = simd_make_float3(0, -8, 4), at = simd_make_float3(0, 0, 1.5f);
        const simd_float3 fwd = simd_normalize(at - eye), right = simd_normalize(simd_cross(fwd, simd_make_float3(0, 0, 1))),
                          up = simd_cross(right, fwd);
        simd_float4x4 lookAt = matrix_identity_float4x4;
        lookAt.columns[0] = simd_make_float4(right.x, up.x, -fwd.x, 0);
        lookAt.columns[1] = simd_make_float4(right.y, up.y, -fwd.y, 0);
        lookAt.columns[2] = simd_make_float4(right.z, up.z, -fwd.z, 0);
        lookAt.columns[3] = simd_make_float4(-simd_dot(right, eye), -simd_dot(up, eye), simd_dot(fwd, eye), 1);
        const FrameView mv{lookAt, cam.projection(1.0f)};
        auto pixelAt = [&](const RgbaImage& im, simd_float3 w) {
            const simd_float4 c = simd_mul(simd_mul(mv.projection, mv.view), simd_make_float4(w, 1));
            const int px = int((c.x / c.w * 0.5f + 0.5f) * float(im.width)), py = int((0.5f - c.y / c.w * 0.5f) * float(im.height));
            return &im.pixels[(size_t(py) * im.width + size_t(px)) * 4];
        };
        const simd_float3 left = simd_make_float3(-2.25f, 0, 1.2f), right2 = simd_make_float3(2.25f, 0, 1.2f);
        mr.setReflections(false);
        img = mr.capture(64, 64, mv, clear);
        p = pixelAt(img, left);
        check(near(p[0], 128) && near(p[1], 153) && near(p[2], 191), "mirror off: the blank mirror colour");
        mr.setReflections(true);
        img = mr.capture(64, 64, mv, clear);
        p = pixelAt(img, left);
        const uint8_t* q = pixelAt(img, right2);
        check(p[0] > 200 && p[1] < 40 && p[2] < 40, "left mirror reflects the red ground");
        check(q[0] > 200 && q[1] < 40 && q[2] < 40, "right mirror reflects the red ground too");
    }

    device->release();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all render tests passed\n");
    return failures ? 1 : 0;
}
