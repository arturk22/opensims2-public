// Positions in the world: occupancy of the lot's tiles.
#include "vm.hpp"

#include "errors.hpp"

#include "formats/binreader.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

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

// Where the bone a routing slot names is, in the object's model space
// (tiles; +Y the model's forward axis, Z up), if the object's model has it.
// The Sims 2's SLOT entries name a bone of the model by index into the
// object's STR# 0x90 ("Model - Bones"), in the first u32 after the Sims 1
// fields (Slots::Slot::ts2Extra[0]), as the containment slots do
// (app/vm_objects.cpp); a slot with flag 0x20000 stands at that bone (the
// Mac binary's RoutingSlotOffset leaves such slots out of the offset it
// computes from the slot's own fields). Inferred.
std::optional<std::array<float, 3>> VM::slotBone(const Slots::Slot& slot, const Object& target) {
    constexpr uint16_t kModelBoneNames = 0x90;
    constexpr uint32_t kTypeCresId = 0xE519C933, kTypeObjdId = 0x4F424A44;
    if (!target.definition || !code().index())
        return std::nullopt;
    const StringTable* names = code().strings(target.group, kModelBoneNames);
    if (!names || slot.ts2Extra[0] >= names->count(lang::EnglishUS))
        return std::nullopt;
    std::string bone = names->get(slot.ts2Extra[0], lang::EnglishUS);
    if (const size_t comma = bone.find(','); comma != std::string::npos)
        bone.resize(comma);
    if (bone.empty())
        return std::nullopt;
    // Skeletons by OBJD, read once.
    static std::map<std::pair<uint32_t, uint16_t>, std::shared_ptr<Skeleton>> skeletons;
    const auto key = std::pair{target.group, target.definitionInstance};
    auto it = skeletons.find(key);
    if (it == skeletons.end()) {
        std::shared_ptr<Skeleton> sk;
        try {
            ResourceIndex& index = *code().index();
            const std::string cres = objectCresName(index, Tgi{kTypeObjdId, target.group, target.definitionInstance, 0});
            if (auto loc = index.findByName(cres + "_cres", kTypeCresId))
                sk = std::make_shared<Skeleton>(Skeleton::fromCres(Rcol(index.read(*loc))));
        } catch (const std::exception&) {
        }
        it = skeletons.emplace(key, sk).first;
    }
    if (!it->second)
        return std::nullopt;
    const int b = it->second->find(bone);
    if (b < 0)
        return std::nullopt;
    const Vec3 t = it->second->world(it->second->restPose())[size_t(b)].t;
    return std::array<float, 3>{t[0], t[1], t[2]};
}

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

    float ox = slot.offset[0], oy = slot.offset[1];
    if (dirs == 0 || ox != 0 || oy != 0) {
        // A slot at a bone of the model (flag 0x20000 of the u32 after the
        // proximities, Slots::Slot::unknown10) stands there.
        bool atBone = false;
        if ((slot.unknown10 & 0x20000) && ox == 0 && oy == 0)
            if (const auto bone = slotBone(slot, target))
                ox = (*bone)[0], oy = (*bone)[1], atBone = true;
        const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
        const float px = cx + float(ox * std::cos(yaw) - oy * std::sin(yaw));
        const float py = cy + float(ox * std::sin(yaw) + oy * std::cos(yaw));
        out.goals.push_back({int(std::floor(px)), int(std::floor(py)), 0});
        out.points.push_back({px, py});
        out.facings.push_back(facingAt(px, py));
        out.onTarget = out.goals.back().x == centre.x && out.goals.back().y == centre.y;
        // Around a bone's point, for when something stands on it (a chair
        // pushed under a table): points half a tile and three quarters out,
        // on the target's side of any wall, each costing its distance more
        // (approximation of the binary's goal scan, cTSRoute::ScanCone /
        // EvalTileForGoal, which is not decoded).
        if (atBone && world)
            for (const float r : {0.5f, 0.75f})
                for (int k = 0; k < 16; ++k) {
                    const double a = k * M_PI / 8;
                    const float qx = px + r * float(std::cos(a)), qy = py + r * float(std::sin(a));
                    const TilePos t{int(std::floor(qx)), int(std::floor(qy)), out.level};
                    if (!world->contains(t) || !world->lineOfSight(centre, t))
                        continue;
                    out.goals.push_back({t.x, t.y, 1 + r});
                    out.points.push_back({qx, qy});
                    out.facings.push_back(facingAt(qx, qy));
                }
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

// Waits and limits of the blocked walker, from the Mac binary's
// cTSRoutingObjectAggregate (docs/routing-mac.md §6): a stationary Sim in
// the way is asked to move up to 3 times and waited for up to 126 ticks
// each time (re-routing when it gets 1.25 tiles away); a walking one is
// routed around; after 5 tries the route fails with "blocked by person".
constexpr int kAskToMoveTries = 3;
constexpr int kMaxBlockedTries = 5;
constexpr int kWaitForPersonTicks = 126;
constexpr float kPersonMovedAway = 1.25f;
// When the walker counts as blocked: another Sim's centre within this of
// the next stretch of the path (the binary's locomotor test, FollowOneStep,
// is not decoded; approximation: a Sim's 0.25-tile core plus a margin).
constexpr float kBlockedReach = 0.35f, kLookAhead = 0.5f;

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
    const std::string age = agePrefix(me);
    for (const std::string& candidate : {age + "-" + name, "a-" + name})
        if (const auto secs = vm.animations.seconds(candidate))
            return {candidate, std::max(1, int(std::ceil(*secs * float(vm.clock.ticksPerMinute))))};
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

// Ticks a walker waits at a door's portal for the door to open: the door's
// open animation (approximation: the game runs the door's portal tree,
// OBJf entry 15, which plays it and the Sim's walk through; 15 ticks when
// the animation is not known).
int doorWaitTicks(VM& vm) {
    if (const auto secs = vm.animations.seconds("o-door-open-CW"))
        return std::clamp(int(std::ceil(*secs * float(vm.clock.ticksPerMinute))), 1, 45);
    return 15;
}

// Plans the route's points from where the walker stands to its goals
// (r.goals on r.level), around the objects and the other Sims.
bool planRoute(VM& vm, Object& me, Object::Route& r, RouteFail& why) {
    PathPlanner& planner = plannerOf(vm);
    addObstacles(vm, planner, me, r.ignore);
    std::vector<PlanGoal> goals;
    for (const auto& g : r.goals)
        goals.push_back({g[0], g[1], g[2]});
    PlannedPath path;
    if (!planner.plan({me.position.x, me.position.y}, me.position.level, r.level, goals, path, why))
        return false;
    r.points.clear();
    int level = me.position.level;
    Point2 prev{me.position.x, me.position.y};
    for (const auto& wp : path.points) {
        Object::Route::Point p = walkPoint(wp.x, wp.y, wp.level);
        if (wp.portal >= 0) {
            const PathPlanner::Portal& portal = planner.portals()[size_t(wp.portal)];
            if (portal.stair >= 0) {
                appendClimb(vm, me, planner.world().stairs()[size_t(portal.stair)], wp.level > level, r.points);
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
    return true;
}

// Another Sim in the way of the next stretch of the route, or 0.
ObjectId simAhead(VM& vm, const Object& me, const Object::Route& r) {
    if (r.points.empty() || r.points.front().ticks > 0)
        return 0;
    // The stretch: up to kLookAhead along the points from here.
    std::vector<Point2> line{{me.position.x, me.position.y}};
    float left = kLookAhead;
    for (const auto& p : r.points) {
        if (p.ticks > 0 || p.level != me.position.level || left <= 0)
            break;
        const Point2 a = line.back();
        const float d = std::hypot(p.x - a[0], p.y - a[1]);
        if (d >= left) {
            line.push_back({a[0] + (p.x - a[0]) * left / d, a[1] + (p.y - a[1]) * left / d});
            break;
        }
        line.push_back({p.x, p.y});
        left -= d;
    }
    if (line.size() < 2)
        return 0;
    for (const auto& [id, o] : vm.objects()) {
        if (id == me.id || !o.person || !o.position.inWorld() || o.data[objdata::ContainerId] != 0 ||
            o.position.level != me.position.level)
            continue;
        const float ox = o.position.x, oy = o.position.y;
        // Overlapping already (both on one spot): walking off is the way out.
        if (std::hypot(ox - me.position.x, oy - me.position.y) < 0.3f)
            continue;
        for (size_t i = 1; i < line.size(); ++i) {
            const float ax = line[i - 1][0], ay = line[i - 1][1], bx = line[i][0], by = line[i][1];
            const float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
            float u = len2 > 0 ? ((ox - ax) * vx + (oy - ay) * vy) / len2 : 0;
            if (i == 1 && u <= 0)
                continue; // behind the walker
            u = std::clamp(u, 0.0f, 1.0f);
            if (std::hypot(ax + vx * u - ox, ay + vy * u - oy) < kBlockedReach)
                return id;
        }
    }
    return 0;
}

enum class Step { Walking, Arrived, Failed, Call };

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
    const std::optional<bool> treeResult = std::exchange(c.thread.rerunResult, std::nullopt);
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
    while (budget > 0 && !r.points.empty()) {
        const Object::Route::Point& p = r.points.front();
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
    if (!r.points.empty())
        return Step::Walking;
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
Exit failRoute(Context& c, RouteFail why, ObjectId blocker, bool failureTrees) {
    Object& me = c.me();
    me.route.reset();
    if (why == RouteFail::Success)
        why = RouteFail::Unknown;
    if (failureTrees) {
        if (const auto tree = c.vm.code().behaviorByName(kGlobalGroup, "CT - Route Failure Feedback")) {
            c.frame.pendingRouteResult = int16_t(why);
            c.vm.call(c.thread, me.id, *tree, {int16_t(why), blocker, 1, 0});
            c.thread.stack.back().rerunCaller = true;
            return Exit::Call;
        }
    }
    if (me.person)
        me.person->data[62] = int16_t(why);
    return Exit::False;
}

// Walks the thread's object to one of `goals` (planned on the first run of
// the instruction): yields every tick while walking, true on arrival,
// false when no goal can be reached (after the failure feedback). Routing
// out of the world is the game's own error. A goal on another level is
// reached by the stairs.
Exit walk(Context& c, ObjectId target, const std::function<VM::RouteGoals()>& plan, bool failureTrees) {
    Object& me = c.me();
    const size_t depth = c.thread.stack.size();
    if (c.frame.pendingRouteResult >= 0) { // back from the failure feedback
        c.thread.rerunResult.reset();
        if (me.person)
            me.person->data[62] = c.frame.pendingRouteResult;
        c.frame.pendingRouteResult = -1;
        return Exit::False;
    }
    if (me.route && me.route->depth == depth && me.route->tree == c.frame.behavior && me.route->node == c.frame.pc) {
        RouteFail why = RouteFail::Success;
        const ObjectId blocker = me.route->blocker;
        switch (advance(c, me, why)) {
        case Step::Walking: return Exit::Yield;
        case Step::Call: return Exit::Call;
        case Step::Arrived:
            if (me.person)
                me.person->data[62] = 0;
            return Exit::True;
        case Step::Failed: return failRoute(c, why, blocker, me.route ? me.route->failureTrees : failureTrees);
        }
    }
    me.route.reset();
    worldOf(c);
    if (!me.position.inWorld())
        throw VMError("Trying to route while out of world.");
    const VM::RouteGoals goals = plan();
    if (goals.goals.empty())
        return failRoute(c, RouteFail::NoValidGoals, 0, failureTrees);
    Object::Route r;
    for (size_t i = 0; i < goals.goals.size(); ++i)
        r.goals.push_back({goals.points[i][0], goals.points[i][1], goals.goals[i].cost});
    r.facings = goals.facings;
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
    if (me.data[objdata::ContainerId] != 0)
        leaveSlot(me);
    me.route = std::move(r);
    RouteFail later = RouteFail::Success;
    switch (advance(c, me, later)) {
    case Step::Walking: return Exit::Yield;
    case Step::Call: return Exit::Call;
    case Step::Arrived:
        if (me.person)
            me.person->data[62] = 0;
        return Exit::True;
    case Step::Failed: break;
    }
    return failRoute(c, later, 0, failureTrees);
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

// The routing person data the trees read after a Go To Routing Slot (labels
// of STR# 0xC8): 37 "route slot" (the routing slot number), 38 "route multi
// slot index" (which of the places the slot gives was taken; "Sofa -
// Animate - Get In" passes it on as the seat; here always 0: the goals of a
// slot are not told apart yet) and 40 "route goal" (the object). Inferred
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
    const int index = scope == 0 ? c.get(kOwnerParam, int16_t(data))
                    : scope == 1 && (flags & 0x02) ? c.thread.temps[0]
                                                    : int(data);
    const Slots::Slot& slot = routingSlot(c.vm, *target, scope == 2, index);
    const ObjectId id = target->id;
    noteRouteSlot(c.me(), index, id);
    const bool ignoreFootprint = flags & 0x04;
    return walk(
        c, id,
        [&] {
            VM::RouteGoals g = c.vm.routingGoals(slot, *c.vm.object(id));
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
    const ObjectId id = target->id;
    return walk(c, id, [&] { return c.vm.routingGoals(slot, *c.vm.object(id)); }, !(o[6] & 0x02));
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
    if (mode == 1) { // out of world
        obj->position.x = obj->position.y = -1;
        obj->route.reset();
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
        const int dir = ((ref->data[objdata::Direction] % 8) + 8) % 8 + (mode == 4 ? 2 : 0);
        const auto [sx, sy] = directionStep(dir % 8);
        for (int i = 0; i < 32; ++i) {
            const int k = i / 2, sign = i % 2 ? -1 : 1;
            if (i % 2 && k == 0)
                continue;
            if (tryPlace(c.vm, *obj, {at.x + sign * sx * k, at.y + sign * sy * k, at.level},
                         allowedFacing(forbidden, dir % 8)))
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
