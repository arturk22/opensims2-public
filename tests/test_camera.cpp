#include "app/camera_follow.hpp"
#include "app/camera_input.hpp"
#include "render/camera.hpp"
#include "render/cas_camera.hpp"

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

    // The rotate/tilt tool (playtest 34, tutorial "O Básico - Parte Um" step
    // 8): the middle button, Ctrl (or Option) + right, Ctrl + left on the
    // lot; plain right and left keep their own tools. Dragged, the vertical
    // motion tilts within the zoom level's pitch range and the horizontal
    // one turns the heading.
    {
        using B = CameraMouseInput::Button;
        check(CameraMouseInput::rotatePress(B::Middle, false, false), "middle button rotates");
        check(CameraMouseInput::rotatePress(B::Right, true, false), "Ctrl + right rotates");
        check(CameraMouseInput::rotatePress(B::Right, false, true), "Option + right rotates");
        check(!CameraMouseInput::rotatePress(B::Right, false, false), "plain right pans");
        check(CameraMouseInput::rotatePress(B::Left, true, false), "Ctrl + left rotates (trackpad)");
        check(!CameraMouseInput::rotatePress(B::Left, false, true), "plain left picks");
        for (int level = 0; level < 6; ++level) {
            LotCamera c(liveCamera());
            c.zoomBy(level - 1); // from level 1
            settle(c);
            const float lo = c.definition().pitchMin[size_t(level)], hi = c.definition().pitchMax[size_t(level)];
            // A drag as AppKit sends it: many small events (y down on the screen).
            auto drag = [&](float dx, float dy, int n) {
                for (int i = 0; i < n; ++i) {
                    c.rotateRequest(dx, dy);
                    c.update(1.0 / 60.0);
                }
                settle(c);
            };
            const float p0 = c.pitch(), y0 = c.yaw();
            drag(0.5f, -4, 10); // up 40 px: steeper
            check(c.pitch() > p0 + 1 && near(c.yaw(), y0), "drag up tilts, no turn");
            drag(0.5f, -4, 200); // far up: held at the level's top
            check(near(c.pitch(), hi, 1e-2f), "tilt held at the level's max");
            drag(-0.5f, 4, 400); // far down: held at its bottom
            check(near(c.pitch(), lo, 1e-2f), "tilt held at the level's min");
            const float p1 = c.pitch();
            drag(5, 1, 20); // right 100 px: turns, no tilt
            check(near(c.yaw(), y0 - 100 * 0.003f / rad, 0.05f) && near(c.pitch(), p1), "drag right turns");
        }
    }

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
    {
        // Following the selected Sim's floor (cCameraLive::Update): up
        // always, down only into a room, and never to the pool's level (a
        // swimmer keeps the view on the ground: live QA round 16).
        Lot lot;
        lot.width = lot.height = 4;
        lot.minLevel = -1, lot.levelCount = 3;
        lot.elevations.assign(size_t(3 * 5 * 5), 0.0f);
        LotCamera c(liveCamera());
        LotCameraFollow f(c, lot);
        vm::CodeLibrary code;
        vm::VM machine(code);
        vm::Object& sim = machine.createObject(0x7FD46CD0);
        sim.position = {1.5f, 1.5f, 0}; // VM level 0: in the pool (lot level -1)
        sim.data[vm::objdata::Room] = 1;
        f.follow(machine, sim.id);
        check(f.level() == 0, "a swimmer leaves the view on the ground");
        sim.position.level = 2;
        f.follow(machine, sim.id);
        check(f.level() == 1, "the view goes up with the Sim");
        sim.position.level = 1, sim.data[vm::objdata::Room] = 0;
        f.follow(machine, sim.id);
        check(f.level() == 1, "down to the yard (no room): the view stays");
        sim.data[vm::objdata::Room] = 3;
        f.follow(machine, sim.id);
        check(f.level() == 0, "down into a room: the view follows");
    }
    {
        // The CreateASim camera (CASCamera.txt's CAS_ClothesAdult): at the
        // locator at once, zoom 0 .. 2.3 in 0.05 steps, the zoom moving the
        // eye forward and up (zoomelevation -36), then a glide to another
        // locator that eases in and ends there with the zoom back to 0.
        CameraDef d;
        d.fov = {54};
        d.nearClip = {0.2f};
        d.farClip = {2000};
        d.params = {{"maxzoom", 2.3f}, {"minzoom", 0}, {"zoomheading", 8}, {"zoomelevation", -36},
                    {"interpolatespeedto", 0.1f}, {"interpolatespeedfrom", 0.1f}, {"keyboardzoomspeed", 0.05f}};
        const CasCameraSpec spec = CasCameraSpec::from(d);
        check(near(spec.zoomElevation, 36 * float(M_PI) / 180), "zoomelevation stored negated, radians");
        CasCamera c;
        // Facing +Y (the locator's forward); the turns are about the world's Z and X.
        c.setActive(spec, simd_make_float3(0, -5, 1.4f), simd_quaternion(0.0f, 0.0f, 0.0f, 1.0f));
        CasCamera::Pose p = c.pose();
        check(near(p.eye.y, -5) && near(p.forward.y, 1) && near(p.fov, 54), "CAS camera at its locator");
        for (int i = 0; i < 100; ++i)
            c.zoomStep(1);
        check(near(c.zoomTarget(), 2.3f), "zoom kept under maxzoom");
        for (int i = 0; i < 400; ++i)
            c.update(50);
        p = c.pose();
        check(p.eye.y > -3.5f && p.eye.z > 2.5f && near(p.forward.y, 1), "zoom moves the eye forward and up, the view unturned");
        c.setActive(spec, simd_make_float3(0, 5, 1), simd_quaternion(float(M_PI) / 2, simd_make_float3(0, 0, 1)));
        check(c.transition() == 0, "a new camera glides");
        c.update(50);
        const float t1 = c.transition();
        int frames = 1;
        while (c.transition() < 1 && frames < 10000)
            c.update(50), ++frames;
        p = c.pose();
        check(t1 > 0 && frames > 10 && frames < 10000 && near(p.eye.y, 5) && near(p.eye.z, 1) && near(c.zoom(), 0),
              "the glide arrives, zoom back to 0");
    }
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all camera tests passed\n");
    return failures ? 1 : 0;
}
