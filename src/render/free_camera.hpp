#pragma once

// The free camera ("CameraMan", the Tab key): nTSSG::cCameraCameraManController
// and the first-person controller it derives from, in the Mac binary
// (docs/camera-ui-mac.md §1.9). Plain C++ (no Metal); the app turns pose()
// into LotCamera::setFirstPerson.
//
// From the binary (BV): the spec's numbers (FirstPersonCameras.txt "CameraMan",
// SetCameraSpec 0xd92340 and 0x7a5f94), the key events (DoMessage 0xd913b0:
// move left/right/forward/back and lower/raise are +-1 on the x, y and z of the
// direction, Shift does not speed them up; zoom in/out step a fov table), and
// the update, once per frame with dt cut to ticklimit (Update 0x7a7ac4):
//
//   velocity target   = maxvelocity * (s + (1 - s) * h) * direction, s =
//                        velocitysensitivitymin, h = the camera's height above
//                        the floor as a fraction of [mincameraheight,
//                        maxcameraheight] (0xd90a4c)
//   position target   = the position + the velocity turned by the heading
//                        (x right, y forward, z up), held to the lot
//                        rectangle shrunk by constraintwidth (ConstrainTranslation 0x7a6330)
//   smoothing         = FUN_008c7c64 (the same as LotCamera::Smooth, along the
//                        line for vectors): position translationsteps /
//                        translationminchange, velocity velocitysteps /
//                        velocityminchange, fov fovsteps / fovminchange
//   height            = the position's z held to [floor + mincameraheight,
//                        floor + maxcameraheight] (Update), then h again
//
// so at the 7 steps and 50 ms frames of the file the camera moves at
// maxvelocity * 1000 / (50 * 7) tiles a second at full height (28.6 with 10),
// a tenth of that at the minimum height.
//
// The mouse look (round n39: the first-person base's HandleNotification
// FUN_00d92cd4 on the mouse's move, 0x1b637482): always on while the app is
// active (no button; the cursor hidden and put back at the window's middle,
// FUN_00d90164), heading -= dx x mouseheadingsensitivity and pitch -= dy x
// mousepitchsensitivity per pixel (the constructor's 0.0045 / 0.001 without
// them: CameraMan has only the heading's, 0.0005). Inferred: the orientation's
// own smoothing (orientationsteps 5 / minchange 0.1 on a quaternion) left out,
// the look is immediate; the pitch held to upperpitchlimit .. lowerpitchlimit;
// where the view starts (the previous camera's eye and direction, the fov
// table's nearest entry).
//
// The orbit (Shift + a d w s, round n39, docs/camera-ui-mac.md 1.11): each of
// those key-downs (repeats too) picks a centre (PickOrbitCenter 0x7a6590: a ray
// from the eye along the view into the scene; the app casts it) and any Stop
// Move / raise / lower ends it; they do not set the direction (only the plain
// keys do). While it lasts the update (0x7a7784) turns the position about the
// centre by the velocity in degrees per update, x (a / d) about Z and z (q / e)
// about X (FUN_000ea0dc(k vz, 0, k vx) = Rz Ry Rx, k = pi / 180), and aims at the
// far side as read: target = centre + M (centre - position), facing along
// M (centre - position) (cVector x cSqMatrix is M v, BV 0x14cf93c). Inferred:
// the scene pick as the floor under the ray.

#include "formats/camera_config.hpp"
#include "render/camera.hpp"

#include <array>
#include <functional>
#include <optional>

namespace os2 {

class FreeCamera {
public:
    explicit FreeCamera(const CameraDef& def); // the "CameraMan" camera

    // Starts at a view: the eye and direction of the camera it replaces
    // (MatchThisControllerWith), the fov of its table nearest `fovDegrees`.
    void start(simd_float3 eye, simd_float3 forward, float fovDegrees);
    // The key state: right (d / Right), forward (w / Up), up (e), each -1, 0
    // or 1 (Start / Stop Move ...; q lowers, e raises).
    void setMove(float right, float forward, float up) { dir_ = {right, forward, up}; }
    // Mouse pixels (x right, y down): the move from the window's middle.
    void look(float dx, float dy);
    // A step of the fov table: +1 zooms in (narrower), -1 out ("Zoom in" /
    // "Zoom Out", 0x8d377398 / 0x8d37738a).
    void zoomStep(int direction);
    // The orbit: its centre (PickOrbitCenter's hit), or none (no hit, or a key
    // released: the orbit ends).
    void setOrbit(std::optional<simd_float3> centre) { orbit_ = centre; }
    const std::optional<simd_float3>& orbit() const { return orbit_; }
    // Camera positions 0-5 ("Save / Restore Camera Position n").
    void savePosition(int slot);
    bool restorePosition(int slot);

    // The lot rectangle in tiles (held shrunk by constraintwidth) and the floor.
    void setBounds(simd_float2 lo, simd_float2 hi) { bounds_ = {lo, hi}; }
    void setFloorSampler(std::function<float(float, float)> f) { floor_ = std::move(f); }

    void update(double dt); // seconds
    LotCamera::FirstPerson pose() const;

    simd_float3 position() const { return pos_; }
    simd_float3 positionTarget() const { return posTarget_; }
    simd_float3 velocity() const { return vel_; }
    float heading() const { return heading_; } // radians, about Z, 0 = +X
    float pitch() const { return pitch_; }     // radians, up positive
    float fov() const;                          // degrees
    size_t fovIndex() const { return fovIndex_; }
    float heightFraction() const { return heightFraction_; }

    // The spec's numbers, as read.
    struct Params {
        std::vector<float> fov; // degrees
        float nearClip = 0.15f, farClip = 2000;
        float mouseHeading = 0.0045f, mousePitch = 0.001f; // the constructor's (BV 0xd92110)
        float upperPitch = 35, lowerPitch = -80; // degrees
        float maxVelocity = 10, velocitySensitivityMin = 0.1f;
        int translationSteps = 7, velocitySteps = 5, fovSteps = 3;
        float translationMin = 0.01f, velocityMin = 0.1f, fovMin = 0.001f;
        float minHeight = 1, maxHeight = 50;
        float tickLimit = 50; // ms
        float constraintWidth = -10;
    };
    const Params& params() const { return params_; }

private:
    float floorAt(float x, float y) const { return floor_ ? floor_(x, y) : 0.0f; }
    simd_float3 constrain(simd_float3 p) const;

    Params params_;
    simd_float3 pos_{0, 0, 10}, posTarget_{0, 0, 10}, vel_{0, 0, 0};
    simd_float3 dir_{0, 0, 0};
    std::optional<simd_float3> orbit_; // +0x2e4 with +0x2f0
    float heading_ = 0, pitch_ = 0;
    size_t fovIndex_ = 0;
    LotCamera::Smooth fovSmooth_; // radians
    float heightFraction_ = 0;
    std::optional<std::pair<simd_float2, simd_float2>> bounds_;
    std::function<float(float, float)> floor_;
    struct Saved {
        simd_float3 pos;
        float heading, pitch;
        size_t fovIndex;
    };
    std::array<std::optional<Saved>, 6> saved_;
};

} // namespace os2
