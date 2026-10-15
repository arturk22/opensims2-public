#include "free_camera.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kFrameMs = 50.0f;
float radians(float deg) { return deg * kPi / 180.0f; }

// FUN_008c7c64: a vector approaches its target along the line, a step of
// remaining * dt / 50 ms / steps, never less than minChange, never past the target.
void smoothVector(simd_float3& value, simd_float3 target, float ms, int steps, float minChange) {
    const simd_float3 r = target - value;
    const float r2 = simd_length_squared(r);
    if (r2 == 0)
        return;
    const simd_float3 step = r * (ms / kFrameMs / float(std::max(steps, 1)));
    const float s2 = simd_length_squared(step), m2 = minChange * minChange;
    if (s2 >= m2) {
        value = s2 <= r2 ? value + step : target;
    } else if (m2 < r2) {
        value = value + simd_normalize(r) * minChange;
    } else {
        value = target;
    }
}

} // namespace

FreeCamera::FreeCamera(const CameraDef& def) {
    Params& p = params_;
    p.fov = def.fov.empty() ? std::vector<float>{45.0f} : def.fov;
    p.nearClip = def.nearClip.empty() ? 0.15f : def.nearClip.front();
    p.farClip = def.farClip.empty() ? 2000.0f : def.farClip.front();
    p.mouseHeading = def.param("mouseheadingsensitivity", p.mouseHeading);
    p.mousePitch = def.param("mousepitchsensitivity", p.mouseHeading); // inferred: the heading's when absent
    p.upperPitch = def.param("upperpitchlimit", p.upperPitch);
    p.lowerPitch = def.param("lowerpitchlimit", p.lowerPitch);
    p.maxVelocity = def.param("maxvelocity", p.maxVelocity);
    p.velocitySensitivityMin = def.param("velocitysensitivitymin", p.velocitySensitivityMin);
    p.translationSteps = int(def.param("translationsteps", float(p.translationSteps)));
    p.translationMin = def.param("translationminchange", p.translationMin);
    p.velocitySteps = int(def.param("velocitysteps", float(p.velocitySteps)));
    p.velocityMin = def.param("velocityminchange", p.velocityMin);
    p.fovSteps = int(def.param("fovsteps", float(p.fovSteps)));
    p.fovMin = def.param("fovminchange", p.fovMin);
    p.minHeight = def.param("mincameraheight", p.minHeight);
    p.maxHeight = def.param("maxcameraheight", p.maxHeight);
    p.tickLimit = def.param("ticklimit", p.tickLimit);
    p.constraintWidth = def.param("constraintwidth", p.constraintWidth);
    fovSmooth_.set(radians(p.fov.front()), p.fovSteps, p.fovMin);
    fovSmooth_.value = fovSmooth_.target;
}

float FreeCamera::fov() const { return fovSmooth_.value * 180.0f / kPi; }

simd_float3 FreeCamera::constrain(simd_float3 p) const {
    if (!bounds_)
        return p;
    // ConstrainTranslation (BV 0x7a6330): clamp(x, minX + w, maxX - w), the lot's rectangle.
    const float w = params_.constraintWidth;
    simd_float2 lo = bounds_->first + w, hi = bounds_->second - w;
    for (int k = 0; k < 2; ++k)
        if (lo[k] > hi[k])
            lo[k] = hi[k] = (bounds_->first[k] + bounds_->second[k]) / 2;
    p.x = std::clamp(p.x, lo.x, hi.x);
    p.y = std::clamp(p.y, lo.y, hi.y);
    return p;
}

void FreeCamera::start(simd_float3 eye, simd_float3 forward, float fovDegrees) {
    const simd_float3 f = simd_normalize(forward);
    heading_ = std::atan2(f.y, f.x);
    pitch_ = std::asin(std::clamp(f.z, -1.0f, 1.0f));
    pitch_ = std::clamp(pitch_, radians(params_.lowerPitch), radians(params_.upperPitch));
    pos_ = posTarget_ = eye;
    vel_ = {0, 0, 0};
    dir_ = {0, 0, 0};
    fovIndex_ = 0;
    for (size_t i = 1; i < params_.fov.size(); ++i)
        if (std::fabs(params_.fov[i] - fovDegrees) < std::fabs(params_.fov[fovIndex_] - fovDegrees))
            fovIndex_ = i;
    fovSmooth_.set(radians(params_.fov[fovIndex_]), params_.fovSteps, params_.fovMin);
    fovSmooth_.value = fovSmooth_.target;
    const float floor = floorAt(pos_.x, pos_.y);
    const float lo = floor + params_.minHeight, hi = floor + params_.maxHeight;
    heightFraction_ = hi > lo ? std::clamp((pos_.z - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
}

void FreeCamera::look(float dx, float dy) {
    heading_ -= dx * params_.mouseHeading * kMouseUnit;
    pitch_ -= dy * params_.mousePitch * kMouseUnit;
    pitch_ = std::clamp(pitch_, radians(params_.lowerPitch), radians(params_.upperPitch));
}

void FreeCamera::zoomStep(int direction) {
    if (params_.fov.empty() || direction == 0)
        return;
    if (direction > 0 && fovIndex_ > 0)
        --fovIndex_;
    else if (direction < 0 && fovIndex_ + 1 < params_.fov.size())
        ++fovIndex_;
    else
        return;
    fovSmooth_.set(radians(params_.fov[fovIndex_]), params_.fovSteps, params_.fovMin);
}

void FreeCamera::savePosition(int slot) {
    if (slot >= 0 && slot < int(saved_.size()))
        saved_[size_t(slot)] = Saved{pos_, heading_, pitch_, fovIndex_};
}

bool FreeCamera::restorePosition(int slot) {
    if (slot < 0 || slot >= int(saved_.size()) || !saved_[size_t(slot)])
        return false;
    const Saved& s = *saved_[size_t(slot)];
    pos_ = posTarget_ = s.pos;
    vel_ = {0, 0, 0};
    heading_ = s.heading, pitch_ = s.pitch;
    fovIndex_ = std::min(s.fovIndex, params_.fov.size() - 1);
    fovSmooth_.set(radians(params_.fov[fovIndex_]), params_.fovSteps, params_.fovMin);
    return true;
}

void FreeCamera::update(double dt) {
    const Params& p = params_;
    const float ms = std::min(float(dt) * 1000.0f, p.tickLimit); // Update: min(dt, ticklimit)
    // The velocity wanted, scaled by how high the camera is (0xd90a4c).
    const float s = p.velocitySensitivityMin;
    const float scale = s + (1 - s) * heightFraction_;
    const simd_float3 velTarget = dir_ * (p.maxVelocity * scale);
    // The position wanted: where the current velocity, turned by the heading, takes it.
    const float c = std::cos(heading_), sn = std::sin(heading_);
    const simd_float3 right{sn, -c, 0}, forward{c, sn, 0};
    if (simd_length_squared(vel_) > 0)
        posTarget_ = constrain(pos_ + right * vel_.x + forward * vel_.y + simd_make_float3(0, 0, vel_.z));
    smoothVector(pos_, posTarget_, ms, p.translationSteps, p.translationMin);
    smoothVector(vel_, velTarget, ms, p.velocitySteps, p.velocityMin);
    fovSmooth_.update(ms);
    // The height held between floor + mincameraheight and floor + maxcameraheight, then its fraction.
    const float floor = floorAt(pos_.x, pos_.y);
    const float lo = floor + p.minHeight, hi = floor + p.maxHeight;
    pos_.z = std::clamp(pos_.z, lo, hi);
    posTarget_.z = std::clamp(posTarget_.z, floorAt(posTarget_.x, posTarget_.y) + p.minHeight,
                              floorAt(posTarget_.x, posTarget_.y) + p.maxHeight);
    heightFraction_ = hi > lo ? (pos_.z - lo) / (hi - lo) : 0.0f;
}

LotCamera::FirstPerson FreeCamera::pose() const {
    LotCamera::FirstPerson f;
    f.eye = pos_;
    f.forward = simd_make_float3(std::cos(pitch_) * std::cos(heading_), std::cos(pitch_) * std::sin(heading_), std::sin(pitch_));
    f.up = simd_make_float3(0, 0, 1);
    f.fov = fov();
    f.nearClip = params_.nearClip;
    f.farClip = params_.farClip;
    return f;
}

} // namespace os2
