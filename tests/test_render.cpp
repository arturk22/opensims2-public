// Offscreen rendering checks: materials (color, blending, culling) reach the
// framebuffer as expected. Needs a Metal device.
#include "formats/image.hpp"
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

    device->release();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all render tests passed\n");
    return failures ? 1 : 0;
}
