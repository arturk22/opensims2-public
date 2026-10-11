// What a Sim can see: Set to Next 24, "Visible object in my view within
// distance in temp0" (STR# 0xA4), which the idle look-at ("CT - Start Idle
// Look At", "CT - Start Routing Look At"), "Reaction - Banter" (Sims within
// 4 tiles) and the goal check of the Sims' wants use.
//
// Inferred (no code to read; the five trees only loop over the results):
// objects placed in the world on my level, other than me, whose distance
// (between positions, in tiles) is at most temp 0, that are not behind me
// (within 90 degrees of the way I face, object data "direction") and with
// in the same room (or, where a tile has no room, with no wall across the
// line between our tiles: Look At's editor strings have ", ignoring Room"
// and ", ignoring Frustrum", so the view is a frustum and a room); by ID
// after the current one, false past the last, as the other iterating
// searches.

#include "vm.hpp"

#include "formats/binreader.hpp"

#include <cmath>

namespace os2::vm {

std::optional<ObjectId> nextVisibleObject(VM& vm, const Object& me, ObjectId current, int16_t distance) {
    if (!me.position.inWorld())
        return std::nullopt;
    const auto [fx, fy] = directionStep(((me.data[objdata::Direction] % 8) + 8) % 8);
    const TilePos from = VM::tileOf(me);
    for (auto it = vm.objects().upper_bound(current); it != vm.objects().end(); ++it) {
        const Object& x = it->second;
        if (x.id == me.id || !x.position.inWorld() || x.position.level != me.position.level)
            continue;
        const float dx = x.position.x - me.position.x, dy = x.position.y - me.position.y;
        if (std::sqrt(dx * dx + dy * dy) > float(distance))
            continue;
        if (dx * float(fx) + dy * float(fy) < 0)
            continue;
        if (vm.world) {
            const TilePos to = VM::tileOf(x);
            const int16_t a = vm.world->room(from), b = vm.world->room(to);
            if (a && b ? a != b : !vm.world->lineOfSight(from, to))
                continue;
        }
        return x.id;
    }
    return std::nullopt;
}

namespace {

// Look At (0x6E): turns a Sim's head (and maybe spine) towards something.
// The editor's strings (Sims2.exe, in reverse order) list "Targeting ID in",
// "Targeting Camera", "default 3/4 height", slots ("targeting slot", "routing
// slot", "containment slot", " number %d"), ", including Spine", ", using
// duration in temp 0", ", no early exit", the turn towards/away speeds in
// degrees per second (or in temps 1/2), "STOP", ", ignoring Room" and
// ", ignoring Frustrum"; the game's errors include "Target object out of
// world." and "Requesting a look_at operation that is already running".
// From the uses: the target ID is (owner u8 at 1, data u16 at 2), bytes 4
// and 5 the turn towards/away speeds (45 or 90 degrees per second in most
// uses) and bytes 6-7 a duration in milliseconds (1000 in "Idle System -
// Look At Something"). The rest of the flags (byte 0, 8-15) are not
// understood. Here the request is only recorded on the Sim (Object::lookAt)
// for the renderer, and the primitive returns at once: true, or false when
// the target does not exist or is not in the world (the idle trees go on
// either way). Inferred.
Exit lookAt(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    const ObjectId target = c.get(o[1], int16_t(le16(&o[2])));
    const Object* x = c.vm.object(target);
    if (!x || (!x->position.inWorld() && !x->data[objdata::ContainerId])) {
        me.lookAt.reset();
        return Exit::False;
    }
    const int ms = le16(&o[6]);
    me.lookAt = Object::LookAt{target, c.vm.ticks, ms * 30 / 1000};
    return Exit::True;
}

} // namespace

void registerViewPrimitives(VM& vm) { vm.setPrimitive(0x6E, lookAt); }

} // namespace os2::vm
