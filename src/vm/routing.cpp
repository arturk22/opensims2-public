// Positions in the world: occupancy of the lot's tiles.
#include "vm.hpp"

#include "errors.hpp"
#include "interactions.hpp"

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
    if (!world->walkable(t))
        return false;
    // As blockedTiles(t.level, ignore)[index], for this one tile only.
    for (const Stair& s : world->stairs())
        for (const TilePos& l : {s.bottom, s.top})
            if (l.level == t.level && world->contains(l) && l.x == t.x && l.y == t.y)
                return true; // stair landings stay open
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
    for (const auto& [id, o] : vm.objects())
        if (o.position.level == level && multiTileGroup(o) != group && VM::blocksRouting(o) && VM::tileOf(o) == t)
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
    if (dirs == 0 || ox != 0 || oy != 0) {
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
        bool first = true;
        for (const SlotBone& b : bones) {
            const auto [px, py] = turn(b.x, b.y);
            if (first)
                out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
            first = false;
            addFacing(px, py, 0, (objDir + b.facing) % 8, b.index);
        }
        // Ours, not the binary's: points half a tile and three quarters out
        // around each bone's point, on the target's side of any wall, each
        // costing its distance more (1.5, 1.75: a later tier than the point
        // itself), for when the point cannot be reached (a chair pushed under
        // a table, which here fills its whole tile for want of the model's
        // footprint).
        if (world)
            for (const SlotBone& b : bones) {
                const auto [px, py] = turn(b.x, b.y);
                for (const float r : {0.5f, 0.75f})
                    for (int k = 0; k < 16; ++k) {
                        const double a = k * M_PI / 8;
                        const float qx = px + r * float(std::cos(a)), qy = py + r * float(std::sin(a));
                        const TilePos t{int(std::floor(qx)), int(std::floor(qy)), out.level};
                        if (!world->contains(t) || !world->lineOfSight(centre, t))
                            continue;
                        addFacing(qx, qy, 1 + r, (objDir + b.facing) % 8, b.index);
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
constexpr int kMaxBlockedTries = 5;
constexpr int kWaitForPersonTicks = 126;
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
std::optional<Point2> moveOutOfWayPoint(VM& vm, const Object& me, const Object& blocker) {
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
    for (int j = 0; j <= 96; ++j)
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
    // None: the wider slot of types 1/2 (§8.3: directions 1, 2, 6 and 7
    // relative to the blocker's facing, 1 to 6 tiles; who uses those types
    // is not traced, so this fallback is inferred).
    const int facing = ((blocker.data[objdata::Direction] % 8) + 8) % 8;
    for (int dist = 1; dist <= 6; ++dist)
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

// The destinations the engine made for "move out of the way" go once no
// Sim has their interaction queued or running any more (the binary keeps
// them as managed destination objects of the router and flushes them,
// cTSRoutingObjectAggregate::FlushManagedDestinationObjects BV 0x4f9eb4;
// when exactly is inferred). Once a tick.
void flushMoveOutOfWayDestinations(VM& vm) {
    static std::map<const VM*, uint64_t> lastTick;
    auto& last = lastTick[&vm];
    if (last == vm.ticks + 1)
        return;
    last = vm.ticks + 1;
    std::vector<ObjectId> done;
    for (const auto& [id, o] : vm.objects()) {
        if (!o.syntheticSlot || !o.definition || o.definition->guid() != kDestinationGuid)
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

// AskBlockerToMove → the blocker's MoveOutOfWay (docs/routing-mac.md §8.1):
// refused when the blocker is already asked or is blocked by me, unless its
// person data 159 has bit 0x02 its priority (person data 33) must be below
// mine less one (at least 1), my route must have more than one goal, we
// must be within 4 tiles and a place to step aside to must exist. Accepted:
// the move-out-of-way interaction goes on its queue with that priority.
bool askBlockerToMove(VM& vm, Object& me, const Object::Route& r, Object& blocker) {
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
    const auto spot = moveOutOfWayPoint(vm, me, blocker);
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
Step advance(Context& c, Object& me, RouteFail& why) {
    VM& vm = c.vm;
    auto& r = *me.route;
    // The interaction was cancelled: "interrupted".
    if (me.activeInteraction >= 0 && size_t(me.activeInteraction) < me.queue.size() &&
        me.queue[size_t(me.activeInteraction)].cancelled) {
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
    if (const ObjectId b = simAhead(vm, me, r)) {
        const Object& blocker = *vm.object(b);
        const bool stationary = !blocker.route || blocker.route->waiting;
        const float d = std::hypot(blocker.position.x - me.position.x, blocker.position.y - me.position.y);
        r.waiting = true;
        if (b != r.blocker) {
            r.blocker = b, r.replans = 0, r.blockedTicks = 0;
        }
        if (r.blockedTicks == 0) { // DoBlocked: one more try
            if (r.replans >= kMaxBlockedTries) {
                // Out of tries: "CT - Route Blocked" on the base route, then
                // "blocked by person" (what the tree's result changes is not
                // decoded; the game's returns true).
                if (!treeResult && c.thread.stack.size() == r.depth && runWalkTree(c, "CT - Route Blocked", {b, 0, 0, 0}))
                    return Step::Call;
                why = RouteFail::BlockedByPerson;
                return Step::Failed;
            }
            ++r.replans;
            if (stationary && r.replans <= kAskToMoveTries) {
                r.blockedTicks = 1;
                if (!treeResult)
                    askBlockerToMove(vm, me, r, *vm.object(b));
                if (!treeResult && runWalkTree(c, "CT - Route Ask To Move Animation", {b, 0, 0, 0}))
                    return Step::Call;
                return Step::Walking;
            }
            // A walking Sim (or asked enough): plan again around it.
            Object::Route next = r;
            RouteFail w;
            if (!planRoute(vm, me, next, w)) {
                why = w;
                return Step::Failed;
            }
            r.points = std::move(next.points);
            r.blockedTicks = 1;
            return Step::Walking;
        }
        // DoWaitingForPersonToMove: until it is 1.25 tiles away or the wait is over.
        if (++r.blockedTicks >= kWaitForPersonTicks || d > kPersonMovedAway)
            r.blockedTicks = 0;
        return Step::Walking;
    }
    if (r.blocker) { // the way is free again
        r.blocker = 0, r.blockedTicks = 0;
    }
    r.waiting = false;
    float budget = walkTilesPerTick(vm, me);
    const TilePos tileBefore = VM::tileOf(me);
    while (budget > 0 && !r.points.empty()) {
        const Object::Route::Point& p = r.points.front();
        if (p.portal)
            return Step::Walking; // its tree starts on the next tick
        if (p.ticks > 0) {
            if (r.climb.ticksLeft == 0) { // the phase starts
                moveTo(vm, me, p.x, p.y, p.level);
                setDirection(me, p.direction);
                r.climb.animation = p.animation;
                r.climb.z = p.z;
                r.climb.ticksLeft = p.ticks;
                ++r.climb.serial;
            }
            r.waiting = p.animation.empty();
            if (--r.climb.ticksLeft == 0) {
                r.points.erase(r.points.begin());
                if (r.points.empty() || r.points.front().ticks == 0)
                    r.climb.animation.clear();
            }
            return Step::Walking; // the tick is spent
        }
        const float dx = p.x - me.position.x, dy = p.y - me.position.y;
        const float d = std::hypot(dx, dy);
        if (d > 0)
            setDirection(me, directionOf(dx, dy));
        if (d <= budget) {
            moveTo(vm, me, p.x, p.y, p.level);
            budget -= d;
            r.points.erase(r.points.begin());
        } else {
            moveTo(vm, me, me.position.x + dx / d * budget, me.position.y + dy / d * budget, p.level);
            budget = 0;
        }
    }
    if (!r.points.empty()) {
        if (!(VM::tileOf(me) == tileBefore)) {
            r.lastCheck = vm.ticks;
            if (startGoalCheck(c, me, r) || startAlongRoute(c, me, r))
                return Step::Call;
        }
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
        std::fprintf(stderr, "route of %d failed with %d in \"%s\" #%u op %02X (stack object %d) at (%.2f, %.2f, %d)\n", me.id,
                     int(why), c.frame.behavior->fileName.c_str(), c.frame.pc, c.instruction.opcode, c.frame.stackObject, me.position.x,
                     me.position.y, me.position.level);
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
    if (c.frame.routeSlot >= 0) // mid-instruction: as read before the route's trees (they change the temps)
        index = c.frame.routeSlot;
    c.frame.routeSlot = int16_t(index);
    const Slots::Slot& slot = routingSlot(c.vm, *target, scope == 2, index);
    const ObjectId id = target->id;
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
// false. Not done: flags 0x02, 0x04, 0x10, 0x20 and 0x80 (what "prefer
// empty" weighs, editable tiles, level ground and borders need the terrain
// and the build mode's data).
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
        for (int i = 2; i < 34; ++i) {
            const int k = i / 2, sign = i % 2 ? -1 : 1;
            if (tryPlace(c.vm, *obj, {at.x + sign * sx * k, at.y + sign * sy * k, at.level},
                         (dir + allowedFacing(forbidden, 0)) % 8))
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
    for (int pass = 0; pass < (mode == 0 ? 2 : 1); ++pass)
        for (int r = 0; r <= 10; ++r)
            for (int x = at.x - r; x <= at.x + r; ++x)
                for (int y = at.y - r; y <= at.y + r; ++y) {
                    if (std::max(std::abs(x - at.x), std::abs(y - at.y)) != r)
                        continue;
                    const TilePos t{x, y, at.level};
                    if (pass == 0 && world.room(t) != room)
                        continue;
                    if (tryPlace(c.vm, *obj, t, facing))
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

void registerRoutingPrimitives(VM& vm) {
    vm.setPrimitive(0x10, findLocationFor);
    vm.setPrimitive(0x1B, goToRelativePosition);
    vm.setPrimitive(0x2D, goToRoutingSlot);
    vm.setPrimitive(0x71, snapInto);
}

} // namespace os2::vm
