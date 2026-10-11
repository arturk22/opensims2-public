#include "camera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace os2 {

namespace {

constexpr float kFrameMs = 50.0f;       // the lerps count steps in 50 ms frames
constexpr float kRotatePerPixel = 0.003f; // radians of heading per pixel (cCameraLive::HandleRequest)
constexpr float kPitchPerPixel = 0.005f;  // of the pitch range per pixel
constexpr float kEyeOverFloor = 0.5f;     // how far over the floor the eye is kept (inferred)

float radians(float deg) { return deg * float(M_PI) / 180.0f; }
float degrees(float rad) { return rad * 180.0f / float(M_PI); }

} // namespace

void LotCamera::Smooth::update(float ms, bool angle) {
    float r = target - value;
    if (angle)
        r = std::remainder(r, 2 * float(M_PI));
    float step = r * ms / kFrameMs / float(std::max(steps, 1));
    if (step * step < minChange * minChange) {
        if (r * r <= minChange * minChange) {
            value = angle ? value + r : target;
            return;
        }
        step = std::copysign(minChange, r);
    }
    if (step * step > r * r)
        value = angle ? value + r : target;
    else
        value += step;
}

LotCamera::LotCamera(CameraDef def) : def_(std::move(def)) {
    pivotHeight_ = def_.param("pivotheightabovefloor", 1.0f);
    minChange_ = def_.param("defaultminchange", 0.02f);
    dist_.value = dist_.target = def_.distances.empty() ? 40.0f : def_.distances.front();
    reset();
    heading_.value = heading_.target;
    snap();
}

float LotCamera::levelOf(float d) const {
    const auto& t = def_.distances;
    if (t.size() < 2)
        return 0;
    if (d >= t.front())
        return 0;
    for (size_t i = 0; i + 1 < t.size(); ++i)
        if (d >= t[i + 1] && t[i] != t[i + 1])
            return float(i) + (t[i] - d) / (t[i] - t[i + 1]);
    return float(t.size() - 1);
}

float LotCamera::zoom() const { return levelOf(dist_.value); }

float LotCamera::clampDistance(float d) const {
    if (def_.distances.empty())
        return d;
    const auto [lo, hi] = std::minmax_element(def_.distances.begin(), def_.distances.end());
    return std::clamp(d, *lo, *hi);
}

float LotCamera::sample(const std::vector<float>& table, float fallback) const {
    if (table.empty())
        return fallback;
    float z = std::clamp(zoom(), 0.0f, float(table.size() - 1));
    size_t i = size_t(z);
    if (i + 1 >= table.size())
        return table.back();
    float f = z - float(i);
    return table[i] + (table[i + 1] - table[i]) * f;
}

float LotCamera::fov() const { return sample(def_.fov, 60.0f); }
float LotCamera::yaw() const { return degrees(heading_.value); }

float LotCamera::pitch() const {
    if (def_.pitchMin.empty() || def_.pitchMax.empty())
        return degrees(std::atan(def_.slope > 0 ? def_.slope : 1.0f));
    float lo = sample(def_.pitchMin, 0), hi = sample(def_.pitchMax, 0);
    return lo + (hi - lo) * pitchT_.value;
}

void LotCamera::zoomBy(int levels) {
    const auto& t = def_.distances;
    if (t.empty() || levels == 0)
        return;
    // The next level closer (or farther) than where the zoom is heading.
    const float d = dist_.target, eps = 1e-3f;
    int at = -1;
    if (levels > 0) {
        for (size_t i = 0; i < t.size() && at < 0; ++i)
            if (t[i] < d - eps)
                at = int(i);
        if (at < 0)
            at = int(t.size()) - 1;
        at += levels - 1;
    } else {
        for (int i = int(t.size()) - 1; i >= 0 && at < 0; --i)
            if (t[size_t(i)] > d + eps)
                at = i;
        if (at < 0)
            at = 0;
        at += levels + 1;
    }
    at = std::clamp(at, 0, int(t.size()) - 1);
    dist_.set(t[size_t(at)], def_.zoomSmoothing, minChange_);
}

void LotCamera::wheel(float notches) {
    const float d = dist_.target;
    dist_.set(clampDistance(d + notches * def_.wheel * (d + 10) / -50.0f), def_.zoomSmoothing, minChange_);
}

void LotCamera::zoomHeld(int direction, double dt) {
    if (direction == 0)
        return;
    const float frames = float(dt) * 1000.0f / kFrameMs;
    const float rate = def_.param("keyboardzoomspeed", 1) * def_.param("keyboardzoomscale", 2);
    const float d = dist_.target;
    dist_.set(clampDistance(d + float(direction) * rate * (d + 10) / -50.0f * frames),
              int(def_.param("keyboardzoomlerpsteps", 1)), def_.param("keyboardzoomlerpminchange", 1));
}

void LotCamera::rotateBy(int steps) {
    const auto& yaws = def_.yaws;
    const float target = degrees(heading_.target);
    float delta = 90.0f * float(steps);
    if (!yaws.empty()) {
        // Orientation closest to where the camera is heading, then step from it.
        int n = int(yaws.size()), best = 0;
        float bestDiff = 1e9f;
        for (int i = 0; i < n; ++i) {
            float d = std::fabs(std::remainder(yaws[size_t(i)] - target, 360.0f));
            if (d < bestDiff)
                bestDiff = d, best = i;
        }
        int next = ((best + steps) % n + n) % n;
        delta = std::remainder(yaws[size_t(next)] - target, 360.0f);
    }
    heading_.set(heading_.target + radians(delta), def_.spinSmoothing, minChange_);
}

void LotCamera::rotateRequest(float dx, float dy) {
    const int steps = int(def_.param("keyboardrotationlerpsteps", 1));
    const float m = def_.param("keyboardrotationlerpminchange", 0.5f);
    if (std::fabs(dx) > std::fabs(dy))
        heading_.set(heading_.target - std::clamp(dx * kRotatePerPixel, -3.0f, 3.0f), steps, m);
    else if (dy != 0)
        pitchT_.set(std::clamp(pitchT_.target - dy * kPitchPerPixel, 0.0f, 1.0f), steps, m);
}

void LotCamera::rotateHeld(int direction, double dt) {
    if (direction != 0)
        rotateRequest(float(direction) * def_.param("keyboardrotationspeed", 200) * float(dt), 0);
}

void LotCamera::freeRotate(float deg) {
    heading_.target += radians(deg);
    heading_.value += radians(deg);
}

void LotCamera::tilt(float deg) {
    float lo = sample(def_.pitchMin, 0), hi = sample(def_.pitchMax, 0);
    if (hi - lo < 1e-3f)
        return;
    pitchT_.value = pitchT_.target = std::clamp(pitchT_.target + deg / (hi - lo), 0.0f, 1.0f);
}

void LotCamera::movePixels(float dx, float dy) {
    if ((dx == 0 && dy == 0) || viewW_ <= 0 || viewH_ <= 0)
        return;
    // The pivot's point on the screen, moved and cast back onto its height.
    const simd_float3 p = raisedPivot();
    const simd_float4x4 vp = simd_mul(projection(viewW_ / viewH_), view());
    const simd_float4 c = simd_mul(vp, simd_make_float4(p.x, p.y, p.z, 1));
    if (c.w <= 0)
        return;
    const float sx = (c.x / c.w + 1) / 2 * viewW_ + dx, sy = (1 - c.y / c.w) / 2 * viewH_ + dy;
    const simd_float4 q =
        simd_mul(simd_inverse(vp), simd_make_float4(2 * sx / viewW_ - 1, 1 - 2 * sy / viewH_, 0.5f, 1));
    const simd_float3 e = eye(), dir = q.xyz / q.w - e;
    if (dir.z > -1e-4f)
        return; // at or above the horizon
    const float t = (p.z - e.z) / dir.z;
    simd_float3 delta = e + dir * t - p;
    delta.z = 0;
    const float len = simd_length(delta), most = distance(); // no runaway near the horizon
    if (len > most)
        delta *= most / len;
    pivotTarget_ = constrain(pivotTarget_ + delta);
    pivotSteps_ = int(def_.param("keyboardtranslationlerpsteps", 1));
    pivotMin_ = def_.param("keyboardtranslationlerpminchange", 0.001f);
}

void LotCamera::pan(float right, float forward, double dt) {
    if (right == 0 && forward == 0)
        return;
    const float px = def_.param("keyboardtranslationspeed", 50.0f) * float(dt) * 1000.0f / kFrameMs;
    movePixels(right * px, -forward * px);
}

void LotCamera::reset() {
    if (!def_.distances.empty())
        dist_.set(def_.distances[size_t(std::clamp(def_.defaultZoom, 0, int(def_.distances.size()) - 1))],
                  def_.zoomSmoothing, minChange_);
    const float yaw = def_.yaws.empty()
                          ? 45.0f
                          : def_.yaws[size_t(std::clamp(def_.defaultOrientation, 0, int(def_.yaws.size()) - 1))];
    heading_.set(heading_.value + std::remainder(radians(yaw) - heading_.value, 2 * float(M_PI)),
                 def_.spinSmoothing, minChange_);
    pitchT_.set(std::clamp(def_.param("defaultcamerapitch", 0.15f), 0.0f, 1.0f), def_.zoomSmoothing, minChange_);
}

void LotCamera::setBounds(simd_float2 lo, simd_float2 hi) {
    bounds_ = {lo, hi};
    pivotTarget_ = constrain(pivotTarget_);
    pivot_ = constrain(pivot_);
}

simd_float3 LotCamera::constrain(simd_float3 p) const {
    if (!bounds_)
        return p;
    // FUN_006a456c (BV): clamp(x, minX + w, maxX - w), so a negative width
    // (LiveCamera.txt: -10) lets the pivot 10 tiles out past the lot's edges.
    const float w = def_.param("constraintwidth", 0);
    simd_float2 lo = bounds_->first + w, hi = bounds_->second - w;
    for (int k = 0; k < 2; ++k)
        if (lo[k] > hi[k])
            lo[k] = hi[k] = (bounds_->first[k] + bounds_->second[k]) / 2;
    p.x = std::clamp(p.x, lo.x, hi.x);
    p.y = std::clamp(p.y, lo.y, hi.y);
    return p;
}

float LotCamera::floorAt(float x, float y, float fallback) const {
    if (!floor_)
        return fallback;
    // HeightSample: the middle and four corners around it (inferred mix).
    const float o = def_.param("cornersampleoffset", 1), ratio = def_.param("cornersampleratio", 0.5f);
    const float corners = (floor_(x - o, y - o) + floor_(x + o, y - o) + floor_(x - o, y + o) + floor_(x + o, y + o)) / 4;
    return floor_(x, y) * (1 - ratio) + corners * ratio;
}

void LotCamera::setPivot(simd_float3 p) {
    p = constrain(p);
    p.z = floorAt(p.x, p.y, p.z);
    pivot_ = pivotTarget_ = p;
}

void LotCamera::moveTo(simd_float3 p) {
    pivotTarget_ = constrain(p);
    pivotSteps_ = def_.zoomSmoothing;
    pivotMin_ = minChange_;
}

void LotCamera::savePosition(int slot) {
    if (slot >= 0 && slot < int(saved_.size()))
        saved_[size_t(slot)] = Saved{pivotTarget_, dist_.target, heading_.target, pitchT_.target};
}

bool LotCamera::restorePosition(int slot, bool smooth) {
    if (slot < 0 || slot >= int(saved_.size()) || !saved_[size_t(slot)])
        return false;
    const Saved& s = *saved_[size_t(slot)];
    pivotTarget_ = constrain(s.pivot);
    pivotSteps_ = def_.zoomSmoothing;
    pivotMin_ = minChange_;
    dist_.set(s.dist, def_.zoomSmoothing, minChange_);
    heading_.set(heading_.value + std::remainder(s.heading - heading_.value, 2 * float(M_PI)), def_.spinSmoothing,
                 minChange_);
    pitchT_.set(s.pitchT, def_.zoomSmoothing, minChange_);
    if (!smooth)
        snap();
    return true;
}

simd_float3 LotCamera::raisedPivot() const { return pivot_ + simd_make_float3(0, 0, pivotHeight_ + lift_); }

void LotCamera::update(double dt) {
    const float ms = float(dt) * 1000.0f;
    dist_.update(ms);
    heading_.update(ms, true);
    pitchT_.update(ms);

    pivotTarget_ = constrain(pivotTarget_);
    pivotTarget_.z = floorAt(pivotTarget_.x, pivotTarget_.y, pivotTarget_.z);
    // The pivot as a vector (FUN_008c7c64): along the line to the target.
    simd_float2 r = pivotTarget_.xy - pivot_.xy;
    const float len = simd_length(r);
    if (len > 0) {
        float step = len * ms / kFrameMs / float(std::max(pivotSteps_, 1));
        step = std::max(step, std::min(pivotMin_, len));
        pivot_.xy = step >= len ? pivotTarget_.xy : pivot_.xy + r * (step / len);
    }
    // Height: the subject height lerp.
    Smooth z{pivot_.z, pivotTarget_.z, int(def_.param("subjectheightlerpsteps", float(def_.zoomSmoothing))),
             def_.param("subjectheightlerpminchange", minChange_)};
    z.update(ms);
    pivot_.z = z.value;

    // ConstrainSubjectHeight: raise the pivot when the eye would be under the floor.
    if (floor_) {
        lift_ = 0;
        const simd_float3 e = eye();
        lift_ = std::max(0.0f, floorAt(e.x, e.y, -std::numeric_limits<float>::infinity()) + kEyeOverFloor - e.z);
    }
}

void LotCamera::snap() {
    dist_.value = dist_.target;
    heading_.value = heading_.target;
    pitchT_.value = pitchT_.target;
    pivotTarget_ = constrain(pivotTarget_);
    pivotTarget_.z = floorAt(pivotTarget_.x, pivotTarget_.y, pivotTarget_.z);
    pivot_ = pivotTarget_;
}

simd_float3 LotCamera::eye() const {
    const float d = distance(), h = heading_.value, s = std::tan(radians(pitch()));
    const float horiz = d / std::sqrt(1 + s * s);
    return raisedPivot() + simd_make_float3(std::cos(h) * horiz, std::sin(h) * horiz, s * horiz);
}

simd_float4x4 LotCamera::view() const {
    simd_float3 e = eye();
    simd_float3 target = raisedPivot();
    simd_float3 f = simd_normalize(target - e);
    simd_float3 s = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
    simd_float3 u = simd_cross(s, f);
    return simd_matrix(simd_make_float4(s.x, u.x, -f.x, 0), simd_make_float4(s.y, u.y, -f.y, 0),
                       simd_make_float4(s.z, u.z, -f.z, 0),
                       simd_make_float4(-simd_dot(s, e), -simd_dot(u, e), simd_dot(f, e), 1));
}

std::optional<simd_float2> LotCamera::project(simd_float3 w) const {
    const simd_float4 c = simd_mul(simd_mul(projection(viewW_ / std::max(viewH_, 1.0f)), view()), simd_make_float4(w, 1));
    if (c.w <= 0)
        return std::nullopt;
    return simd_make_float2((c.x / c.w + 1) / 2 * viewW_, (1 - c.y / c.w) / 2 * viewH_);
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
