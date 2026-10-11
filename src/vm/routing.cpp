// Positions in the world: occupancy of the lot's tiles.
#include "vm.hpp"

#include "formats/binreader.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace os2::vm {

bool VM::blocksRouting(const Object& o) {
    if (!o.position.inWorld() || o.person || o.data[objdata::ContainerId] != 0)
        return false;
    if (o.definition) {
        switch (o.definition->objectType()) {
        case kObjectTypeDoor:
        case kObjectTypeWindow:
        case kObjectTypeStairs:
        case kObjectTypeModularStairs:
        case kObjectTypeModularStairsPortal: return false;
        default: break;
        }
    }
    return (o.data[objdata::Flags] & (0x01 | 0x04 | 0x10)) == 0;
}

BlockedGrid VM::blockedTiles(int level, std::span<const ObjectId> ignore) {
    if (!world)
        throw VMError("no world (positions need a lot)");
    std::set<ObjectId> skip;
    for (ObjectId id : ignore)
        if (const Object* o = object(id))
            skip.insert(multiTileGroup(*o));
    BlockedGrid grid(size_t(world->width()) * world->height(), 0);
    for (const auto& [id, o] : objects_) {
        if (o.position.level != level || !blocksRouting(o) || skip.count(multiTileGroup(o)))
            continue;
        const TilePos t = tileOf(o);
        if (world->contains(t))
            grid[world->index(t.x, t.y)] = 1;
    }
    return grid;
}

bool VM::tileFree(TilePos t, std::span<const ObjectId> ignore) {
    if (!world)
        throw VMError("no world (positions need a lot)");
    if (!world->walkable(t))
        return false;
    return !blockedTiles(t.level, ignore)[world->index(t.x, t.y)];
}

std::vector<ObjectId> VM::objectsOnTile(TilePos t) const {
    std::vector<ObjectId> out;
    for (const auto& [id, o] : objects_)
        if (o.position.inWorld() && o.data[objdata::ContainerId] == 0 && tileOf(o) == t)
            out.push_back(id);
    return out;
}

} // namespace os2::vm

namespace os2::vm {

// Routing slots (SLOT type 3) say where a Sim stands to use an object. Read
// as FreeSO's VMSlotParser reads The Sims 1's, with the Sims 2 values (see
// Slots::Slot): flags bits 0-7 are the allowed directions around the object
// (bit n = direction n relative to the object's own; 0x55 the four sides,
// 0xFF all eight), 0x200 makes them absolute, 0x800 ignores rooms, 0x1000
// snaps the facing, 0x10000 uses the average position of a multi-tile
// object; the distances are in tiles; "facing" is -3 anywhere, -2 towards the
// object, -1 away from it, 0-7 a direction relative to the object's
// (inferred: the global SLOT's eight offset slots, 0.7 tiles around the
// object, all face away from it that way, as a Sim stands to sit down). A slot
// with an offset or without directions is a point (offsets are in tiles in
// the object's model space, whose forward axis is +Y); in the Sims 2 most
// object slots are points with no offset: the position comes from a bone of
// the model ("No bone on model corresponding to name in routing slot"), which
// is taken here as the object's own tile. Goals are whole tiles: a range of
// distances becomes the tiles whose centres are within it (half a tile of
// slack), in an allowed direction, with no wall in between and in the
// object's room; each costs how far it is from the optimal distance.
VM::RouteGoals VM::routingGoals(const Slots::Slot& slot, const Object& target) {
    RouteGoals out;
    out.level = target.position.level;
    float cx = target.position.x, cy = target.position.y;
    if ((slot.flags & 0x10000) && multiTileGroup(target)) {
        float sx = 0, sy = 0;
        int n = 0;
        for (const auto& [id, o] : objects_)
            if (multiTileGroup(o) == multiTileGroup(target) && o.position.inWorld())
                sx += o.position.x, sy += o.position.y, ++n;
        if (n)
            cx = sx / float(n), cy = sy / float(n);
    }
    const int objDir = ((target.data[objdata::Direction] % 8) + 8) % 8;
    const uint32_t dirs = slot.flags & 0xFF;
    const bool absolute = slot.flags & 0x200, ignoreRooms = slot.flags & 0x800;
    auto facingAt = [&](float px, float py) {
        if (slot.facing >= 0)
            return (objDir + slot.facing) % 8;
        if (slot.facing == -2 || slot.facing == -1) {
            const int d = slot.facing == -2 ? directionOf(cx - px, cy - py) : directionOf(px - cx, py - cy);
            return d < 0 ? objDir : d;
        }
        return -1;
    };
    const TilePos centre = tileOf(target);

    const float ox = slot.offset[0], oy = slot.offset[1];
    if (dirs == 0 || ox != 0 || oy != 0) {
        const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
        const float px = cx + float(ox * std::cos(yaw) - oy * std::sin(yaw));
        const float py = cy + float(ox * std::sin(yaw) + oy * std::cos(yaw));
        out.goals.push_back({int(std::floor(px)), int(std::floor(py)), 0});
        out.points.push_back({px, py});
        out.facings.push_back(facingAt(px, py));
        out.onTarget = out.goals.back().x == centre.x && out.goals.back().y == centre.y;
        return out;
    }
    if (!world)
        return out;
    const float minD = slot.minDistance, maxD = std::max(slot.maxDistance, slot.minDistance);
    const float optimal = slot.optimalDistance > 0 ? slot.optimalDistance : minD;
    const int reach = int(std::ceil(maxD)) + 1;
    for (int x = centre.x - reach; x <= centre.x + reach; ++x)
        for (int y = centre.y - reach; y <= centre.y + reach; ++y) {
            const TilePos t{x, y, out.level};
            if (!world->contains(t) || (x == centre.x && y == centre.y))
                continue;
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float d = std::hypot(px - cx, py - cy);
            if (d < minD - 0.5f || d > maxD + 0.5f)
                continue;
            int dir = directionOf(px - cx, py - cy);
            if (!absolute)
                dir = ((dir - objDir) % 8 + 8) % 8;
            if (!(dirs & (1u << dir)))
                continue;
            if (!ignoreRooms && world->room(t) != world->room(centre))
                continue;
            if (!world->lineOfSight(centre, t))
                continue;
            out.goals.push_back({x, y, std::fabs(d - optimal)});
            out.points.push_back({px, py});
            out.facings.push_back(facingAt(px, py));
        }
    return out;
}

} // namespace os2::vm

// --- Primitives --------------------------------------------------------------

namespace os2::vm {

namespace {

constexpr uint8_t kOwnerParam = 0x09;
constexpr uint8_t kOwnerLocal = 0x19;
constexpr uint16_t kGlobalSlots = 0x64; // the global SLOT (as FreeSO's global.iff SLOT 100)

World& worldOf(Context& c) {
    if (!c.vm.world)
        throw VMError("no world (positions need a lot)");
    return *c.vm.world;
}

void setDirection(Object& o, int dir) {
    if (dir >= 0)
        o.data[objdata::Direction] = int16_t(dir % 8);
}

// Puts an object (not in a slot any more) at a point of a level and keeps its
// room up to date.
void moveTo(VM& vm, Object& o, float x, float y, int level) {
    o.position = {x, y, level};
    if (vm.world)
        o.data[objdata::Room] = vm.world->room(VM::tileOf(o));
}

void leaveSlot(Object& o) {
    o.data[objdata::ContainerId] = 0;
    o.data[objdata::SlotNumber] = -1;
}

// Moves a walking object along its route by one tick's worth; true once it
// has arrived (the route is then dropped and the final facing set).
bool advance(VM& vm, Object& me) {
    auto& r = *me.route;
    float budget = walkTilesPerTick(vm, me);
    while (budget > 0 && !r.points.empty()) {
        const auto [px, py] = r.points.front();
        const float dx = px - me.position.x, dy = py - me.position.y;
        const float d = std::hypot(dx, dy);
        if (d > 0)
            setDirection(me, directionOf(dx, dy));
        if (d <= budget) {
            moveTo(vm, me, px, py, r.level);
            budget -= d;
            r.points.erase(r.points.begin());
        } else {
            moveTo(vm, me, me.position.x + dx / d * budget, me.position.y + dy / d * budget, r.level);
            budget = 0;
        }
    }
    if (!r.points.empty())
        return false;
    setDirection(me, r.facing);
    me.route.reset();
    return true;
}

// Walks the thread's object to one of `goals` (planned once, on the first
// run of the instruction): yields every tick while walking, true on
// arrival, false when no goal can be reached. Routing out of the world is
// the game's own error; routes to another level would need the stairs and
// are not implemented.
Exit walk(Context& c, ObjectId target, const std::function<VM::RouteGoals()>& plan) {
    Object& me = c.me();
    const size_t depth = c.thread.stack.size();
    if (me.route && me.route->depth == depth && me.route->tree == c.frame.behavior && me.route->node == c.frame.pc)
        return advance(c.vm, me) ? Exit::True : Exit::Yield;
    me.route.reset();
    World& world = worldOf(c);
    if (!me.position.inWorld())
        throw VMError("Trying to route while out of world.");
    const VM::RouteGoals goals = plan();
    if (goals.goals.empty())
        return Exit::False;
    if (goals.level != me.position.level)
        throw VMError("routing to another level not implemented");
    std::vector<ObjectId> ignore{me.id};
    if (goals.onTarget)
        ignore.push_back(target);
    const BlockedGrid blocked = c.vm.blockedTiles(goals.level, ignore);
    Path path;
    if (!findPath(world, blocked, VM::tileOf(me), goals.goals, path))
        return Exit::False;
    Object::Route r;
    for (const TilePos& t : path.tiles)
        r.points.push_back({float(t.x) + 0.5f, float(t.y) + 0.5f});
    // The last point is the goal's own (a slot's offset); a route that stays
    // on its tile only moves to it.
    if (!r.points.empty())
        r.points.pop_back();
    r.points.push_back(goals.points[path.goal]);
    r.level = goals.level;
    r.facing = goals.facings[path.goal];
    r.target = target;
    r.depth = depth, r.tree = c.frame.behavior, r.node = c.frame.pc;
    if (me.data[objdata::ContainerId] != 0)
        leaveSlot(me);
    me.route = std::move(r);
    return advance(c.vm, me) ? Exit::True : Exit::Yield;
}

// The routing slots (type 3) of an object's SLOT, in order: the routing slot
// numbers of the primitives count only these (as FreeSO does).
std::vector<const Slots::Slot*> routingSlots(const Slots* table) {
    std::vector<const Slots::Slot*> out;
    if (table)
        for (const auto& s : table->slots)
            if (s.type == 3)
                out.push_back(&s);
    return out;
}

const Slots* slotTable(VM& vm, const Object& o) {
    return o.definition ? vm.code().slots(o.group, o.definition->slotId()) : nullptr;
}

// Routing slot `index` of `target` (or of the global SLOT); the game's error
// when there is none.
const Slots::Slot& routingSlot(VM& vm, const Object& target, bool global, int index) {
    const auto slots = routingSlots(global ? vm.code().slots(kGlobalGroup, kGlobalSlots) : slotTable(vm, target));
    if (index < 0 || size_t(index) >= slots.size())
        throw VMError("Invalid routing slot specified (" + std::string(global ? "global " : "") + "slot " +
                      std::to_string(index) + " of " + std::to_string(slots.size()) + ")");
    return *slots[size_t(index)];
}

// Go To Routing Slot (0x2D): slot u16 at 0, where it is u16 at 2 (STR#
// 0xD7: 0 the stack param of that number, 1 the literal routing slot, 2 the
// global slot), flags u8 at 4 (FreeSO: 0x01 no failure trees; the game also
// uses 0x02-0x10, not understood). The thread's object walks to that routing
// slot of the stack object; false when the stack object is not in the world
// or no goal can be reached. Bytes 5+ are not understood.
Exit goToRoutingSlot(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t data = le16(&o[0]), scope = le16(&o[2]);
    if (scope > 2)
        throw VMError("Go To Routing Slot: slot scope " + std::to_string(scope) + " not implemented");
    const Object* target = c.vm.object(c.frame.stackObject);
    if (!target || !target->position.inWorld())
        return Exit::False;
    const int index = scope == 0 ? c.get(kOwnerParam, int16_t(data)) : int(data);
    const Slots::Slot& slot = routingSlot(c.vm, *target, scope == 2, index);
    const ObjectId id = target->id;
    return walk(c, id, [&] { return c.vm.routingGoals(slot, *c.vm.object(id)); });
}

// Go To Relative Position (0x1B), with The Sims 1's layout (FreeSO), which
// the game's trees fit: u16 at 0 (TS1: how long to wander around objects),
// location s8 at 2 (STR# 0x82: -2 on top of, -1 anywhere near, 0-7 in front
// of, front right, right, ... relative to the stack object), direction s8 at
// 3 (STR# 0x83: -2 facing it, -1 any, 0-7 relative to its direction), u16 at
// 4, flags u8 at 6 (FreeSO: 0x01 allow different altitudes, 0x02 no failure
// trees). The location becomes a routing slot as FreeSO builds it: on top =
// the object's tile, anywhere near = 1 to 2 tiles all around, a direction =
// 1 to 1.5 tiles that way.
Exit goToRelativePosition(Context& c) {
    const auto& o = c.instruction.operands;
    const auto location = int8_t(o[2]), direction = int8_t(o[3]);
    if (location < -2 || location > 7 || direction < -2 || direction > 7)
        throw VMError("Go To Relative Position: location " + std::to_string(location) + "/direction " +
                      std::to_string(direction) + " not implemented");
    const Object* target = c.vm.object(c.frame.stackObject);
    if (!target || !target->position.inWorld())
        return Exit::False;
    Slots::Slot slot;
    slot.type = 3;
    if (location == -1) {
        slot.flags = 0xFF;
        slot.minDistance = slot.optimalDistance = 1, slot.maxDistance = 2;
    } else if (location >= 0) {
        slot.flags = 1u << location;
        slot.minDistance = slot.optimalDistance = 1, slot.maxDistance = 1.5f;
    }
    slot.facing = direction == -2 ? -2 : direction == -1 ? -3 : direction;
    const ObjectId id = target->id;
    return walk(c, id, [&] { return c.vm.routingGoals(slot, *c.vm.object(id)); });
}

// Places `o` (and the other parts of its multi-tile object, keeping their
// offsets) with its lead on tile `t`, if every part's tile is free.
bool tryPlace(VM& vm, Object& o, TilePos t, int direction = -1) {
    std::vector<Object*> parts;
    const ObjectId group = multiTileGroup(o);
    for (const auto& [id, x] : vm.objects())
        if (multiTileGroup(x) == group)
            parts.push_back(vm.object(id));
    const TilePos from = VM::tileOf(o);
    const std::vector<ObjectId> ignore{o.id};
    for (Object* p : parts) {
        TilePos pt = t;
        if (p != &o && p->position.inWorld() && o.position.inWorld())
            pt.x += VM::tileOf(*p).x - from.x, pt.y += VM::tileOf(*p).y - from.y;
        if (!vm.tileFree(pt, ignore))
            return false;
    }
    for (Object* p : parts) {
        float x = float(t.x) + 0.5f, y = float(t.y) + 0.5f;
        if (p != &o && p->position.inWorld() && o.position.inWorld())
            x += p->position.x - o.position.x, y += p->position.y - o.position.y;
        moveTo(vm, *p, p == &o || !o.position.inWorld() ? float(t.x) + 0.5f : x,
               p == &o || !o.position.inWorld() ? float(t.y) + 0.5f : y, t.level);
        if (p->data[objdata::ContainerId] != 0)
            leaveSlot(*p);
        setDirection(*p, direction);
        p->route.reset();
    }
    return true;
}

// Find Location For (0x10): places an object (it does not walk). Mode u8 at
// 0 (STR# 0xEF: 0 normal, 1 out of world, 2 normal in the same room, 3 along
// the reference object's vector, 4 lateral to it, 5-8 in a routing slot of
// the reference object: slot number in the stack param, local, literal or
// the global SLOT), local u8 at 1, flags u8 at 2, u8 at 3, then (with flag
// 0x08) the object to place (owner u8 at 4, data u16) and the reference
// object (owner u8 at 7, data u16), slot number u16 at 10. Without flag 0x08
// the object is the stack object and the reference me, or the object in the
// local of byte 1 with flag 0x01 (The Sims 1's layout, FreeSO). Searches as
// FreeSO does: the reference's tile, then rings around it up to 10 tiles
// (normal), or both ways along a line (vector modes), for a tile where every
// part of the object fits. Not understood: flag 0x01 together with 0x08,
// 0x02 (FreeSO: do not prefer empty tiles), 0x04 (user editable tiles only),
// 0x10/0x40 (the editor's strings name "with empty border", "on level
// ground", "begin in front of refobj", "with line of site to center"), byte
// 3 (0xFF/0xFE/0xAA/...: allowed facings?) and bytes 12-13; facing is kept,
// except along a vector, where it is the vector's.
Exit findLocationFor(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t mode = o[0], flags = o[2];
    if (mode > 8)
        throw VMError("Find Location For: mode " + std::to_string(mode) + " not implemented");
    Object* obj = nullptr;
    const Object* ref = nullptr;
    if (flags & 0x08) {
        obj = c.vm.object(c.get(o[4], int16_t(le16(&o[5]))));
        ref = c.vm.object(c.get(o[7], int16_t(le16(&o[8]))));
    } else {
        obj = c.vm.object(c.frame.stackObject);
        ref = flags & 0x01 ? c.vm.object(c.get(kOwnerLocal, o[1])) : &c.me();
    }
    if (!obj)
        throw VMError("Find Location For: no object to place");
    if (mode == 1) { // out of world
        obj->position.x = obj->position.y = -1;
        obj->route.reset();
        return Exit::True;
    }
    World& world = worldOf(c);
    if (!ref || !ref->position.inWorld())
        return Exit::False;
    const TilePos at = VM::tileOf(*ref);
    if (mode >= 5) {
        const uint16_t data = le16(&o[10]);
        const int index = mode == 5 ? c.get(kOwnerParam, int16_t(data))
                        : mode == 6 ? c.get(kOwnerLocal, int16_t(data))
                                    : int(data);
        const Slots::Slot& slot = routingSlot(c.vm, *ref, mode == 8, index);
        VM::RouteGoals goals = c.vm.routingGoals(slot, *ref);
        std::vector<size_t> order(goals.goals.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return goals.goals[a].cost < goals.goals[b].cost; });
        for (size_t i : order)
            if (tryPlace(c.vm, *obj, {goals.goals[i].x, goals.goals[i].y, goals.level}, goals.facings[i]))
                return Exit::True;
        return Exit::False;
    }
    if (mode == 3 || mode == 4) {
        const int dir = ((ref->data[objdata::Direction] % 8) + 8) % 8 + (mode == 4 ? 2 : 0);
        const auto [sx, sy] = directionStep(dir);
        for (int i = 0; i < 32; ++i) {
            const int k = i / 2, sign = i % 2 ? -1 : 1;
            if (i % 2 && k == 0)
                continue;
            if (tryPlace(c.vm, *obj, {at.x + sign * sx * k, at.y + sign * sy * k, at.level}, dir % 8))
                return Exit::True;
        }
        return Exit::False;
    }
    // 0, 2: rings around the reference.
    const int16_t room = world.room(at);
    for (int r = 0; r <= 10; ++r)
        for (int x = at.x - r; x <= at.x + r; ++x)
            for (int y = at.y - r; y <= at.y + r; ++y) {
                if (std::max(std::abs(x - at.x), std::abs(y - at.y)) != r)
                    continue;
                const TilePos t{x, y, at.level};
                if (mode == 2 && world.room(t) != room)
                    continue;
                if (tryPlace(c.vm, *obj, t))
                    return Exit::True;
            }
    return Exit::False;
}

// Snap Into (0x71), a Sims 2 primitive: puts an object into a containment
// slot of another. The editor's strings (Sims2.exe, cEdithLanguage::
// CatSnapIntoParam) read "(Snap Object in X into Object in Y, using slot #
// in temp 0, Reset root bones". Operands, from how the trees use them: the
// object (owner u8 at 0, data u16), the container (owner u8 at 3, data u16),
// slot u8 at 6 (containment slots, numbered as object data "slot number"
// and the slot data owners: the SLOT's type 0 entries; a Sim sits in slot 0
// of a chair, kids in slots 14 and 16 of a car), flags u8 at 9: 0x01 the slot
// number is in temp 0 (inferred: the slot byte is then 0), 0x04 reset root
// bones (the renderer's; ignored). 0x02 (16 uses, in "Be Fed - Find Drop
// Slot", "Sub - Is Locked?") is not understood; bytes 7-8 (0x09B4, 0x0BEF,
// ...) neither. The object takes the container's position, as the saved
// lots keep contained objects; false when another object is in the slot;
// the game's error when the slot does not exist.
Exit snapInto(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[9];
    if (flags & ~0x05)
        throw VMError("Snap Into: flags " + hexString(flags) + " not implemented");
    Object* x = c.vm.object(c.get(o[0], int16_t(le16(&o[1]))));
    Object* into = c.vm.object(c.get(o[3], int16_t(le16(&o[4]))));
    if (!x || !into)
        throw VMError("Invalid object id.");
    if (x == into)
        throw VMError("Snap Into: an object into itself");
    const int slot = flags & 0x01 ? c.thread.temps[0] : o[6];
    size_t containers = 0;
    if (const Slots* table = slotTable(c.vm, *into))
        for (const auto& s : table->slots)
            containers += s.type == 0;
    if (slot < 0 || size_t(slot) >= containers)
        throw VMError("Slot number out of range. (slot " + std::to_string(slot) + " of " + std::to_string(containers) +
                      ")");
    const ObjectId there = c.vm.containedIn(into->id, int16_t(slot));
    if (there && there != x->id)
        return Exit::False;
    x->data[objdata::ContainerId] = into->id;
    x->data[objdata::SlotNumber] = int16_t(slot);
    x->position = into->position;
    x->data[objdata::Room] = into->data[objdata::Room];
    x->route.reset();
    return Exit::True;
}

} // namespace

void registerRoutingPrimitives(VM& vm) {
    vm.setPrimitive(0x10, findLocationFor);
    vm.setPrimitive(0x1B, goToRelativePosition);
    vm.setPrimitive(0x2D, goToRoutingSlot);
    vm.setPrimitive(0x71, snapInto);
}

} // namespace os2::vm
