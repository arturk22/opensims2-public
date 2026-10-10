#include "camera.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

namespace {

constexpr float kTickRate = 30.0f; // smoothing durations are counted in these ticks

float radians(float deg) { return deg * float(M_PI) / 180.0f; }

// Fraction of the remaining distance covered after `dt` seconds when every
// tick covers 1/steps of it.
float smoothing(int steps, double dt) {
    if (steps <= 1)
        return 1.0f;
    return 1.0f - std::pow(1.0f - 1.0f / float(steps), float(dt) * kTickRate);
}

float approach(float current, float target, float alpha, float minChange) {
    float next = current + (target - current) * alpha;
    return std::fabs(target - next) < minChange ? target : next;
}

// Angle of `deg` moved to within 180 degrees of `ref`.
float nearAngle(float deg, float ref) {
    return ref + std::remainder(deg - ref, 360.0f);
}

} // namespace

LotCamera::LotCamera(CameraDef def) : def_(std::move(def)) {
    pivotHeight_ = def_.param("pivotheightabovefloor", 1.0f);
    reset();
    zoom_ = zoomTarget_;
    yaw_ = yawTarget_;
}

float LotCamera::sample(const std::vector<float>& table, float fallback) const {
    if (table.empty())
        return fallback;
    float z = std::clamp(zoom_, 0.0f, float(table.size() - 1));
    size_t i = size_t(z);
    if (i + 1 >= table.size())
        return table.back();
    float f = z - float(i);
    return table[i] + (table[i + 1] - table[i]) * f;
}

float LotCamera::distance() const { return sample(def_.distances, 40.0f); }
float LotCamera::fov() const { return sample(def_.fov, 45.0f); }

float LotCamera::pitch() const {
    if (def_.pitchMin.empty() || def_.pitchMax.empty())
        return std::atan(def_.slope > 0 ? def_.slope : 1.0f) * 180.0f / float(M_PI);
    float lo = sample(def_.pitchMin, 0), hi = sample(def_.pitchMax, 0);
    return lo + (hi - lo) * pitchT_;
}

void LotCamera::zoomBy(int levels) {
    float maxLevel = def_.distances.empty() ? 0.0f : float(def_.distances.size() - 1);
    zoomTarget_ = std::clamp(std::round(zoomTarget_) + float(levels), 0.0f, maxLevel);
}

void LotCamera::rotateBy(int steps) {
    const auto& yaws = def_.yaws;
    if (yaws.empty()) {
        yawTarget_ += 90.0f * float(steps);
        return;
    }
    // Orientation closest to where the camera is heading, then step from it.
    int n = int(yaws.size()), best = 0;
    float bestDiff = 1e9f;
    for (int i = 0; i < n; ++i) {
        float d = std::fabs(std::remainder(yaws[i] - yawTarget_, 360.0f));
        if (d < bestDiff)
            bestDiff = d, best = i;
    }
    int next = ((best + steps) % n + n) % n;
    // Turn in the requested direction, the long way round if need be.
    float delta = std::fmod(yaws[next] - yawTarget_, 360.0f);
    if (steps > 0 && delta <= 0)
        delta += 360.0f;
    if (steps < 0 && delta >= 0)
        delta -= 360.0f;
    yawTarget_ += delta;
}

void LotCamera::freeRotate(float degrees) {
    yawTarget_ += degrees;
    yaw_ += degrees;
}

void LotCamera::tilt(float degrees) {
    float lo = sample(def_.pitchMin, 0), hi = sample(def_.pitchMax, 0);
    if (hi - lo < 1e-3f)
        return;
    pitchT_ = std::clamp(pitchT_ + degrees / (hi - lo), 0.0f, 1.0f);
}

void LotCamera::pan(float right, float forward, double dt) {
    if (right == 0 && forward == 0)
        return;
    float speed = def_.param("keyboardtranslationspeed", 50.0f);
    float farthest = def_.distances.empty() ? distance() : def_.distances.front();
    float step = speed * float(dt) * distance() / farthest;
    float y = radians(yaw_);
    simd_float3 f = simd_make_float3(std::cos(y), std::sin(y), 0);
    simd_float3 r = simd_make_float3(std::sin(y), -std::cos(y), 0);
    pivot_ += (r * right + f * forward) * step;
}

void LotCamera::reset() {
    zoomTarget_ = def_.distances.empty() ? 0.0f
                                         : float(std::clamp(def_.defaultZoom, 0, int(def_.distances.size()) - 1));
    float yaw = def_.yaws.empty() ? 45.0f
                                  : def_.yaws[size_t(std::clamp(def_.defaultOrientation, 0, int(def_.yaws.size()) - 1))];
    yawTarget_ = nearAngle(yaw, yaw_);
    pitchT_ = 0.5f;
}

void LotCamera::setPivot(simd_float3 floorPoint) { pivot_ = floorPoint; }

void LotCamera::update(double dt) {
    const float minChange = def_.param("defaultminchange", 0.02f);
    zoom_ = approach(zoom_, zoomTarget_, smoothing(def_.zoomSmoothing, dt), minChange * 0.1f);
    yaw_ = approach(yaw_, yawTarget_, smoothing(def_.spinSmoothing, dt), minChange);
}

simd_float3 LotCamera::eye() const {
    float d = distance(), y = radians(yaw_), p = radians(pitch());
    simd_float3 target = pivot_ + simd_make_float3(0, 0, pivotHeight_);
    return target + simd_make_float3(-std::cos(y) * d, -std::sin(y) * d, std::tan(p) * d);
}

simd_float4x4 LotCamera::view() const {
    simd_float3 e = eye();
    simd_float3 target = pivot_ + simd_make_float3(0, 0, pivotHeight_);
    simd_float3 f = simd_normalize(target - e);
    simd_float3 s = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
    simd_float3 u = simd_cross(s, f);
    return simd_matrix(simd_make_float4(s.x, u.x, -f.x, 0), simd_make_float4(s.y, u.y, -f.y, 0),
                       simd_make_float4(s.z, u.z, -f.z, 0),
                       simd_make_float4(-simd_dot(s, e), -simd_dot(u, e), simd_dot(f, e), 1));
}

simd_float4x4 LotCamera::projection(float aspect) const {
    float n = sample(def_.nearClip, 0.1f), fa = sample(def_.farClip, 2000.0f);
    float ys = 1.0f / std::tan(radians(fov()) * 0.5f);
    float xs = ys / aspect;
    float zs = fa / (n - fa);
    // Right-handed, Metal clip depth in [0, 1].
    return simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                       simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
}

} // namespace os2
