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
constexpr int kSubjectSteps = 7;          // the pivot's smoothing (cTSCameraSmoothSims1Controller::Reset, BV 0x86d688)
constexpr float kSideStep = 0.002f;       // the edge steps' constants (DAT_02c426d8 / DAT_02c426dc,
constexpr float kForwardStep = 0.0032f;   // written by the static initializer at BV 0x2170d64)

float radians(float deg) { return deg * float(M_PI) / 180.0f; }
float degrees(float rad) { return rad * 180.0f / float(M_PI); }

} // namespace

void smoothVector(simd_float3& value, simd_float3 target, float ms, int steps, float minChange) {
    const simd_float3 r = target - value;
    const float r2 = simd_length_squared(r);
    if (r2 == 0)
        return;
    const simd_float3 step = r * (ms / kFrameMs / float(std::max(steps, 1)));
    const float s2 = simd_length_squared(step), m2 = minChange * minChange;
    if (s2 >= m2)
        value = s2 <= r2 ? value + step : target;
    else if (m2 < r2)
        value = value + simd_normalize(r) * minChange;
    else
        value = target;
}

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

// pivotheightabovefloor without the parameter: cCameraLive::Reset (BV 0x6a3c70)
// puts 1.2 before the spec is read (the "Live" and "Build" cameras, BuildCamera.txt
// has none); the base controller's Reset sets a zero subject offset (the
// neighbourhood's).
static float defaultPivotHeight(const CameraDef& d) {
    const bool live = d.controller == "Live" || d.controller == "Build";
    return d.param("pivotheightabovefloor", live ? 1.2f : 0.0f);
}

LotCamera::LotCamera(CameraDef def) : def_(std::move(def)) {
    pivotHeight_ = defaultPivotHeight(def_);
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

float LotCamera::fov() const { return firstPerson_ ? firstPerson_->fov : sample(def_.fov, 60.0f); }
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

void LotCamera::wheel(float notches) { wheelBy(notches, def_.wheel); }

// The wheel request 0x9c17e415: d += delta * sensitivity * (d + 10) / -50, held
// between the first and last levels, smoothed as the zoom.
void LotCamera::wheelBy(float delta, float sensitivity) {
    const float d = dist_.target;
    dist_.set(clampDistance(d + delta * sensitivity * (d + 10) / -50.0f), def_.zoomSmoothing, minChange_);
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
    if (dx != 0 && dy != 0) {
        const float ratio = std::fabs(dx / dy);
        if (ratio > 1)
            dy = 0;
        else if (ratio < 1)
            dx = 0;
    }
    if (dx != 0)
        heading_.set(heading_.value - std::clamp(dx * kRotatePerPixel, -3.0f, 3.0f), heading_.steps, heading_.minChange);
    if (dy != 0)
        pitchT_.set(std::clamp(pitchT_.value - dy * kPitchPerPixel, 0.0f, 1.0f), pitchT_.steps, pitchT_.minChange);
}

void LotCamera::mouseRotate(float dx, float dy) { rotateRequest(rotateSims1Mode_ ? dx : -dx, dy); }

namespace {
// The keyboard velocities' smoothing (SetCameraSpec, BV 0x86f310): the
// <prefix>lerpsteps / lerpminchange pair when both are given, else Reset's 20
// steps and defaultminchange (Reset: 0.05).
void keyLerp(const CameraDef& d, const char* steps, const char* minChange, int& n, float& m) {
    n = 20;
    m = d.param("defaultminchange", 0.05f);
    if (d.params.count(steps) && d.params.count(minChange))
        n = int(d.param(steps, 20)), m = d.param(minChange, m);
}
} // namespace

void LotCamera::setKeyMove(float right, float forward) {
    // DoMessage (FUN_00871c0c): left x = -speed, right +speed, forward y = -speed, back +speed (pixels, y down).
    const float speed = def_.param("keyboardtranslationspeed", 0);
    keyMoveTarget_ = simd_make_float3(right * speed, -forward * speed, 0);
}

void LotCamera::setKeyRotate(int direction) {
    // cCameraLive / cCameraNeighborhood::DoMessage swap the two starts unless rotateCameraSims1Mode.
    if (!rotateSims1Mode_)
        direction = -direction;
    int n;
    float m;
    keyLerp(def_, "keyboardrotationlerpsteps", "keyboardrotationlerpminchange", n, m);
    keyRotate_.set(float(direction) * def_.param("keyboardrotationspeed", 5), n, m);
}

void LotCamera::setKeyZoom(int direction) {
    int n;
    float m;
    keyLerp(def_, "keyboardzoomlerpsteps", "keyboardzoomlerpminchange", n, m);
    keyZoom_.set(float(direction) * def_.param("keyboardzoomspeed", 5), n, m);
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

void LotCamera::moveRequest(float dx, float dy) {
    ++userMoves_; // SetTracking(false)
    subjectVel_ = simd_make_float3(0, 0, 0);
    if ((dx == 0 && dy == 0) || viewW_ <= 0 || viewH_ <= 0)
        return;
    // The pivot's point on the screen and the point (dx, dy) pixels away at the
    // same depth, back in the world (the viewer's 0x238 / 0x240).
    const simd_float4x4 vp = simd_mul(projection(viewW_ / viewH_), view());
    const simd_float4 c = simd_mul(vp, simd_make_float4(pivot_.x, pivot_.y, pivot_.z, 1));
    if (c.w <= 0)
        return;
    const simd_float4 q = simd_mul(simd_inverse(vp), simd_make_float4(c.x / c.w + 2 * dx / viewW_,
                                                                      c.y / c.w - 2 * dy / viewH_, c.z / c.w, 1));
    const simd_float3 delta = q.xyz / q.w - pivot_;
    // In the viewer's frame (x right, z up), onto the floor by the heading.
    const simd_float3 f = forward();
    const simd_float3 side = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
    const simd_float3 up = simd_cross(side, f);
    const float lx = simd_dot(delta, side), lz = simd_dot(delta, up), h = heading_.value;
    subjectVel_ = simd_make_float3(-lz * std::cos(h) - lx * std::sin(h), -lz * std::sin(h) + lx * std::cos(h), 0);
}

void LotCamera::step(float axis, simd_float2 dir, float k) {
    ++userMoves_; // SetTracking(false)
    stepCount_ = axis == 0 ? 1.0f : stepCount_ + 1;
    const float s = std::sqrt(fov() * float(M_PI) / 180.0f * 30.0f) * (-axis * dist_.value * k) * stepCount_;
    trackTo(simd_make_float3(pivot_.x + dir.x * s, pivot_.y + dir.y * s, pivot_.z));
}

void LotCamera::stepSideways(float axis) {
    const float h = heading_.value;
    step(axis, simd_make_float2(-std::sin(h), std::cos(h)), kSideStep);
}

void LotCamera::stepForward(float axis) {
    const float h = heading_.value;
    step(axis, simd_make_float2(std::cos(h), std::sin(h)), kForwardStep);
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
    pitchT_.set(std::clamp(def_.param("defaultcamerapitch", 0.15f), 0.0f, 1.0f), kSubjectSteps, minChange_); // the pitch: 7 steps (Reset)
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
    ++userMoves_;
    trackTo(p);
}

void LotCamera::trackTo(simd_float3 p) { pivotTarget_ = constrain(p); }

void LotCamera::savePosition(int slot) {
    if (slot >= 0 && slot < int(saved_.size()))
        saved_[size_t(slot)] = Saved{pivotTarget_, dist_.target, heading_.target, pitchT_.target};
}

bool LotCamera::restorePosition(int slot, bool smooth) {
    if (slot < 0 || slot >= int(saved_.size()) || !saved_[size_t(slot)])
        return false;
    const Saved& s = *saved_[size_t(slot)];
    pivotTarget_ = constrain(s.pivot);
    dist_.set(s.dist, def_.zoomSmoothing, minChange_);
    heading_.set(heading_.value + std::remainder(s.heading - heading_.value, 2 * float(M_PI)), def_.spinSmoothing,
                 minChange_);
    pitchT_.set(s.pitchT, kSubjectSteps, minChange_);
    if (!smooth)
        snap();
    return true;
}

void LotCamera::setPose(const Pose& p) {
    pivotTarget_ = constrain(p.pivot);
    dist_.set(p.dist, def_.zoomSmoothing, minChange_);
    heading_.set(p.heading, def_.spinSmoothing, minChange_);
    pitchT_.set(p.pitchT, kSubjectSteps, minChange_);
    snap();
}

void LotCamera::setDefinition(CameraDef def) {
    const Pose p = pose();
    savedByCamera_[def_.name] = saved_;
    def_ = std::move(def);
    pivotHeight_ = defaultPivotHeight(def_);
    minChange_ = def_.param("defaultminchange", 0.02f);
    if (auto it = savedByCamera_.find(def_.name); it != savedByCamera_.end())
        saved_ = it->second;
    else
        saved_ = {};
    lift_ = 0;
    setPose(p); // the targets (with the pivot held by the new camera's constraint) and the snap
}

void LotCamera::matchView(simd_float3 eye, simd_float3 forward) {
    const simd_float3 f = simd_normalize(forward);
    float down = -std::asin(std::clamp(f.z, -1.0f, 1.0f)); // radians, positive looking down
    if (!def_.pitchMin.empty() && !def_.pitchMax.empty())
        down = std::clamp(down, radians(def_.pitchMin.front()), radians(def_.pitchMax.front()));
    float h = heading_.target;
    if (f.x * f.x + f.y * f.y > 1e-8f)
        h = std::atan2(-f.y, -f.x);
    float d = dist_.target;
    if (std::sin(down) > 1e-3f)
        d = (eye.z - pivotHeight_) / std::sin(down);
    d = clampDistance(d);
    simd_float2 along = simd_make_float2(f.x, f.y);
    if (simd_length(along) > 1e-6f)
        along = simd_normalize(along);
    const simd_float2 at = eye.xy + along * (d * std::cos(down));
    dist_.value = dist_.target = d;
    heading_.value = heading_.target = h;
    pivot_ = pivotTarget_ = constrain(simd_make_float3(at.x, at.y, 0));
    const float lo = sample(def_.pitchMin, 0), hi = sample(def_.pitchMax, 0);
    if (hi - lo > 1e-3f)
        pitchT_.value = pitchT_.target = std::clamp((degrees(down) - lo) / (hi - lo), 0.0f, 1.0f);
    snap();
}

simd_float3 LotCamera::raisedPivot() const { return pivot_ + simd_make_float3(0, 0, pivotHeight_ + lift_); }

void LotCamera::update(double dt) {
    const float ms = float(dt) * 1000.0f;
    // The keyboard (cTSCameraSmoothSims1Controller::Update, BV 0x86ffb8): the
    // rotation's velocity, then a rotate request of its whole pixels.
    keyRotate_.update(ms);
    rotateRequest(float(int(keyRotate_.value)), 0);
    // The zoom's velocity: while it was moving, a wheel step of keyboardzoomscale.
    const float zoomBefore = keyZoom_.value;
    keyZoom_.update(ms);
    if (zoomBefore != 0) {
        const int pos = int(float(wheelPos_) + keyZoom_.value);
        wheelBy(float(pos - wheelPos_), def_.param("keyboardzoomscale", 1));
        wheelPos_ = pos;
    }
    // The translation's velocity (pixels): while it was moving, a move request.
    const simd_float3 moveBefore = keyMove_;
    int n;
    float m;
    keyLerp(def_, "keyboardtranslationlerpsteps", "keyboardtranslationlerpminchange", n, m);
    smoothVector(keyMove_, keyMoveTarget_, ms, n, m);
    if (simd_length_squared(moveBefore) != 0)
        moveRequest(keyMove_.x, keyMove_.y);
    // UpdateSubjectMotion (BV 0x86b9d4): the pivot's target, the velocity x the zoom's smoothing ahead.
    if (simd_length(subjectVel_) > 0.001f)
        trackTo(pivot_ + subjectVel_ * float(def_.zoomSmoothing));

    dist_.update(ms);
    heading_.update(ms, true);
    pitchT_.update(ms);

    pivotTarget_ = constrain(pivotTarget_);
    pivotTarget_.z = floorAt(pivotTarget_.x, pivotTarget_.y, pivotTarget_.z);
    // The pivot as a vector (FUN_008c7c64): along the line to the target.
    simd_float2 r = pivotTarget_.xy - pivot_.xy;
    const float len = simd_length(r);
    if (len > 0) {
        float step = len * ms / kFrameMs / float(kSubjectSteps);
        step = std::max(step, std::min(minChange_, len));
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
    if (firstPerson_)
        return firstPerson_->eye;
    const float d = distance(), h = heading_.value, s = std::tan(radians(pitch()));
    const float horiz = d / std::sqrt(1 + s * s);
    return raisedPivot() + simd_make_float3(std::cos(h) * horiz, std::sin(h) * horiz, s * horiz);
}

simd_float3 LotCamera::forward() const {
    if (firstPerson_)
        return simd_normalize(firstPerson_->forward);
    return simd_normalize(raisedPivot() - eye());
}

simd_float4x4 LotCamera::view() const {
    simd_float3 e = eye();
    simd_float3 f, s, u;
    if (firstPerson_) {
        f = simd_normalize(firstPerson_->forward);
        s = simd_normalize(simd_cross(f, firstPerson_->up));
        u = simd_cross(s, f);
    } else {
        simd_float3 target = raisedPivot();
        f = simd_normalize(target - e);
        s = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
        u = simd_cross(s, f);
    }
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
    if (firstPerson_)
        n = firstPerson_->nearClip, fa = firstPerson_->farClip;
    // The viewer's perspective (BV 0xccd254): the FOV is the horizontal
    // angle, at 4:3 and narrower; wider windows keep the 4:3 height (ours).
    const simd_float2 sc = fovScales(radians(fov()), aspect, def_.verticalFov);
    const float xs = sc.x, ys = sc.y;
    float zs = fa / (n - fa);
    // Right-handed, Metal clip depth in [0, 1].
    return simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                       simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
}

} // namespace os2
