#include "sim_motion.hpp"

#include "animation.hpp"
#include "vm.hpp"

#include <algorithm>
#include <cmath>

namespace os2::vm {

float headingOfDirection(int direction) {
    return float((90.0 - 45.0 * double(((direction % 8) + 8) % 8)) * M_PI / 180.0);
}

float headingOfMotion(float dx, float dy) { return std::atan2(-dx, dy); }

float turnTowards(float current, float target, float maxStep) {
    float d = std::remainder(target - current, float(2 * M_PI));
    if (std::fabs(d) <= maxStep)
        return target;
    return std::remainder(current + (d > 0 ? maxStep : -maxStep), float(2 * M_PI));
}

void SimMotion::sample(VM& vm, const Object& o) {
    State s;
    s.x = o.position.x, s.y = o.position.y, s.level = o.position.level;
    s.direction = o.data[objdata::Direction];
    s.walking = o.route.has_value() && !o.route->waiting; // waiting (a door, a Sim in the way): standing
    if (o.animation.playing()) {
        s.animation = o.animation.name;
        s.flipped = o.animation.flipped;
        s.animLevel = o.animation.level;
        s.animWeight = o.animation.weight;
    }
    s.serial = o.animation.serial;
    if (s.walking)
        s.walk = locomotionAnimation(vm, o, "walk");
    if (o.route && !o.route->climb.animation.empty()) {
        s.climb = o.route->climb.animation;
        s.climbSerial = o.route->climb.serial;
        s.z = o.route->climb.z;
        s.onStep = o.route->climb.turnSteps == 0; // a turn stands on the floor
        s.turnSteps = o.route->climb.turnSteps;
        s.turnMirror = o.route->climb.mirror;
    } else if (const Object* step = vm.object(o.data[objdata::ContainerId]); step && vm.world) {
        // In a stair's step (the stair's portal tree snaps the Sim into each
        // in turn): at the step's height, playing the tree's animation as a
        // climb phase. The step k quarter tiles up from the bottom edge
        // (its centre at 0.125 + 0.25 k) stands at the bottom floor plus
        // k + 1 rises, as the engine's own climb has it (inferred: the game
        // puts the Sim at the step's containment slot, a bone of its model).
        static const uint32_t stairGlobals = semiGlobalGroup("StairModularGlobals");
        if (step->semiGroup == stairGlobals)
            for (const Stair& st : vm.world->stairs()) {
                const float u = (step->position.x - st.bottomEdge[0]) * st.up[0] +
                                (step->position.y - st.bottomEdge[1]) * st.up[1];
                const auto at = st.at(u);
                if (u < -0.01f || u > st.length() + 0.01f ||
                    std::hypot(at[0] - step->position.x, at[1] - step->position.y) > 0.3f)
                    continue;
                const float rise = (st.topZ - st.bottomZ) / float(std::max(st.steps, 1));
                const int k = std::clamp(int(std::lround((u - 0.125f) / 0.25f)), 0, std::max(st.steps - 1, 0));
                if (o.animation.playing())
                    s.climb = o.animation.name;
                s.onStep = true;
                s.climbSerial = o.animation.serial;
                s.z = st.bottomZ + rise * float(k + 1);
                s.walking = false;
                break;
            }
    }
    previous_ = count_ ? current_ : s;
    current_ = std::move(s);
    if (!count_)
        heading_ = headingOfDirection(current_.direction);
    // A turn in place ended: its animation turned the body by its steps.
    if (count_ && previous_.turnSteps != 0 &&
        (current_.turnSteps == 0 || current_.climbSerial != previous_.climbSerial))
        heading_ = std::remainder(heading_ + float(previous_.turnSteps) * float(M_PI / 4), float(2 * M_PI));
    ++count_;
}

float SimMotion::x(float t) const { return previous_.x + (current_.x - previous_.x) * t; }
float SimMotion::y(float t) const { return previous_.y + (current_.y - previous_.y) * t; }

float SimMotion::turn(float dt, float turnSpeed) {
    const float dx = current_.x - previous_.x, dy = current_.y - previous_.y;
    if (current_.turnSteps != 0) // the turn's animation turns the body
        return heading_;
    float target = headingOfDirection(current_.direction);
    if (current_.walking && std::hypot(dx, dy) > 1e-4f)
        target = headingOfMotion(dx, dy);
    heading_ = turnTowards(heading_, target, turnSpeed * dt);
    return heading_;
}

} // namespace os2::vm
