// What a Sim can see: Set to Next 24, "Visible object in my view within
// distance in temp0" (STR# 0xA4), which the idle look-at ("CT - Start Idle
// Look At", "CT - Start Routing Look At"), "Reaction - Banter" (Sims within
// 4 tiles) and the goal check of the Sims' wants use. By the Mac binary
// (cTSPerson::GetNextObjectInView, BV 0xacfa00; TrySetToNext calls it on a
// Sim, else error 0x1C): the distance, at most 16 tiles, makes a cone of
// tiles ahead of the Sim: rows 0 to r - 1 ahead (row 0 is the Sim's own
// tile), each 2 x row + 1 tiles to either side (at most r), inside a circle
// of radius r + 1 (facing a diagonal the grid turns 45 degrees: r * sqrt(2)
// + 1 rows of half-diagonal steps, 0.7071 x 0.7071). On those tiles, the
// objects in the world (not in a slot) other than me and the objects of
// type 5, in my room, not being deleted and with nothing in the way (the
// world's line test from my tile to theirs); by ID. The binary keeps the
// list while it iterates; a current object that is not on it ends the
// search (0, false). Inferred: where the half-tile steps round to, and that
// the line test is the walls' (vm::World::lineOfSight).

#include "vm.hpp"

#include "formats/binreader.hpp"

#include <cmath>
#include <set>

namespace os2::vm {

std::optional<ObjectId> nextVisibleObject(VM& vm, const Object& me, ObjectId current, int16_t distance) {
    if (!me.position.inWorld())
        return std::nullopt;
    const int dir = ((me.data[objdata::Direction] % 8) + 8) % 8;
    const auto [fx, fy] = directionStep(dir);
    const int rx = -fy, ry = fx; // across (the cone is symmetric)
    const bool diagonal = dir & 1;
    const int r = std::min<int>(distance, 16);
    const int rows = diagonal ? int(float(r) * 1.4142135f) + 1 : r;
    const int bound = diagonal ? (2 * r + 2) * (r + 1) : (r + 1) * (r + 1);
    const float step = diagonal ? 0.5f : 1.0f;
    const TilePos from = VM::tileOf(me);
    const int16_t myRoom = vm.world ? vm.world->room(from) : 0;
    std::set<ObjectId> seen;
    for (int d = 0; d < rows; ++d)
        for (int l = std::max(-rows, -(2 * d + 1)); l <= std::min(rows, 2 * d + 1); ++l) {
            if (l * l + d * d > bound)
                continue;
            const TilePos t{int(std::floor(float(from.x) + 0.5f + float(l * rx + d * fx) * step)),
                            int(std::floor(float(from.y) + 0.5f + float(l * ry + d * fy) * step)), from.level};
            for (ObjectId id : vm.objectsOnTile(t)) {
                const Object* x = vm.object(id);
                if (!x || id == me.id || seen.count(id) || vm.dyingObjects.contains(id))
                    continue;
                if (x->definition && x->definition->objectType() == 5)
                    continue;
                if (vm.world && (vm.world->room(t) != myRoom || !vm.world->lineOfSight(from, t)))
                    continue;
                seen.insert(id);
            }
        }
    if (current != 0 && !seen.count(current))
        return std::nullopt;
    auto next = seen.upper_bound(current);
    return next == seen.end() ? std::nullopt : std::optional<ObjectId>(*next);
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
//
// The Mac binary's disassembler (docs/primitives-mac.md §5) names the flags
// of byte 0 (node version 1, 415 of the 480 uses; the game converts version
// 0 on load, LookAtParam::UpgradeVersion: bytes 4 and 5 = the old u16 at 4
// / 2, byte 0 bit 0x08 also sets bit 0x02 of byte 15, byte 0 stays): 0x01 STOP (the Sim stops looking: done here), 0x80 the camera is
// the target (nothing recorded here), 0x02 no early exit, 0x04 including
// the spine, 0x08 speed in temp 1, 0x10 the duration is in temp 0 (done; in
// the same unit as u16 6, inferred), 0x20 ignoring the room, 0x40 ignoring
// the frustum; the slot byte 8 of type s8 14, an event tree u16 9 with its
// place s8 11, byte 15 (speeds back in temps 1/2, not hurryable).
Exit lookAt(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    const uint8_t flags = o[0];
    if (flags & 0x01) {
        me.lookAt.reset();
        return Exit::True;
    }
    if (flags & 0x80)
        return Exit::True;
    const ObjectId target = c.get(o[1], int16_t(le16(&o[2])));
    const Object* x = c.vm.object(target);
    if (!x || (!x->position.inWorld() && !x->data[objdata::ContainerId])) {
        me.lookAt.reset();
        return Exit::False;
    }
    const int ms = flags & 0x10 ? int(c.thread.temps[0]) : int(le16(&o[6]));
    me.lookAt = Object::LookAt{target, c.vm.ticks, ms * 30 / 1000};
    return Exit::True;
}

} // namespace

void registerViewPrimitives(VM& vm) { vm.setPrimitive(0x6E, lookAt); }

} // namespace os2::vm
