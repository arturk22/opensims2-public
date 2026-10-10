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

    // Defaults: zoom level 1 (40 units away), orientation 1 (55 degrees),
    // pitch halfway through [22, 45].
    check(near(cam.distance(), 40) && near(cam.yaw(), 55) && near(cam.pitch(), 33.5f), "defaults");

    // The pivot (raised to the subject height) is at the center of the screen, in front of the camera.
    simd_float4 c = clip(cam, simd_make_float3(0, 0, 1));
    check(near(c.x / c.w, 0) && near(c.y / c.w, 0) && c.w > 0 && c.z / c.w > 0 && c.z / c.w < 1, "pivot centered");

    // The eye sits at the horizontal distance and above the pivot by d * tan(pitch).
    simd_float3 e = cam.eye();
    check(near(std::hypot(e.x, e.y), 40, 1e-2f), "horizontal distance");
    check(near(e.z - 1, 40 * std::tan(33.5f * float(M_PI) / 180), 1e-2f), "height from pitch");

    // Zoom one level in and let it settle.
    cam.zoomBy(1);
    settle(cam);
    check(near(cam.zoom(), 2) && near(cam.distance(), 25), "zoom in");
    check(near(cam.pitch(), 14 + (50 - 14) * 0.5f), "pitch follows the zoom level's range");
    cam.zoomBy(10);
    settle(cam);
    check(near(cam.distance(), 3), "zoom clamps to the closest level");

    // Rotation steps through the table and wraps the short way round.
    cam.rotateBy(1);
    settle(cam);
    check(near(std::remainder(cam.yaw() - 100, 360.0f), 0), "rotate to next orientation");
    for (int i = 0; i < 6; ++i)
        cam.rotateBy(1);
    settle(cam);
    check(near(std::remainder(cam.yaw() - 10, 360.0f), 0), "rotate wraps past 325");
    check(cam.yaw() > 100, "keeps turning the same way across the wrap");
    cam.rotateBy(-1);
    settle(cam);
    check(near(std::remainder(cam.yaw() - 325, 360.0f), 0), "rotate backwards");

    // Pitch stays inside the range.
    cam.tilt(1000);
    check(near(cam.pitch(), 65), "tilt clamps to max");

    // Panning forward moves the pivot along the view direction on the floor.
    cam.reset();
    settle(cam);
    cam.setPivot(simd_make_float3(0, 0, 0));
    cam.pan(0, 1, 0.1);
    simd_float3 p = cam.pivot();
    float y = 55 * float(M_PI) / 180;
    check(near(p.z, 0) && p.x * std::cos(y) + p.y * std::sin(y) > 0 && near(p.x * std::sin(y) - p.y * std::cos(y), 0),
          "pan forward");

    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all camera tests passed\n");
    return failures ? 1 : 0;
}
