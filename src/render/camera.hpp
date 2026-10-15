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
// - Requests: rotate (dx, dy pixels: only the dominant axis counts; heading
//   -= dx * 0.003 rad, t -= dy * 0.005), move (pixels on the screen:
//   the pivot's screen point moved and unprojected back onto its floor, so
//   panning scales with the zoom), wheel (d += notches * wheel * (d + 10) /
//   -50, held between the first and last levels), zoom a level (z/x).
// - Pivot: held inside the lot rectangle shrunk by `constraintwidth`
//   (clamp(x, min + w, max - w): the -10 of LiveCamera.txt lets it 10 tiles
//   out past the edges; a positive width wider than the lot keeps the pivot
//   in the middle), at `pivotheightabovefloor` over the floor sampled by the app
//   (the selected Sim's level), raised if the eye would be under the floor.
//
// The lerp's dt is in milliseconds (binary). The FOV is the full
// horizontal angle (binary: the view volume's matrix, BV 0xccd254, takes
// x scale = 1 / tan(fov / 2) and y scale = x scale x aspect) at the game's
// 4:3 (its default 800x600); wider windows keep the 4:3 height and see more
// at the sides (ours, see fovScales). Inferred: keyboard
// translation speed is pixels per 50 ms frame, keyboard rotation pixels per
// second; the floor sample (centre and four corners mixed by
// `cornersampleratio`); the minimum change is per update.
class LotCamera {
public:
    explicit LotCamera(CameraDef def);

    // --- Requests ----------------------------------------------------------
    void zoomBy(int levels);         // positive zooms in (towards the closer levels), smoothed
    void wheel(float notches);       // positive zooms in, continuous
    void zoomHeld(int direction, double dt); // = / - held: continuous zoom (+1 in)
    void rotateBy(int steps);        // to the next/previous orientation of the table, smoothed
    void rotateRequest(float dx, float dy); // pixels, as the mouse sends them
    void rotateHeld(int direction, double dt); // , / . held (+1 = right)
    void freeRotate(float degrees);  // heading, immediate (tests, scripted views)
    void tilt(float degrees);        // pitch, kept inside the zoom level's range
    void movePixels(float dx, float dy); // screen pixels (x right, y down); the pivot follows, smoothed
    void pan(float right, float forward, double dt); // axes in [-1, 1] at the keyboard translation speed
    void reset();                    // default zoom, orientation and pitch
    void setPivot(simd_float3 floorPoint); // immediate (also the target)
    void moveTo(simd_float3 floorPoint);   // smoothed (Return: centre on the active Sim)

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

    // --- Environment -------------------------------------------------------
    void setViewport(float width, float height) { viewW_ = width, viewH_ = height; }
    // A world point on the screen (viewport pixels, origin top left); empty behind the eye.
    std::optional<simd_float2> project(simd_float3 world) const;
    const CameraDef& definition() const { return def_; }
    // Lot rectangle in tiles; the pivot stays inside it shrunk by constraintwidth.
    void setBounds(simd_float2 lo, simd_float2 hi);
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

    simd_float3 pivot_{0, 0, 0}, pivotTarget_{0, 0, 0};
    int pivotSteps_ = 1;
    float pivotMin_ = 0.001f;
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
