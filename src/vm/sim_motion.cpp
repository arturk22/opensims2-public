#include "sim_motion.hpp"

#include "animation.hpp"
#include "vm.hpp"

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
    if (o.animation.playing())
        s.animation = o.animation.name;
    s.serial = o.animation.serial;
    if (s.walking)
        s.walk = locomotionAnimation(vm, o, "walk");
    if (o.route && !o.route->climb.animation.empty()) {
        s.climb = o.route->climb.animation;
        s.climbSerial = o.route->climb.serial;
        s.z = o.route->climb.z;
    }
    previous_ = count_ ? current_ : s;
    current_ = std::move(s);
    if (!count_)
        heading_ = headingOfDirection(current_.direction);
    ++count_;
}

float SimMotion::x(float t) const { return previous_.x + (current_.x - previous_.x) * t; }
float SimMotion::y(float t) const { return previous_.y + (current_.y - previous_.y) * t; }

float SimMotion::turn(float dt, float turnSpeed) {
    const float dx = current_.x - previous_.x, dy = current_.y - previous_.y;
    float target = headingOfDirection(current_.direction);
    if (current_.walking && std::hypot(dx, dy) > 1e-4f)
        target = headingOfMotion(dx, dy);
    heading_ = turnTowards(heading_, target, turnSpeed * dt);
    return heading_;
}

} // namespace os2::vm
