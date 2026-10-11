#include "look_at.hpp"

#include "formats/sim_actor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {

constexpr float kDeg = float(M_PI / 180.0);
constexpr float kMaxYaw = 75 * kDeg, kMaxPitch = 30 * kDeg;
constexpr float kSpeed = 120 * kDeg; // per second
constexpr float kNeckShare = 0.4f;

float wrap(float a) { return std::remainder(a, float(2 * M_PI)); }

float approach(float from, float to, float step) {
    if (std::fabs(to - from) <= step)
        return to;
    return from + (to > from ? step : -step);
}

Quat axisAngle(const Vec3& axis, float angle) {
    const float s = std::sin(angle / 2);
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(angle / 2)};
}

} // namespace

HeadLook::HeadLook(const SimActor& actor) : actor_(actor) {
    const Skeleton& sk = actor.skin.skeleton;
    neck_ = sk.find("neck");
    head_ = sk.find("head");
    if (head_ >= 0) {
        const auto rest = sk.world(sk.restPose());
        // The rest pose faces +Y: the face's direction in the head's frame.
        const Xform inv{conjugate(rest[size_t(head_)].q), {0, 0, 0}};
        forward_ = inv.rotate({0, 1, 0});
    }
}

void HeadLook::apply(std::vector<Xform>& local, double now) {
    const float dt = last_ < 0 ? 0.0f : float(std::clamp(now - last_, 0.0, 1.0));
    last_ = now;
    const Skeleton& sk = actor_.skin.skeleton;
    if (head_ < 0 || size_t(head_) >= local.size())
        return;
    std::vector<Xform> world = sk.world(local);
    const Xform& head = world[size_t(head_)];
    const Vec3 face = head.rotate(forward_);
    const float faceYaw = std::atan2(-face[0], face[1]);
    const float facePitch = std::asin(std::clamp(face[2], -1.0f, 1.0f));
    float wantYaw = 0, wantPitch = 0;
    if (target_) {
        const Vec3 p = actor_.placement().inverse().apply(*target_); // in model space
        const Vec3 d{p[0] - head.t[0], p[1] - head.t[1], p[2] - head.t[2]};
        if (std::hypot(d[0], d[1]) > 0.05f) {
            wantYaw = std::clamp(wrap(std::atan2(-d[0], d[1]) - faceYaw), -kMaxYaw, kMaxYaw);
            wantPitch = std::clamp(std::atan2(d[2], std::hypot(d[0], d[1])) - facePitch, -kMaxPitch, kMaxPitch);
        }
    }
    if (std::getenv("OS2_DEBUG_LOOK") && target_)
        std::fprintf(stderr, "look: face yaw %.0f pitch %.0f, want %.0f %.0f, now %.0f %.0f\n", faceYaw / kDeg,
                     facePitch / kDeg, wantYaw / kDeg, wantPitch / kDeg, yaw_ / kDeg, pitch_ / kDeg);
    yaw_ = approach(yaw_, wantYaw, kSpeed * dt);
    pitch_ = approach(pitch_, wantPitch, kSpeed * dt);
    if (std::fabs(yaw_) < 1e-4f && std::fabs(pitch_) < 1e-4f)
        return;
    // A model-space turn of a bone about its own origin: its local rotation
    // becomes parent^-1 * turn * parent * local.
    auto turn = [&](int bone, float share, float yawBefore) {
        if (bone < 0)
            return;
        const float yaw = yaw_ * share, pitch = pitch_ * share;
        const float heading = faceYaw + yawBefore + yaw;
        const Vec3 right{std::cos(heading), std::sin(heading), 0};
        const Quat r = axisAngle({0, 0, 1}, yaw) * axisAngle(right, pitch);
        const int parent = sk.bones[size_t(bone)].parent;
        const Quat pq = parent >= 0 ? world[size_t(parent)].q : Quat{};
        local[size_t(bone)].q = normalized(conjugate(pq) * r * pq * local[size_t(bone)].q);
        world = sk.world(local);
    };
    if (neck_ >= 0 && neck_ != head_) {
        turn(neck_, kNeckShare, 0);
        turn(head_, 1 - kNeckShare, yaw_ * kNeckShare);
    } else {
        turn(head_, 1, 0);
    }
}

} // namespace os2
