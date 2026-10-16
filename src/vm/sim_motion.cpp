#include "sim_motion.hpp"

#include "animation.hpp"
#include "locomotion.hpp"
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
        s.animIkObject = o.animation.eventObject;
        s.animHeading = o.animation.headingOffset;
        s.animPlayed = o.animation.requestId ? o.animation.playedTicks : 0.0f;
        s.animFadeIn = o.animation.fadeInMs / 1000.0f;
        s.animFadeOut = o.animation.fadeOutMs / 1000.0f;
        s.animSpeed = o.animation.speed;
    }
    s.serial = o.animation.serial;
    s.handOff = count_ && o.locoHandOff != handOffs_;
    handOffs_ = o.locoHandOff;
    if (s.walking) {
        using Loco = Object::Route::Loco;
        const Loco& L = o.route->loco;
        if (L.kind != Loco::None && L.moving && !L.animation.empty()) {
            s.walk = L.animation;
            s.walkSerial = L.serial;
            s.walkMirror = L.mirror;
            s.walkLoop = L.kind == Loco::Walk;
            s.held = L.kind != Loco::Walk;
            s.endTurn = L.endTurn;
            s.framed = true;
            s.frameHeading = L.heading;
            s.walkLayers = L.layers;
            s.walkFade = L.fade;
            s.walkEase = L.crossover;
            s.x = L.fx, s.y = L.fy;
            if (L.kind == Loco::Walk)
                if (auto secs = vm.animations.seconds(L.animation))
                    s.walkPhase = L.cycleStart * *secs;
            if (L.kind == Loco::Walk)
                s.walkPortion = L.cycleStart;
            // A phase begun before its first tick (the walk's first, 5 or
            // 100 ms back): its clips start that far in.
            if (L.kind != Loco::Walk && L.lead > 0 && L.ticks > 0) {
                s.walkPortion = std::min(1.0f, L.lead / float(L.ticks));
                s.walkPhase = L.lead / float(vm.clock.ticksPerSecond);
            }
            if (s.walkLayers.empty())
                s.walkLayers.push_back({L.animation, 1, L.mirror, 1});
        } else if (L.moving || (!o.route->points.empty() && o.route->points.front().portal)) {
            const Gait g = gaitOf(vm, o); // on its way to a portal, or between clips
            s.walk = g.animation;
            s.walkLayers = g.layers();
        } else {
            s.walking = false; // stopped, or not off yet: standing
        }
    }
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
    // A stop or a step ended: its root left the body turned off its frame.
    if (count_ && previous_.held && (!current_.held || current_.walkSerial != previous_.walkSerial))
        heading_ = std::remainder(previous_.frameHeading + previous_.endTurn, float(2 * M_PI));
    ++count_;
}

bool SimMotion::jumped() const {
    return previous_.level != current_.level ||
           std::hypot(current_.x - previous_.x, current_.y - previous_.y) > kMaxStep;
}

// A move farther than a step in a tick (a Snap, a stair's end putting the
// climber at the top, a teleport) is not walked between the two places: the
// body is drawn at the new one at once. Drawn halfway, with the stair's last
// frame (its root 4.4 tiles up the steps) over it, the climber flashed a
// tile and a half off the stair's top for a frame or two (OS2_DEBUG_JUMPS,
// N001_Lot68's straight stairs).
float SimMotion::x(float t) const { return jumped() ? current_.x : previous_.x + (current_.x - previous_.x) * t; }
float SimMotion::y(float t) const { return jumped() ? current_.y : previous_.y + (current_.y - previous_.y) * t; }

float SimMotion::turn(float dt, float turnSpeed) {
    const float dx = current_.x - previous_.x, dy = current_.y - previous_.y;
    if (current_.turnSteps != 0) // the turn's animation turns the body
        return heading_;
    if (current_.held) // a start or a stop: drawn along its frame, the clip's root turning the body
        return heading_ = current_.frameHeading;
    float target = headingOfDirection(current_.direction);
    if (current_.framed)
        target = current_.frameHeading;
    else if (current_.walking && std::hypot(dx, dy) > 1e-4f && !jumped())
        target = headingOfMotion(dx, dy);
    // An Animate Sim animation plays in the frame of the Sim's direction:
    // turning the body there over a few frames turned the animation with it
    // (a chair's get-out, from the corner the trees snap the Sim to, turned
    // around, swung the seated body out of the chair for a third of a
    // second; inferred: the engine places the animation at the Sim's
    // direction at once).
    if (!current_.walking && !current_.animation.empty() && current_.climb.empty())
        return heading_ = target;
    heading_ = turnTowards(heading_, target, turnSpeed * dt);
    return heading_;
}

} // namespace os2::vm
