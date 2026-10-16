// Planning a way (part of the routing, routing.cpp): the lot's path planner
// and its obstacles, the climb of a stair, the portal trees (doors, stairs,
// the objects' portals), the turns in place and the walk shaped from the
// planned way (planRoute).
#include "vm.hpp"

#include "locomotion.hpp"
#include "routing_internal.hpp"
#include "sim_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>

namespace os2::vm {

using namespace routing_internal;

namespace routing_internal {

PathPlanner& plannerOf(VM& vm) {
    World& w = *vm.world;
    if (!vm.planner || !vm.planner->builtFrom(w))
        vm.planner = std::make_shared<PathPlanner>(w);
    return *vm.planner;
}

} // namespace routing_internal

namespace {

Object::Route::Point walkPoint(float x, float y, int level) {
    Object::Route::Point p;
    p.x = x, p.y = y, p.level = level;
    return p;
}

bool smallSim(const Object& o) {
    const std::string a = agePrefix(o);
    return a == "c" || a == "p";
}

} // namespace

namespace routing_internal {

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
    for (const auto& [id, o] : vm.eachObject()) {
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
        if (landing)
            continue;
        // Its footprint's rectangles (the model's, in 1/8 tile cells, or the
        // OBJD box), each inflated by the planner as AddObjectRectangle does.
        // OS2_TILE_FOOTPRINTS=1 puts back the whole tile, to compare.
        static const bool wholeTiles = std::getenv("OS2_TILE_FOOTPRINTS") != nullptr;
        if (wholeTiles) {
            planner.addObstacle(t.level, float(t.x), float(t.y), float(t.x + 1), float(t.y + 1), PathPlanner::kObject);
            continue;
        }
        for (const auto& r : vm.routingFootprint(o))
            if (r[0] < r[2] && r[1] < r[3])
                planner.addObstacle(o.position.level, r[0], r[1], r[2], r[3], PathPlanner::kObject);
    }
}

} // namespace routing_internal

namespace {

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
    for (const auto& [id, o] : vm.eachObject())
        if (o.definition && o.definition->objectType() == kObjectTypeDoor && o.position.inWorld() &&
            VM::tileOf(o) == d.tile)
            return id;
    return 0;
}

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
    for (const auto& [id, o] : vm.eachObject()) {
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
    for (const auto& [id, o] : vm.eachObject()) {
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
// the levels of the two objects (TSObjectPortalLockedCB, BV 0xe208c8: each
// object's vtable 0x90, the level; read in round 15 — before, the rooms:
// the pool ladder's and the diving board's trees go into the pool when
// parameter 2 > parameter 3, and the board's TEST locks the way up), 4/5
// their IDs, 6/7 the routing slots of
// the two ends (the door's "Function - Portal" picks its side and the
// opening animation from parameter 6), 8 the gait (1 walking, 2 running)
// and 9 a constant of the binary (0 here). The stack object is the entry
// object. A stair's portal made of its pieces' slots (refreshObjectPortals)
// passes each end's slot; the old geometric one (a stair without such
// objects) passes slot 0 (inferred).
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
    } else if (p.object >= 0) {
        // An object's portal (a stair's lane, the pool ladder): the parts
        // whose slots make its two ends, and those slots
        // (PortalAddToPathPlannerImpl passes each end's object and slot).
        const World::ObjectPortal& op = planner.world().objectPortals()[size_t(p.object)];
        if (!vm.object(op.partA) || !vm.object(op.partB))
            return std::nullopt;
        u.entry = op.partA, u.exit = op.partB;
        slotA = op.slotA, slotB = op.slotB;
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
    u.params = {0, 0, int16_t(a.position.level), int16_t(b.position.level), int16_t(u.entry), int16_t(u.exit),
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

} // namespace

namespace routing_internal {

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

} // namespace routing_internal

namespace {

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
    float skip = 0; // the cut points the moved first point is past (inferred: not walked back to)
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
            for (const auto& [id, o] : vm.eachObject())
                if (clear && id != me.id && o.person && o.position.inWorld() && o.position.level == level &&
                    o.data[objdata::ContainerId] == 0 &&
                    std::hypot(o.position.x - q[0], o.position.y - q[1]) <
                        (smallSim(me) ? 0.1875f : 0.25f) + (smallSim(o) ? 0.1875f : 0.25f) + kStepTiles * kHorizonSteps)
                    clear = false;
            if (clear) {
                out.push_back(walkPoint(q[0], q[1], level));
                // The first leg is cut from the moved point when that leg is
                // as clear as the planned one; else (inferred) from where the
                // Sim stands, so that the walk comes back onto the way the
                // planner kept clear of objects: N001_Lot68's invited visitor
                // turned and walked a leg beside her planned one into the
                // mailbox's footprint, and her knock on the door failed with
                // "blocked by object".
                bool legClear = true;
                const float qd = std::hypot(first.x - q[0], first.y - q[1]);
                for (float u = 0; legClear && u < qd; u += 0.1f)
                    legClear = plannerOf(vm).free(level, {q[0] + (first.x - q[0]) * u / qd, q[1] + (first.y - q[1]) * u / qd});
                if (legClear)
                    at = q;
                else
                    skip = lead;
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
                if (u * len < skip)
                    continue;
                out.push_back(walkPoint(at[0] + (p.x - at[0]) * u, at[1] + (p.y - at[1]) * u, level));
            }
        }
        out.push_back(p);
        at = {p.x, p.y};
        level = p.level;
        skip = 0;
    }
    r.points = std::move(out);
    if (!floorFirst || moving || steps == 0 || locoClip(vm, me, "turnAndGo", steps))
        return;
    if (auto t = turnPoint(vm, me, steps))
        r.points.insert(r.points.begin(), *t);
}

} // namespace

namespace routing_internal {

void notePlace(Object::Route& r, const Object& who) {
    for (auto& [id, at] : r.places)
        if (id == who.id) {
            at = {who.position.x, who.position.y};
            return;
        }
    r.places.push_back({who.id, {who.position.x, who.position.y}});
}

bool planRoute(VM& vm, Object& me, Object::Route& r, RouteFail& why) {
    // InitRoute (BV 0x5737cc) notes where every router of the route manager
    // stands (SetRouterLastKnownLocation), the walker among them: what
    // DoBlockedGoalStillAvailable compares (docs/routing-mac.md §9.3).
    r.places.clear();
    for (const ObjectId id : vm.people())
        if (const Object* o = vm.object(id); o && o->position.inWorld())
            notePlace(r, *o);
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
                if (planner.portalLocked(i, forward)) {
                    const auto& p = planner.portals()[i];
                    std::fprintf(stderr, "  portal %zu (%s) (%.2f, %.2f, %d) - (%.2f, %.2f, %d) locked %s for %d\n", i,
                                 p.door >= 0 ? "door" : p.object >= 0 ? "object" : "stair", p.a[0], p.a[1], p.levelA, p.b[0],
                                 p.b[1], p.levelB, forward ? "a->b" : "b->a", me.id);
                }
    if (!planned && std::getenv("OS2_ROUTE_FAILS_MAP")) { // the planner's cells around the walker, half a tile each
        const int lv = me.position.level;
        std::fprintf(stderr, "  map of %d at (%.2f, %.2f, %d), goals on level %d:", me.id, me.position.x, me.position.y, lv, r.level);
        for (const auto& g : r.goals)
            std::fprintf(stderr, " (%.2f,%.2f)", g[0], g[1]);
        std::fprintf(stderr, " (why %d)\n", int(why));
        for (const auto& [oid, o] : vm.eachObject()) // the objects whose footprint covers a goal
            if (!o.person && o.position.inWorld() && o.position.level == r.level && o.data[objdata::ContainerId] == 0)
                for (const auto& f : vm.routingFootprint(o))
                    for (const auto& g : r.goals)
                        if (f[0] - 0.15f < g[0] && g[0] < f[2] + 0.15f && f[1] - 0.15f < g[1] && g[1] < f[3] + 0.15f) {
                            std::fprintf(stderr, "    goal (%.2f,%.2f) near %d %s footprint (%.2f,%.2f)-(%.2f,%.2f) blocks %d\n", g[0], g[1], oid,
                                         o.definition ? o.definition->fileName.c_str() : "?", f[0], f[1], f[2], f[3], int(VM::blocksRouting(o)));
                            break;
                        }
        for (const auto& [oid, o] : vm.eachObject()) // and the blocking objects within 2 tiles of the walker
            if (!o.person && o.position.inWorld() && o.position.level == lv && o.data[objdata::ContainerId] == 0 &&
                VM::blocksRouting(o) && std::fabs(o.position.x - me.position.x) < 2.5f &&
                std::fabs(o.position.y - me.position.y) < 2.5f)
                for (const auto& f : vm.routingFootprint(o))
                    std::fprintf(stderr, "    near: %d %s footprint (%.2f,%.2f)-(%.2f,%.2f)\n", oid,
                                 o.definition ? o.definition->fileName.c_str() : "?", f[0], f[1], f[2], f[3]);
        // OS2_ROUTE_FAILS_MAP=N: N half tiles each way up and down (12), N * 4 / 3 across.
        const int ry = std::max(12, std::atoi(std::getenv("OS2_ROUTE_FAILS_MAP"))), rx = ry * 4 / 3;
        for (int qy = int(me.position.y * 2) + ry; qy >= int(me.position.y * 2) - ry; --qy) {
            std::fprintf(stderr, "  %5.1f ", qy / 2.0);
            for (int qx = int(me.position.x * 2) - rx; qx <= int(me.position.x * 2) + rx; ++qx) {
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
    if (const char* d = std::getenv("OS2_PLAN_DEBUG"); d && std::atoi(d) >= 3) { // the way planned
        std::fprintf(stderr, "t%llu way of %d from (%.2f, %.2f, %d):", (unsigned long long)vm.ticks, me.id, me.position.x,
                     me.position.y, me.position.level);
        for (const auto& wp : path.points)
            std::fprintf(stderr, " (%.2f, %.2f, %d%s)", wp.x, wp.y, wp.level, wp.portal >= 0 ? " portal" : "");
        std::fprintf(stderr, "\n");
    }
    r.facing = r.facings.empty() ? -1 : r.facings[path.goal];
    // Person data 38 "route multi slot index": which of a bone slot's places
    // the way goes to (the goal's +0x14 in the binary; "Sofa - Animate - Get
    // In" reads it as the seat, the dining chairs as the side to sit from).
    if (me.person && path.goal < r.multi.size())
        me.person->data[38] = r.multi[path.goal];
    // The goal's chair, for the post-route tree (a sitting goal; 0 standing).
    r.chair = path.goal < r.chairs.size() ? r.chairs[path.goal] : 0;
    shapeWalk(vm, me, r, me.route.has_value() && !me.route->waiting && me.route->climb.ticksLeft == 0);
    ++r.plans; // a new path (cTSRoute +0xd8: DoAlongRouteCheckTest checks again)
    return true;
}

} // namespace routing_internal

const std::vector<int16_t>* accessibleRooms(VM& vm, int16_t room) {
    if (!vm.world)
        return nullptr;
    const auto& rooms = plannerOf(vm).accessibleRooms(room);
    return rooms.empty() ? nullptr : &rooms;
}

uint32_t plannerNoise(VM& vm) {
    return vm.world && vm.planner && vm.planner->builtFrom(*vm.world) ? vm.planner->noise() : 1;
}

void restorePlannerNoise(VM& vm, uint32_t noise) {
    if (vm.world)
        plannerOf(vm).seed(noise);
}

// The portals objects make of their routing slots, as the binary gathers
// them (docs/routing-mac.md §8.5, §8.6):
// - cTSObject::PortalFillInSlotInfoList (BV 0xe21a18; a multi-tile object's
//   walks its parts into the lead's list, 0xaf17f4) gathers the routing
//   slots of the object, and a modular stair's "Portal - A" (type 12) also
//   those of the stair's other end (PortalGetOtherEndOfStairs, BV 0xe21144:
//   the last type 11/12 member of its object group, the "... - End"; here
//   the end of object data 80 "Get - Next (down)", as stairSteps finds it).
// - PortalGatherSlotInformation (BV 0xe1ff3c) names each slot by
//   ModelBoneName (BV 0xafbb30: only a slot with 0x20000 in its slot flags,
//   Slots::Slot::unknown10; the slot's STR# 0x90 entry "own,other"; a name
//   already gathered keeps the first) and makes a connection to another
//   name: a portal pair (cITSRoutingSlot::IsPortalPair, RS flag 0x200000,
//   confirmed in round n03) to its second name; any other slot of a door,
//   straight stair, modular stair or its portal (types 8, 10, 11, 12) to
//   the name TSObjectAutoPortalConnectionNameGenerator (BV 0xe1fa5c) makes
//   by swapping the suffix (_n/_s, _e/_w, _nw/_se, _ne/_sw, _top/_bottom).
// - PortalConnectSlotInformation (BV 0xe1f878) joins each connection to the
//   slot with that name and drops the ones without.
// - PortalAddToPathPlannerImpl (BV 0xe22a44): a connection becomes a portal
//   only when its first slot's object has the portal function (OBJf entry
//   15); a stair's (types 10, 11, 12) runs from the end whose slot is
//   number 0 and is one-way (cPortal +0x61, IsOneWay, BV 0xba2f04), two-way
//   when neither end is slot 0; any other object's (the pool ladder, the
//   diving board) runs from its first slot to its second, one-way. Doors go
//   by AddDoorPortal (here World::doors), so not here.
// Each base-game stair piece carries two lanes of slots, "_0_*" and "_1_*"
// a third of a tile apart: slot 0 is "_0_top" on the top piece and
// "_1_bottom" on the bottom one, so "_0" is the lane down and "_1" the lane
// up, and Sims going up and down do not meet. The portal of a stair keeps
// the World::stairs entry it climbs (for the engine's own climb, when the
// portal tree cannot run), which then makes no portal of its own.
// The pool ladder's and diving board's two slots name each other: two
// one-way connections, kept here as one two-way portal.
void VM::refreshObjectPortals() {
    if (!world)
        return;
    constexpr uint32_t kPortalPair = 0x200000;
    constexpr int32_t kNamedSlot = 0x20000;
    constexpr uint16_t kModelBoneNames = 0x90;
    struct Info {
        ObjectId owner, part;
        int16_t slot;
        uint16_t type; // of the part
        int level;
        std::array<float, 2> at;
        std::string own;
    };
    struct Connection {
        size_t from;
        std::string to;
    };
    std::vector<Info> infos;
    std::vector<Connection> connections;
    auto lower = [](std::string b) {
        while (!b.empty() && b.front() == ' ')
            b.erase(b.begin());
        while (!b.empty() && b.back() == ' ')
            b.pop_back();
        for (char& c : b)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return b;
    };
    // TSObjectAutoPortalConnectionNameGenerator: the other end's name, or "".
    auto otherName = [](const std::string& name) -> std::string {
        static const std::pair<const char*, const char*> kSwap[] = {
            {"_n", "_s"},   {"_s", "_n"},   {"_e", "_w"},    {"_w", "_e"},     {"_nw", "_se"},
            {"_se", "_nw"}, {"_ne", "_sw"}, {"_sw", "_ne"}, {"_top", "_bottom"}, {"_bottom", "_top"}};
        for (const auto& [from, to] : kSwap) {
            const size_t n = std::strlen(from);
            if (name.size() >= n && name.compare(name.size() - n, n, from) == 0)
                return name.substr(0, name.size() - n) + to;
        }
        return {};
    };
    auto gather = [&](ObjectId owner, const Object& o) {
        const uint16_t type = o.definition->objectType();
        const auto slots = routingSlots(slotTable(*this, o));
        const StringTable* names = code().strings(o.group, kModelBoneNames);
        for (size_t k = 0; k < slots.size(); ++k) {
            const Slots::Slot& slot = *slots[k];
            if (!(slot.unknown10 & kNamedSlot) || !names || slot.ts2Extra[0] >= names->count(lang::EnglishUS))
                continue;
            std::vector<std::string> list;
            const std::string text = names->get(slot.ts2Extra[0], lang::EnglishUS);
            for (size_t start = 0; start <= text.size();) {
                size_t end = text.find(',', start);
                if (end == std::string::npos)
                    end = text.size();
                list.push_back(lower(text.substr(start, end - start)));
                start = end + 1;
            }
            if (list.empty() || list[0].empty())
                continue;
            const bool pair = slot.flags & kPortalPair;
            std::string to;
            if (pair)
                to = list.size() > 1 ? list[1] : std::string();
            else if (type == kObjectTypeDoor || type == kObjectTypeStairs || type == kObjectTypeModularStairs ||
                     type == kObjectTypeModularStairsPortal)
                to = otherName(list[0]);
            else
                continue;
            // A name already gathered for this owner keeps its first slot.
            auto found = std::find_if(infos.begin(), infos.end(),
                                      [&](const Info& x) { return x.owner == owner && x.own == list[0]; });
            size_t index = size_t(found - infos.begin());
            if (found == infos.end()) {
                const RouteGoals g = routingGoals(slot, o);
                if (g.points.empty())
                    continue;
                infos.push_back({owner, o.id, int16_t(k), type, o.position.level, g.points[0], list[0]});
                index = infos.size() - 1;
            } else if (!pair) {
                continue;
            }
            if (!to.empty())
                connections.push_back({index, to});
        }
    };
    for (const auto& [id, o] : objects_) {
        if (o.person || !o.definition || !o.position.inWorld() || o.data[objdata::ContainerId] != 0)
            continue;
        const uint16_t type = o.definition->objectType();
        if (type == kObjectTypeDoor || type == kObjectTypeModularStairs)
            continue; // doors: World::doors; a modular stair's pieces: through its portal piece
        // OS2_STAIR_ONE_PORTAL=1: the stairs' old portal instead (one, both
        // ways, landing to landing), to compare.
        static const bool onePortal = std::getenv("OS2_STAIR_ONE_PORTAL") != nullptr;
        if (onePortal && (type == kObjectTypeStairs || type == kObjectTypeModularStairsPortal))
            continue;
        const ObjectId owner = multiTileGroup(o);
        gather(owner, o);
        if (type == kObjectTypeModularStairsPortal) {
            ObjectId end = id;
            for (int i = 0; i < 64; ++i) {
                const Object* x = object(end);
                const ObjectId next = x ? x->data[80] : 0;
                if (next <= 0 || !object(next))
                    break;
                end = next;
            }
            const Object* e = end != id ? object(end) : nullptr;
            if (e && e->definition &&
                (e->definition->objectType() == kObjectTypeModularStairs ||
                 e->definition->objectType() == kObjectTypeModularStairsPortal))
                gather(owner, *e);
        }
    }
    // The stair a stair portal climbs: the World::stairs entry between the
    // two levels whose bottom landing is nearest the lower end.
    auto stairOf = [&](const Info& lo, const Info& hi) {
        int best = -1;
        float bestD = 1.5f;
        const auto& stairs = world->stairs();
        for (size_t s = 0; s < stairs.size(); ++s) {
            if (stairs[s].bottom.level != lo.level || stairs[s].top.level != hi.level)
                continue;
            const auto b = stairs[s].at(0), t = stairs[s].at(stairs[s].length());
            const float d = std::hypot(b[0] - lo.at[0], b[1] - lo.at[1]) + std::hypot(t[0] - hi.at[0], t[1] - hi.at[1]);
            if (d < bestD * 2)
                best = int(s), bestD = d / 2;
        }
        return best;
    };
    std::vector<World::ObjectPortal> out;
    for (const Connection& c : connections) {
        const Info& a = infos[c.from];
        size_t to = infos.size();
        for (size_t j = 0; j < infos.size(); ++j)
            if (j != c.from && infos[j].owner == a.owner && infos[j].own == c.to) {
                to = j;
                break;
            }
        if (to == infos.size())
            continue;
        const Object* first = object(a.part);
        if (!first || !functionTree(*first, 15))
            continue;
        const Info& b = infos[to];
        const Info *from = &a, *dest = &b;
        bool oneWay = true;
        const bool stair = a.type == kObjectTypeStairs || a.type == kObjectTypeModularStairs ||
                           a.type == kObjectTypeModularStairsPortal;
        if (stair) {
            if (a.slot != 0 && b.slot == 0)
                std::swap(from, dest);
            else if (a.slot != 0)
                oneWay = false;
        }
        World::ObjectPortal p{from->level, dest->level, from->at,    dest->at,    from->part,
                              dest->part,  from->slot,  dest->slot,  oneWay};
        if (stair && from->level != dest->level)
            p.stair = from->level < dest->level ? stairOf(*from, *dest) : stairOf(*dest, *from);
        // The same portal again (a straight stair's two pads both have the
        // portal function): once. The other way of a one-way one: two-way.
        bool merged = false;
        for (World::ObjectPortal& q : out) {
            if (q.partA == p.partA && q.slotA == p.slotA && q.partB == p.partB && q.slotB == p.slotB) {
                q.oneWay = q.oneWay && p.oneWay;
                merged = true;
            } else if (q.partA == p.partB && q.slotA == p.slotB && q.partB == p.partA && q.slotB == p.slotA) {
                q.oneWay = false;
                merged = true;
            }
            if (merged)
                break;
        }
        if (!merged)
            out.push_back(p);
    }
    if (std::getenv("OS2_OBJECT_PORTALS"))
        for (const auto& p : out)
            std::fprintf(stderr, "object portal %d slot %d (%.2f, %.2f, %d) %s %d slot %d (%.2f, %.2f, %d) stair %d\n", p.partA,
                         p.slotA, p.a[0], p.a[1], p.levelA, p.oneWay ? "->" : "<->", p.partB, p.slotB, p.b[0], p.b[1], p.levelB,
                         p.stair);
    world->setObjectPortals(std::move(out));
}

} // namespace os2::vm
