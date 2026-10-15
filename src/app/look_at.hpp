#pragma once

// A Sim's head turning towards what the VM's Look At (0x6E) points it at
// (vm::Object::lookAt), on top of whatever animation plays: a procedural
// turn of the neck and head bones (SimActor::adjust), limited and eased.
//
// The turn rates are the primitive's (setRates: degrees a second towards
// the target and back, try_look_at BV 0x6a19f8; 120 when none). Inferred
// (cActiveAnimLookAt::AnimUpdate, BV 0xd41e04, is not ported: it turns an IK
// chain of the look-at strategy with an accelerating rate profile and
// joint limits): the turn is split between the neck (40%) and the head
// (60%), at most 75 degrees to either side and 30 degrees up or down from
// where the animation has the head face, at a constant rate, easing back to
// the animation when the look ends. docs/anim-mac.md §11.

#include "formats/xform.hpp"

#include <optional>

namespace os2 {

struct SimActor;

class HeadLook {
public:
    explicit HeadLook(const SimActor& actor);

    // Where to look, in world space (nothing: back to the animation).
    void setTarget(std::optional<Vec3> target) { target_ = target; }
    // Degrees a second towards the target and back to the animation (0: 120).
    void setRates(float towards, float away) { towards_ = towards, away_ = away; }
    const std::optional<Vec3>& target() const { return target_; }
    // The turn now (radians, for tests and debugging).
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

    // SimActor::adjust: turns the neck and head of the animated pose.
    void apply(std::vector<Xform>& local, double now);

private:
    const SimActor& actor_;
    int neck_ = -1, head_ = -1;
    Vec3 forward_{0, 0, 1}; // the face's direction in the head bone's frame
    std::optional<Vec3> target_;
    float yaw_ = 0, pitch_ = 0;
    float towards_ = 0, away_ = 0;
    double last_ = -1;
};

} // namespace os2
