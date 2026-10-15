#pragma once

// The Create-a-Sim camera, as nTSSG::cCameraCreateASimController does it
// (Mac BV; docs/camera-ui-mac.md 1.6). A first-person camera: it stands at a
// target place and turn (the CAS studio's camera locators,
// "camera_face_adult", "camera_clothes_teen", "camera_family_0", ...) and
// glides from the previous camera's place to the new one (SetActive
// 0x7a9898, Update 0x7a9d68): t runs 0 -> 1 at
// lerp(interpolatespeedfrom of the old spec, interpolatespeedto, t) / the
// eye's travel per 50 ms (either speed 0: a cut), eased by s = t^2 (3 - 2t)
// for the position (0x7a928c), the turn (slerp, 0x7a93c8), the FOV (FOV
// 0x7a970c), the zoom and its heading and elevation. The zoom moves the eye
// along the camera's forward (local +Y, DAT_02c2fbf8) turned by
// zoomheading about Z and -zoomelevation about X (0x7aa284), clamped to
// [minzoom, maxzoom] (DoMessage 0x7aa0f8: keyboardzoomspeed a step), and
// smoothed (FUN_008c77d4, 10 steps). Which spec stands at which locator is
// the CAS - Backdrop object's SimAntics: camera effects CASCam_<spec>_Effect
// started on the studio's camera bones (docs/cas-mac.md, round n21). Plain
// C++.

#include "formats/camera_config.hpp"

#include <simd/simd.h>

namespace os2 {

// One CreateASim camera of CASCamera.txt (SetCameraSpec, Mac BV 0x7a8fb8).
struct CasCameraSpec {
    float fov = 30;                  // degrees, the full horizontal angle (the ctor's 30 without a table)
    float nearClip = 0.2f, farClip = 2000;
    float minZoom = 0, maxZoom = 0;  // metres along the zoom direction
    float zoomHeading = 0;           // radians (zoomheading)
    float zoomElevation = 0;         // radians, as stored: -zoomelevation
    float speedTo = 0.1f, speedFrom = 0.1f; // interpolatespeedto / from (ctor 0.1)
    float keyboardZoomSpeed = 0.05f; // a zoom step (ctor 0.05)
    float wheelSensitivity = 1;      // mousewheelsensitivity (ctor 1)
    static CasCameraSpec from(const CameraDef& def);
};

class CasCamera {
public:
    struct Pose {
        simd_float3 eye{0, 0, 0}, forward{0, 1, 0}, up{0, 0, 1};
        float fov = 30; // degrees, horizontal
        float nearClip = 0.2f, farClip = 2000;
    };

    // Switches to a camera spec at a target place and turn. The first one,
    // or `cut`, is taken at once with the zoom at minzoom; later ones glide
    // from where the camera is, the zoom going back to 0 on arrival.
    void setActive(const CasCameraSpec& spec, simd_float3 position, simd_quatf orientation, bool cut = false);
    // A zoom step (+1 in, -1 out; messages 0xCCA298C4 / 0x2CA298E9):
    // keyboardzoomspeed, kept in [minzoom, maxzoom].
    void zoomStep(int direction);
    // The mouse wheel (HandleRequest 0x9C17E415, the wheel's travel / 120):
    // notches x mousewheelsensitivity, kept in [minzoom, maxzoom].
    void wheel(float notches);
    // The puck's zoom buttons held (DoMessage: 0x6D41F7AB / 0xAD41F8D8 on
    // press, 0xED409660 / 0xED40965A on release): +-keyboardzoomspeed
    // until released (0); Update adds it x 50 / dt each update.
    void zoomHeld(int direction) { held_ = direction; }
    // Sets the zoom at once (a headless aid), kept in [minzoom, maxzoom].
    void setZoom(float zoom);

    void update(double ms);
    void snap(); // ends the glide and the zoom smoothing

    Pose pose() const;
    float transition() const { return t_; }
    float zoom() const;
    float zoomTarget() const { return zoomTarget_; }
    const CasCameraSpec& spec() const { return spec_; }
    bool active() const { return active_; }

private:
    simd_float3 eyeAt(float t) const;
    float ease(float t) const { return t * t * (3 - 2 * t); }

    bool active_ = false;
    CasCameraSpec spec_;
    float t_ = 1;                 // +0x2fc
    float travel_ = -1;           // +0x300: the eye's path length, -1 = to measure
    float speedFrom_ = 0.1f;      // +0x304
    simd_float3 pos0_{0, 0, 0}, pos1_{0, 0, 0};
    simd_quatf q0_{simd_quaternion(0.0f, 0.0f, 0.0f, 1.0f)}, q1_{simd_quaternion(0.0f, 0.0f, 0.0f, 1.0f)};
    float fov0_ = 30, fov1_ = 30;
    float zoom0_ = 0, zoom1_ = 0; // +0x348 / +0x34c
    float heading0_ = 0, heading1_ = 0, elevation0_ = 0, elevation1_ = 0;
    float zoomTarget_ = 0, zoomValue_ = 0; // +0x374 / +0x370
    int held_ = 0;
};

} // namespace os2
