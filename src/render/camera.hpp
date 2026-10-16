#pragma once

#include "formats/camera_config.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace os2 {

// The aspect at which the game's horizontal FOV is kept: its default mode,
// 800x600 (BV 0xabc90c), and the 4:3 screens of 2004.
inline constexpr float kFovReferenceAspect = 4.0f / 3.0f;

// The projection's x and y scales for a field of view of `fovRadians`.
// The binary (BV 0xccd254) takes it as the full horizontal angle at any
// aspect: x = 1 / tan(fov / 2), y = x * width / height, so a window wider
// than the game's 4:3 shows less height ("Vert-": at 16:9 the closest zoom
// cuts a Sim's head and feet). Ours: up to 4:3 exactly that; wider, the
// height of the 4:3 view stays and the sides open up ("Hor+", what the
// widescreen fixes for the game do). `vertical` takes the angle as the
// vertical one instead (our CAS framing).
inline simd_float2 fovScales(float fovRadians, float aspect, bool vertical = false) {
    const float t = 1.0f / std::tan(fovRadians * 0.5f);
    const float ys = vertical ? t : t * std::min(aspect, kFovReferenceAspect);
    return simd_make_float2(ys / aspect, ys);
}

// A vector approaching its target along the line (FUN_008c7c64 in the Mac
// binary): a step of remaining * ms / 50 / steps, never less than minChange
// (when farther than that), never past the target.
void smoothVector(simd_float3& value, simd_float3 target, float ms, int steps, float minChange);

// The live-mode lot camera, driven by a CameraDef from the game's camera
// configuration (LiveCamera.txt), as the Mac binary's nTSSG::cCameraLive
// does it (docs/camera-ui-mac.md §1). World space is the game's: Z up, 1 unit
// per tile.
//
// - Position: the zoom table's number is the 3D distance d from the eye to
//   the pivot along the line of sight. With s = tan(pitch), the eye sits at
//   pivot + (cos h, sin h, s) * d / sqrt(1 + s^2) for the heading h, so the
//   camera looks along -(cos h, sin h). The pitch is lerp(pitchMin, pitchMax,
//   t) of the current (fractional) zoom level, t in [0, 1]. The saved lot's
//   camera (0xEC44BDDC, docs/data-findings) agrees: its eye is on the
//   +(cos h, sin h) side of what it looks at.
// - Smoothing: every value approaches its target exponentially in 50 ms
//   frames (step = remaining * dt / 50 ms / N), never slower than a minimum
//   change per update (`defaultminchange`), angles the short way round.
//   `smoothing 5 8` are the N of the zoom and the spin; requests from the
//   keyboard or the mouse use the keyboard*lerpsteps/lerpminchange params.
// - Requests (cTSCameraSmoothSims1Controller::HandleRequest, BV 0x870cb0;
//   docs/camera-ui-mac.md 1.4 and 1.11): rotate (dx, dy pixels: the
//   dominant axis; the heading's target = the current heading - dx * 0.003
//   rad, the pitch fraction's = the current one - dy * 0.005); move
//   (0x9b66110a: the screen offset at the pivot's depth kept as a world
//   velocity, up on the screen = forward; UpdateSubjectMotion aims the pivot
//   at pivot + velocity x the zoom smoothing every update); the steps of the
//   window's edge (0x3b6c780f / 0xbb6c7808: sqrt(fov x 30) x axis x distance
//   x 0.0032 / 0.002 x a repeat count, from the current pivot); the wheel
//   (d += notches * wheel * (d + 10) / -50, held between the first and last
//   levels); zoom a level (z/x).
// - Keys (the accelerators' Start/Stop messages, DoMessage FUN_00871c0c):
//   each sets the target of a smoothed velocity (keyboard*lerpsteps /
//   lerpminchange) that every update turns into a request: the
//   translation in pixels (keyboardtranslationspeed x 1, Shift 2) into a
//   move, the rotation (keyboardrotationspeed) into a rotate of its whole
//   pixels, the zoom (keyboardzoomspeed) into a wheel step of
//   keyboardzoomscale. rotateCameraSims1Mode (off by default) leaves , and .
//   as they are; off, they are swapped and the mouse's dx is negated.
// - Pivot: held inside the lot rectangle shrunk by `constraintwidth`
//   (clamp(x, min + w, max - w): the -10 of LiveCamera.txt lets it 10 tiles
//   out past the edges; a positive width wider than the lot keeps the pivot
//   in the middle), at `pivotheightabovefloor` over the floor sampled by the app
//   (the selected Sim's level), raised if the eye would be under the floor;
//   smoothed in 7 steps (cTSCameraSmoothSims1Controller::Reset) whoever moves it.
//
// The lerp's dt is in milliseconds (binary). The FOV is the full
// horizontal angle (binary: the view volume's matrix, BV 0xccd254, takes
// x scale = 1 / tan(fov / 2) and y scale = x scale x aspect) at the game's
// 4:3 (its default 800x600); wider windows keep the 4:3 height and see more
// at the sides (ours, see fovScales). Inferred: the floor sample (centre and
// four corners mixed by `cornersampleratio`).
class LotCamera {
public:
    explicit LotCamera(CameraDef def);

    // --- Requests ----------------------------------------------------------
    // A zoom level in / out (z / x, 0x9b65fb1f / 0x7b65fb6b), smoothed. Right after
    // the wheel the first one in only goes to the wheel's level (the first not
    // farther than where it stopped) and clears that (HandleRequest; SetZoomLevel
    // 0x86b74c clears it too).
    void zoomBy(int levels);
    void wheel(float notches);       // positive zooms in, continuous
    void rotateBy(int steps);        // to the next/previous orientation of the table, smoothed
    // The rotate request 0xbc17e41c (pixels): with both axes, |dx / dy| < 1 keeps
    // only dy, > 1 only dx, = 1 both; the heading's target = the current heading -
    // clamp(dx * 0.003, -3, 3) rad, the pitch fraction's = clamp(the current one -
    // dy * 0.005, 0, 1).
    void rotateRequest(float dx, float dy);
    // The rotate tool's drag (cTSUserToolCameraMiddleClick::MouseEvent, BV
    // 0x34cf1c): the rotate request with dx negated unless rotateCameraSims1Mode.
    void mouseRotate(float dx, float dy);
    void freeRotate(float degrees);  // heading, immediate (tests, scripted views)
    void tilt(float degrees);        // pitch, kept inside the zoom level's range
    // The move request 0x9b66110a (screen pixels, x right, y down): the offset at
    // the pivot's depth becomes the pivot's velocity until the next request (0
    // stops it). The right-button drag sends it, and the keyboard every update.
    void moveRequest(float dx, float dy);
    // The step requests of the window's edge (DoWindowEdgeScrolling, once a frame):
    // 0xbb6c7808 sideways (+ = left) and 0x3b6c780f forward (+ = forward). Each
    // one counts up the repeat count, and an axis of 0 resets it to 1 and stops
    // the pivot where it is.
    void stepSideways(float axis);
    void stepForward(float axis);
    // The keyboard (the accelerators' Start / Stop messages), as held now:
    // move right / forward (-1..1, x2 with Shift: the message's argument),
    // rotate (+1 "Start Rotate Right" = '.', -1 left = ','), zoom (+1 in = '=',
    // -1 out = '-'); 0 = released.
    void setKeyMove(float right, float forward);
    void setKeyRotate(int direction);
    void setKeyZoom(int direction);
    // The option rotateCameraSims1Mode (cTSMainToolbar +0xad, off by default).
    void setRotateSims1Mode(bool on) { rotateSims1Mode_ = on; }
    bool rotateSims1Mode() const { return rotateSims1Mode_; }
    void reset();                    // default zoom, orientation and pitch
    void setPivot(simd_float3 floorPoint); // immediate (also the target)
    // Centre on a point, smoothed (Return: centre on the active Sim; the HandleRequest
    // 0xbb661105): as every move the player asks for, it first stops the
    // camera following a Sim (SetTracking(false), BV 0x870cb0), see userMoves().
    void moveTo(simd_float3 floorPoint);
    // A new pivot target without that (the controller's SetSubjectTarget, vtable
    // 0xfc): what following a Sim does each time the Sim leaves the dead zone.
    void trackTo(simd_float3 floorPoint);
    // How many moves the player asked for (keys, the window's edge, the
    // right-button drag, moveTo): in the game each of them ends the following of a Sim.
    unsigned userMoves() const { return userMoves_; }

    // Camera positions 0-5 (Control 4-9 saves, 4-9 restores smoothly,
    // Shift 4-9 jumps).
    void savePosition(int slot);
    bool restorePosition(int slot, bool smooth);
    // The whole pose (the targets), to carry a view across a scene reload.
    struct Pose {
        simd_float3 pivot;
        float dist, heading, pitchT;
    };
    Pose pose() const { return {pivotTarget_, dist_.target, heading_.target, pitchT_.target}; }
    void setPose(const Pose& p); // immediate
    // Another camera of the same kind takes over (the top-down view: "Live"
    // <-> "Build", BuildCamera.txt). The new cCameraLive copies the old one's
    // smoothed values and snaps to them (cCameraLive::SetActive, BV 0x6a56e0:
    // FUN_0086bcd4 then the snap at vtable 0x104, FUN_0086b7f8): the pivot, the
    // heading, the 3D distance and the pitch fraction are kept as numbers
    // (the distance is not moved onto the new zoom table), the tables, the
    // pitch range and the params are the new camera's. Each camera keeps its
    // own saved positions (cCameraLive::SaveCameraPosition).
    void setDefinition(CameraDef def);
    // Takes over from the free camera ("CameraMan": Tab again, Escape):
    // MatchThisControllerWith(viewer) (cTSCameraSmoothSims1Controller, BV
    // 0x86c05c): the heading of the view, the pitch held to the first level's
    // range, the distance to the pivot's height along the view ((eye z -
    // pivotheightabovefloor) / sin(pitch), held between the first and last
    // levels), the pivot that far along the view, and the pitch fraction of
    // the level it lands on; snapped.
    void matchView(simd_float3 eye, simd_float3 forward);

    // --- Environment -------------------------------------------------------
    void setViewport(float width, float height) { viewW_ = width, viewH_ = height; }
    // A world point on the screen (viewport pixels, origin top left); empty behind the eye.
    std::optional<simd_float2> project(simd_float3 world) const;
    const CameraDef& definition() const { return def_; }
    // Lot rectangle in tiles; the pivot stays inside it shrunk by constraintwidth.
    void setBounds(simd_float2 lo, simd_float2 hi);
    const std::optional<std::pair<simd_float2, simd_float2>>& bounds() const { return bounds_; }
    // The floor's height under a point as the camera samples it (0 without a sampler).
    float floorHeight(float x, float y) const { return floorAt(x, y, 0); }
    // Floor height under a point (the visible level's); empty = flat at the pivot's z.
    void setFloorSampler(std::function<float(float x, float y)> f) { floor_ = std::move(f); }

    // A first-person pose that overrides the orbit (the CAS's CreateASim
    // camera, render/cas_camera.hpp): eye, view direction and up, the
    // horizontal FOV in degrees and the clip planes. Empty: the orbit.
    struct FirstPerson {
        simd_float3 eye{0, 0, 0}, forward{0, 1, 0}, up{0, 0, 1};
        float fov = 30, nearClip = 0.1f, farClip = 2000;
    };
    void setFirstPerson(std::optional<FirstPerson> f) { firstPerson_ = f; }
    const std::optional<FirstPerson>& firstPerson() const { return firstPerson_; }

    void update(double dt); // advance smoothing towards the targets
    void snap();            // jump to the targets (no smoothing)

    simd_float4x4 view() const;
    simd_float4x4 projection(float aspect) const;
    simd_float3 eye() const;
    simd_float3 forward() const;                       // the view axis, unit (towards what the camera looks at)
    simd_float3 pivot() const { return pivot_; }       // on the floor
    simd_float3 pivotTarget() const { return pivotTarget_; }
    float zoom() const;                                // fractional zoom level of the distance
    float yaw() const;                                 // heading, degrees (unwrapped)
    float pitch() const;                               // degrees above horizontal
    float distance() const { return dist_.value; }     // eye to the raised pivot, 3D
    float fov() const;

    // One smoothed value (FUN_008c77d4 in the Mac binary).
    struct Smooth {
        float value = 0, target = 0;
        int steps = 1;
        float minChange = 0;
        void set(float t, int n, float m) { target = t, steps = n, minChange = m; }
        void update(float ms, bool angle = false);
    };

private:
    float sample(const std::vector<float>& table, float fallback) const;
    float levelOf(float d) const; // fractional zoom level of a distance
    float clampDistance(float d) const;
    simd_float3 constrain(simd_float3 p) const;
    float floorAt(float x, float y, float fallback) const;
    simd_float3 raisedPivot() const;

    CameraDef def_;
    float pivotHeight_, minChange_;
    float viewW_ = 1024, viewH_ = 768;
    std::optional<std::pair<simd_float2, simd_float2>> bounds_;
    std::function<float(float, float)> floor_;

    void wheelBy(float delta, float sensitivity);
    void step(float axis, simd_float2 dir, float k);

    simd_float3 pivot_{0, 0, 0}, pivotTarget_{0, 0, 0};
    unsigned userMoves_ = 0;
    simd_float3 subjectVel_{0, 0, 0}; // +0x1b0: the pivot's velocity (move request)
    float stepCount_ = 1;            // +0x1c0: the edge steps' repeat count
    simd_float3 keyMove_{0, 0, 0}, keyMoveTarget_{0, 0, 0}; // +0x238: pixels a request
    Smooth keyRotate_, keyZoom_;     // +0x200, +0x21c
    int wheelPos_ = 0;               // +0x1fc: the wheel's position, in notches
    int wheelLevel_ = -1;            // +0x70 while this[500] (the wheel's level), else -1
    bool rotateSims1Mode_ = false;
    float lift_ = 0; // extra pivot height so the eye stays over the floor
    Smooth dist_, heading_, pitchT_; // heading in radians, unwrapped

    struct Saved {
        simd_float3 pivot;
        float dist, heading, pitchT;
    };
    std::array<std::optional<Saved>, 6> saved_;
    std::map<std::string, std::array<std::optional<Saved>, 6>> savedByCamera_; // the other cameras' positions
    std::optional<FirstPerson> firstPerson_;
};

} // namespace os2
