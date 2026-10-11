#pragma once

// How a Sim (or any object) the VM moves looks between VM ticks: the VM
// moves walkers from tile centre to tile centre at a fixed speed, one step a
// tick (30 a second), and turns them in eighths; the renderer draws many
// frames per tick. SimMotion keeps the last two ticks' states and gives the
// position in between, a facing that turns smoothly towards where the Sim
// goes (or, standing, the direction the VM gives it), and which animation
// to play: the locomotion walk while it walks, the one Animate Sim plays,
// else an idle.

#include <cstdint>
#include <string>

namespace os2::vm {

class VM;
struct Object;

// Heading of a direction (object data 1, 0-7) about +Z, in radians, with 0
// along +Y (the skeleton's forward axis): the model's rotation, 90 - 45 *
// direction degrees (world.hpp).
float headingOfDirection(int direction);
// Heading of a motion (dx, dy) in the same convention.
float headingOfMotion(float dx, float dy);
// `current` turned towards `target` by at most `maxStep` radians, the short way.
float turnTowards(float current, float target, float maxStep);

class SimMotion {
public:
    // What the object was doing at the end of a tick.
    struct State {
        float x = 0, y = 0;
        int level = 0;
        int direction = 0;
        bool walking = false;
        std::string animation;  // Animate Sim's, while it plays
        uint32_t serial = 0;    // of that animation
        std::string walk;       // the locomotion walk animation while walking
    };

    // Records the object's state after a VM tick (call once per tick,
    // starting with the state before the first).
    void sample(VM& vm, const Object& o);
    bool sampled() const { return count_ > 0; }
    const State& previous() const { return previous_; }
    const State& current() const { return current_; }

    // Position at `t` (0 = the previous tick, 1 = the last one).
    float x(float t) const;
    float y(float t) const;

    // Heading after `dt` seconds of turning at `turnSpeed` radians per
    // second towards the motion's direction (walking) or the VM's direction.
    float turn(float dt, float turnSpeed = 7.0f);
    float heading() const { return heading_; }

private:
    State previous_, current_;
    size_t count_ = 0;
    float heading_ = 0;
};

} // namespace os2::vm
