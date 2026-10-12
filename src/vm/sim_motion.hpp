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
#include <vector>

#include "locomotion.hpp"

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
        bool flipped = false;   // played mirrored (Animate Sim's flip flags)
        int animLevel = 0;      // its channel level (vm::animlevel::ofSim)
        float animWeight = 1;   // its request weight
        std::string walk;       // the locomotion walk animation while walking
        // On a stair (Object::Route::climb): the phase's animation, bumped
        // serial per phase, and the height of the walker's anchor.
        std::string climb;
        uint32_t climbSerial = 0;
        float z = 0;
        bool onStep = false; // at height z (a climb phase, or in a stair's step)
        // A turn in place (a phase of the route): its 45-degree steps
        // (counter-clockwise positive) and whether it plays mirrored.
        int turnSteps = 0;
        bool turnMirror = false;
        // The locomotion over the floor (Object::Route::Loco): `walk` is its
        // clip (the walk cycle, a start, a stop), bumped serial per clip,
        // played mirrored or not, from `walkPhase` seconds in (the walk
        // cycle goes on from where the start left it); the frame the clip
        // moves with (drawn there: the clip's root makes the offset) and
        // which way it faces; `held`: a start or a stop, whose root turns
        // the body off the frame; `endTurn`: where such a clip leaves it.
        uint32_t walkSerial = 0;
        bool walkMirror = false, walkLoop = true, held = false, framed = false;
        float walkPhase = 0;
        float frameHeading = 0, endTurn = 0;
        // The clips blended with their weights and rates (the first is
        // `walk`): two start or stop clips 45 degrees apart, or the weighed
        // walk cycles.
        std::vector<LocoLayer> walkLayers;
        float walkPortion = 0; // the walk cycle portion its layers start at
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
    // Height at `t` on a stair; `floor(level, x, y)` gives it off the stair.
    template <class Floor>
    float z(float t, Floor floor) const {
        const float a = !previous_.onStep ? floor(previous_.level, previous_.x, previous_.y) : previous_.z;
        const float b = !current_.onStep ? floor(current_.level, x(t), y(t)) : current_.z;
        return a + (b - a) * t;
    }

    // Heading after `dt` seconds of turning at `turnSpeed` radians per
    // second towards the motion's direction (walking) or the VM's direction.
    float turn(float dt, float turnSpeed = 7.0f);
    float heading() const { return heading_; }
    // Where the Sim faces now when the host placed it some other way (in a
    // chair's slot), so that it turns on from there.
    void setHeading(float h) { heading_ = h; }

private:
    State previous_, current_;
    size_t count_ = 0;
    float heading_ = 0;
};

} // namespace os2::vm
