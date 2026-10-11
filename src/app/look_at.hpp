#pragma once

// A Sim's head turning towards what the VM's Look At (0x6E) points it at
// (vm::Object::lookAt), on top of whatever animation plays: a procedural
// turn of the neck and head bones (SimActor::adjust), limited and eased.
//
// Inferred (the game's look-at code is not read): the turn is split between
// the neck (40%) and the head (60%), at most 75 degrees to either side and
// 30 degrees up or down from where the animation has the head face, turning
// at 120 degrees a second (the trees ask for 45-90 degrees a second turning
// towards and away; the speeds are not passed on yet) and easing back to
// the animation when the look ends.

#include "formats/xform.hpp"

#include <optional>

namespace os2 {

struct SimActor;

class HeadLook {
public:
    explicit HeadLook(const SimActor& actor);

    // Where to look, in world space (nothing: back to the animation).
    void setTarget(std::optional<Vec3> target) { target_ = target; }
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
    double last_ = -1;
};

} // namespace os2
