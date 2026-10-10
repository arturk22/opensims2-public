#pragma once

#include "formats/camera_config.hpp"

#include <simd/simd.h>

namespace os2 {

// The live-mode lot camera, driven by a CameraDef from the game's camera
// configuration (LiveCamera.txt). World space is the game's: Z up, 1 unit per
// tile.
//
// The camera orbits a pivot on the floor at a horizontal distance taken from
// the zoom table, looking down at a pitch inside that zoom level's
// [min, max] range. Zoom is a fractional level index, so every table is
// interpolated while a zoom transition is under way. Keyboard rotation steps
// through the orientation (yaw) table; mouse rotation is free.
//
// Inferred, not confirmed against the game: yaw 0 looks along +X and grows
// counter-clockwise seen from above, FOVs are vertical, smoothing durations
// are steps of a 30 Hz tick, and pan speed scales with the zoom distance.
class LotCamera {
public:
    explicit LotCamera(CameraDef def);

    void zoomBy(int levels);         // positive zooms in (towards the closer levels)
    void rotateBy(int steps);        // positive goes to the next orientation in the table
    void freeRotate(float degrees);  // continuous yaw (mouse drag)
    void tilt(float degrees);        // pitch, kept inside the zoom level's range
    void pan(float right, float forward, double dt); // axes in [-1, 1], relative to the view
    void reset();                    // default zoom, orientation and pitch
    void setPivot(simd_float3 floorPoint);

    void update(double dt); // advance smoothing towards the targets

    simd_float4x4 view() const;
    simd_float4x4 projection(float aspect) const;
    simd_float3 eye() const;
    simd_float3 pivot() const { return pivot_; }
    float zoom() const { return zoom_; }
    float yaw() const { return yaw_; }
    float pitch() const; // degrees
    float distance() const;
    float fov() const;

private:
    float sample(const std::vector<float>& table, float fallback) const;

    CameraDef def_;
    float pivotHeight_;

    simd_float3 pivot_{0, 0, 0};
    float zoom_ = 0, zoomTarget_ = 0;
    float yaw_ = 0, yawTarget_ = 0; // degrees, unwrapped
    float pitchT_ = 0.5f;           // position inside [pitchMin, pitchMax] of the current zoom
};

} // namespace os2
