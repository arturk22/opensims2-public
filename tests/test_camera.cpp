#include "app/camera_follow.hpp"
#include "render/camera.hpp"

#include <cmath>
#include <cstdio>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// Values from the game's LiveCamera.txt.
static CameraDef liveCamera() {
    CameraDef c;
    c.name = c.controller = "Live";
    c.defaultZoom = 1;
    c.distances = {70, 40, 25, 15, 8, 3};
    c.fov = {45, 45, 45, 45, 45, 45};
    c.pitchMin = {30, 22, 14, 10, 6, 2};
    c.pitchMax = {40, 45, 50, 55, 60, 65};
    c.nearClip = {0.6f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f};
    c.farClip = {2000, 2000, 2000, 2000, 2000, 2000};
    c.defaultOrientation = 1;
    c.yaws = {10, 55, 100, 145, 190, 235, 280, 325};
    c.zoomSmoothing = 5;
    c.spinSmoothing = 8;
    c.params["pivotheightabovefloor"] = 1;
    c.params["keyboardtranslationspeed"] = 50;
    c.params["defaultminchange"] = 0.02f;
    c.params["constraintwidth"] = -10;
    c.wheel = 5;
    return c;
}

static void settle(LotCamera& cam) {
    for (int i = 0; i < 600; ++i)
        cam.update(1.0 / 60.0);
}

// Clip-space position of a world point.
static simd_float4 clip(const LotCamera& cam, simd_float3 p) {
    simd_float4x4 vp = simd_mul(cam.projection(16.0f / 9.0f), cam.view());
    return simd_mul(vp, simd_make_float4(p.x, p.y, p.z, 1));
}

int main() {
    LotCamera cam(liveCamera());
    const float rad = float(M_PI) / 180;

    // Defaults: zoom level 1 (40 units away), orientation 1 (55 degrees),
    // pitch at defaultcamerapitch (0.15) of [22, 45].
    check(near(cam.distance(), 40) && near(cam.yaw(), 55) && near(cam.pitch(), 22 + 23 * 0.15f), "defaults");

    // The pivot (raised to the subject height) is at the center of the screen, in front of the camera.
    simd_float4 c = clip(cam, simd_make_float3(0, 0, 1));
    check(near(c.x / c.w, 0) && near(c.y / c.w, 0) && c.w > 0 && c.z / c.w > 0 && c.z / c.w < 1, "pivot centered");

    // The eye sits at the 3D distance along the line of sight, on the
    // +(cos h, sin h) side of the pivot.
    simd_float3 e = cam.eye();
    const float p0 = cam.pitch() * rad;
    check(near(simd_length(e - simd_make_float3(0, 0, 1)), 40, 1e-2f), "3D distance");
    check(near(e.x, 40 * std::cos(p0) * std::cos(55 * rad), 1e-2f) &&
              near(e.y, 40 * std::cos(p0) * std::sin(55 * rad), 1e-2f),
          "eye towards the heading");
    check(near(e.z - 1, 40 * std::sin(p0), 1e-2f), "height from pitch");

    // Zoom one level in and let it settle.
    cam.zoomBy(1);
    settle(cam);
    check(near(cam.zoom(), 2) && near(cam.distance(), 25), "zoom in");
    check(near(cam.pitch(), 14 + (50 - 14) * 0.15f), "pitch follows the zoom level's range");
    cam.zoomBy(10);
    settle(cam);
    check(near(cam.distance(), 3), "zoom clamps to the closest level");

    // The wheel: d += notches * wheel * (d + 10) / -50, held to the table.
    cam.zoomBy(-5);
    settle(cam);
    cam.wheel(1);
    settle(cam);
    check(near(cam.distance(), 70 - 5 * 80 / 50.0f, 1e-2f), "wheel step");
    cam.wheel(-100);
    settle(cam);
    check(near(cam.distance(), 70), "wheel clamps to the farthest level");
    check(near(cam.zoom(), 0), "zoom level of the distance");

    // Smoothing: 50 ms frames over N (zoom N = 5): one 50 ms update covers 1/5.
    cam.zoomBy(1); // 70 -> 40
    cam.update(0.05);
    check(near(cam.distance(), 70 - 30 / 5.0f, 1e-3f), "exponential step");
    settle(cam);
    // ... never slower than defaultminchange (0.02) per update.
    LotCamera::Smooth s{0, 0.03f, 5, 0.02f};
    s.update(1);
    check(near(s.value, 0.02f), "minimum change");
    s.update(1);
    check(near(s.value, 0.03f), "reaches the target");

    // Rotation steps through the table and wraps the short way round.
    cam.rotateBy(1);
    settle(cam);
    check(near(std::remainder(cam.yaw() - 100, 360.0f), 0), "rotate to next orientation");
    for (int i = 0; i < 6; ++i) {
        cam.rotateBy(1);
        settle(cam);
    }
    check(near(std::remainder(cam.yaw() - 10, 360.0f), 0), "rotate wraps past 325");
    cam.rotateBy(-1);
    settle(cam);
    check(near(std::remainder(cam.yaw() - 325, 360.0f), 0), "rotate backwards");

    // Mouse rotate: only the dominant axis; heading -= dx * 0.003 rad.
    const float before = cam.yaw(), tBefore = cam.pitch();
    cam.rotateRequest(100, 20);
    settle(cam);
    check(near(cam.yaw(), before - 0.3f / rad, 1e-2f) && near(cam.pitch(), tBefore), "rotate request (x)");

    // Pitch stays inside the range.
    cam.tilt(1000);
    check(near(cam.pitch(), 45), "tilt clamps to max");

    // Moving forward moves the pivot along the view direction on the floor.
    cam.reset();
    settle(cam);
    cam.setPivot(simd_make_float3(0, 0, 0));
    cam.pan(0, 1, 0.1);
    settle(cam);
    simd_float3 p = cam.pivot();
    float y = 55 * rad; // looking along -(cos y, sin y)
    check(near(p.z, 0) && p.x * std::cos(y) + p.y * std::sin(y) < 0 &&
              near(p.x * std::sin(y) - p.y * std::cos(y), 0, 1e-2f),
          "pan forward");
    // Sideways (Right / d, "Start Move Right" of the accelerators
    // 0x6A9362F2/F3): the pivot moves across the view, the heading stays.
    {
        const float yaw0 = cam.yaw();
        cam.setPivot(simd_make_float3(0, 0, 0));
        cam.pan(1, 0, 0.1);
        settle(cam);
        const simd_float3 q = cam.pivot();
        check(near(cam.yaw(), yaw0) && simd_length(q) > 0.1f && near(q.x * std::cos(y) + q.y * std::sin(y), 0, 1e-2f),
              "pan right moves across the view without turning");
    }
    // Farther away, the same pixels move the pivot farther.
    const float near1 = simd_length(p);
    cam.zoomBy(-1);
    settle(cam);
    cam.setPivot(simd_make_float3(0, 0, 0));
    cam.pan(0, 1, 0.1);
    settle(cam);
    check(simd_length(cam.pivot()) > near1 * 1.3f, "pan scales with the distance");

    // The pivot stays inside the lot shrunk by constraintwidth: -10 lets it
    // 10 tiles out (clamp(x, min + w, max - w), FUN_006a456c).
    cam.setBounds(simd_make_float2(0, 0), simd_make_float2(40, 30));
    cam.setPivot(simd_make_float3(-15, 50, 0));
    check(near(cam.pivot().x, -10) && near(cam.pivot().y, 40), "pivot held to the lot");
    cam.setPivot(simd_make_float3(3, 2, 0));
    check(near(cam.pivot().x, 3) && near(cam.pivot().y, 2), "pivot near the lot's corner kept");
    cam.setBounds(simd_make_float2(0, 0), simd_make_float2(10, 10));
    check(near(cam.pivot().x, 3) && near(cam.pivot().y, 2), "small lot: pivot inside kept");

    // Floor height: the pivot sits on the sampled floor.
    cam.setBounds(simd_make_float2(-100, -100), simd_make_float2(100, 100));
    cam.setFloorSampler([](float, float) { return 3.0f; });
    cam.setPivot(simd_make_float3(0, 0, 0));
    check(near(cam.pivot().z, 3), "pivot on the floor");

    // Saved positions.
    cam.savePosition(2);
    cam.setPivot(simd_make_float3(20, 20, 0));
    cam.rotateBy(2);
    check(cam.restorePosition(2, false) && near(cam.pivot().x, 0) && near(std::remainder(cam.yaw() - 55, 360.0f), 0),
          "restore camera position");
    check(!cam.restorePosition(4, true), "empty camera position");

    // The FOV is the horizontal angle (BV 0xccd254): x scale 1 / tan(fov / 2),
    // y scale that times the aspect, up to the game's 4:3; wider windows keep
    // the 4:3 height (ours); the CAS camera keeps a vertical one.
    {
        LotCamera h(liveCamera());
        const float t = 1.0f / std::tan(45.0f * rad * 0.5f);
        const simd_float4x4 a = h.projection(4.0f / 3.0f);
        check(near(a.columns[0][0], t) && near(a.columns[1][1], t * 4 / 3), "horizontal fov at 4:3");
        const simd_float4x4 s = h.projection(1.0f);
        check(near(s.columns[0][0], t) && near(s.columns[1][1], t), "horizontal fov, narrower than 4:3");
        const simd_float4x4 p = h.projection(16.0f / 9.0f);
        check(near(p.columns[1][1], t * 4 / 3) && near(p.columns[0][0], t * 4 / 3 * 9 / 16),
              "wider than 4:3 keeps the 4:3 height");
        // At the closest zoom (3 along the view, LiveCamera.txt) a whole Sim
        // (~1.8 tall) fits around the pivot 1 over the floor at 16:9.
        check(3.0f / p.columns[1][1] > 0.9f, "closest zoom fits a Sim");
        CameraDef v = liveCamera();
        v.verticalFov = true;
        const simd_float4x4 q = LotCamera(v).projection(2.0f);
        check(near(q.columns[1][1], t) && near(q.columns[0][0], t / 2), "vertical fov (CAS)");
    }
    {
        // Before the VM: the camera starts on the floor of the lot's first
        // Sim (a foundation's floor, 0.75 up, here level 1), else the ground.
        Lot lot;
        lot.width = lot.height = 4;
        lot.minLevel = 0, lot.levelCount = 2;
        lot.elevations.assign(size_t(2 * 5 * 5), 0.0f);
        for (size_t i = 25; i < 50; ++i)
            lot.elevations[i] = 0.75f;
        check(LotCameraFollow::startLevel(lot) == 0, "camera starts on the ground without Sims");
        Lot::Object sim;
        sim.person = true;
        sim.position = {2.5f, 2.5f, 0.75f};
        lot.objects.push_back(sim);
        check(LotCameraFollow::startLevel(lot) == 1, "camera starts on the first Sim's floor");
    }
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all camera tests passed\n");
    return failures ? 1 : 0;
}
