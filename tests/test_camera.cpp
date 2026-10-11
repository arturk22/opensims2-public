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
    // Farther away, the same pixels move the pivot farther.
    const float near1 = simd_length(p);
    cam.zoomBy(-1);
    settle(cam);
    cam.setPivot(simd_make_float3(0, 0, 0));
    cam.pan(0, 1, 0.1);
    settle(cam);
    check(simd_length(cam.pivot()) > near1 * 1.3f, "pan scales with the distance");

    // The pivot stays inside the lot shrunk by constraintwidth (-10).
    cam.setBounds(simd_make_float2(0, 0), simd_make_float2(40, 30));
    cam.setPivot(simd_make_float3(-5, 50, 0));
    check(near(cam.pivot().x, 10) && near(cam.pivot().y, 20), "pivot held to the lot");
    cam.setBounds(simd_make_float2(0, 0), simd_make_float2(10, 10));
    check(near(cam.pivot().x, 5) && near(cam.pivot().y, 5), "small lot: pivot in the middle");

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

    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all camera tests passed\n");
    return failures ? 1 : 0;
}
