#include "cas_camera.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

namespace {
constexpr float kFrameMs = 50;     // the binary's update unit (FLOAT_0274b85c)
constexpr int kZoomSteps = 10;     // +0x364
constexpr float kDegrees = float(M_PI) / 180.0f;

float lerp(float a, float b, float s) { return a + (b - a) * s; }

// Rotation about Z by `h` then about X by `e`, as FUN_000ea0dc builds them
// (Rz(h) Rx(e) applied to a column vector).
simd_float3 turnZX(simd_float3 v, float h, float e) {
    const float ce = std::cos(e), se = std::sin(e);
    v = simd_make_float3(v.x, v.y * ce - v.z * se, v.y * se + v.z * ce);
    const float ch = std::cos(h), sh = std::sin(h);
    return simd_make_float3(v.x * ch - v.y * sh, v.x * sh + v.y * ch, v.z);
}
} // namespace

CasCameraSpec CasCameraSpec::from(const CameraDef& def) {
    CasCameraSpec s;
    if (!def.fov.empty())
        s.fov = def.fov.front();
    if (!def.nearClip.empty())
        s.nearClip = def.nearClip.front();
    if (!def.farClip.empty())
        s.farClip = def.farClip.front();
    s.minZoom = def.param("minzoom", 0);
    s.maxZoom = def.param("maxzoom", 0);
    s.zoomHeading = def.param("zoomheading", 0) * kDegrees;
    s.zoomElevation = -def.param("zoomelevation", 0) * kDegrees;
    s.speedTo = def.param("interpolatespeedto", 0.1f);
    s.speedFrom = def.param("interpolatespeedfrom", 0.1f);
    s.keyboardZoomSpeed = def.param("keyboardzoomspeed", 0.05f);
    s.wheelSensitivity = def.param("mousewheelsensitivity", 1);
    return s;
}

float CasCamera::zoom() const {
    return t_ >= 1 ? zoomValue_ : lerp(zoom0_, zoom1_, ease(t_));
}

simd_float3 CasCamera::eyeAt(float t) const {
    const float s = ease(t);
    const simd_float3 pos = simd_mix(pos0_, pos1_, simd_make_float3(s, s, s));
    const simd_quatf q = simd_slerp(q0_, q1_, s);
    const float z = t >= 1 ? zoomValue_ : lerp(zoom0_, zoom1_, s);
    const simd_float3 dir = simd_normalize(turnZX(simd_act(q, simd_make_float3(0, 1, 0)), lerp(heading0_, heading1_, s),
                                                  lerp(elevation0_, elevation1_, s)));
    return pos + dir * z;
}

void CasCamera::setActive(const CasCameraSpec& spec, simd_float3 position, simd_quatf orientation, bool cut) {
    if (!active_ || cut) {
        // No camera before it: there at once, the zoom at minzoom.
        pos0_ = pos1_ = position;
        q0_ = q1_ = orientation;
        fov0_ = fov1_ = spec.fov;
        heading0_ = heading1_ = spec.zoomHeading;
        elevation0_ = elevation1_ = spec.zoomElevation;
        zoom0_ = zoom1_ = 0;
        zoomTarget_ = zoomValue_ = spec.minZoom;
        t_ = 1;
    } else {
        // From where the camera is now (SetActive reads the old controller's
        // interpolated place, turn, FOV, zoom, heading and elevation).
        const float s = ease(t_);
        pos0_ = simd_mix(pos0_, pos1_, simd_make_float3(s, s, s));
        q0_ = simd_slerp(q0_, q1_, s);
        fov0_ = lerp(fov0_, fov1_, s);
        zoom0_ = zoom();
        heading0_ = lerp(heading0_, heading1_, s);
        elevation0_ = lerp(elevation0_, elevation1_, s);
        speedFrom_ = spec_.speedFrom;
        pos1_ = position;
        q1_ = orientation;
        fov1_ = spec.fov;
        zoom1_ = 0;
        heading1_ = spec.zoomHeading;
        elevation1_ = spec.zoomElevation;
        zoomTarget_ = zoomValue_ = zoom0_;
        t_ = 0;
        travel_ = -1;
    }
    spec_ = spec;
    active_ = true;
    held_ = 0;
}

void CasCamera::zoomStep(int direction) {
    if (t_ < 1)
        return;
    zoomTarget_ = std::clamp(zoomTarget_ + float(direction) * spec_.keyboardZoomSpeed, spec_.minZoom,
                             std::max(spec_.minZoom, spec_.maxZoom));
}

void CasCamera::wheel(float notches) {
    if (t_ < 1)
        return;
    zoomTarget_ = std::clamp(zoomTarget_ + notches * spec_.wheelSensitivity, spec_.minZoom,
                             std::max(spec_.minZoom, spec_.maxZoom));
}

void CasCamera::update(double ms) {
    if (!active_ || ms <= 0)
        return;
    const float dt = float(ms);
    if (t_ >= 1) {
        if (held_)
            zoomTarget_ = std::clamp(zoomTarget_ + float(held_) * spec_.keyboardZoomSpeed * dt / kFrameMs, spec_.minZoom,
                                     std::max(spec_.minZoom, spec_.maxZoom));
        // FUN_008c77d4: a tenth of the way per 50 ms, no minimum change.
        const float r = zoomTarget_ - zoomValue_;
        const float step = r * dt / kFrameMs / float(kZoomSteps);
        zoomValue_ = step * step >= r * r ? zoomTarget_ : zoomValue_ + step;
        return;
    }
    if (speedFrom_ == 0 || spec_.speedTo == 0) {
        t_ = 1;
    } else {
        if (travel_ < 0) {
            travel_ = simd_distance(eyeAt(0.99f), eyeAt(0));
            if (travel_ < 0.001f)
                travel_ = 1;
        }
        const float v = std::max(lerp(speedFrom_, spec_.speedTo, t_) / travel_, 0.001f);
        t_ += v * dt / kFrameMs;
    }
    if (t_ >= 1) {
        t_ = 1;
        zoomTarget_ = zoomValue_ = zoom1_;
    }
}

void CasCamera::snap() {
    if (t_ < 1) {
        t_ = 1;
        zoomTarget_ = zoom1_;
    }
    zoomValue_ = zoomTarget_;
}

CasCamera::Pose CasCamera::pose() const {
    Pose p;
    const float s = ease(t_);
    const simd_quatf q = simd_slerp(q0_, q1_, s);
    p.eye = eyeAt(t_);
    p.forward = simd_normalize(simd_act(q, simd_make_float3(0, 1, 0)));
    p.up = simd_normalize(simd_act(q, simd_make_float3(0, 0, 1)));
    p.fov = lerp(fov0_, fov1_, s);
    p.nearClip = spec_.nearClip;
    p.farClip = spec_.farClip;
    return p;
}

} // namespace os2
