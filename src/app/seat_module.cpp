#include "app/seat_module.hpp"

#include "formats/sim_actor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace os2 {

bool SeatModule::isJump(const Xform& from, const Xform& to) {
    const Xform d = from.inverse().then(to);
    const float moved = std::hypot(d.t[0], d.t[1], d.t[2]);
    const float turned = 2 * std::acos(std::min(1.0f, std::fabs(d.q.w)));
    return moved > vm::SimMotion::kMaxStep || turned > 1.0f;
}

void SeatModule::place(int16_t id, SimActor& actor, vm::SimMotion& motion, double now, float t, float dt, float z,
                       float zOffset) {
    Track& k = tracks_[id];
    // Rule 1: where the VM has it, or the slot holding it. (On a stair's
    // steps the climber is contained in each step object in turn: the VM's
    // anchor and height draw it, the climb's animation moving it.)
    actor.position = {motion.x(t), motion.y(t), z + zOffset};
    actor.heading = motion.turn(dt);
    bool seated = false;
    if (slot && !motion.current().onStep)
        if (const auto at = slot(id, now)) {
            const Vec3 f = at->rotate({0, 1, 0});
            actor.position = at->t;
            actor.heading = std::atan2(-f[0], f[1]);
            motion.setHeading(actor.heading); // it turns on from there once out
            seated = true;
        }
    actor.rootMotion = false;
    // Rule 3: the root jumped (in or out of a seat, a Snap, a stair's end,
    // a walk handed off at its stop's fade out): what plays once or fades
    // out stays where it was in the world.
    const Xform root = actor.placement();
    static const bool keep = std::getenv("OS2_NO_STAY_IN_WORLD") == nullptr; // (test aid: the body jumps with the root)
    if (k.last && keep && (seated != k.seated || isJump(*k.last, root) || motion.current().handOff))
        actor.player.stayInWorld(root.inverse().then(*k.last), now);
    k.seated = seated;
    k.last = root;
}

bool SeatModule::seated(int16_t id) const {
    const auto it = tracks_.find(id);
    return it != tracks_.end() && it->second.seated;
}

} // namespace os2
