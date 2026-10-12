// Positions in the world: occupancy of the lot's tiles.
#include "vm.hpp"

#include "errors.hpp"
#include "go_here.hpp"
#include "interactions.hpp"
#include "locomotion.hpp"
#include "sim_motion.hpp"

#include "formats/binreader.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace os2::vm {

bool VM::blocksRouting(const Object& o) {
    if (!o.position.inWorld() || o.person || o.data[objdata::ContainerId] != 0)
        return false;
    // The modular stairs' landing pieces ("Stair Modular - Deck - Hi Stub",
    // "- Hi Cutout": object type 4) share the stairs' semi-global and lie
    // flat on the landings; Sims walk over them.
    static const uint32_t stairGlobals = semiGlobalGroup("StairModularGlobals");
    if (o.semiGroup == stairGlobals)
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
    // Off the floor (placement flags, od 42, STR# 0xCA): "attached to
    // ceiling" (0x1000: the ceiling lamps), "detached from floor" (0x80), or
    // hung on a wall in the air (0x200 "allow in air" with a wall required
    // on a side, od 13 bits 0-3: curtains). Their model footprints, which
    // the game routes around, are not on the floor (inferred; the VM has no
    // footprints). A ceiling lamp blocked a tile in the middle of rooms.
    const int16_t placement = o.data[42];
    if ((placement & (0x1000 | 0x80)) || ((placement & 0x200) && (o.data[13] & 0x0F)))
        return false;
    // Small things that go on surfaces too (allowed height flags, od 4,
    // beyond bit 0 "floor": glasses, trays, bowls, books) left on the floor:
    // their model footprints are a small part of a tile the game's walkers
    // step around; here they would close the whole tile, and the glasses and
    // trays piling up on a kitchen floor shut Sims away from the fridge
    // (N001_Lot11, N001_Lot36; inferred).
    if (o.data[4] & ~0x01)
        return false;
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
    // Stair landings stay open: build mode keeps them clear, and what the
    // lots have there hangs on a wall (lamps, paintings), which blocks here
    // for want of the models' footprints.
    for (const Stair& s : world->stairs())
        for (const TilePos& t : {s.bottom, s.top})
            if (t.level == level && world->contains(t))
                grid[world->index(t.x, t.y)] = 0;
    return grid;
}

bool VM::tileFree(TilePos t, std::span<const ObjectId> ignore) {
    if (!world)
        throw VMError("no world (positions need a lot)");
    // A stair's landings and the tiles over its steps (on either of its
    // levels) stay open to walkers, floor or not: the portal trees' TEST
    // snaps the walker onto the portal's point at the top of the steps, and
    // its goal checks look there (every stair of N003_Lot192's decks was
    // locked). Placement (tryPlace) still wants a floor.
    for (const Stair& s : world->stairs())
        if ((t.level == s.bottom.level || t.level == s.top.level) && world->contains(t) &&
            t.x >= std::min(s.bottom.x, s.top.x) && t.x <= std::max(s.bottom.x, s.top.x) &&
            t.y >= std::min(s.bottom.y, s.top.y) && t.y <= std::max(s.bottom.y, s.top.y))
            return true;
    if (!world->walkable(t))
        return false;
    // As blockedTiles(t.level, ignore)[index], for this one tile only.
    std::set<ObjectId> skip;
    for (ObjectId id : ignore)
        if (const Object* o = object(id))
            skip.insert(multiTileGroup(*o));
    for (const Object* o : byId_) {
        if (!o || o->position.level != t.level || !blocksRouting(*o))
            continue;
        const TilePos at = tileOf(*o);
        if (at.x == t.x && at.y == t.y && !skip.count(multiTileGroup(*o)))
            return false;
    }
    return true;
}

std::vector<ObjectId> VM::objectsOnTile(TilePos t) const {
    std::vector<ObjectId> out;
    for (const Object* o : byId_) // by ID, as objects_ is
        if (o && o->position.inWorld() && o->data[objdata::ContainerId] == 0 && tileOf(*o) == t)
            out.push_back(o->id);
    return out;
}

} // namespace os2::vm

namespace os2::vm {

namespace {

// cTSObject::RunTree(5, ...): whether `o`'s "allow intersection" tree lets
// `other` overlap it (defined with the Go Here code below).
bool allowsIntersection(VM& vm, const Object& o, ObjectId other);
// Either one's tree lets the two overlap (TSObjectCanSetPosition and
// TSObjectMoveAwayFromOtherObjects ask both ways).
bool intersectionAllowed(VM& vm, const Object& a, const Object& b) {
    return allowsIntersection(vm, a, b.id) || allowsIntersection(vm, b, a.id);
}

// cTSRoute::EvalTileForGoal (docs/routing-mac.md §8.2), for a candidate
// goal point of a slot around `target` (whose tile is `centre`): the code
// (0 out of the world, 2 another room, 3 a wall between it and the object,
// 4 a wall within 0.3 tile, 5 nothing to stand on, 6 an object on it, 7
// valid) and, when valid, the penalties added to its cost: +10 for a wall
// within 0.4 tile, +10 for another room when the slot ignores rooms, +5 for
// a door on its tile. Occupied, from TSObjectCanSetPositionAndOrientation's
// errors in the binary: 1 for an object in the way (here: a blocking object
// on the tile, not a part of the target) and 6 for a person (object type 2;
// here: another Sim, not the router, whose centre is within half a tile, two
// bodies' radii: inferred). Not done: the pushable objects (+10 instead of
// 1: an object of type 8 when the route may push), the water/terrain query
// (+5), and the slot flags that allow 2/3 with +10.
int evalGoal(VM& vm, const World& w, const Object& target, TilePos centre, float px, float py, int level,
             bool ignoreRooms, ObjectId router, float& penalty) {
    penalty = 0;
    const TilePos t{int(std::floor(px)), int(std::floor(py)), level};
    if (!w.contains(t))
        return 0;
    if (w.room(t) != w.room(centre)) {
        if (!ignoreRooms)
            return 2;
        penalty += 10;
    }
    if (!(t.x == centre.x && t.y == centre.y) && !w.lineOfSight(centre, t))
        return 3;
    // The nearest wall edge of the tiles around.
    float nearest = 1e9f;
    for (int x = t.x - 1; x <= t.x + 1; ++x)
        for (int y = t.y - 1; y <= t.y + 1; ++y) {
            const TilePos u{x, y, level};
            if (!w.contains(u))
                continue;
            if (w.diagonalWall(u))
                nearest = std::min(nearest, std::hypot(float(x) + 0.5f - px, float(y) + 0.5f - py));
            auto segment = [&](float ax, float ay, float bx, float by) {
                const float vx = bx - ax, vy = by - ay;
                const float k = std::clamp(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
                nearest = std::min(nearest, std::hypot(ax + vx * k - px, ay + vy * k - py));
            };
            if (w.wallBetween(u, -1, 0)) segment(float(x), float(y), float(x), float(y + 1));
            if (w.wallBetween(u, 0, -1)) segment(float(x), float(y), float(x + 1), float(y));
        }
    if (nearest < 0.3f)
        return 4;
    if (nearest < 0.4f)
        penalty += 10;
    if (!w.walkable(t))
        return 5;
    const ObjectId group = multiTileGroup(target);
    const Object* walker = vm.object(router);
    for (const auto& [id, o] : vm.objects())
        if (o.position.level == level && multiTileGroup(o) != group && VM::blocksRouting(o) && VM::tileOf(o) == t &&
            !(walker && intersectionAllowed(vm, o, *walker)))
            return 1;
    for (const auto& [id, o] : vm.objects())
        if (o.person && id != router && id != target.id && o.position.inWorld() && o.position.level == level &&
            o.data[objdata::ContainerId] == 0 && std::hypot(o.position.x - px, o.position.y - py) < 0.5f)
            return 6;
    for (const World::Door& d : w.doors())
        if (d.tile == t)
            penalty += 5;
    return 7;
}

// The route result of a candidate EvalTileForGoal turned down (BV
// 0x478f8c..: 1/6 destination occupied, 2-4 wall in the way, 5 altitudes
// don't match; 0 records nothing).
int16_t refusedResult(int code) {
    switch (code) {
    case 1:
    case 6: return int16_t(RouteFail::DestinationOccupied);
    case 2:
    case 3:
    case 4: return int16_t(RouteFail::WallInTheWay);
    case 5: return int16_t(RouteFail::AltitudesDontMatch);
    default: return 0;
    }
}

} // namespace

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
VM::RouteGoals VM::routingGoals(const Slots::Slot& slot, const Object& target, bool evaluate, ObjectId router) {
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
    // The model of a multi-tile object is the lead tile's: its bones are
    // placed from the lead's point and direction, whichever part's slot
    // names them (the straight staircase's top pad, "Hi 0,0", has the
    // "route_staircasestraight_*_top" bones 3.7 tiles up from the lead "Lo
    // 0,4"; from the pad itself they fell 3.7 tiles past the top, the stair
    // portal's Snap failed and no one reached the upper floors of N001_Lot11;
    // inferred: the scene graph's model is the lead's).
    float bcx = cx, bcy = cy;
    int boneDir = objDir;
    if (const ObjectId lead = multiTileGroup(target); lead != target.id)
        if (const Object* l = object(lead); l && l->position.inWorld())
            bcx = l->position.x, bcy = l->position.y, boneDir = ((l->data[objdata::Direction] % 8) + 8) % 8;
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
    // A candidate goal: with `evaluate`, EvalTileForGoal's verdict and penalties.
    auto add = [&](float px, float py, float cost) {
        if (evaluate && world) {
            float penalty = 0;
            const int code = evalGoal(*this, *world, target, centre, px, py, out.level, ignoreRooms, router, penalty);
            if (code != 7) {
                if (const int16_t why = refusedResult(code))
                    out.refused = why;
                return false;
            }
            cost += penalty;
        }
        out.goals.push_back({int(std::floor(px)), int(std::floor(py)), cost});
        out.points.push_back({px, py});
        out.facings.push_back(facingAt(px, py));
        return true;
    };

    float ox = slot.offset[0], oy = slot.offset[1];
    // 0x1000 "snap to direction": the slot is the object's point (plus its
    // offset) facing the object's direction turned by the slot's facing (a
    // direction 0-7), or by its lowest direction bit (none: N). The global
    // SLOT's "CT - Snap Me" slots (0x0F, 0x10, 0x1F, 0x20) turn an object in
    // place this way: "Align Trashcan To Sim" snaps the can a step at a time
    // until it faces the Sim, and spun forever when the snap moved it one
    // tile off without turning it (N001_Lot36). As FreeSO's VMSlotParser
    // (inferred for the Sims 2).
    if (slot.flags & 0x1000) {
        // At the bones of the model (flag 0x20000): each bone's point with
        // its facing, as the slots that stand there (the chairs' get-out
        // snaps the Sim to the corner bone it gets up at: snapping it to the
        // chair's own point left it standing inside the chair).
        if ((slot.unknown10 & 0x20000) && ox == 0 && oy == 0) {
            const std::vector<SlotBone> bones = slotBones(slot, target);
            if (!bones.empty()) {
                const double yaw = (90.0 - 45.0 * boneDir) * M_PI / 180.0;
                for (const SlotBone& b : bones) {
                    const float px = bcx + float(b.x * std::cos(yaw) - b.y * std::sin(yaw));
                    const float py = bcy + float(b.x * std::sin(yaw) + b.y * std::cos(yaw));
                    if (out.goals.empty())
                        out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
                    out.goals.push_back({int(std::floor(px)), int(std::floor(py)), 0});
                    out.points.push_back({px, py});
                    out.facings.push_back((boneDir + b.facing) % 8);
                    out.multi.push_back(b.index);
                }
                return out;
            }
        }
        int turnBy = 0;
        if (slot.facing >= 0)
            turnBy = slot.facing % 8;
        else
            for (int d = 0; d < 8; ++d)
                if (dirs & (1u << d)) {
                    turnBy = d;
                    break;
                }
        const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
        const float px = cx + float(ox * std::cos(yaw) - oy * std::sin(yaw));
        const float py = cy + float(ox * std::sin(yaw) + oy * std::cos(yaw));
        out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
        out.goals.push_back({int(std::floor(px)), int(std::floor(py)), 0});
        out.points.push_back({px, py});
        out.facings.push_back((objDir + turnBy) % 8);
        return out;
    }
    // A slot at the bones of the model (flag 0x20000) stands at its bones
    // whatever its directions say (GetPossibleSlotsFromModelRoutingSlot: one
    // goal per bone): the fridge's slot 0 has direction 0 and 2 to 4 tiles,
    // and its "route_0" bone a tile in front of it; scanning 2 tiles out put
    // the Sims of N002_Lot22 on the counter across the kitchen and they
    // starved.
    const bool atBones = (slot.unknown10 & 0x20000) && ox == 0 && oy == 0 && !slotBones(slot, target).empty();
    if (dirs == 0 || ox != 0 || oy != 0 || atBones) {
        const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
        auto turn = [&](float x, float y) {
            return std::array<float, 2>{cx + float(x * std::cos(yaw) - y * std::sin(yaw)),
                                        cy + float(x * std::sin(yaw) + y * std::cos(yaw))};
        };
        // A slot at the bones of the model (flag 0x20000 of the u32 after the
        // proximities, Slots::Slot::unknown10) stands there: one goal per
        // bone, facing the object's direction plus the bone's
        // (GetPossibleSlotsFromModelRoutingSlot: score 100 less the
        // penalties; slot_bones.cpp).
        std::vector<SlotBone> bones;
        if ((slot.unknown10 & 0x20000) && ox == 0 && oy == 0)
            bones = slotBones(slot, target);
        if (bones.empty()) {
            const auto [px, py] = turn(ox, oy);
            out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
            add(px, py, 0);
            return out;
        }
        // Each goal keeps its bone's index in the slot's list, the goal's
        // multi slot index (+0x14; person data 38 once the way is planned).
        auto addFacing = [&](float px, float py, float cost, int facing, int16_t multi) {
            if (!add(px, py, cost))
                return;
            out.facings.back() = facing;
            out.multi.resize(out.goals.size() - 1, 0);
            out.multi.push_back(multi);
        };
        const double boneYaw = (90.0 - 45.0 * boneDir) * M_PI / 180.0;
        auto turnBone = [&](float x, float y) {
            return std::array<float, 2>{bcx + float(x * std::cos(boneYaw) - y * std::sin(boneYaw)),
                                        bcy + float(x * std::sin(boneYaw) + y * std::cos(boneYaw))};
        };
        bool first = true;
        for (const SlotBone& b : bones) {
            const auto [px, py] = turnBone(b.x, b.y);
            if (first)
                out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
            first = false;
            addFacing(px, py, 0, (boneDir + b.facing) % 8, b.index);
        }
        // Ours, not the binary's: points half a tile and three quarters out
        // around each bone's point, on the target's side of any wall, each
        // costing its distance more (1.5, 1.75: a later tier than the point
        // itself), for when the point cannot be reached (a chair pushed under
        // a table, which here fills its whole tile for want of the model's
        // footprint).
        if (world)
            for (const SlotBone& b : bones) {
                const auto [px, py] = turnBone(b.x, b.y);
                for (const float r : {0.5f, 0.75f})
                    for (int k = 0; k < 16; ++k) {
                        const double a = k * M_PI / 8;
                        const float qx = px + r * float(std::cos(a)), qy = py + r * float(std::sin(a));
                        const TilePos t{int(std::floor(qx)), int(std::floor(qy)), out.level};
                        if (!world->contains(t) || !world->lineOfSight(centre, t))
                            continue;
                        addFacing(qx, qy, 1 + r, (boneDir + b.facing) % 8, b.index);
                    }
            }
        return out;
    }
    if (!world)
        return out;
    // The binary's goal scan (docs/routing-mac.md §8.2; BV 0x478d2c, the
    // scan 0x4778b4 and its standing callback 0x4740d8): one origin per
    // allowed direction (the slot's offset turned that way), and from each,
    // rows across the direction at distances from the preferred one outwards
    // (preferred, then a step nearer, a step further, ...: the row count is
    // the larger of max - preferred and preferred - min in tiles, rounded
    // up, not in steps, as the binary has it), each row as wide as a cone of
    // 45 degrees (|lateral| <= distance * tan 22.5 degrees), on the slot's
    // grid (its resolution, at least 1/16 tile). The distances must be
    // ordered (0 <= min <= preferred <= max, max at most 32) or there are no
    // goals. It stops after 100 candidates or 48 accepted ones. A candidate's
    // cost (the binary keeps a score, 100 less this) is |d - preferred| * 16
    // * the slot's gradient + EvalTileForGoal's penalties + 0.001 * gradient
    // * |lateral| (a tie-break) + 10 per routing cost of its floor above 1.
    // Not done: the router's own radius added to the distances (vtable 0xB4
    // of the router: its "slot displacement", 0 for the base game's Sims),
    // the +25 for a direction off the object's walls and +20 for the Sim's room (world queries not decoded), and the
    // sitting goals (ScanConeForSittingGoals) of slots with a sitting weight.
    const float minD = slot.minDistance, optimal = slot.optimalDistance;
    const float maxD = std::min(slot.maxDistance, 32.0f);
    if (minD < 0 || maxD < 0 || maxD < minD || optimal < minD || maxD < optimal)
        return out;
    const float res = std::max(slot.resolutionTiles, 0.0625f);
    const float spread = std::tan(0.7853982f * 0.5f);
    const float weight = slot.gradient;
    const int rows = int(std::ceil(std::max(maxD - optimal, optimal - minD)));
    std::vector<int> entries; // absolute directions
    for (int d = 0; d < 8; ++d)
        if (dirs & (1u << d))
            entries.push_back(absolute ? d : (d + objDir) % 8);
    const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
    const float ex = float(ox * std::cos(yaw) - oy * std::sin(yaw));
    const float ey = float(ox * std::sin(yaw) + oy * std::cos(yaw));
    int candidates = 0, accepted = 0;
    struct Tile {
        float cost;
        size_t index;
        float lateral;
    };
    std::map<std::pair<int, int>, Tile> tiles; // without `evaluate`: the best point of each tile
    // Slot flag 0x2000 "random scoring" (the slot's vtable 0x78; FreeSO's
    // SLOTFlags.RandomScoring): when the scan has more candidates than the
    // 100 it may look at, each is taken with a chance of 100 in the total
    // (BV 0x4778b4: a random number below the total, at most 100 - the count
    // so far at the start), and a goal's score gets a random 20 x [0, 1) - 10
    // (the standing callback; here its cost, lower is better: + 10 - 20 x).
    const bool randomScoring = slot.flags & 0x2000;
    int total = 0;
    if (randomScoring)
        for (int i = 0; i <= rows; ++i)
            for (const int sign : {-1, 1}) {
                const float r = optimal + float(sign * i) * res;
                if ((i == 0 && sign == 1) || r < minD || r > maxD)
                    continue;
                total += (int(std::ceil((r * spread) / res)) - int(std::floor(-(r * spread) / res)) + 1) *
                         int(entries.size());
            }
    for (int i = 0; i <= rows; ++i)
        for (const int sign : {-1, 1}) {
            if (i == 0 && sign == 1)
                continue;
            const float r = optimal + float(sign * i) * res;
            if (r < minD || r > maxD)
                continue;
            const int lo = int(std::floor(-(r * spread) / res)), hi = int(std::ceil((r * spread) / res));
            for (int j = lo; j <= hi; ++j) {
                const float lateral = float(j) * res;
                for (const int d : entries) {
                    if (candidates >= 100 || accepted >= 48)
                        goto done;
                    if (total > 100 && std::uniform_int_distribution<int>(0, total - 1)(random) > 100)
                        continue;
                    ++candidates;
                    const auto [sx, sy] = directionStep(d);
                    const float n = std::hypot(float(sx), float(sy));
                    const float fx = float(sx) / n, fy = float(sy) / n; // forward
                    const float px = cx + ex + fx * r - fy * lateral, py = cy + ey + fy * r + fx * lateral;
                    float cost = std::fabs(std::hypot(r, lateral) - optimal) * 16 * weight +
                                 0.001f * weight * std::fabs(lateral);
                    if (randomScoring)
                        cost += 10 - 20 * std::uniform_real_distribution<float>(0, 1)(random);
                    if (!evaluate) {
                        const TilePos t{int(std::floor(px)), int(std::floor(py)), out.level};
                        if (!world->contains(t) || (t.x == centre.x && t.y == centre.y))
                            continue;
                        if (!ignoreRooms && world->room(t) != world->room(centre))
                            continue;
                        if (!world->lineOfSight(centre, t))
                            continue;
                        // The tile's best point: the cheapest, then the nearest the row's middle.
                        auto it = tiles.find({t.x, t.y});
                        if (it != tiles.end()) {
                            if (cost < it->second.cost ||
                                (cost == it->second.cost && std::fabs(lateral) < it->second.lateral)) {
                                const size_t k = it->second.index;
                                it->second = {cost, k, std::fabs(lateral)};
                                out.goals[k].cost = cost;
                                out.points[k] = {px, py};
                                out.facings[k] = facingAt(px, py);
                            }
                            continue;
                        }
                        tiles[{t.x, t.y}] = {cost, out.goals.size(), std::fabs(lateral)};
                    }
                    const TilePos t{int(std::floor(px)), int(std::floor(py)), out.level};
                    const float floorCost = world->contains(t) ? world->cost(t) : 1;
                    if (add(px, py, cost + (floorCost > 1 ? (floorCost - 1) * 10 : 0)))
                        ++accepted;
                }
            }
        }
done:
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

// Waits and limits of the blocked walker, from the Mac binary's
// cTSRoutingObjectAggregate (docs/routing-mac.md §6): a stationary Sim in
// the way is asked to move up to 3 times and waited for up to 126 ticks
// each time (re-routing when it gets 1.25 tiles away); a walking one is
// routed around; after 5 tries the route fails with "blocked by person".
constexpr int kAskToMoveTries = 3;
constexpr int kWaitForPersonTicks = 126, kWaitForPersonLongTicks = 420;
constexpr float kBlockerMovedTries = 0.25f;
constexpr float kPersonMovedAway = 1.25f;
// The locomotor's predicted blocking (cLocomotable::CollisionCheck, BV
// 0x6b49e8; docs/routing-mac.md §6): it looks 10.02 steps of 50 ms each
// ahead (a step is 50 / speed ms, so the horizon is half a tile whatever
// the speed), window by window, at where the others will be; a window in
// which another router collides blocks the walk if the two are getting
// closer (else the scan goes on), when it starts within 2 steps plus the
// stopping time (5 ms) of travel. Inferred: the collision test (vtable
// 0x478, not decoded) as two bodies' radii (0.25 tile for an adult, 0.1875
// for a child, as AddBlockerToPathPlanner sizes them).
constexpr float kStepTiles = 0.05f;  // 50 ms of a walk at 1 tile per second, times the speed: 0.05 / speed s
constexpr int kHorizonSteps = 10;    // 10.02 steps
constexpr float kBlockedWithin = 2 * kStepTiles + 0.005f;

PathPlanner& plannerOf(VM& vm) {
    World& w = *vm.world;
    if (!vm.planner || !vm.planner->builtFrom(w))
        vm.planner = std::make_shared<PathPlanner>(w);
    return *vm.planner;
}

Object::Route::Point walkPoint(float x, float y, int level) {
    Object::Route::Point p;
    p.x = x, p.y = y, p.level = level;
    return p;
}

bool smallSim(const Object& o) {
    const std::string a = agePrefix(o);
    return a == "c" || a == "p";
}

// The obstacles of a plan: the objects that block (their tiles, inflated;
// not those in `ignore`, nor the parts of their multi-tile objects, nor
// those on stair landings: build mode keeps landings clear, and what the
// lots have there hangs on a wall), and every other Sim in the world as cost
// rectangles (AddBlockerToPathPlanner).
void addObstacles(VM& vm, PathPlanner& planner, const Object& me, std::span<const ObjectId> ignore) {
    planner.clearObstacles();
    std::set<ObjectId> skip;
    for (ObjectId id : ignore)
        if (const Object* o = vm.object(id))
            skip.insert(multiTileGroup(*o));
    const World& w = planner.world();
    for (const auto& [id, o] : vm.objects()) {
        if (id == me.id || !o.position.inWorld() || o.data[objdata::ContainerId] != 0)
            continue;
        if (o.person) {
            planner.addRouter(o.position.level, o.position.x, o.position.y, o.data[objdata::Direction],
                              o.route && !o.route->waiting, smallSim(o));
            continue;
        }
        if (!VM::blocksRouting(o) || skip.count(multiTileGroup(o)))
            continue;
        // An object whose "allow intersection" tree (OBJf 5) lets the walker
        // through: the toilet's privacy marker ("Social - 1 Tile Marker",
        // put in front of the toilet for its user) was a wall to the user's
        // own sitting slot (inferred for the planner; the binary's
        // TSObjectCanSetPosition asks the tree for the goals).
        if (me.id && intersectionAllowed(vm, o, me))
            continue;
        const TilePos t = VM::tileOf(o);
        const bool landing = std::any_of(w.stairs().begin(), w.stairs().end(),
                                         [&](const Stair& s) { return s.bottom == t || s.top == t; });
        if (!landing)
            planner.addObstacle(t.level, float(t.x), float(t.y), float(t.x + 1), float(t.y + 1), PathPlanner::kObject);
    }
}

// The stair animation `name` ("stairs-up-loop-L") for the walker's age, and
// how many ticks it lasts (10 when it does not exist).
std::pair<std::string, int> stairAnimation(VM& vm, const Object& me, const std::string& name) {
    if (!vm.animations.index())
        vm.animations.setIndex(vm.code().index());
    const std::string age = agePrefix(me);
    for (const std::string& candidate : {age + "-" + name, "a-" + name})
        if (const auto secs = vm.animations.seconds(candidate))
            return {candidate, std::max(1, int(std::ceil(*secs * float(vm.clock.ticksPerSecond))))};
    return {age + "-" + name, 10};
}

// Appends the walk onto a stair, its climb phases and the walk off it.
// Where each phase puts the walker (its animation's anchor) is read off the
// adult animations' root bone, which chain from one to the next: going up,
// "stairs-up-start-R" stands on the bottom landing with the anchor one step
// up, then "stairs-up-loop-L"/"-R" in turn each climb one step (anchor a
// quarter tile on and 3/16 up), steps - 1 of them, and "stairs-up-stop-R"
// steps onto the top landing (anchor a quarter tile past the top, on its
// floor). Going down, "stairs-down-start-R" starts on the top landing
// (anchor a quarter tile down the stair, on the top floor), steps - 2 loops
// go one step down each and "stairs-down-stop-R" takes the last two steps
// (anchor a quarter tile onto the bottom landing). Inferred: the game snaps
// the Sim into a containment slot of each step (a bone of its model), and
// picks the animations by mood ("-happy", "-angry", ...) in the stair's
// trees; the neutral ones are used here.
void appendClimb(VM& vm, const Object& me, const Stair& st, bool up, std::vector<Object::Route::Point>& out) {
    const float rise = (st.topZ - st.bottomZ) / float(std::max(st.steps, 1));
    const float len = st.length();
    auto walkTo = [&](float u, int level) {
        const auto p = st.at(u);
        out.push_back(walkPoint(p[0], p[1], level));
    };
    auto phase = [&](float u, float z, int level, const std::string& name, int direction) {
        const auto p = st.at(u);
        const auto [anim, ticks] = stairAnimation(vm, me, name);
        out.push_back({p[0], p[1], level, ticks, z, direction, anim});
    };
    const int lower = st.bottom.level, upper = st.top.level;
    if (up) {
        walkTo(-0.1f, lower);
        phase(0.125f, st.bottomZ + rise, lower, "stairs-up-start-R", st.direction);
        for (int j = 1; j < st.steps; ++j)
            phase(0.125f + 0.25f * float(j), st.bottomZ + rise * float(1 + j), lower,
                  j % 2 ? "stairs-up-loop-L" : "stairs-up-loop-R", st.direction);
        phase(len + 0.25f, st.topZ, upper, "stairs-up-stop-R", st.direction);
        walkTo(len + 0.12f, upper);
    } else {
        const int down = (st.direction + 4) % 8;
        walkTo(len + 0.18f, upper);
        phase(len - 0.25f, st.topZ, upper, "stairs-down-start-R", down);
        for (int j = 1; j + 1 < st.steps; ++j)
            phase(len - 0.25f - 0.25f * float(j), st.topZ - rise * float(j), lower,
                  j % 2 ? "stairs-down-loop-L" : "stairs-down-loop-R", down);
        phase(-0.25f, st.bottomZ, lower, "stairs-down-stop-R", down);
        walkTo(-0.27f, lower);
    }
}

// The door object at a door of the world (0 if the VM has none there: the
// saved door states are not always where their cObjects are).
ObjectId doorObject(VM& vm, const World::Door& d) {
    for (const auto& [id, o] : vm.objects())
        if (o.definition && o.definition->objectType() == kObjectTypeDoor && o.position.inWorld() &&
            VM::tileOf(o) == d.tile)
            return id;
    return 0;
}

std::vector<const Slots::Slot*> routingSlots(const Slots* table);
const Slots* slotTable(VM& vm, const Object& o);

// The steps of a modular stair in the VM: the top one ("... - Portal - A",
// which has the portal function, OBJf entry 15) and the bottom one ("...
// - End"), found from the top by following object data 80 ("Get - Next
// (down)" of StairModularGlobals) to the step with none (-1). The top step
// is the one with the portal function nearest to where the stair meets its
// top landing.
std::pair<ObjectId, ObjectId> stairSteps(VM& vm, const Stair& st) {
    static const uint32_t stairGlobals = semiGlobalGroup("StairModularGlobals");
    const auto top = st.at(st.length() - 0.125f);
    ObjectId best = 0;
    float bestD = 0.6f;
    for (const auto& [id, o] : vm.objects()) {
        if (o.semiGroup != stairGlobals || !o.position.inWorld() || o.position.level != st.top.level ||
            !vm.functionTree(o, 15))
            continue;
        const float d = std::hypot(o.position.x - top[0], o.position.y - top[1]);
        if (d < bestD)
            best = id, bestD = d;
    }
    if (!best)
        return {0, 0};
    ObjectId bottom = best;
    for (int i = 0; i < 64; ++i) {
        const Object* o = vm.object(bottom);
        const ObjectId next = o ? o->data[80] : 0;
        if (next <= 0 || !vm.object(next))
            break;
        bottom = next;
    }
    return {best, bottom == best ? 0 : bottom};
}

// The pads of a straight staircase ("Stairs - Straight - ... - Lo 0,5 Pad" on
// its bottom landing and "... - Hi 0,0 Pad" on its top one), which carry its
// portal function (OBJf entry 15, StairsStraightGlobals' "Portal"): the
// top and bottom ones, or none.
std::pair<ObjectId, ObjectId> straightStairPads(VM& vm, const Stair& st) {
    static const uint32_t globals = semiGlobalGroup("StairsStraightGlobals");
    ObjectId top = 0, bottom = 0;
    for (const auto& [id, o] : vm.objects()) {
        if (o.semiGroup != globals || !o.position.inWorld() || !vm.functionTree(o, 15))
            continue;
        const TilePos t = VM::tileOf(o);
        if (t == st.bottom)
            bottom = id;
        else if (t == st.top)
            top = id;
    }
    return {top, bottom};
}

// The portal tree of a portal crossed one way, as the binary's
// TriggerPortalTree / TSObjectPortalLockedCB set it up (docs/routing-mac.md
// §8): the objects at the entry and the exit end (a door at both; the
// bottom and top steps of a stair), the object whose OBJf entry 15 runs
// (the entry's if it has one, else the exit's) and its guard (the TEST
// tree) and action, and the parameters: 0/1 the containment slots of the
// two portal slots (0 here: not decoded, no portal tree reads them), 2/3
// the rooms of the two objects, 4/5 their IDs, 6/7 the routing slots of
// the two ends (the door's "Function - Portal" picks its side and the
// opening animation from parameter 6), 8 the gait (1 walking, 2 running)
// and 9 a constant of the binary (0 here). The stack object is the entry
// object. Inferred: which routing slot is a stair step's portal slot
// (the first).
struct PortalUse {
    ObjectId entry = 0, exit = 0, owner = 0;
    uint16_t guard = 0, action = 0;
    std::array<int16_t, 10> params{};
};

std::optional<PortalUse> portalUse(VM& vm, const PathPlanner& planner, size_t index, bool forward) {
    const PathPlanner::Portal& p = planner.portals()[index];
    PortalUse u;
    int slotA = 0, slotB = 0;
    if (p.door >= 0) {
        const ObjectId door = doorObject(vm, planner.world().doors()[size_t(p.door)]);
        if (!door)
            return std::nullopt;
        u.entry = u.exit = door;
        // The door's routing slot on each side: the one whose place is
        // nearest to that end of the portal.
        const Object& d = *vm.object(door);
        const auto slots = routingSlots(slotTable(vm, d));
        float da = 1e9f, db = 1e9f;
        for (size_t k = 0; k < slots.size(); ++k) {
            const auto g = vm.routingGoals(*slots[k], d);
            if (g.points.empty())
                continue;
            const float ka = std::hypot(g.points[0][0] - p.a[0], g.points[0][1] - p.a[1]);
            const float kb = std::hypot(g.points[0][0] - p.b[0], g.points[0][1] - p.b[1]);
            if (ka < da)
                da = ka, slotA = int(k);
            if (kb < db)
                db = kb, slotB = int(k);
        }
    } else if (p.stair >= 0) {
        const Stair& st = planner.world().stairs()[size_t(p.stair)];
        auto [top, bottom] = stairSteps(vm, st);
        if (!top || !bottom)
            std::tie(top, bottom) = straightStairPads(vm, st);
        if (!top || !bottom)
            return std::nullopt;
        u.entry = bottom, u.exit = top; // end a is the bottom landing
    } else {
        return std::nullopt;
    }
    if (!forward) {
        std::swap(u.entry, u.exit);
        std::swap(slotA, slotB);
    }
    for (const ObjectId id : {u.entry, u.exit}) {
        const Object& o = *vm.object(id);
        const ObjectFunctions* f = vm.code().functions(o.group, o.definitionInstance);
        if (f && f->functions.size() > 15 && f->functions[15].action) {
            u.owner = id;
            u.guard = f->functions[15].guard;
            u.action = f->functions[15].action;
            break;
        }
    }
    if (!u.owner)
        return std::nullopt;
    const Object &a = *vm.object(u.entry), &b = *vm.object(u.exit);
    u.params = {0, 0, a.data[objdata::Room], b.data[objdata::Room], int16_t(u.entry), int16_t(u.exit),
                int16_t(slotA), int16_t(slotB), 1, 0};
    return u;
}

// The TEST tree of a portal (its OBJf entry 15's guard) for the walker:
// false locks the portal that way. Run as a check tree on a thread of its
// own (TSObjectPortalLockedCB runs it as a test on the Sim). No guard, or a
// guard that cannot run here (a VM error): open (inferred; the binary has
// no such case).
bool portalLockedFor(VM& vm, const Object& me, const PathPlanner& planner, size_t index, bool forward) {
    if (!vm.portalTrees || !me.person)
        return false;
    const auto u = portalUse(vm, planner, index, forward);
    if (!u || !u->guard)
        return false;
    Thread t = vm.start(me.id, u->guard, {}, u->entry, u->owner);
    if (t.status != Thread::Status::Running)
        return false;
    auto& f = t.stack.back();
    f.params.resize(std::max(f.params.size(), u->params.size()));
    std::copy(u->params.begin(), u->params.end(), f.params.begin());
    for (int turn = 0; t.status == Thread::Status::Running && turn < 4; ++turn)
        vm.run(t, vm.instructionsPerTurn);
    return t.status == Thread::Status::Finished && !t.result;
}

// Ticks a walker waits at a door's portal for the door to open: the door's
// open animation (approximation: the game runs the door's portal tree,
// OBJf entry 15, which plays it and the Sim's walk through; 15 ticks when
// the animation is not known).
int doorWaitTicks(VM& vm) {
    if (const auto secs = vm.animations.seconds("o-door-open-CW"))
        return std::clamp(int(std::ceil(*secs * float(vm.clock.ticksPerSecond))), 1, 45);
    return 15;
}

// Plans the route's points from where the walker stands to its goals
// (r.goals on r.level), around the objects and the other Sims.
// cLocomotable::ComputeTurn (BV 0x6ae2e0, docs/routing-mac.md §6.1): the
// angle from heading `from` to `to`, in [-pi, pi], as 45-degree steps: while
// under -40 degrees one step less and 45 more, while over 40 one more and 45
// less; up to 40 degrees no turn (the walk's curve takes it).
int turnSteps(float from, float to) {
    float a = std::remainder(to - from, float(2 * M_PI));
    int steps = 0;
    const float limit = float(40 * M_PI / 180), step = float(M_PI / 4);
    while (a < -limit)
        --steps, a += step;
    while (a > limit)
        ++steps, a -= step;
    return std::clamp(steps, -4, 4);
}

// A turn in place of `steps` 45-degree steps where the walker stands: the
// set's turn in place clip ("<age>-TurnInPlace-<45|90|135|180>", the adult
// one if the age has none), as long as it lasts. The clips turn clockwise
// (a-TurnInPlace-90: header turn -1.5708) and the set plays the one whose
// turn is the opposite mirrored (cLocoSet vtable 0x30, BV 0xd0c7ec: a
// counter-clockwise turn). The walker keeps its direction until it walks on
// (the animation's root turns the body).
std::optional<Object::Route::Point> turnPoint(VM& vm, const Object& me, int steps) {
    if (steps == 0 || !me.person)
        return std::nullopt;
    if (!vm.animations.index())
        vm.animations.setIndex(vm.code().index());
    std::vector<std::pair<std::string, float>> candidates; // name, playback speed
    if (auto pick = locoClip(vm, me, "turnInPlace", steps))
        candidates.push_back({pick->animation, pick->speed});
    const std::string name = "TurnInPlace-" + std::to_string(45 * std::abs(steps));
    candidates.push_back({agePrefix(me) + "-" + name, 1.0f});
    candidates.push_back({"a-" + name, 1.0f});
    for (const auto& [candidate, speed] : candidates)
        if (const auto secs = vm.animations.seconds(candidate)) {
            Object::Route::Point p = walkPoint(me.position.x, me.position.y, me.position.level);
            p.ticks = std::max(1, int(std::ceil(*secs / speed * float(vm.clock.ticksPerSecond))));
            p.animation = candidate;
            p.direction = me.data[objdata::Direction];
            p.turnSteps = steps;
            p.mirror = steps > 0;
            return p;
        }
    return std::nullopt;
}

// Where the walker faces now (radians, 0 = +Y): its frame's way while it
// walks, else its direction.
float facingOf(const Object& me) {
    if (me.route && me.route->loco.kind == Object::Route::Loco::Walk)
        return me.route->loco.heading;
    return headingOfDirection(me.data[objdata::Direction]);
}

// What BeginFollow makes of a planned way (BV 0x6b5770, §6.1): the way out
// first, a point min(d / 3, 0.25) out from where the walker stands (d the
// distance to the first point) along where it faces turned by the 45-degree
// steps ComputeTurn gives the first leg, so that the start clip that turns
// by those steps leaves straight along it; then each leg on the floor cut
// every 0.2 tile (floor(length / 0.2) points between its ends). Without
// start clips (a set that has none) a standing walker more than 40 degrees
// off its first leg turns in place first.
void shapeWalk(VM& vm, Object& me, Object::Route& r, bool moving) {
    std::vector<Object::Route::Point> out;
    Point2 at{me.position.x, me.position.y};
    int level = me.position.level;
    const bool floorFirst = !r.points.empty() && r.points.front().ticks == 0 && !r.points.front().portal &&
                            !r.points.front().viaPortal && r.points.front().level == level;
    int steps = 0;
    if (floorFirst) {
        const auto& first = r.points.front();
        const float dx = first.x - at[0], dy = first.y - at[1], d = std::hypot(dx, dy);
        if (d > 1e-4f) {
            const float facing = facingOf(me);
            steps = turnSteps(facing, headingOfMotion(dx, dy));
            const float lead = std::min(d * 0.3333f, 0.25f), h = facing + float(steps) * float(M_PI / 4);
            const Point2 q{at[0] - std::sin(h) * lead, at[1] + std::cos(h) * lead};
            // Inferred: not into another Sim's way (the game's curve keeps
            // off the Sims its planner went around; a walker planning again
            // in front of the one that blocked it would walk into it).
            bool clear = plannerOf(vm).free(level, q);
            for (const auto& [id, o] : vm.objects())
                if (clear && id != me.id && o.person && o.position.inWorld() && o.position.level == level &&
                    o.data[objdata::ContainerId] == 0 &&
                    std::hypot(o.position.x - q[0], o.position.y - q[1]) <
                        (smallSim(me) ? 0.1875f : 0.25f) + (smallSim(o) ? 0.1875f : 0.25f) + kStepTiles * kHorizonSteps)
                    clear = false;
            if (clear) {
                out.push_back(walkPoint(q[0], q[1], level));
                at = q;
            }
        }
    }
    for (const auto& p : r.points) {
        const bool plain = p.ticks == 0 && !p.portal && !p.viaPortal && p.level == level && !p.door;
        if (plain) {
            const float len = std::hypot(p.x - at[0], p.y - at[1]);
            const int n = int(std::floor(len / 0.2f));
            for (int k = 1; k < n; ++k) {
                const float u = float(k) / float(n);
                out.push_back(walkPoint(at[0] + (p.x - at[0]) * u, at[1] + (p.y - at[1]) * u, level));
            }
        }
        out.push_back(p);
        at = {p.x, p.y};
        level = p.level;
    }
    r.points = std::move(out);
    if (!floorFirst || moving || steps == 0 || locoClip(vm, me, "turnAndGo", steps))
        return;
    if (auto t = turnPoint(vm, me, steps))
        r.points.insert(r.points.begin(), *t);
}

bool planRoute(VM& vm, Object& me, Object::Route& r, RouteFail& why) {
    PathPlanner& planner = plannerOf(vm);
    addObstacles(vm, planner, me, r.ignore);
    // The goals go to the planner by tiers, as TSCreateGotoRelativeSlot
    // hands them over (BV 0x472e00, from FindPathWithPlanner): first the
    // untried ones within 1 of the best cost (the binary: score >= best -
    // 1), then, if none of them can be reached, the next tier the same way,
    // then all that are left; within a tier the shortest way wins (the
    // goals' costs are not added to the walk).
    PlannedPath path;
    planner.portalLocked = [&](size_t portal, bool forward) { return portalLockedFor(vm, me, planner, portal, forward); };
    std::vector<bool> tried(r.goals.size(), false);
    bool planned = false;
    for (int attempt = 0; !planned; ++attempt) {
        float best = std::numeric_limits<float>::infinity();
        for (size_t i = 0; i < r.goals.size(); ++i)
            if (!tried[i])
                best = std::min(best, r.goals[i][2]);
        if (!std::isfinite(best))
            break;
        std::vector<PlanGoal> goals;
        std::vector<size_t> index;
        for (size_t i = 0; i < r.goals.size(); ++i)
            if (!tried[i] && (attempt >= 2 || r.goals[i][2] <= best + 1)) {
                goals.push_back({r.goals[i][0], r.goals[i][1], 0});
                index.push_back(i);
                tried[i] = true;
            }
        planned = planner.plan({me.position.x, me.position.y}, me.position.level, r.level, goals, path, why);
        if (planned)
            path.goal = index[path.goal];
    }
    if (!planned && std::getenv("OS2_ROUTE_FAILS_NEAR")) // the portals locked for the walker
        for (size_t i = 0; i < planner.portals().size(); ++i)
            for (const bool forward : {true, false})
                if (planner.portalLocked(i, forward))
                    std::fprintf(stderr, "  portal %zu (%s) locked %s for %d\n", i,
                                 planner.portals()[i].door >= 0 ? "door" : "stair", forward ? "a->b" : "b->a", me.id);
    if (!planned && std::getenv("OS2_ROUTE_FAILS_MAP")) { // the planner's cells around the walker, half a tile each
        const int lv = me.position.level;
        std::fprintf(stderr, "  map of %d at (%.2f, %.2f, %d), goals on level %d:", me.id, me.position.x, me.position.y, lv, r.level);
        for (const auto& g : r.goals)
            std::fprintf(stderr, " (%.2f,%.2f)", g[0], g[1]);
        std::fprintf(stderr, "\n");
        for (int qy = int(me.position.y * 2) + 12; qy >= int(me.position.y * 2) - 12; --qy) {
            std::fprintf(stderr, "  %5.1f ", qy / 2.0);
            for (int qx = int(me.position.x * 2) - 16; qx <= int(me.position.x * 2) + 16; ++qx) {
                const Point2 q{(qx + 0.5f) / 2, (qy + 0.5f) / 2};
                const uint8_t b = planner.at(lv, q);
                char ch = b & PathPlanner::kWall ? '#' : b & PathPlanner::kDoor ? 'D' : b & PathPlanner::kNoFloor ? ' ' : b & PathPlanner::kObject ? 'o' : b ? '?' : planner.costAt(lv, q) > 1.5f ? '+' : '.';
                if (int(std::floor(me.position.x * 2)) == qx && int(std::floor(me.position.y * 2)) == qy)
                    ch = 'S';
                std::fputc(ch, stderr);
            }
            std::fputc('\n', stderr);
        }
    }
    planner.portalLocked = nullptr;
    if (!planned)
        return false;
    r.points.clear();
    int level = me.position.level;
    Point2 prev{me.position.x, me.position.y};
    for (const auto& wp : path.points) {
        Object::Route::Point p = walkPoint(wp.x, wp.y, wp.level);
        if (wp.portal >= 0) {
            const PathPlanner::Portal& portal = planner.portals()[size_t(wp.portal)];
            // Which way: from end a when the walker is there.
            const bool forward = std::hypot(prev[0] - portal.a[0], prev[1] - portal.a[1]) <=
                                     std::hypot(prev[0] - portal.b[0], prev[1] - portal.b[1]) &&
                                 level == portal.levelA;
            const auto use = vm.portalTrees && me.person ? portalUse(vm, planner, size_t(wp.portal), forward)
                                                         : std::nullopt;
            const size_t engineFrom = r.points.size();
            if (use) {
                Object::Route::Point at = walkPoint(prev[0], prev[1], level);
                at.direction = directionOf(wp.x - prev[0], wp.y - prev[1]);
                at.portal = use->entry, at.portalOwner = use->owner, at.portalParams = use->params;
                at.door = portal.door >= 0 ? use->entry : 0;
                r.points.push_back(at);
            }
            if (portal.stair >= 0) {
                appendClimb(vm, me, planner.world().stairs()[size_t(portal.stair)], wp.level > level, r.points);
                if (use) // the tree climbs: the engine's phases stay as the way if it cannot
                    for (size_t k = engineFrom + 1; k < r.points.size(); ++k)
                        r.points[k].viaPortal = true;
            } else if (use) {
                p.door = use->entry;
            } else if (portal.door >= 0) {
                const ObjectId door = doorObject(vm, planner.world().doors()[size_t(portal.door)]);
                Object::Route::Point wait = walkPoint(prev[0], prev[1], level);
                wait.ticks = doorWaitTicks(vm);
                wait.direction = directionOf(wp.x - prev[0], wp.y - prev[1]);
                wait.door = door;
                r.points.push_back(wait);
                p.door = door;
            }
        }
        r.points.push_back(p);
        level = wp.level;
        prev = {wp.x, wp.y};
    }
    r.facing = r.facings.empty() ? -1 : r.facings[path.goal];
    // Person data 38 "route multi slot index": which of a bone slot's places
    // the way goes to (the goal's +0x14 in the binary; "Sofa - Animate - Get
    // In" reads it as the seat, the dining chairs as the side to sit from).
    if (me.person && path.goal < r.multi.size())
        me.person->data[38] = r.multi[path.goal];
    shapeWalk(vm, me, r, me.route.has_value() && !me.route->waiting && me.route->climb.ticksLeft == 0);
    return true;
}

// Where a walker will be `tiles` further along its route points (on its
// level; stops at a phase or the end).
Point2 alongRoute(const Object& o, float tiles) {
    Point2 at{o.position.x, o.position.y};
    if (!o.route)
        return at;
    for (const auto& p : o.route->points) {
        if (p.ticks > 0 || p.portal || p.level != o.position.level)
            break;
        const float d = std::hypot(p.x - at[0], p.y - at[1]);
        if (d >= tiles) {
            if (d > 0)
                at = {at[0] + (p.x - at[0]) * tiles / d, at[1] + (p.y - at[1]) * tiles / d};
            return at;
        }
        at = {p.x, p.y};
        tiles -= d;
    }
    return at;
}

// Another Sim the walker is predicted to run into (CollisionCheck), or 0.
ObjectId simAhead(VM& vm, const Object& me, const Object::Route& r) {
    if (r.points.empty() || r.points.front().ticks > 0 || r.points.front().portal)
        return 0;
    const float mySpeed = std::max(walkTilesPerTick(vm, me), 1e-4f);
    const float myRadius = smallSim(me) ? 0.1875f : 0.25f;
    for (const auto& [id, o] : vm.objects()) {
        if (id == me.id || !o.person || !o.position.inWorld() || o.data[objdata::ContainerId] != 0 ||
            o.position.level != me.position.level)
            continue;
        // Overlapping already (both on one spot): walking off is the way out.
        if (std::hypot(o.position.x - me.position.x, o.position.y - me.position.y) < 0.3f)
            continue;
        // The Sim walked to (a social's partner) is met, not run into (inferred).
        if (id == r.target)
            continue;
        const bool walking = o.route && !o.route->waiting;
        const float ratio = walking ? walkTilesPerTick(vm, o) / mySpeed : 0;
        const float reach = myRadius + (smallSim(o) ? 0.1875f : 0.25f);
        for (int k = 0; k < kHorizonSteps; ++k) {
            const float t0 = kStepTiles * float(k), t1 = kStepTiles * float(k + 1);
            const Point2 a0 = alongRoute(me, t0), a1 = alongRoute(me, t1);
            const Point2 b0 = walking ? alongRoute(o, t0 * ratio) : Point2{o.position.x, o.position.y};
            const Point2 b1 = walking ? alongRoute(o, t1 * ratio) : b0;
            const float d0 = std::hypot(a0[0] - b0[0], a0[1] - b0[1]);
            const float d1 = std::hypot(a1[0] - b1[0], a1[1] - b1[1]);
            if (std::min(d0, d1) >= reach)
                continue;
            if (d1 > d0)
                continue; // moving apart: the scan goes on
            if (t0 <= kBlockedWithin)
                return id;
            break;
        }
    }
    return 0;
}

// MoveOutOfWay (docs/routing-mac.md §8.1): a destination object ("Destination
// - For Go Here & Shoo", GUID 0x7C4) is created on the blocker, with a
// routing slot 0 to where it should go, and its "Interaction - Move Out Of
// Way" (the TTAB entry whose action is 0x100B: Go To Routing Slot 0 of the
// destination) is queued on the blocker with the asker's priority.
constexpr uint32_t kDestinationGuid = 0x000007C4;
constexpr uint16_t kMoveOutOfWayTree = 0x100B;

// Whether the Sim already has a "move out of the way" queued (IsAskedToMove).
bool askedToMove(VM& vm, const Object& sim) {
    for (const auto& q : sim.queue)
        if (const Object* o = vm.object(q.object); o && o->definition && o->definition->guid() == kDestinationGuid &&
                                                   q.action == kMoveOutOfWayTree)
            return true;
    return false;
}

// Where a blocker steps aside to (CreateMoveOutOfWayDestination, type 0,
// BV 0x57dcac; docs/routing-mac.md §8.3): the synthetic slot looks from the
// blocker 45 degrees off the asker's direction d: d+7 when the blocker is
// nearer the point a tenth of a tile to the asker's left (d+6), else d+1;
// its row is 0.3 tile out (minimum and maximum distance 0.3) on a grid of
// 1/16 tile, as wide as wanted (width 100), the nearest the middle first.
// Here: the first point of that row, outwards from its middle up to 6
// tiles either way, that is free, reachable and clear of the asker's way
// (0.75 tile of its line from it to 2 tiles past the blocker; inferred, the
// binary leaves that to the slot's costs).
std::optional<Point2> moveOutOfWayPoint(VM& vm, const Object& me, const Object& blocker, int type) {
    PathPlanner& planner = plannerOf(vm);
    const std::vector<ObjectId> ignore{me.id, blocker.id};
    addObstacles(vm, planner, blocker, ignore);
    auto unit = [](int d) {
        const auto [sx, sy] = directionStep(((d % 8) + 8) % 8);
        const float n = std::hypot(float(sx), float(sy));
        return Point2{float(sx) / n, float(sy) / n};
    };
    const int d = ((me.data[objdata::Direction] % 8) + 8) % 8;
    const Point2 l = unit(d + 6), rgt = unit(d + 2);
    const Point2 pl{me.position.x + 0.1f * l[0], me.position.y + 0.1f * l[1]};
    const Point2 pr{me.position.x + 0.1f * rgt[0], me.position.y + 0.1f * rgt[1]};
    const bool nearerLeft = std::hypot(blocker.position.x - pl[0], blocker.position.y - pl[1]) <=
                            std::hypot(blocker.position.x - pr[0], blocker.position.y - pr[1]);
    const int dir = nearerLeft ? d + 7 : d + 1;
    const Point2 f = unit(dir), side = unit(dir + 2);
    const float lx = blocker.position.x - me.position.x, ly = blocker.position.y - me.position.y;
    const float ll = std::max(std::hypot(lx, ly), 1e-3f);
    const Point2 a{me.position.x, me.position.y}, b{blocker.position.x + lx / ll * 2, blocker.position.y + ly / ll * 2};
    auto nearWay = [&](Point2 q) {
        const float vx = b[0] - a[0], vy = b[1] - a[1], len2 = vx * vx + vy * vy;
        const float u = len2 > 0 ? std::clamp(((q[0] - a[0]) * vx + (q[1] - a[1]) * vy) / len2, 0.0f, 1.0f) : 0;
        return std::hypot(a[0] + vx * u - q[0], a[1] + vy * u - q[1]) < 0.75f;
    };
    for (int j = 0; type == 0 && j <= 96; ++j)
        for (const int sign : {1, -1}) {
            if (j == 0 && sign == -1)
                continue;
            const float lat = float(sign * j) * 0.0625f;
            const Point2 q{blocker.position.x + f[0] * 0.3f + side[0] * lat, blocker.position.y + f[1] * 0.3f + side[1] * lat};
            if (!planner.free(blocker.position.level, q) || nearWay(q))
                continue;
            PlannedPath path;
            RouteFail why;
            if (planner.plan({blocker.position.x, blocker.position.y}, blocker.position.level, blocker.position.level,
                             {{q[0], q[1], 0}}, path, why))
                return q;
        }
    // Type 1 (the blocker stands on my goal; §8.3: directions 1, 2, 6 and 7,
    // 1 to 3 tiles out, a step of 1 tile), and the fallback of type 0 when
    // its row has no place (inferred, as the wider slot of type 2: up to 6
    // tiles). The directions are taken relative to the asker's (inferred:
    // the slot's object is not decoded), so the blocker steps aside off the
    // asker's way.
    const int facing = type == 1 ? d : ((blocker.data[objdata::Direction] % 8) + 8) % 8;
    for (int dist = 1; dist <= (type == 1 ? 3 : 6); ++dist)
        for (const int rel : {2, 6, 1, 7}) {
            const Point2 u = unit(facing + rel);
            const Point2 q{blocker.position.x + u[0] * float(dist), blocker.position.y + u[1] * float(dist)};
            if (!planner.free(blocker.position.level, q) || nearWay(q))
                continue;
            PlannedPath path;
            RouteFail why;
            if (planner.plan({blocker.position.x, blocker.position.y}, blocker.position.level, blocker.position.level,
                             {{q[0], q[1], 0}}, path, why))
                return q;
        }
    return std::nullopt;
}

// The destinations the engine made ("move out of the way", "go here") go
// once no Sim has their interaction queued or running any more
// (flushDestinations, go_here.hpp). Once a tick.
void flushMoveOutOfWayDestinations(VM& vm) {
    static std::map<const VM*, uint64_t> lastTick;
    auto& last = lastTick[&vm];
    if (last == vm.ticks + 1)
        return;
    last = vm.ticks + 1;
    flushDestinations(vm);
}

// AskBlockerToMove → the blocker's MoveOutOfWay (docs/routing-mac.md §8.1):
// refused when the blocker is already asked or is blocked by me, unless its
// person data 159 has bit 0x02 its priority (person data 33) must be below
// mine less one (at least 1), my route must have more than one goal, we
// must be within 4 tiles and a place to step aside to must exist. Accepted:
// the move-out-of-way interaction goes on its queue with that priority.
bool askBlockerToMove(VM& vm, Object& me, const Object::Route& r, Object& blocker) {
    // DoBlockedGoalStillAvailable asks with type 1 when the route has a
    // destination object and the blocker is within 1.5 tiles of it (on my
    // goal: the cook at the stove when the other comes for the oven), else
    // type 0 (docs/routing-mac.md §8.3).
    int type = 0;
    if (const Object* goal = vm.object(r.target); goal && goal->id != me.id &&
        std::hypot(blocker.position.x - goal->position.x, blocker.position.y - goal->position.y) < 1.5f)
        type = 1;
    if (!me.person || !blocker.person || askedToMove(vm, blocker))
        return false;
    if (blocker.route && (blocker.route->blocker == me.id || blocker.route->blocker != 0))
        return false;
    const int16_t priority = int16_t(std::max(1, me.person->data[persondata::Priority] - 1));
    if (!(blocker.person->data[159] & 0x02) && blocker.person->data[persondata::Priority] >= priority)
        return false;
    if (r.goals.size() < 2 || blocker.position.level != me.position.level ||
        std::hypot(blocker.position.x - me.position.x, blocker.position.y - me.position.y) > 4)
        return false;
    const auto spot = moveOutOfWayPoint(vm, me, blocker, type);
    if (!spot)
        return false;
    std::vector<ObjectId> existing;
    for (const auto& [id, o] : vm.objects())
        existing.push_back(id);
    Object* dest = vm.instantiateGuid(kDestinationGuid);
    if (!dest)
        return false;
    // The destination stands where the blocker is ("Interaction - Move Out
    // Of Way" walks only when it is on it), and its routing slot 0 is the
    // place to step aside to (the binary attaches a synthetic slot; here a
    // point offset from the destination).
    const ObjectId destId = dest->id;
    auto place = [&](Object& d) {
        moveTo(vm, d, blocker.position.x, blocker.position.y, blocker.position.level);
        d.data[objdata::Direction] = 0; // model +Y = world +X: offset (dy, -dx)
        Slots::Slot slot;
        slot.type = 3;
        slot.facing = -3;
        slot.offset = {(*spot)[1] - blocker.position.y, blocker.position.x - (*spot)[0], 0};
        d.syntheticSlot = slot;
    };
    place(*dest);
    vm.initialize(destId, existing);
    dest = vm.object(destId);
    if (!dest)
        return false;
    place(*dest); // its Init may move it
    const auto table = interactionTable(vm, *dest);
    if (table && table->table)
        for (const auto& e : table->table->interactions)
            if (e.action == kMoveOutOfWayTree)
                if (auto q = makeInteraction(vm, destId, uint16_t(e.stringIndex))) {
                    q->priority = priority;
                    q->pushedBy = me.id;
                    enqueue(vm, blocker, *q);
                    return true;
                }
    vm.removeObject(destId);
    return false;
}

enum class Step { Walking, Arrived, Failed, Call };

// How far the walker still has to go along its route (tiles).
float remainingLength(const Object& me, const Object::Route& r) {
    float length = 0;
    Point2 at{me.position.x, me.position.y};
    for (const auto& p : r.points) {
        length += std::hypot(p.x - at[0], p.y - at[1]);
        at = {p.x, p.y};
    }
    return length;
}

int16_t hundredths(float tiles) {
    return int16_t(std::clamp(tiles * 100.0f, 0.0f, 32767.0f));
}

// Pushes a route callback tree (`tree` of `owner`) on the walker's thread
// with `stackObject` and `params`; the routing instruction runs again when it
// returns.
void callRouteTree(Context& c, ObjectId owner, uint16_t tree, ObjectId stackObject, std::span<const int16_t> params) {
    c.vm.call(c.thread, owner, tree, {});
    Frame& f = c.thread.stack.back();
    f.stackObject = stackObject;
    f.params.resize(std::max(f.params.size(), params.size()));
    std::copy(params.begin(), params.end(), f.params.begin());
    f.rerunCaller = true;
    f.routeCallback = true;
}

// While walking, at each tile crossed (docs/routing-mac.md §6 and §8.3,
// DoWalking BV 0x576acc): the goal check tree (OBJf entry 0x27, "goal
// check" of STR# 0xC9; TriggerGoalCheckTree BV 0x575e34) of the object
// whose code called the Go To, on the walker's thread with that frame's
// stack object and the parameters 0 the routing
// slot (WorkOutSlotIndex), 1 which of the slot's places (the goal's +0x14:
// 0 here), 2 the goal's object (0 for a standing goal) and 3 the distance
// left in hundredths of a tile; its false fails the route with 13 "goal
// not available" (DoWalkingWaitingForGoalCheckTree). Then the walker's own
// along route tree (entry 0x29; TriggerAlongRouteTree BV 0x5766a0; the
// person's "Function - Along Route": idles, looks at things, greets people
// it passes) with itself as the stack object and 0 the target, 1 the
// routing slot, 2 the place, 3/4 the current sub-goal object and its
// index (0, -1), 5 a route field (vtable 0x1D0, 0 here), 6 the distance
// left in hundredths; its false fails the route with 16 "interrupted by
// callback" (DoWaitingForAlongRouteTree).
bool startGoalCheck(Context& c, Object& me, Object::Route& r) {
    // The goal check of the object whose code called the Go To (its frame's
    // code owner; docs/routing-mac.md §8.3), with that frame's stack object
    // (the route's target when it has none).
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!me.person || !owner || !c.vm.object(r.target) || r.goals.empty())
        return false;
    const uint16_t tree = c.vm.functionTree(*owner, 0x27);
    if (!tree)
        return false;
    const ObjectId stack = c.vm.object(c.frame.stackObject) ? c.frame.stackObject : r.target;
    // 1: the goal's +0x14, its multi slot index (person data 38).
    const std::array<int16_t, 4> params{me.person->data[37], me.person->data[38], 0, hundredths(remainingLength(me, r))};
    callRouteTree(c, owner->id, tree, stack, params);
    r.check = 1;
    return true;
}

// The walker's eye attractor, as cTSRoutingObjectAggregate::AlongRouteUpdate
// keeps it (BV 0x56e2b8..): a "Routing - Eye Attractor" (GUID 0xAC3FB057)
// made the first time it is needed and put at the furthest point of the way
// ahead the walker can see, tried every half tile from 2 tiles on (or from
// the end, when nearer), up to the end; its ID is the route's 0x1D0 that the
// along route tree gets as parameter 5 ("CT - Start Routing Look At" looks at
// it). None when no point ahead is in sight (it stays where it was). The
// binary's look-ahead length (route vtable 0x2C4) is taken as the rest of
// the way (inferred).
ObjectId eyeAttractor(VM& vm, Object& me, const Object::Route& r) {
    constexpr uint32_t kEyeAttractorGuid = 0xAC3FB057;
    const float rest = remainingLength(me, r);
    const TilePos from = VM::tileOf(me);
    std::optional<Point2> seen;
    for (float d = std::min(2.0f, rest); d < rest + 0.5f; d += 0.5f) {
        const Point2 p = alongRoute(me, std::min(d, rest));
        const TilePos t{int(std::floor(p[0])), int(std::floor(p[1])), me.position.level};
        if (!vm.world || !vm.world->contains(t) || !vm.world->lineOfSight(from, t))
            break;
        seen = p;
    }
    if (!seen)
        return vm.object(me.eyeAttractor) ? me.eyeAttractor : 0;
    if (!vm.object(me.eyeAttractor)) {
        const auto def = vm.code().definitionByGuid(kEyeAttractorGuid);
        if (!def)
            return 0;
        std::vector<ObjectId> before;
        for (const auto& [id, _] : vm.objects())
            before.push_back(id);
        const ObjectId id = vm.instantiate(def->first, def->second).id;
        me.eyeAttractor = id;
        if (vm.initCreatedObjects)
            vm.initialize(id, before);
        if (!vm.object(id))
            return me.eyeAttractor = 0;
    }
    Object& eye = *vm.object(me.eyeAttractor);
    moveTo(vm, eye, (*seen)[0], (*seen)[1], me.position.level);
    return eye.id;
}

bool startAlongRoute(Context& c, Object& me, Object::Route& r) {
    if (!me.person || !c.vm.object(r.target))
        return false;
    const uint16_t tree = c.vm.functionTree(me, 0x29);
    if (!tree)
        return false;
    const ObjectId eye = eyeAttractor(c.vm, me, r);
    const std::array<int16_t, 7> params{int16_t(r.target), me.person->data[37], me.person->data[38], 0, -1, eye,
                                        hundredths(remainingLength(me, r))};
    callRouteTree(c, me.id, tree, me.id, params);
    r.check = 2;
    return true;
}

// The walk checks at each tile crossed (DoAlongRouteCheckTest); the
// binary also gates the callbacks by a timer (GRS_State +0x20) not decoded
// here.

// Runs a global tree (by name) on the walker's thread in the middle of the
// walk: the instruction runs again when it returns.
bool runWalkTree(Context& c, const std::string& name, std::array<int16_t, 4> args) {
    // Global, or the walker's semi-global ("CT - Route Blocked" is the
    // person semi-global's).
    auto tree = c.vm.code().behaviorByName(kGlobalGroup, name);
    if (!tree && c.me().semiGroup)
        tree = c.vm.code().behaviorByName(c.me().semiGroup, name);
    if (!tree)
        return false;
    c.vm.call(c.thread, c.me().id, *tree, args);
    c.thread.stack.back().rerunCaller = true;
    return true;
}

// Moves a walking object along its route by one tick's worth. A wait or
// climb phase takes whole ticks: the walker is put at its anchor when it
// starts and stays there while it lasts. A Sim in the way blocks the walk:
// the walker waits, asks it to move (its animation), plans again around
// it, and after too many tries fails (docs/routing-mac.md §6).
// A route point the walker walks to over the floor (not a stair or turn
// phase, not a portal).
bool floorPoint(const Object::Route::Point& p) { return p.ticks == 0 && !p.portal; }

constexpr float kAnimTicksPerSecond = 1000; // ANIM ticks are milliseconds

// One tick of the locomotion along the floor (cLocomotable::FollowOneStep, BV
// 0x6b80e8, and the scene graph's loco request; docs/routing-mac.md §6.1):
// the phases BeginFollow lines up, each a clip of the walker's set, with a
// frame moving along the route and the walker put where the clip's root is
// (LocoPlaceFromSceneGraph: its translation, and the facing its rotation's).
//   start  standing: "turnAndGo" turned by the first leg's 45-degree steps
//          (the way out already leaves along them), "turnAndGoShort" when
//          the stretch is shorter than a start and a stop, or "stepShort"
//          when it is at most 0.25 tile (BeginFollow tests role 9,
//          stepShort, for such a way): the body keeps facing and steps
//          that way. Its frame covers the clip's distance (or the
//          stretch) in the clip's length.
//   walk   the gait's cycle at its speed (gaitOf), the frame turning with
//          each leg.
//   stop   at the end of the route, once what is left is a stop's
//          distance: "turnAndStop" (or the short one after a short start)
//          turned from the way the walker comes to its final facing, of the
//          foot the cycle is on ("R", cycle portion 0, in the first or last
//          quarter of the cycle; inferred from the clips' cycle portions);
//          its frame covers what is left in the clip's length.
// The game's frame follows a curve and the clips are blended by angle
// (cActiveAnimLoco::CreateStepCycle); here the nearest 45-degree clip plays
// on the polyline (inferred). A set without these clips (tests, private sets
// without turns) walks at the gait's speed from the first tick to the last.
struct LocoStep {
    Object::Route::Loco loco;
    size_t consumed = 0; // route points reached
    float x = 0, y = 0;  // where the walker (the root) stands
    int level = 0;
    float facing = 0; // the body's heading
};

LocoStep locoStep(VM& vm, const Object& me, const Object::Route& r) {
    using Loco = Object::Route::Loco;
    LocoStep out;
    Loco& L = out.loco;
    L = r.loco;
    const float tps = float(vm.clock.ticksPerSecond);
    // The stretch over the floor from here: its points and length.
    size_t end = 0;
    while (end < r.points.size() && floorPoint(r.points[end]))
        ++end;
    const bool final = end == r.points.size();
    if (L.kind == Loco::None || !L.moving) {
        L.fx = me.position.x, L.fy = me.position.y;
    }
    auto lengthFrom = [&](float x, float y, size_t from) {
        float len = 0;
        for (size_t i = from; i < end; ++i) {
            len += std::hypot(r.points[i].x - x, r.points[i].y - y);
            x = r.points[i].x, y = r.points[i].y;
        }
        return len;
    };
    const float stretch = lengthFrom(L.fx, L.fy, 0);
    const auto& first = r.points.front();
    const float legX = first.x - L.fx, legY = first.y - L.fy;
    const float legHeading = std::hypot(legX, legY) > 1e-5f ? headingOfMotion(legX, legY) : facingOf(me);
    AnimationLibrary& lib = vm.animations;
    auto clipTicks = [&](const LocoClip& c, float speed) {
        return std::max(1, int(std::lround(float(c.ticks) / kAnimTicksPerSecond * tps / speed)));
    };
    auto begin = [&](Loco::Kind kind, const LocoPick& pick, const LocoClip& clip, float distance, float heading) {
        L.kind = kind;
        L.animation = pick.animation;
        L.mirror = pick.mirror;
        L.tick = 0;
        L.ticks = clipTicks(clip, pick.speed);
        L.distance = distance;
        L.heading = heading;
        L.turn = pick.mirror ? -clip.turn : clip.turn;
        // A stop or a step leaves the body turned off the frame (a start's
        // turn ends on the way, the walk going on from its sway).
        L.endTurn = 0;
        if (kind == Loco::Stop || pick.animation.find("tep") != std::string::npos)
            L.endTurn = clip.root.empty() ? L.turn : (pick.mirror ? -clip.root.back()[2] : clip.root.back()[2]);
        ++L.serial;
    };
    // A stop's clip for the way the walker comes and its final facing.
    auto stopClip = [&](float heading) -> std::optional<std::pair<LocoPick, const LocoClip*>> {
        const float facing = r.facing >= 0 ? headingOfDirection(r.facing) : heading;
        const int steps = turnSteps(heading, facing);
        const bool otherFoot = L.cycle < 0.25f || L.cycle >= 0.75f;
        const std::string role = std::string(L.shortWay ? "turnAndStopShort" : "turnAndStop") + (otherFoot ? "R" : "");
        if (auto pick = locoClip(vm, me, role, steps))
            if (const LocoClip* clip = lib.loco(pick->animation, vm.clock.ticksPerSecond); clip && clip->ticks > 0)
                return std::make_pair(*pick, clip);
        return std::nullopt;
    };
    if (L.kind == Loco::None || (!L.moving && L.kind != Loco::Start)) {
        L.kind = Loco::None;
        if (!L.moving && me.person) {
            const float facing = headingOfDirection(me.data[objdata::Direction]);
            const int steps = turnSteps(facing, legHeading);
            const LocoClip* go = nullptr;
            std::optional<LocoPick> pick;
            if (stretch <= 0.25f && final) {
                pick = locoClip(vm, me, "stepShort", turnSteps(facing, legHeading));
            } else {
                pick = locoClip(vm, me, "turnAndGo", steps);
                const LocoClip* full = pick ? lib.loco(pick->animation, vm.clock.ticksPerSecond) : nullptr;
                const auto stop = stopClip(legHeading);
                const float need = (full ? full->distance : 0) + (stop ? stop->second->distance : 0);
                if (final && stretch < need) {
                    if (auto shorter = locoClip(vm, me, "turnAndGoShort", steps)) {
                        pick = shorter;
                        L.shortWay = true;
                    }
                }
            }
            go = pick ? lib.loco(pick->animation, vm.clock.ticksPerSecond) : nullptr;
            if (pick && go && go->ticks > 0) {
                const bool step = pick->animation.find("tep") != std::string::npos;
                begin(Loco::Start, *pick, *go, step ? stretch : std::min(go->distance, stretch), legHeading);
                L.cycleStart = L.cycle = go->cycle;
            }
        }
        L.moving = true;
    }
    if (L.kind == Loco::None || L.kind == Loco::Walk) {
        const Gait g = gaitOf(vm, me);
        if (L.kind == Loco::None || L.animation != g.animation) {
            if (L.kind != Loco::Walk)
                L.cycleStart = L.cycle;
            L.kind = Loco::Walk;
            L.animation = g.animation;
            L.mirror = false;
            L.turn = 0;
            ++L.serial;
        }
        L.speed = (g.speed > 0 ? g.speed : vm.walkTilesPerTick * tps) / tps;
        // The stop, once what is left is a stop's distance.
        if (final) {
            const float left = stretch;
            if (auto stop = stopClip(legHeading); stop && left - L.speed <= stop->second->distance) {
                begin(Loco::Stop, stop->first, *stop->second, left, legHeading);
                L.endX = r.points[end - 1].x, L.endY = r.points[end - 1].y;
            }
        }
    }
    if (L.kind == Loco::Stop && final && end > 0 &&
        (r.points[end - 1].x != L.endX || r.points[end - 1].y != L.endY)) {
        L.kind = Loco::Walk; // planned again: walk on
        L.speed = std::max(gaitOf(vm, me).speed, 0.1f) / tps;
    }
    // The frame's way this tick.
    float step = L.kind == Loco::Walk ? L.speed : L.distance / float(std::max(L.ticks, 1));
    float x = L.fx, y = L.fy;
    size_t i = 0;
    float heading = L.heading;
    while (step > 0 && i < end) {
        const auto& p = r.points[i];
        const float dx = p.x - x, dy = p.y - y, d = std::hypot(dx, dy);
        if (d > 1e-6f)
            heading = headingOfMotion(dx, dy);
        if (d <= step) {
            x = p.x, y = p.y;
            step -= d;
            ++i;
        } else {
            x += dx / d * step, y += dy / d * step;
            step = 0;
        }
    }
    if (L.kind == Loco::Walk) {
        L.heading = heading;
        // The cycle goes on at the gait's speed (one cycle per clip length).
        if (const LocoClip* c = lib.loco(L.animation, vm.clock.ticksPerSecond); c && c->ticks > 0)
            L.cycle = std::fmod(L.cycle + kAnimTicksPerSecond / tps / float(c->ticks), 1.0f);
    }
    L.fx = x, L.fy = y;
    out.consumed = i;
    out.level = i > 0 ? r.points[i - 1].level : me.position.level;
    // The root: the clip's offset from the frame at this tick.
    float rx = 0, ry = 0, yaw = 0;
    if (L.kind == Loco::Start || L.kind == Loco::Stop) {
        ++L.tick;
        if (const LocoClip* c = lib.loco(L.animation, vm.clock.ticksPerSecond); c && !c->root.empty()) {
            const size_t k = std::min(c->root.size() - 1, size_t(std::lround(float(L.tick) / float(L.ticks) *
                                                                               float(c->root.size() - 1))));
            rx = c->root[k][0], ry = c->root[k][1], yaw = c->root[k][2];
            if (L.mirror)
                rx = -rx, yaw = -yaw;
        }
        if (L.tick >= L.ticks) {
            if (L.kind == Loco::Stop || L.endTurn != 0) { // the end of the way (a stop, a step): there
                out.consumed = end;
                L.fx = end > 0 ? r.points[end - 1].x : x;
                L.fy = end > 0 ? r.points[end - 1].y : y;
                out.level = end > 0 ? r.points[end - 1].level : out.level;
                rx = ry = 0;
                yaw = L.endTurn;
                L.kind = Loco::Stop;
            } else {
                // The start's turn is behind it: the walk goes on from the
                // cycle portion the start ends at.
                L.kind = Loco::Walk;
                L.cycle = L.cycleStart;
                L.animation = gaitOf(vm, me).animation;
                L.mirror = false;
                L.turn = 0;
                ++L.serial;
                rx = ry = 0;
                yaw = 0;
            }
        }
    }
    const float h = L.heading;
    out.x = L.fx + rx * std::cos(h) - ry * std::sin(h);
    out.y = L.fy + rx * std::sin(h) + ry * std::cos(h);
    out.facing = h + yaw;
    return out;
}

// The other Sim the walker would stand in at `to` (TSObjectCanSetPosition
// with error 0x0C "intersects object", whose object LocoPlaceFromSceneGraph
// makes the blocker), or 0. Inferred: the Sims as discs of 0.25 tile (0.1875
// for a child or a toddler), as the planner sizes them; a walker already
// standing in the other may walk on (LocoPlace puts it where it is stuck
// when its own place does not pass either).
ObjectId stepBlocker(VM& vm, const Object& me, float x, float y, int level) {
    const float mine = smallSim(me) ? 0.1875f : 0.25f;
    for (const auto& [id, o] : vm.objects()) {
        if (id == me.id || !o.person || !o.position.inWorld() || o.data[objdata::ContainerId] != 0 ||
            o.position.level != level || allowsIntersection(vm, o, me.id))
            continue;
        const float reach = mine + (smallSim(o) ? 0.1875f : 0.25f);
        const float now = std::hypot(o.position.x - me.position.x, o.position.y - me.position.y);
        const float then = std::hypot(o.position.x - x, o.position.y - y);
        if (then < reach && now >= reach)
            return id;
    }
    return 0;
}

Step advance(Context& c, Object& me, RouteFail& why) {
    VM& vm = c.vm;
    auto& r = *me.route;
    // The interaction was cancelled, or something more important waits
    // (cTSPerson::ShouldInterrupt): "interrupted". Inferred: only inside an
    // interaction, as before (a walk of the Main itself goes on).
    if (me.activeInteraction >= 0 && size_t(me.activeInteraction) < me.queue.size() && shouldInterrupt(me)) {
        why = RouteFail::Interrupted;
        return Step::Failed;
    }
    std::optional<bool> treeResult = std::exchange(c.thread.rerunResult, std::nullopt);
    flushMoveOutOfWayDestinations(vm);
    if (r.check) { // back from the goal check or along route tree
        const int which = std::exchange(r.check, 0);
        const bool ok = treeResult.value_or(true);
        treeResult.reset();
        if (!ok) {
            why = which == 1 ? RouteFail::GoalNotAvailable : RouteFail::InterruptedByCallback;
            return Step::Failed;
        }
        if (which == 1 && startAlongRoute(c, me, r))
            return Step::Call;
    }
    if (r.inPortal) {
        // Back from the portal tree (DoWaitingForPortal): the walk goes on
        // from the portal's other end; false fails the route ("interrupted
        // by callback", inferred from DoWaitingForPortal's failure branch).
        r.inPortal = false;
        r.waiting = false;
        r.blockedTicks = 0;
        if (!r.points.empty())
            r.points.erase(r.points.begin());
        if (!treeResult.value_or(false)) {
            why = RouteFail::InterruptedByCallback;
            return Step::Failed;
        }
        while (!r.points.empty() && r.points.front().viaPortal)
            r.points.erase(r.points.begin());
        if (me.data[objdata::ContainerId] != 0)
            leaveSlot(me);
        r.climb = {};
    }
    if (!r.points.empty() && r.points.front().portal) {
        // At a portal (DoStartPortalTree): wait while another Sim uses it
        // (object data 0x3E of the portal: DoWaitingForPortalToBecomeFree,
        // up to 126 ticks, then blocked by an object), else run its tree on
        // my thread (TriggerPortalTree) and wait for it (DoWaitingForPortal).
        const Object::Route::Point& p = r.points.front();
        const float d = std::hypot(p.x - me.position.x, p.y - me.position.y);
        const float budget = walkTilesPerTick(vm, me);
        if (d > budget) {
            setDirection(me, directionOf(p.x - me.position.x, p.y - me.position.y));
            moveTo(vm, me, me.position.x + (p.x - me.position.x) / d * budget,
                   me.position.y + (p.y - me.position.y) / d * budget, p.level);
            r.waiting = false;
            return Step::Walking;
        }
        moveTo(vm, me, p.x, p.y, p.level);
        setDirection(me, p.direction);
        const Object* portal = vm.object(p.portal);
        const Object* owner = vm.object(p.portalOwner);
        const uint16_t tree = owner ? vm.functionTree(*owner, 15) : 0;
        if (!portal || !tree) { // gone: the engine's way
            r.points.erase(r.points.begin());
            return Step::Walking;
        }
        const int16_t user = portal->data[0x3E];
        if (user != 0 && user != me.id && vm.object(user)) {
            r.waiting = true;
            if (++r.blockedTicks > kWaitForPersonTicks) {
                why = RouteFail::BlockedByObject;
                return Step::Failed;
            }
            return Step::Walking;
        }
        if (me.person && me.person->data.size() > 0x44)
            me.person->data[0x44] &= int16_t(~0x40); // TriggerPortalTree clears it first
        vm.call(c.thread, owner->id, tree, {});
        Frame& f = c.thread.stack.back();
        f.stackObject = p.portal;
        f.params.resize(std::max(f.params.size(), p.portalParams.size()));
        std::copy(p.portalParams.begin(), p.portalParams.end(), f.params.begin());
        f.rerunCaller = true;
        f.routeCallback = true;
        r.inPortal = true;
        r.waiting = true;
        return Step::Call;
    }
    if (r.blockedTree) { // back from "CT - Route Blocked"
        r.blockedTree = false;
        why = RouteFail::BlockedByPerson;
        return Step::Failed;
    }
    if (std::exchange(r.pausing, false)) { // DoContinueAfterPause: DoInitial
        treeResult.reset();
        Object::Route next = r;
        RouteFail w;
        if (!planRoute(vm, me, next, w)) {
            why = w;
            return Step::Failed;
        }
        r.points = std::move(next.points);
    }
    // Waiting for the Sim asked to move (DoWaitingForPersonToMove) goes on
    // whether or not it is still in the way, until it is 1.25 tiles off or
    // the time is up.
    const bool waitingForPerson = r.blockedTicks > 0 && r.blocker && vm.object(r.blocker);
    // This tick's step over the floor and the Sim it would walk into (the
    // locomotor's per-step placement test), when the prediction sees none.
    std::optional<LocoStep> pending;
    ObjectId b = waitingForPerson ? r.blocker : simAhead(vm, me, r);
    if (!b && !r.points.empty() && floorPoint(r.points.front()) && me.position.inWorld()) {
        pending = locoStep(vm, me, r);
        b = stepBlocker(vm, me, pending->x, pending->y, pending->level);
    }
    if (b) {
        const Object& blocker = *vm.object(b);
        const float d = std::hypot(blocker.position.x - me.position.x, blocker.position.y - me.position.y);
        r.waiting = true;
        // It stops (the game slows down over 5 ticks) and starts again
        // from standing.
        r.loco.kind = Object::Route::Loco::None;
        r.loco.moving = false;
        // DoBlockedGoalStillAvailable's tries start again when the blocker
        // changes or has moved 0.25 tile since the last one.
        // (Not while waiting for it to move: DoWaitingForPersonToMove
        // does not come here.)
        if (b != r.blocker ||
            (r.blockedTicks == 0 &&
             std::hypot(blocker.position.x - r.blockerX, blocker.position.y - r.blockerY) >= kBlockerMovedTries)) {
            r.blocker = b, r.replans = 0, r.blockedTicks = 0;
            r.blockerX = blocker.position.x, r.blockerY = blocker.position.y;
        }
        if (r.blockedTicks == 0) {
            // DoBlocked -> DoBlockedGoalStillAvailable (BV 0x57a8d4,
            // docs/routing-mac.md §6): the try counter (1 for a new blocker
            // or one that moved 0.25 tile), then by what the blocker does.
            const int counter = ++r.replans;
            if (std::getenv("OS2_DEBUG_BLOCK"))
                std::fprintf(stderr, "t%llu %d blocked by %d (%s, its blocker %d) counter %d at (%.2f,%.2f) it (%.2f,%.2f)\n",
                             (unsigned long long)vm.ticks, me.id, b, blocker.route ? "routing" : "standing",
                             blocker.route ? blocker.route->blocker : 0, counter, me.position.x, me.position.y,
                             blocker.position.x, blocker.position.y);
            const bool base = c.thread.stack.size() == r.depth; // the route stack holds only this route
            const TilePos blockerTile = VM::tileOf(blocker);
            const bool offRecordedTile = int(std::floor(r.blockerX)) != blockerTile.x ||
                                         int(std::floor(r.blockerY)) != blockerTile.y;
            // Out of tries: "CT - Route Blocked" on the base route, then
            // "blocked by person" (what the tree's result changes is not
            // decoded; the game's returns true).
            auto routeBlocked = [&]() {
                if (base && runWalkTree(c, "CT - Route Blocked", {b, 0, 0, 0})) {
                    r.blockedTree = true;
                    return Step::Call;
                }
                why = RouteFail::BlockedByPerson;
                return Step::Failed;
            };
            // DoInitial again: plan around it and walk on.
            auto replan = [&]() {
                Object::Route next = r;
                RouteFail w;
                if (!planRoute(vm, me, next, w)) {
                    why = w;
                    return Step::Failed;
                }
                r.points = std::move(next.points);
                return Step::Walking;
            };
            // DoPause: "CT - Route Blocked Animation" (the blocker, the
            // pauses before), then DoContinueAfterPause plans again.
            auto pause = [&]() {
                r.pausing = true;
                if (runWalkTree(c, "CT - Route Blocked Animation", {b, int16_t(r.pauses++), 0, 0}))
                    return Step::Call;
                r.pausing = false;
                return replan();
            };
            // TryGetSelfOutOfWay (BV 0x57e9ac): both in the world and at most
            // 4 tiles apart, a move-out-of-way place for me away from the
            // blocker's way (CreateMoveOutOfWayDestination type 0), walked as
            // a route of its own before this one goes on.
            auto selfOutOfWay = [&]() {
                if (!me.position.inWorld() || !blocker.position.inWorld() || d > 4)
                    return false;
                const auto spot = moveOutOfWayPoint(vm, blocker, me, 0);
                if (!spot)
                    return false;
                Object::Route aside = r;
                aside.goals = {{(*spot)[0], (*spot)[1], 0}};
                aside.facings.clear();
                aside.multi.clear();
                aside.level = me.position.level;
                RouteFail w;
                if (!planRoute(vm, me, aside, w))
                    return false;
                r.points = std::move(aside.points);
                r.aside = r.asideTried = true;
                r.blocker = 0, r.blockedTicks = 0;
                r.waiting = false;
                return true;
            };
            if (!blocker.route) { // the blocker stands (it is not routing)
                // Inferred: never asleep here (cTSPerson::IsSleeping: then
                // plan again once, else "blocked by object").
                if (counter < 2 && offRecordedTile)
                    return replan();
                if (counter <= kAskToMoveTries && askBlockerToMove(vm, me, r, *vm.object(b))) {
                    r.blockedTicks = 1; // DoWaitingForPersonToMove
                    if (runWalkTree(c, "CT - Route Ask To Move Animation", {b, 0, 0, 0}))
                        return Step::Call;
                    return Step::Walking;
                }
                if (!r.asideTried && selfOutOfWay())
                    return Step::Walking;
                return routeBlocked();
            }
            if (blocker.route->blocker == me.id) { // it routes and I block it: the two block each other
                if (!base)
                    return routeBlocked();
                if (!r.asideTried && counter < 3 && selfOutOfWay())
                    return Step::Walking;
                if (counter > 4)
                    return routeBlocked();
                if (offRecordedTile)
                    return replan();
                return pause();
            }
            // It routes, blocked by something else (or walking).
            if (counter < 2)
                return pause();
            if (counter == 2 && offRecordedTile)
                return replan();
            return routeBlocked();
        }
        // DoWaitingForPersonToMove: until it is 1.25 tiles away (then plan
        // again) or 126 ticks have passed since the request (420 when the
        // blocker's object data 8 has 0x40), "CT - Route Waiting Animation"
        // for (1.25 - distance) * 24 ticks at a time (at least 1).
        // (blockedTicks counts the ticks since the request: each tick here,
        // and the waiting animation's ticks when it is called.)
        const int limit = blocker.data[8] & 0x40 ? kWaitForPersonLongTicks : kWaitForPersonTicks;
        ++r.blockedTicks;
        if (d > kPersonMovedAway) { // it moved off: plan again (DoInitial) and walk on
            r.blockedTicks = 0;
            r.blocker = 0;
            Object::Route next = r;
            RouteFail w;
            if (!planRoute(vm, me, next, w)) {
                why = w;
                return Step::Failed;
            }
            r.points = std::move(next.points);
            return Step::Walking;
        }
        if (r.blockedTicks >= limit) { // DoBlocked again
            r.blockedTicks = 0;
            return Step::Walking;
        }
        const int wait = std::max(1, int((kPersonMovedAway - d) * 24));
        if (!treeResult && runWalkTree(c, "CT - Route Waiting Animation", {b, int16_t(wait), 0, 0})) {
            r.blockedTicks += wait;
            return Step::Call;
        }
        return Step::Walking;
    }
    if (r.blocker) { // the way is free again
        r.blocker = 0, r.blockedTicks = 0;
    }
    r.waiting = false;
    const TilePos tileBefore = VM::tileOf(me);
    if (!r.points.empty() && floorPoint(r.points.front())) {
        const LocoStep st = pending ? *pending : locoStep(vm, me, r);
        // OS2_DEBUG_LOCO=1: each step (the frame's speed against the clip's
        // own: what the feet slide).
        if (std::getenv("OS2_DEBUG_LOCO")) {
            static const char* kinds[] = {"none", "start", "walk", "stop"};
            const float frame = std::hypot(st.loco.fx - (r.loco.moving ? r.loco.fx : me.position.x),
                                           st.loco.fy - (r.loco.moving ? r.loco.fy : me.position.y)) *
                                float(vm.clock.ticksPerSecond);
            const LocoClip* clip = vm.animations.loco(st.loco.animation, vm.clock.ticksPerSecond);
            const float own = clip ? (clip->ticks > 0 && st.loco.kind != Object::Route::Loco::Walk
                                          ? st.loco.distance / (float(st.loco.ticks) / float(vm.clock.ticksPerSecond))
                                          : clip->velocity)
                                   : 0;
            const float natural = clip ? clip->velocity : 0;
            std::fprintf(stderr,
                         "loco t%llu %d %-5s %-32s %s tick %d/%d frame (%.3f,%.3f) h %6.1f root (%.3f,%.3f) face %6.1f "
                         "speed %.3f clip %.3f (own %.3f) slide %.3f\n",
                         (unsigned long long)vm.ticks, me.id, kinds[st.loco.kind], st.loco.animation.c_str(),
                         st.loco.mirror ? "M" : "-", st.loco.tick, st.loco.ticks, st.loco.fx, st.loco.fy,
                         st.loco.heading * 180 / float(M_PI), st.x, st.y, st.facing * 180 / float(M_PI), frame, natural,
                         own, std::fabs(frame - natural));
        }
        r.loco = st.loco;
        r.points.erase(r.points.begin(), r.points.begin() + std::ptrdiff_t(std::min(st.consumed, r.points.size())));
        moveTo(vm, me, st.x, st.y, st.level);
        setDirection(me, directionOf(-std::sin(st.facing), std::cos(st.facing)));
        if (r.loco.kind == Object::Route::Loco::Stop && r.loco.tick >= r.loco.ticks) {
            // Stopped, turned to the final facing (BeginCachedEndTurnPath
            // is not needed).
            r.endTurned = true;
            r.loco.kind = Object::Route::Loco::None;
            r.loco.moving = false;
        }
    } else if (!r.points.empty() && r.points.front().ticks > 0) {
        const Object::Route::Point& p = r.points.front();
        if (r.climb.ticksLeft == 0) { // the phase starts
            moveTo(vm, me, p.x, p.y, p.level);
            setDirection(me, p.direction);
            r.climb.animation = p.animation;
            r.climb.z = p.z;
            r.climb.ticksLeft = p.ticks;
            r.climb.turnSteps = p.turnSteps;
            r.climb.mirror = p.mirror;
            ++r.climb.serial;
        }
        // A turn in place stands; a stair's climb goes on walking after it.
        r.loco.kind = Object::Route::Loco::None;
        r.loco.moving = p.turnSteps == 0 && !p.animation.empty();
        r.waiting = p.animation.empty();
        if (--r.climb.ticksLeft == 0) {
            r.points.erase(r.points.begin());
            if (r.points.empty() || r.points.front().ticks == 0)
                r.climb.animation.clear();
        }
        return Step::Walking; // the tick is spent
    }
    if (!r.points.empty()) {
        if (!(VM::tileOf(me) == tileBefore)) {
            r.lastCheck = vm.ticks;
            if (startGoalCheck(c, me, r) || startAlongRoute(c, me, r))
                return Step::Call;
        }
        return Step::Walking;
    }
    // The turn to the final facing (BeginCachedEndTurnPath): in place when
    // more than 40 degrees off the way the walker came.
    if (!r.endTurned && !r.aside && r.facing >= 0) {
        r.endTurned = true;
        if (auto t = turnPoint(vm, me, turnSteps(headingOfDirection(me.data[objdata::Direction]),
                                                 headingOfDirection(r.facing)))) {
            r.points.push_back(*t);
            return Step::Walking;
        }
    }
    if (std::exchange(r.aside, false)) { // stepped aside: the route itself again
        Object::Route next = r;
        RouteFail w;
        if (!planRoute(vm, me, next, w)) {
            why = w;
            return Step::Failed;
        }
        r.points = std::move(next.points);
        return Step::Walking;
    }
    setDirection(me, r.facing);
    me.route.reset();
    return Step::Arrived;
}

// A failed route: if the primitive allows them, "CT - Route Failure
// Feedback" (global) runs first on the walker's thread, with the failure
// code, the blocker, 1 and 0 as TriggerFailureAnimation passes them (the
// last two: whether the slot has a distance and that distance; not decoded
// further); then the route result goes to person data 62 (after the
// feedback, as DoPostFailedOrSucceeded comes after DoNeedFailureFeedback:
// a route the feedback tree walks does not overwrite it) and the primitive
// returns false (docs/routing-mac.md §7).
// The walker's post-route tree (OBJf entry 0x26, "post route" of STR#
// 0xC9; the person's "Function - Post Route": it stops the walk's overlays,
// sits the Sim on a chair goal through the chair's function, and runs the
// toddler's states), as DoFailedOrSucceeded runs it (BV 0x577ea0;
// TriggerPostRouteTree BV 0x577cf4) after an arrival or a failure of a base
// route (not a route inside another's callback tree), on the walker's
// thread with the parameters 0 the goal's object (a chair goal's chair; 0
// for a standing goal) and 1 the route result; DoWaitingForPostRouteTree:
// its false after an arrival fails the route with 12 "post-route failed".
bool startPostRoute(Context& c, int stage, int16_t result) {
    Object& me = c.me();
    if (!me.person)
        return false;
    for (const Frame& f : c.thread.stack)
        if (f.routeCallback)
            return false;
    const uint16_t tree = c.vm.functionTree(me, 0x26);
    if (!tree)
        return false;
    const std::array<int16_t, 4> params{0, result, 0, 0};
    c.frame.postRoute = uint8_t(stage);
    callRouteTree(c, me.id, tree, me.id, params);
    return true;
}

// A failed route: if the primitive allows them, "CT - Route Failure
// Feedback" (global) runs first on the walker's thread, with the failure
// code, the blocker, 1 and 0 as TriggerFailureAnimation passes them (the
// last two: whether the slot has a distance and that distance; not decoded
// further); then the post-route tree; then the route result goes to person
// data 62 (after the feedback, as DoPostFailedOrSucceeded comes after
// DoNeedFailureFeedback: a route the feedback tree walks does not overwrite
// it) and the primitive returns false (docs/routing-mac.md §7).
Exit failRoute(Context& c, RouteFail why, ObjectId blocker, bool failureTrees, bool postRoute = true) {
    Object& me = c.me();
    if (std::getenv("OS2_ROUTE_FAILS")) // why routes fail, for looking into
        std::fprintf(stderr, "t%llu route of %d failed with %d in \"%s\" #%u op %02X (stack object %d) at (%.2f, %.2f, %d)\n", (unsigned long long)c.vm.ticks, me.id,
                     int(why), c.frame.behavior->fileName.c_str(), c.frame.pc, c.instruction.opcode, c.frame.stackObject, me.position.x,
                     me.position.y, me.position.level);
    if (std::getenv("OS2_ROUTE_FAILS_NEAR")) // and what blocks around the walker
        for (const auto& [id, x] : c.vm.objects())
            if (id != me.id && x.position.inWorld() && x.position.level == me.position.level &&
                std::fabs(x.position.x - me.position.x) < 4.6f && std::fabs(x.position.y - me.position.y) < 2.6f && VM::blocksRouting(x))
                std::fprintf(stderr, "  near: %d %s at (%.2f, %.2f) flags %X layers %X h %X p %X w %X%s%s\n", id, x.definition ? x.definition->fileName.c_str() : "?",
                             x.position.x, x.position.y, x.data[objdata::Flags], x.data[63], x.data[4], x.data[42], x.data[13], x.person ? " person" : "", VM::blocksRouting(x) ? " blocks" : "");
    me.route.reset();
    if (why == RouteFail::Success)
        why = RouteFail::Unknown;
    c.frame.pendingRouteResult = int16_t(why);
    if (failureTrees) {
        if (const auto tree = c.vm.code().behaviorByName(kGlobalGroup, "CT - Route Failure Feedback")) {
            if (!postRoute)
                c.frame.postRoute = 3; // no post-route tree after it
            c.vm.call(c.thread, me.id, *tree, {int16_t(why), blocker, 1, 0});
            c.thread.stack.back().rerunCaller = true;
            c.thread.stack.back().routeCallback = true;
            return Exit::Call;
        }
    }
    if (postRoute && startPostRoute(c, 2, int16_t(why)))
        return Exit::Call;
    c.frame.pendingRouteResult = -1;
    c.frame.postRoute = 0;
    if (me.person)
        me.person->data[62] = int16_t(why);
    return Exit::False;
}

// Walks the thread's object to one of `goals` (planned on the first run of
// the instruction): yields every tick while walking, true on arrival,
// false when no goal can be reached (after the failure feedback). Routing
// out of the world is the game's own error. A goal on another level is
// reached by the stairs.
Exit walkStep(Context& c, ObjectId target, const std::function<VM::RouteGoals()>& plan, bool failureTrees) {
    Object& me = c.me();
    const size_t depth = c.thread.stack.size();
    if (c.frame.postRoute == 1) { // back from the post-route tree after an arrival
        const bool ok = std::exchange(c.thread.rerunResult, std::nullopt).value_or(true);
        c.frame.postRoute = 0;
        if (!ok)
            return failRoute(c, RouteFail::PostRouteFailed, 0, failureTrees, false);
        if (me.person)
            me.person->data[62] = 0;
        return Exit::True;
    }
    if (c.frame.pendingRouteResult >= 0) { // back from the failure feedback or the post-route tree
        c.thread.rerunResult.reset();
        const int16_t result = c.frame.pendingRouteResult;
        if (c.frame.postRoute == 0 && startPostRoute(c, 2, result))
            return Exit::Call;
        if (me.person)
            me.person->data[62] = result;
        c.frame.pendingRouteResult = -1;
        c.frame.postRoute = 0;
        return Exit::False;
    }
    if (me.route && me.route->depth == depth && me.route->tree == c.frame.behavior && me.route->node == c.frame.pc) {
        RouteFail why = RouteFail::Success;
        const ObjectId blocker = me.route->blocker;
        switch (advance(c, me, why)) {
        case Step::Walking: return Exit::Yield;
        case Step::Call: return Exit::Call;
        case Step::Arrived:
            if (startPostRoute(c, 1, 0))
                return Exit::Call;
            if (me.person)
                me.person->data[62] = 0;
            return Exit::True;
        case Step::Failed: return failRoute(c, why, blocker, me.route ? me.route->failureTrees : failureTrees);
        }
    }
    me.route.reset();
    worldOf(c);
    if (c.frame.preRouteRan) // back from the pre-route tree
        if (const auto ran = std::exchange(c.thread.rerunResult, std::nullopt); ran && !*ran)
            return failRoute(c, RouteFail::PreRouteFailed, 0, failureTrees);
    if (!me.position.inWorld())
        throw VMError("Trying to route while out of world.");
    const VM::RouteGoals goals = plan();
    if (goals.goals.empty()) // the last refused candidate's result (InitRoute: none at all → 6)
        return failRoute(c, goals.refused ? RouteFail(goals.refused) : RouteFail::NoValidGoals, 0, failureTrees);
    Object::Route r;
    for (size_t i = 0; i < goals.goals.size(); ++i)
        r.goals.push_back({goals.points[i][0], goals.points[i][1], goals.goals[i].cost});
    r.facings = goals.facings;
    r.multi = goals.multi;
    r.level = goals.level;
    r.ignore = {me.id};
    if (goals.onTarget)
        r.ignore.push_back(target);
    r.target = target;
    r.failureTrees = failureTrees;
    r.depth = depth, r.tree = c.frame.behavior, r.node = c.frame.pc;
    RouteFail why = RouteFail::Success;
    if (!planRoute(c.vm, me, r, why))
        return failRoute(c, why, 0, failureTrees);
    // A way was found: the walker's pre-route tree first (OBJf entry 0x25,
    // the person's "Function - Pre Route": it stands a seated Sim up through
    // its chair's stand function, stops the idles and decides whether to
    // run), on its thread with itself as the stack object (TriggerPreRouteTree;
    // parameters: 0 the target, 1 the routing slot, 5 the distance in tiles,
    // rounded). Its false fails the route ("pre-route failed"); after it the
    // way is planned again from where it left the walker
    // (DoWaitingForPreRouteTree).
    if (!c.frame.preRouteRan && me.person) {
        if (const uint16_t tree = c.vm.functionTree(me, 0x25)) {
            float length = 0;
            Point2 at{me.position.x, me.position.y};
            for (const auto& p : r.points) {
                length += std::hypot(p.x - at[0], p.y - at[1]);
                at = {p.x, p.y};
            }
            c.frame.preRouteRan = true;
            c.vm.call(c.thread, me.id, tree, {});
            Frame& f = c.thread.stack.back();
            f.stackObject = me.id;
            f.params.resize(std::max<size_t>(f.params.size(), 6));
            f.params[0] = target;
            f.params[1] = me.person->data[37];
            f.params[5] = int16_t(std::floor(length + 0.5f));
            f.rerunCaller = true;
            f.routeCallback = true;
            return Exit::Call;
        }
    }
    if (me.data[objdata::ContainerId] != 0)
        leaveSlot(me);
    me.route = std::move(r);
    RouteFail later = RouteFail::Success;
    switch (advance(c, me, later)) {
    case Step::Walking: return Exit::Yield;
    case Step::Call: return Exit::Call;
    case Step::Arrived:
        if (startPostRoute(c, 1, 0))
            return Exit::Call;
        if (me.person)
            me.person->data[62] = 0;
        return Exit::True;
    case Step::Failed: break;
    }
    return failRoute(c, later, 0, failureTrees);
}

Exit walk(Context& c, ObjectId target, const std::function<VM::RouteGoals()>& plan, bool failureTrees) {
    const Exit e = walkStep(c, target, plan, failureTrees);
    if (e != Exit::Yield && e != Exit::Call) {
        c.frame.preRouteRan = false; // the instruction is done
        c.frame.postRoute = 0;
        c.frame.routeSlot = -1;
    }
    return e;
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
    if (!global && index == 0 && target.syntheticSlot)
        return *target.syntheticSlot;
    const auto slots = routingSlots(global ? vm.code().slots(kGlobalGroup, kGlobalSlots) : slotTable(vm, target));
    if (index < 0 || size_t(index) >= slots.size())
        throw VMError("Invalid routing slot specified (" + std::string(global ? "global " : "") + "slot " +
                      std::to_string(index) + " of " + std::to_string(slots.size()) + ")");
    return *slots[size_t(index)];
}

// The routing person data the trees read after a Go To Routing Slot (labels
// of STR# 0xC8): 37 "route slot" (the routing slot number), 38 "route multi
// slot index" (which of the places the slot gives was taken; "Sofa -
// Animate - Get In" passes it on as the seat; 0 here, then the bone of a
// bone slot the planned way goes to) and 40 "route goal" (the object). Inferred
// from the labels and those uses; 39 "route status" is left alone.
void noteRouteSlot(Object& me, int slot, ObjectId target) {
    if (!me.person)
        return;
    me.person->data[37] = int16_t(slot);
    me.person->data[38] = 0;
    me.person->data[40] = target;
}

// Go To Routing Slot (0x2D): slot u16 at 0, where it is u16 at 2 (STR#
// 0xD7: 0 the stack param of that number, 1 the literal routing slot, 2 the
// global slot; 3 "???" is not done), flags u8 at 4, from the editor's
// disassembler in the Mac binary (docs/primitives-mac.md §5): 0x01 no
// failure trees, 0x02 the slot number is in temp 0 (with the literal
// scope), 0x04 ignore the destination object's footprint (its parts do not
// block the goal), 0x08 allow different altitudes (TryGotoRoutingSlot
// passes it to the slot's SetAllowDifferentAlts: it is not about levels, a
// stack object on another level is reached by the stairs; altitudes, the
// height of an object on a surface, are not checked here), 0x10 a u16 at 9
// goes to ProcessGoto with the u16 at 7 (not understood). The thread's
// object walks to that routing slot of the stack object; false when the
// stack object is not in the world or no goal can be reached.
Exit goToRoutingSlot(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t data = le16(&o[0]), scope = le16(&o[2]);
    const uint8_t flags = o[4];
    if (scope > 2)
        throw VMError("Go To Routing Slot: slot scope " + std::to_string(scope) + " not implemented");
    const Object* target = c.vm.object(c.frame.stackObject);
    if (!target || !target->position.inWorld())
        return Exit::False;
    int index = scope == 0 ? c.get(kOwnerParam, int16_t(data))
              : scope == 1 && (flags & 0x02) ? c.thread.temps[0]
                                              : int(data);
    const bool starting = c.frame.routeSlot < 0;
    if (!starting) // mid-instruction: as read before the route's trees (they change the temps)
        index = c.frame.routeSlot;
    c.frame.routeSlot = int16_t(index);
    const Slots::Slot& slot = routingSlot(c.vm, *target, scope == 2, index);
    const ObjectId id = target->id;
    // Once, as the walk starts: each tick of the walk runs the instruction
    // again, and resetting person data 38 then lost the side of the chair
    // the planned way had chosen (the Sims sat sideways on the chairs they
    // reached from their south-west bone).
    if (starting)
        noteRouteSlot(c.me(), index, id);
    const bool ignoreFootprint = flags & 0x04;
    return walk(
        c, id,
        [&] {
            VM::RouteGoals g = c.vm.routingGoals(slot, *c.vm.object(id), true, c.me().id);
            g.onTarget = g.onTarget || ignoreFootprint;
            return g;
        },
        !(flags & 0x01));
}

// Go To Relative Position (0x1B), with The Sims 1's layout (FreeSO), which
// the game's trees fit: u16 at 0 (TS1: how long to wander around objects),
// location s8 at 2 (STR# 0x82: -2 on top of, -1 anywhere near, 0-7 in front
// of, front right, right, ... relative to the stack object), direction s8 at
// 3 (STR# 0x83: -2 facing it, -1 any, 0-7 relative to its direction), u16 at
// 4, flags u8 at 6, from the editor's disassembler in the Mac binary
// (docs/primitives-mac.md §5; not The Sims 1's 0x01 altitude / 0x02 no
// failure): 0x02 no failure trees, 0x04 allow different altitudes (as Go To
// Routing Slot's 0x08: a stack object on another level is reached too). The location becomes a routing
// slot as FreeSO builds it: on top = the object's tile, anywhere near = 1 to
// 2 tiles all around, a direction = 1 to 1.5 tiles that way.
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
    // Inferred: the resolution and gradient the binary's CreateBestGoalPointList
    // gives the synthetic slot are not read (whole tiles, the row's middle first).
    slot.resolutionTiles = 1, slot.gradient = 1;
    const ObjectId id = target->id;
    return walk(c, id, [&] { return c.vm.routingGoals(slot, *c.vm.object(id), true, c.me().id); }, !(o[6] & 0x02));
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
        if (!vm.world->walkable(pt) || !vm.tileFree(pt, ignore))
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

// Find Location For (0x10): places an object (it does not walk). As the Mac
// binary has it (TryFindGoodLocation; docs/primitives-mac.md §4): mode u8 at
// 0 (STR# 0xEF: 0 normal, 1 out of world, 2 normal in the same room, 3 along
// the reference object's vector, 4 lateral to it (+90°), 5-8 in a routing
// slot of the reference object: slot number in the stack param, local,
// literal or the global SLOT), local u8 at 1, flags u8 at 2, byte 3 = the
// mask of the FORBIDDEN directions (bit n set = direction n not allowed:
// 0xFE = only N, 0xAA = only N/E/S/W; no allowed direction left, as with
// 0xFF and 0, means any), the object (owner u8 at 4, data u16) and the
// reference (owner u8 at 7, data u16) with flag 0x08, routing slot u16 at
// 10. Flags: 0x01 the reference is the object in the local of byte 1 (out
// of range: error 0x33), 0x02 do NOT prefer empty tiles, 0x04 user
// editable tiles only, 0x08 explicit objects (missing: error 0x17), 0x10 on
// level ground, 0x20 with an empty border, 0x40 begin in front of the
// reference, 0x80 with line of sight to the centre. Without 0x08 the object
// is the stack object (none: error 0x15) and the reference me. Mode 0 looks
// in the reference's room first and then anywhere; mode 2 only in its room;
// both search rings around the reference's tile (from the tile in front of
// it with 0x40) up to 10 tiles, for a tile where every part of the object
// fits. Modes 3/4 need a reference (error 0x3D) and search both ways along
// the line. Modes 5-8 try each goal of the slot (slot number out of range:
// error 0x08 for the parameter/local, 0x1B for a slot that does not exist);
// where the slot does not fix the facing, the eight directions are tried
// from a random one, skipping the forbidden ones. Found: placed, true; else
// false. Without flag 0x02 empty tiles are tried first (inferred weight).
// Not done: flags 0x04, 0x10, 0x20 and 0x80 (editable tiles, level ground
// and borders need the terrain and the build mode's data).
int allowedFacing(uint8_t forbidden, int dir) {
    const uint8_t mask = uint8_t(~forbidden);
    dir = ((dir % 8) + 8) % 8;
    if (mask == 0 || (mask & (1u << dir)))
        return dir;
    for (int k = 1; k <= 4; ++k)
        for (int d : {dir + k, dir - k})
            if (mask & (1u << (((d % 8) + 8) % 8)))
                return ((d % 8) + 8) % 8;
    return dir;
}

Exit findLocationFor(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t mode = o[0], flags = o[2], forbidden = o[3];
    if (mode > 8)
        throw VMError("Find Location For: mode " + std::to_string(mode) + " not implemented");
    Object* obj = nullptr;
    const Object* ref = nullptr;
    if (flags & 0x08) {
        const int16_t objId = c.get(o[4], int16_t(le16(&o[5])));
        obj = c.vm.object(objId);
        if (!obj)
            throw gameError(err::NoObject, "object " + std::to_string(objId));
        ref = c.vm.object(c.get(o[7], int16_t(le16(&o[8]))));
    } else {
        obj = c.vm.object(c.frame.stackObject);
        if (!obj)
            throw gameError(err::NoStackObject, "Find Location For");
        if (flags & 0x01) {
            if (o[1] >= c.frame.locals.size())
                throw gameError(err::LocalOutOfRange, "local " + std::to_string(o[1]));
            ref = c.vm.object(c.get(kOwnerLocal, o[1]));
        } else {
            ref = &c.me();
        }
    }
    if (mode == 1) { // out of world: out of its container too (the binary's
                     // vtable 0x2ac "remove from world"; the car's "Sub - Send
                     // Passengers Out of World" loops until its slots are empty)
        obj->position.x = obj->position.y = -1;
        obj->route.reset();
        if (obj->data[objdata::ContainerId] != 0)
            leaveSlot(*obj);
        obj->data[objdata::Room] = -1; // as every out-of-world object of a saved lot; "Get Out TEST" reads it
        return Exit::True;
    }
    if ((mode == 3 || mode == 4) && !ref)
        throw gameError(err::StartObjectRequired, "Find Location For");
    World& world = worldOf(c);
    if (!ref || !ref->position.inWorld())
        return Exit::False;
    TilePos at = VM::tileOf(*ref);
    // Without flag 0x02 empty tiles are preferred (modes 0-4): first only
    // tiles with nothing else on them (Sims included), then any. Inferred:
    // what "prefer empty" weighs is not read; three arriving Sims ("Sub -
    // Generate Family", mode 4 by the mailbox) otherwise share one tile.
    const bool preferEmpty = !(flags & 0x02);
    const ObjectId group = multiTileGroup(*obj);
    auto empty = [&](const TilePos& t) {
        for (ObjectId other : c.vm.objectsOnTile(t)) {
            const Object* x = c.vm.object(other);
            if (x && multiTileGroup(*x) != group)
                return false;
        }
        return true;
    };
    if (mode >= 5) {
        const uint16_t data = le16(&o[10]);
        int index = int(data);
        if (mode == 5 || mode == 6) {
            const auto& values = mode == 5 ? c.frame.params : c.frame.locals;
            if (data >= values.size())
                throw gameError(err::StackNumberOutOfRange, std::string(mode == 5 ? "parameter " : "local ") +
                                                                std::to_string(data));
            index = c.get(mode == 5 ? kOwnerParam : kOwnerLocal, int16_t(data));
        }
        const auto slots = routingSlots(mode == 8 ? c.vm.code().slots(kGlobalGroup, kGlobalSlots) : slotTable(c.vm, *ref));
        if (index < 0 || size_t(index) >= slots.size())
            throw gameError(err::SlotOutOfRange, "routing slot " + std::to_string(index) + " of " +
                                                     std::to_string(slots.size()));
        const Slots::Slot& slot = *slots[size_t(index)];
        VM::RouteGoals goals = c.vm.routingGoals(slot, *ref);
        std::vector<size_t> order(goals.goals.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return goals.goals[a].cost < goals.goals[b].cost; });
        for (size_t i : order) {
            int facing = goals.facings[i];
            if (facing < 0) { // the slot leaves it open: the first allowed direction from a random one
                const int start = std::uniform_int_distribution<int>(0, 7)(c.vm.random);
                facing = allowedFacing(forbidden, start);
            } else {
                facing = allowedFacing(forbidden, facing);
            }
            if (tryPlace(c.vm, *obj, {goals.goals[i].x, goals.goals[i].y, goals.level}, facing))
                return Exit::True;
        }
        return Exit::False;
    }
    if (mode == 3 || mode == 4) {
        // Along a vector the forbidden directions are taken relative to
        // the line (the reference's direction): the carpool's car, placed at
        // the "Car - Start" portal with only N allowed (0xFE), faces down the
        // street the way the portal does, and a new puddle (0xAA, along its
        // parent) keeps a side direction its "rotate until straight" loop can
        // reach. Inferred: TryFindGoodLocation hands these modes to the
        // object's vtable 0x2d8 with the parameters, which was not read.
        const int dir = ((ref->data[objdata::Direction] % 8) + 8) % 8 + (mode == 4 ? 2 : 0);
        const auto [sx, sy] = directionStep(dir % 8);
        // Not on the reference's own tile: a new puddle placed on its
        // parent's tile kept "Puddle - New Puddle - Add" turning and
        // placing it again (N002_Lot22, round 8).
        for (int pass = preferEmpty ? 0 : 1; pass < 2; ++pass)
            for (int i = 2; i < 34; ++i) {
                const int k = i / 2, sign = i % 2 ? -1 : 1;
                const TilePos t{at.x + sign * sx * k, at.y + sign * sy * k, at.level};
                if ((pass == 1 || empty(t)) && tryPlace(c.vm, *obj, t, (dir + allowedFacing(forbidden, 0)) % 8))
                    return Exit::True;
            }
        return Exit::False;
    }
    // 0, 2: rings around the reference (in its room; mode 0 then anywhere).
    const int16_t room = world.room(at);
    if (flags & 0x40) {
        const auto [dx, dy] = directionStep(((ref->data[objdata::Direction] % 8) + 8) % 8);
        at.x += dx, at.y += dy;
    }
    const int facing = forbidden && uint8_t(~forbidden) ? allowedFacing(forbidden, obj->data[objdata::Direction]) : -1;
    for (int emptyPass = preferEmpty ? 0 : 1; emptyPass < 2; ++emptyPass)
        for (int pass = 0; pass < (mode == 0 ? 2 : 1); ++pass)
            for (int r = 0; r <= 10; ++r)
                for (int x = at.x - r; x <= at.x + r; ++x)
                    for (int y = at.y - r; y <= at.y + r; ++y) {
                        if (std::max(std::abs(x - at.x), std::abs(y - at.y)) != r)
                            continue;
                        const TilePos t{x, y, at.level};
                        if (pass == 0 && world.room(t) != room)
                            continue;
                        if ((emptyPass == 1 || empty(t)) && tryPlace(c.vm, *obj, t, facing))
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
// bones (the renderer's; ignored), 0x02 only tests whether it would fit
// ("TEST ONLY" in the editor's disassembler, Mac binary; "Be Fed - Find Drop
// Slot", "Sub - Is Locked?"). Bytes 7-8 (a u16: 0x09B4, 0x0BEF, ...) are not
// understood. The object takes the container's position, as the saved
// lots keep contained objects; false when another object is in the slot;
// the game's error when the slot does not exist.
Exit snapInto(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[9];
    if (flags & ~0x07)
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
    if (flags & 0x02)
        return Exit::True; // TEST ONLY
    x->data[objdata::ContainerId] = into->id;
    x->data[objdata::SlotNumber] = int16_t(slot);
    x->position = into->position;
    x->data[objdata::Room] = into->data[objdata::Room];
    if (!(x->route && x->route->inPortal)) // a portal tree snaps the walker step by step
        x->route.reset();
    return Exit::True;
}

} // namespace

const std::vector<int16_t>* accessibleRooms(VM& vm, int16_t room) {
    if (!vm.world)
        return nullptr;
    const auto& rooms = plannerOf(vm).accessibleRooms(room);
    return rooms.empty() ? nullptr : &rooms;
}

// The binary keeps the destinations as managed destination objects of the
// router and flushes them (cTSRoutingObjectAggregate::
// FlushManagedDestinationObjects, BV 0x4f9eb4: kept while in the person's
// queue, current or sub-queue interaction, or in its routing state); when
// it runs is inferred (each routing step, and when a pie menu closes).
void flushDestinations(VM& vm) {
    std::vector<ObjectId> done;
    for (const auto& [id, o] : vm.objects()) {
        if (!(o.syntheticSlot || o.managedDestination) || o.destinationHeld)
            continue;
        bool used = false;
        for (const auto& [sid, sim] : vm.objects())
            if (sim.person)
                for (const auto& q : sim.queue)
                    used = used || q.object == id;
        if (!used)
            done.push_back(id);
    }
    for (const ObjectId id : done)
        vm.removeObject(id);
}

namespace {

// A footprint: rectangles x0, y0, x1, y1 in world tiles.
using Footprint = std::vector<std::array<float, 4>>;

// The OBJD field of an object (file offset), 0 without a definition.
uint16_t objdField(const Object& o, size_t offset) { return o.definition ? o.definition->field(offset) : 0; }
constexpr size_t kObjdTileWidth = 0xC6;     // field 0x43 (TSObjectBaseFootprintWidth)
constexpr size_t kObjdFootprintMask = 0xDA; // field 0x4D
constexpr int kTypePerson = 2, kTypeDoor = 8;

int objectType(const Object& o) { return o.person ? kTypePerson : int(objdField(o, 0x52)); }

// TSObjectWorldSpaceFootprint's box (BV 0x8aa40c): a square of half side
// 0.5 * the base width (a person 0.2, else OBJD field 0x43 / 16, 1 when 0)
// about the object's position, each side moved in by a nibble of OBJD
// field 0x4D / 16 (out by person data 73 for a person facing an even
// direction), the nibbles turned with the facing: (8 - direction) / 2
// picks which nibble each side takes. Inferred: the direction index is
// object data 1, and the model's aggregate footprint (AggregateFootprint,
// used before this box when there is one) is not read: the box stands in.
Footprint footprintOf(const Object& o) {
    const float half = 0.5f * (o.person ? 0.2f : (objdField(o, kObjdTileWidth) ? float(objdField(o, kObjdTileWidth)) / 16 : 1.0f));
    std::array<float, 4> c{0, 0, 0, 0};
    const int dir = ((o.data[objdata::Direction] % 8) + 8) % 8;
    if (const uint16_t m = objdField(o, kObjdFootprintMask); m && !o.person)
        for (int k = 0; k < 4; ++k)
            c[size_t(k)] = float((m >> (4 * k)) & 0xF) / 16;
    if (o.person && o.person->data.size() > 73 && (dir & 1) == 0)
        if (const uint16_t m = uint16_t(o.person->data[73]))
            for (int k = 0; k < 4; ++k)
                c[size_t(k)] -= float((m >> (4 * k)) & 0xF) / 16;
    const int u = (8 - dir) / 2;
    float x0 = o.position.x - half, y0 = o.position.y - half, x1 = o.position.x + half, y1 = o.position.y + half;
    x0 += c[size_t(u % 4)];
    y1 -= c[size_t((u + 1) % 4)];
    x1 -= c[size_t((u + 2) % 4)];
    y0 += c[size_t((u + 3) % 4)];
    return {{x0, y0, x1, y1}};
}

// A door's indexed footprints 0 and 2 (TSObjectIndexedWorldSpaceFootprint;
// the model's footprint data is not read). Inferred: the leaf's swing, the
// half tile on each side of the wall the door cuts (0 in front, 2 behind).
Footprint doorFootprint(const Object& o, int index) {
    const TilePos t = VM::tileOf(o);
    const auto [dx, dy] = directionStep(o.data[objdata::Direction] + 4); // towards the wall behind
    float x0 = float(t.x), y0 = float(t.y), x1 = x0 + 1, y1 = y0 + 1;
    const float side = index == 0 ? 0.0f : 1.0f; // 0: the door's tile, 2: the one behind
    if (dx > 0)
        x0 = float(t.x) + 0.5f + side, x1 = x0 + 0.5f;
    else if (dx < 0)
        x1 = float(t.x) + 0.5f - side, x0 = x1 - 0.5f;
    else if (dy > 0)
        y0 = float(t.y) + 0.5f + side, y1 = y0 + 0.5f;
    else if (dy < 0)
        y1 = float(t.y) + 0.5f - side, y0 = y1 - 0.5f;
    else
        return {};
    return {{x0, y0, x1, y1}};
}

// FUN_0191f6d0 (docs/routing-mac.md §8.4): `p` out of the footprint
// inflated by d, along the shorter axis unless both moves are short.
void pushOutOf(const Footprint& fp, float d, Point2& p) {
    constexpr float kEps = 0.0001f;
    float dx = 0, dy = 0;
    for (const auto& r : fp) {
        if (!(r[0] - d <= p[0] && p[0] <= d + r[2] && r[1] - d <= p[1] && p[1] <= d + r[3]))
            continue;
        if ((r[0] + r[2]) * 0.5f < p[0]) {
            if (p[0] < d + r[2])
                dx = std::max(dx, d + r[2] + kEps - p[0]);
        } else if (const float v = r[0] - d - kEps - p[0]; v < dx) {
            dx = v;
        }
        if ((r[1] + r[3]) * 0.5f < p[1]) {
            if (p[1] < d + r[3])
                dy = std::max(dy, d + r[3] + kEps - p[1]);
        } else if (const float v = r[1] - d - kEps - p[1]; v < dy) {
            dy = v;
        }
    }
    if (dx == 0 && dy == 0)
        return;
    const float far = d + kEps + kEps;
    if (std::fabs(dy) <= std::fabs(dx)) {
        if (std::fabs(dx) >= far) {
            p[1] += dy;
            return;
        }
    } else if (std::fabs(dy) >= far) {
        p[0] += dx;
        return;
    }
    p[0] += dx, p[1] += dy;
}

bool canIntersectObjects(const Object& o) { return (o.data[objdata::Flags] & 0x06) != 0; }
bool canIntersectPeople(const Object& o) { return (o.data[objdata::Flags] & 0x14) != 0; }

// cTSObject::RunTree(5, other, ...): the "allow intersection" tree (OBJf
// entry 5) of `o` about `other`; false without one. Inferred: the other's
// ID goes in parameter 0 and as the stack object.
bool allowsIntersection(VM& vm, const Object& o, ObjectId other) {
    const uint16_t tree = vm.functionTree(o, 5);
    if (!tree)
        return false;
    const std::array<int16_t, 1> args{int16_t(other)};
    Thread t = vm.start(o.id, tree, args, other);
    for (int turn = 0; t.status == Thread::Status::Running && turn < 4; ++turn)
        vm.run(t, vm.instructionsPerTurn);
    return t.status == Thread::Status::Finished && t.result;
}

// IsDiagonalDoor: a door (type 8) whose wall placement flags (object data
// 13) have 0x50.
bool isDiagonalDoor(const Object& o) { return objectType(o) == kTypeDoor && (o.data[13] & 0x50) != 0; }

} // namespace

Point2 moveAwayFromOtherObjects(VM& vm, const Object& me, Point2 p, int level, float d, bool skipPeople, bool doors) {
    if (me.person && me.person->data.size() > 68 && (me.person->data[68] & 0x02))
        return p; // a ghost that walks through things (person data 68 "ghost flags")
    if (canIntersectObjects(me))
        return p;
    const World& w = *vm.world;
    const TilePos c{int(std::floor(p[0])), int(std::floor(p[1])), level};
    const bool mePerson = objectType(me) == kTypePerson;
    for (int ty = c.y - 1; ty <= c.y + 1; ++ty)
        for (int tx = c.x - 1; tx <= c.x + 1; ++tx) {
            const TilePos t{tx, ty, level};
            if (!w.contains(t))
                continue;
            for (const ObjectId id : vm.objectsOnTile(t)) {
                const Object* o = vm.object(id);
                if (!o)
                    continue;
                if (doors && objectType(*o) == kTypeDoor) {
                    pushOutOf(doorFootprint(*o, 0), d, p);
                    pushOutOf(doorFootprint(*o, 2), d, p);
                }
                const bool oPerson = objectType(*o) == kTypePerson;
                if ((mePerson || oPerson) && (canIntersectPeople(*o) || canIntersectPeople(me)))
                    continue;
                if (canIntersectObjects(*o) || (skipPeople && oPerson))
                    continue;
                if (allowsIntersection(vm, me, o->id) || allowsIntersection(vm, *o, me.id))
                    continue;
                pushOutOf(footprintOf(*o), d, p);
            }
        }
    if (doors)
        for (int ty = c.y - 2; ty <= c.y + 2; ++ty)
            for (int tx = c.x - 2; tx <= c.x + 2; ++tx) {
                if (std::abs(tx - c.x) <= 1 && std::abs(ty - c.y) <= 1)
                    continue;
                const TilePos t{tx, ty, level};
                if (!w.contains(t))
                    continue;
                for (const ObjectId id : vm.objectsOnTile(t))
                    if (const Object* o = vm.object(id); o && isDiagonalDoor(*o)) {
                        pushOutOf(doorFootprint(*o, 0), d, p);
                        pushOutOf(doorFootprint(*o, 2), d, p);
                    }
            }
    return p;
}

Point2 movePointAwayFromWalls(const World& w, Point2 at, int level, float d) {
    constexpr float kEps = 0.001f;
    const float r2 = std::sqrt(2.0f);
    const TilePos t{int(std::floor(at[0])), int(std::floor(at[1])), level};
    const int diag = w.diagonalSides(t);
    for (int side = 1; side <= 0x20; side <<= 1) {
        const bool wall = side == 1   ? w.wallBetween(t, 0, -1)
                          : side == 2 ? w.wallBetween(t, -1, 0)
                          : side == 4 ? w.wallBetween(t, 0, 1)
                          : side == 8 ? w.wallBetween(t, 1, 0)
                                      : (diag & side) != 0;
        if (!wall)
            continue;
        float& x = at[0];
        float& y = at[1];
        switch (side) {
        case 1:
            if (y - std::floor(y) < d)
                y += d - (y - std::floor(y)) + kEps;
            break;
        case 2:
            if (x - std::floor(x) < d)
                x += d - (x - std::floor(x)) + kEps;
            break;
        case 4:
            if (std::ceil(y) - y < d)
                y -= d - (std::ceil(y) - y) + kEps;
            break;
        case 8:
            if (std::ceil(x) - x < d)
                x -= d - (std::ceil(x) - x) + kEps;
            break;
        case 0x10: { // as the binary has it: measured to fx + fy = 1, moved along it
            const float s = ((y - std::floor(y)) - (std::ceil(x) - x)) / r2;
            if (s >= 0 && s < d) {
                const float v = d - s + kEps;
                y += v / -r2, x += v / r2;
            } else if (s < 0 && s > -d) {
                const float v = d + s + kEps;
                y += v / r2, x += v / -r2;
            }
            break;
        }
        case 0x20: { // measured to fx = fy, moved along it
            const float s = ((y - std::floor(y)) - (x - std::floor(x))) / r2;
            if (s >= 0 && s < d) {
                const float v = (d - s + kEps) / r2;
                y += v, x += v;
            } else if (s < 0 && s > -d) {
                const float v = (s + d + kEps) / -r2;
                y += v, x += v;
            }
            break;
        }
        }
    }
    return at;
}

std::optional<Position> goHerePoint(VM& vm, ObjectId simId, float x, float y, int level) {
    if (!vm.world)
        return std::nullopt;
    const World& w = *vm.world;
    const TilePos tile{int(std::floor(x)), int(std::floor(y)), level};
    if (!w.contains(tile))
        return std::nullopt;
    const Object* sim = vm.object(simId);
    Object none;
    // GoHere (docs/routing-mac.md §8.4): 0.4 away from the objects around
    // (the Sim's view of them: TSObjectMoveAwayFromOtherObjects with the
    // Sims counted and the doors), then 0.45 away from the walls of its tile.
    Point2 at = moveAwayFromOtherObjects(vm, sim ? *sim : none, {x, y}, level, 0.4f, false, true);
    at = movePointAwayFromWalls(w, at, level, 0.45f);
    // TSObjectCanSetPosition, inferred as: in the lot, something to stand
    // on (a pool tile for the swim destination) and nothing that blocks.
    const TilePos t{int(std::floor(at[0])), int(std::floor(at[1])), level};
    if (!w.contains(t))
        return std::nullopt;
    if (w.pool(t))
        return Position{at[0], at[1], level};
    if (!w.walkable(t))
        return std::nullopt;
    PathPlanner& planner = plannerOf(vm);
    addObstacles(vm, planner, sim ? *sim : none, {});
    if (!planner.free(level, at))
        return std::nullopt;
    return Position{at[0], at[1], level};
}

ObjectId createGoHereDestination(VM& vm, ObjectId simId, float x, float y, int level) {
    const auto p = goHerePoint(vm, simId, x, y, level);
    if (!p)
        return 0;
    const TilePos tile{int(std::floor(p->x)), int(std::floor(p->y)), level};
    const uint32_t guid = vm.world->pool(tile) ? kSwimHereGuid : kGoHereGuid;
    std::vector<ObjectId> existing;
    for (const auto& [id, o] : vm.objects())
        existing.push_back(id);
    Object* dest = vm.instantiateGuid(guid);
    if (!dest)
        return 0;
    const ObjectId id = dest->id;
    auto place = [&](Object& d) {
        moveTo(vm, d, p->x, p->y, level);
        d.managedDestination = true;
        d.destinationHeld = true;
    };
    place(*dest);
    vm.initialize(id, existing);
    dest = vm.object(id);
    if (!dest)
        return 0;
    place(*dest); // its Init may move it
    return id;
}

bool objectsMayIntersect(VM& vm, const Object& a, const Object& b) { return intersectionAllowed(vm, a, b); }

bool isGoHereRug(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    return o && !o->person && objdField(*o, 0x52) == 0x13 && o->data[objdata::Category] != 0xA7;
}

ClickedMenu objectClick(VM& vm, ObjectId sim, ObjectId object, std::optional<Position> ground, bool hasInteractions) {
    if (!isGoHereRug(vm, object))
        return {hasInteractions ? object : ObjectId(0), 0};
    const ObjectId dest = ground ? createGoHereDestination(vm, sim, ground->x, ground->y, ground->level) : 0;
    if (hasInteractions)
        return {object, dest};
    return {dest, 0};
}

void releaseDestination(VM& vm, ObjectId id) {
    if (Object* o = vm.object(id); o && o->managedDestination) {
        o->destinationHeld = false;
        flushDestinations(vm);
    }
}

void registerRoutingPrimitives(VM& vm) {
    vm.setPrimitive(0x10, findLocationFor);
    vm.setPrimitive(0x1B, goToRelativePosition);
    vm.setPrimitive(0x2D, goToRoutingSlot);
    vm.setPrimitive(0x71, snapInto);
}

} // namespace os2::vm
