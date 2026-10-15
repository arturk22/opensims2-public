#include "app/camera_follow.hpp"
#include "app/camera_input.hpp"
#include "app/camera_mode.hpp"
#include "app/object_fade.hpp"
#include "render/camera.hpp"
#include "render/cas_camera.hpp"
#include "render/free_camera.hpp"

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

// Values from the game's BuildCamera.txt (the top-down view).
static CameraDef buildCamera() {
    CameraDef c;
    c.name = c.controller = "Build";
    c.defaultZoom = 1;
    c.distances = {75, 32, 24, 16, 8};
    c.fov = {33.2f, 28.6f, 24.1f, 23.0f, 21.8f};
    c.pitchMin = {84, 84, 84, 84, 84};
    c.pitchMax = {84, 84, 84, 84, 84};
    c.nearClip = {1, 1, 0.8f, 0.5f, 0.3f};
    c.farClip = {2000, 2000, 2000, 2000, 2000};
    c.defaultOrientation = 1;
    c.yaws = {0, 45, 90, 135, 180, 225, 270, 315};
    c.zoomSmoothing = 5;
    c.spinSmoothing = 8;
    c.params["defaultminchange"] = 0.02f;
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
        // A zoom button held (Update, BV 0x7a9d68): keyboardzoomspeed x 50 /
        // dt per update, so a 25 ms update moves twice as far as a 50 ms one.
        c.setZoom(1);
        c.zoomHeld(-1);
        c.update(50);
        check(near(c.zoomTarget(), 0.95f), "held zoom: keyboardzoomspeed per 50 ms update");
        c.update(25);
        check(near(c.zoomTarget(), 0.85f), "held zoom: x 50 / dt (twice the step at 25 ms)");
        c.zoomHeld(0);
        c.update(50);
        check(near(c.zoomTarget(), 0.85f), "released: the zoom stays");
        c.wheel(0.1f);
        check(near(c.zoomTarget(), 0.95f), "the wheel: notches x mousewheelsensitivity (1 without the parameter)");
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
    {
        // Following a Sim (cTSCameraSmoothSims1Controller::Update, BV 0x86ffb8): the
        // pivot's target moves to the Sim once their floor points are more than
        // 30 pixels apart on the screen; the pivot's target, not where the pivot
        // is on its way there. The player's own moves end it (SetTracking(false)).
        Lot lot;
        lot.width = lot.height = 60;
        lot.minLevel = 0, lot.levelCount = 1;
        lot.elevations.assign(size_t(61 * 61), 0.0f);
        LotCamera c(liveCamera());
        LotCameraFollow f(c, lot);
        c.setViewport(1024, 768);
        c.setPivot(simd_make_float3(30, 30, 0));
        settle(c);
        vm::CodeLibrary code;
        vm::VM machine(code);
        vm::Object& other = machine.createObject(0x7FD46CD0);
        other.position = {1.5f, 1.5f, 1};
        other.data[vm::objdata::Room] = 1;
        vm::Object& sim = machine.createObject(0x7FD46CD0);
        sim.position = {30.0f, 30.0f, 1};
        sim.data[vm::objdata::Room] = 1;
        f.setTracked(sim.id);
        f.follow(machine, other.id);
        check(near(c.pivotTarget().x, 30) && near(c.pivotTarget().y, 30), "a Sim inside the dead zone leaves the target");
        sim.position.x = 30.2f; // a few pixels away
        f.follow(machine, other.id);
        check(near(c.pivotTarget().x, 30), "inside the 30-pixel dead zone: no retarget");
        sim.position.x = 36;
        f.follow(machine, other.id);
        check(near(c.pivotTarget().x, 36) && near(c.pivotTarget().y, 30), "past the dead zone: the Sim is the target");
        // The pivot is still on its way (target 36), the Sim moves a little: the target, not the
        // pivot, is what the dead zone is measured from.
        check(c.pivot().x < 35, "the pivot has not caught up yet");
        sim.position.x = 36.2f;
        f.follow(machine, other.id);
        check(near(c.pivotTarget().x, 36), "measured from the target: 0.2 tiles from it is inside the dead zone");
        // The player pans: tracking ends.
        c.pan(1, 0, 0.05);
        sim.position.x = 50;
        f.follow(machine, other.id);
        check(f.tracked() == 0 && c.pivotTarget().x < 49, "a move the player asked for ends the following");
        // Centring on a Sim (the right-click on its thumbnail starts with it, then tracks).
        check(f.centre(machine, sim.id) && near(c.pivotTarget().x, 50), "centre on a Sim");
        f.setTracked(sim.id);
        f.follow(machine, other.id);
        check(f.tracked() == sim.id, "centring first does not end the following it starts");
        sim.position.x = 60;
        f.follow(machine, other.id);
        check(near(c.pivotTarget().x, 60), "and it follows");
        c.moveTo(simd_make_float3(10, 10, 0)); // Return: centre on the active Sim
        f.follow(machine, other.id);
        check(f.tracked() == 0, "centring (Return) stops it");
    }
    {
        // The objects that obstruct the view fade near the camera (cObject::
        // UpdateVisibilityAndRendering): alpha = (depth / distance) ^ falloff
        // under the distance, from the camera's objectFade* params.
        const auto cams = parseCameraConfig(R"(
camera "Live" Live
zoom 1 (70 40 25 15 8 3)
setparam objectFadeDistance    10
setparam objectFadeFalloff     4
setparam objectFadeAllObjects  0
setparam objectFadeCineCam     0
camera "Build" Build
zoom 1 (75 32)
camera "Animated"
   type Animated
   setparam objectFadeDistance    1
   setparam objectFadeFalloff     2
   setparam objectFadeAllObjects  0
   setparam objectFadeCineCam     1
end
)");
        const auto live = objectFadeParams(*findCamera(cams, "Live"));
        check(live && near(live->distance, 10) && near(live->falloff, 4) && !live->allObjects && !live->cineCam,
              "the Live camera's fade params");
        check(!objectFadeParams(*findCamera(cams, "Build")), "the top-down camera fades nothing");
        const CameraDef* cine = findCamera(cams, "Animated");
        check(cine && objectFadeParams(*cine) && objectFadeParams(*cine)->cineCam, "the cinematic camera has its own fade (not done)");
        check(live && near(objectFadeAlpha(*live, 5), 0.0625f) && near(objectFadeAlpha(*live, 8), 0.4096f, 1e-3f),
              "alpha = (depth / 10) ^ 4: 5 tiles away 6%, 8 tiles 41%");
        check(live && near(objectFadeAlpha(*live, 10), 1) && near(objectFadeAlpha(*live, 25), 1), "fully drawn from the distance on");
        check(live && near(objectFadeAlpha(*live, 0), 1) && near(objectFadeAlpha(*live, -3), 1), "behind the eye: not faded");
        check(live && objectFades(*live, 1 << 3) && !objectFades(*live, 0x200 | 1), "only Flag Field 2 bit 3 (Obstructs View) fades");
        ObjectFadeParams all = live ? *live : ObjectFadeParams{};
        all.allObjects = true;
        check(objectFades(all, 0), "objectFadeAllObjects fades every object");
    }
    {
        // Back from the free camera: the Live camera takes the view over (MatchThisControllerWith).
        LotCamera c(liveCamera());
        c.setBounds(simd_make_float2(0, 0), simd_make_float2(64, 64));
        // At (40, 20), 11 up, looking 40 degrees down towards -X: the view's pitch is inside the first level's range.
        const float rad = float(M_PI) / 180;
        const simd_float3 fwd = simd_make_float3(-std::cos(40 * rad), 0, -std::sin(40 * rad));
        c.matchView(simd_make_float3(40, 20, 11), fwd);
        check(near(c.yaw(), 0), "heading: the eye is on the +X side of what it looks at");
        check(near(c.distance(), 10 / std::sin(40 * rad), 1e-2f), "distance: down to the pivot's height along the view");
        check(near(c.pivot().x, 40 - c.distance() * std::cos(40 * rad), 1e-2f) && near(c.pivot().y, 20, 1e-2f),
              "the pivot is that far along the view");
        check(near(c.pitch(), 40, 1e-2f), "the pitch is the view's");
        const simd_float3 e = c.eye();
        check(near(e.x, 40, 0.05f) && near(e.y, 20, 0.05f) && near(e.z, 11, 0.05f), "the Live camera's eye is where the free camera's was");
        // A steep view is held to the first level's pitch (30 .. 40 degrees).
        c.matchView(simd_make_float3(40, 20, 11), simd_make_float3(0, 0.2f, -1));
        check(near(c.pitch(), 40, 1e-2f) || c.pitch() <= 40.01f, "a steep view keeps the first level's top pitch");
    }
    {
        // The free camera (Tab, cCameraCameraManController): FirstPersonCameras.txt's "CameraMan".
        const auto cams = parseCameraConfig(R"(
camera     "CameraMan" CameraMan
fov        (20 25 30 35 40 45 50 55 60 65 70 75 80 85 90)
clip       0.15 2000
setparam defaultminchange        0.5
setparam mouseheadingsensitivity 0.0005
setparam upperpitchlimit         35
setparam lowerpitchlimit         -80
setparam orientationminchange	 0.1
setparam orientationsteps    	 5
setparam fovminchange		 0.001
setparam fovsteps		 3
setparam mincameraheight         1
setparam maxcameraheight         50
setparam translationminchange	 0.01
setparam translationsteps    	 7
setparam maxvelocity             10
setparam velocitysensitivitymin  0.1
setparam velocityminchange	 0.1
setparam velocitysteps    	 5
setparam ticklimit               50
setparam constraintwidth         -10
)");
        const CameraDef* def = findCamera(cams, "CameraMan");
        check(def != nullptr, "the CameraMan camera parses");
        if (def) {
            FreeCamera f(*def);
            const auto& p = f.params();
            check(p.fov.size() == 15 && near(p.nearClip, 0.15f) && near(p.maxVelocity, 10) && p.translationSteps == 7 &&
                      p.velocitySteps == 5 && near(p.maxHeight, 50) && near(p.tickLimit, 50),
                  "the spec's numbers");
            // Starts where the previous camera was, looking where it looked; the pitch kept in its limits.
            f.start(simd_make_float3(10, 10, 30), simd_make_float3(0, 1, -0.5f), 45);
            check(near(f.heading(), float(M_PI) / 2) && f.pitch() < 0 && near(f.fov(), 45) && f.fovIndex() == 5,
                  "starts at the previous view, the fov table's 45");
            f.start(simd_make_float3(10, 10, 30), simd_make_float3(0, 0.1f, -1), 45);
            check(near(f.pitch(), -80 * float(M_PI) / 180), "pitch held to lowerpitchlimit");
            // Forward at full height (50): maxvelocity * 1000 / (50 ms * 7 steps) = 28.6 tiles a second once the velocity is up.
            f.start(simd_make_float3(0, 0, 50), simd_make_float3(1, 0, 0), 45);
            f.setMove(0, 1, 0);
            for (int i = 0; i < 180; ++i)
                f.update(1.0 / 60);
            check(f.position().x > 76 && f.position().x < 82 && near(f.position().y, 0, 0.05f) && near(f.position().z, 50, 1e-2f),
                  "full speed at full height: 28.6 tiles a second after the velocity ramps up");
            // The speed scales with the height (velocitysensitivitymin 0.1 at the minimum height).
            f.start(simd_make_float3(0, 0, 1), simd_make_float3(1, 0, 0), 45);
            f.setMove(0, 1, 0);
            for (int i = 0; i < 180; ++i)
                f.update(1.0 / 60);
            check(f.position().x > 6.5f && f.position().x < 8.5f, "a tenth of the speed at the lowest height");
            // Strafing and the heading: facing +Y, right is +X.
            f.start(simd_make_float3(0, 0, 50), simd_make_float3(0, 1, 0), 45);
            f.setMove(1, 0, 0);
            for (int i = 0; i < 60; ++i)
                f.update(1.0 / 60);
            check(f.position().x > 10 && near(f.position().y, 0, 0.05f), "right of a camera facing +Y is +X");
            // Raise and lower: z held to [floor + 1, floor + 50], the floor under the camera sampled.
            f.setFloorSampler([](float, float) { return 10.0f; });
            f.start(simd_make_float3(0, 0, 30), simd_make_float3(1, 0, 0), 45);
            f.setMove(0, 0, 1);
            for (int i = 0; i < 1200; ++i)
                f.update(1.0 / 60);
            check(near(f.position().z, 60, 1e-3f) && near(f.heightFraction(), 1, 1e-3f), "raised to floor + maxcameraheight");
            f.setMove(0, 0, -1);
            for (int i = 0; i < 1200; ++i)
                f.update(1.0 / 60);
            check(near(f.position().z, 11, 1e-3f) && near(f.heightFraction(), 0, 1e-3f), "lowered to floor + mincameraheight");
            // The lot's rectangle shrunk by constraintwidth (-10): 10 tiles out past the edge.
            f.setFloorSampler(nullptr);
            f.setBounds(simd_make_float2(0, 0), simd_make_float2(30, 30));
            f.start(simd_make_float3(15, 15, 50), simd_make_float3(1, 0, 0), 45);
            f.setMove(0, 1, 0);
            for (int i = 0; i < 600; ++i)
                f.update(1.0 / 60);
            check(near(f.position().x, 40, 0.1f), "held 10 tiles past the lot's edge");
            // One huge frame counts as ticklimit (50 ms).
            FreeCamera a(*def), b(*def);
            for (FreeCamera* c : {&a, &b}) {
                c->start(simd_make_float3(0, 0, 50), simd_make_float3(1, 0, 0), 45);
                c->setMove(0, 1, 0);
            }
            a.update(2.0);
            b.update(0.05);
            check(near(a.velocity().y, b.velocity().y) && near(a.position().x, b.position().x), "dt cut to ticklimit");
            // The mouse: right turns right (the heading falls), up looks up, the pitch held in its limits.
            FreeCamera m(*def);
            m.start(simd_make_float3(0, 0, 10), simd_make_float3(1, 0, 0), 45);
            m.look(100, 0);
            check(m.heading() < 0 && near(m.heading(), -100 * 0.0005f * FreeCamera::kMouseUnit), "mouse right turns right");
            m.look(0, -10000);
            check(near(m.pitch(), 35 * float(M_PI) / 180), "pitch held to upperpitchlimit");
            m.look(0, 100000);
            check(near(m.pitch(), -80 * float(M_PI) / 180), "pitch held to lowerpitchlimit (looking down)");
            // The fov table: Zoom in steps to a narrower fov, held at the table's ends, smoothed.
            FreeCamera z(*def);
            z.start(simd_make_float3(0, 0, 10), simd_make_float3(1, 0, 0), 45);
            z.zoomStep(1);
            check(z.fovIndex() == 4, "zoom in: the next narrower fov");
            for (int i = 0; i < 100; ++i)
                z.update(1.0 / 60);
            check(near(z.fov(), 40, 1e-2f), "the fov reaches 40");
            for (int i = 0; i < 40; ++i)
                z.zoomStep(1);
            check(z.fovIndex() == 0, "held at the narrowest");
            for (int i = 0; i < 40; ++i)
                z.zoomStep(-1);
            check(z.fovIndex() == 14, "held at the widest");
            // Saved positions.
            z.start(simd_make_float3(5, 6, 20), simd_make_float3(0, 1, 0), 45);
            z.savePosition(2);
            z.start(simd_make_float3(0, 0, 10), simd_make_float3(1, 0, 0), 45);
            check(z.restorePosition(2) && near(z.position().x, 5) && near(z.position().z, 20) && near(z.heading(), float(M_PI) / 2),
                  "restore a saved position");
            check(!z.restorePosition(4), "nothing saved there");
        }
    }
    {
        // The top-down view: "Build" takes over "Live" with the pivot, the
        // heading, the 3D distance and the pitch fraction as numbers
        // (cCameraLive::SetActive, BV 0x6a56e0), the tables of BuildCamera.txt.
        LotCamera c(liveCamera());
        c.setBounds(simd_make_float2(0, 0), simd_make_float2(64, 64));
        c.setPivot(simd_make_float3(20, 30, 0));
        c.rotateBy(2);
        c.zoomBy(2);
        c.savePosition(1);
        settle(c);
        const float yaw = c.yaw(), dist = c.distance();
        const LotCamera::Pose before = c.pose();
        c.setDefinition(buildCamera());
        check(c.definition().name == "Build", "the Build camera is current");
        check(near(c.pivot().x, 20) && near(c.pivot().y, 30), "top-down keeps the pivot");
        check(near(c.yaw(), yaw) && near(c.distance(), dist), "top-down keeps the heading and the 3D distance as numbers");
        check(near(c.pitch(), 84), "the Build camera looks almost straight down");
        check(near(c.pose().pitchT, before.pitchT), "the pitch fraction is carried over");
        check(near(c.zoom(), 3.125f) && near(c.fov(), 23.0f + (21.8f - 23.0f) * 0.125f), "the fov follows the Build table (15 is between its levels 3 and 4)");
        c.rotateBy(1);
        settle(c);
        check(near(std::remainder(c.yaw() - 180, 360.0f), 0), "the Build camera has its own orientations (145 -> 135 -> 180)");
        c.setDefinition(liveCamera());
        check(c.definition().name == "Live" && near(c.pivot().x, 20) && near(c.pivot().y, 30), "back to the Live camera");
        check(c.pitch() > 10 && c.pitch() < 56, "the Live pitch range again");
        // Each camera keeps its own saved positions.
        check(c.restorePosition(1, false), "the Live camera's saved position survives the round trip");
        c.setDefinition(buildCamera());
        check(!c.restorePosition(1, false), "the Build camera has no position saved yet");
        c.savePosition(1);
        c.setDefinition(liveCamera());
        check(c.restorePosition(1, false), "positions per camera");
        // Build holds the pivot to the lot rectangle (constraintwidth 0), Live lets it 10 tiles out.
        c.setPivot(simd_make_float3(-8, 70, 0));
        check(near(c.pivot().x, -8) && near(c.pivot().y, 70), "Live: pivot 10 tiles out");
        c.setDefinition(buildCamera());
        check(near(c.pivot().x, 0) && near(c.pivot().y, 64), "top-down: pivot held to the lot");
    }
    {
        // cTSMainToolbar::SetMode / SetTopDown (BV 0x1ade24 / 0x1a9674) and the
        // key T of the Buy and Build panels.
        using S = CameraSelector;
        S s;
        check(s.controller() == S::Controller::Live && !s.topDown(), "starts in Live");
        check(!s.toggleTopDown(), "T does nothing in Live mode");
        s.setMode(S::Mode::Build);
        check(s.controller() == S::Controller::Live, "Build mode with the flag off: the Live camera");
        check(s.toggleTopDown() && s.controller() == S::Controller::Build && s.topDown(), "T in Build mode: top-down");
        s.setMode(S::Mode::Buy);
        check(s.controller() == S::Controller::Build, "Buy mode keeps the top-down view");
        s.setMode(S::Mode::Live);
        check(s.controller() == S::Controller::Live && s.topDown(), "Live mode shows the Live camera but remembers the flag");
        check(!s.toggleTopDown() && s.topDown(), "T in Live mode changes nothing");
        s.setMode(S::Mode::Build);
        check(s.controller() == S::Controller::Build, "back in Build mode: top-down again");
        check(s.toggleTopDown() && s.controller() == S::Controller::Live && !s.topDown(), "T again: the Live camera, flag off");
        s.setMode(S::Mode::Buy);
        s.setMode(S::Mode::Live);
        s.setMode(S::Mode::Buy);
        check(s.controller() == S::Controller::Live && !s.topDown(), "the flag off stays off across modes");
        // Tab: the free camera and back to where it came from; Escape in it too.
        S t;
        t.cycleController();
        check(t.controller() == S::Controller::CameraMan && t.freeCamera(), "Tab: the free camera");
        t.cycleController();
        check(t.controller() == S::Controller::Live, "Tab again: back to Live");
        t.setMode(S::Mode::Build);
        t.toggleTopDown();
        t.cycleController();
        check(t.freeCamera() && t.escape() && t.controller() == S::Controller::Build,
              "Escape leaves the free camera for the top-down view it came from");
        check(!t.escape(), "Escape elsewhere is not the camera's");
        t.cycleController();
        t.cycleController();
        check(t.controller() == S::Controller::Build, "Tab from the top-down view and back");
        t.cycleController();
        t.setMode(S::Mode::Live);
        check(!t.freeCamera() && t.controller() == S::Controller::Live, "a mode change ends the free camera");
        t.cycleController();
        t.toggleTopDown();
        check(t.freeCamera(), "no top-down button in Live mode: T leaves the free camera alone");
    }
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all camera tests passed\n");
    return failures ? 1 : 0;
}
