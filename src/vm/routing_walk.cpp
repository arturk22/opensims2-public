// The routing primitives (Go To Routing Slot, Go To Relative Position, Find
// Location For, Snap Into) and the walk they run, tick by tick (part of the
// routing, routing.cpp): the route's callback trees (goal check, along
// route, pre- and post-route, failure feedback), what a blocked walker does
// and the end of the route.
#include "vm.hpp"

#include "errors.hpp"
#include "go_here.hpp"
#include "interactions.hpp"
#include "locomotion.hpp"
#include "placement.hpp"
#include "routing_internal.hpp"
#include "sim_motion.hpp"

#include "formats/binreader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

// --- Primitives --------------------------------------------------------------

namespace os2::vm {

using namespace routing_internal;

namespace {

constexpr uint8_t kOwnerParam = 0x09;
constexpr uint8_t kOwnerLocal = 0x19;
constexpr uint16_t kGlobalSlots = 0x64; // the global SLOT (as FreeSO's global.iff SLOT 100)

World& worldOf(Context& c) {
    if (!c.vm.world)
        throw VMError("no world (positions need a lot)");
    return *c.vm.world;
}

} // namespace

namespace routing_internal {

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

} // namespace routing_internal

namespace {

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
        for (const auto& [id, _] : vm.eachObject())
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
        // DoInitial and DoFoundPath are states of their own: the walk goes
        // on from the next tick, not in the pause's tick (where the same
        // blocker, not moved yet, counted the second try at once and failed
        // the route: a tree that routes again on failure, "Route To Can"
        // with "CT - Route Failure Feedback", went round hundreds of times
        // in one tick).
        r.waiting = false;
        return Step::Walking;
    }
    // Waiting for the Sim asked to move (DoWaitingForPersonToMove) goes on
    // whether or not it is still in the way, until it is 1.25 tiles off or
    // the time is up.
    const bool waitingForPerson = r.blockedTicks > 0 && r.blocker && vm.object(r.blocker);
    // This tick's step over the floor and the Sim it would walk into (the
    // locomotor's per-step placement test), when the prediction sees none.
    const bool onFloor = !r.points.empty() && floorPoint(r.points.front()) && me.position.inWorld();
    if (r.loco.halting && onFloor) {
        // Stopping for a blocker (StopLocomotion): the stop clip plays where
        // the walker is, its frame going on only while the way is free;
        // once it has played the walk is blocked (FollowOneStep state 7).
        LocoStep st = locoStep(vm, me, r);
        if (stepBlocker(vm, me, r, st.x, st.y, st.level))
            holdStill(me, st);
        r.waiting = false;
        applyLocoStep(vm, me, r, st);
        if (r.loco.tick >= r.loco.ticks) {
            r.loco.kind = Object::Route::Loco::None;
            r.loco.moving = false;
            r.loco.halting = false;
            r.loco.haltDone = true;
        }
        return Step::Walking;
    }
    std::optional<LocoStep> pending;
    // The stop for a blocker has played: the walk is blocked by it
    // (FollowOneStep returns 0 -> DoBlocked), whether or not it would be
    // seen in the way now; gone, the walk goes on (re-routed).
    ObjectId b = waitingForPerson ? r.blocker : 0;
    if (std::exchange(r.loco.haltDone, false) && !b && vm.object(r.loco.halted))
        b = r.loco.halted;
    if (!b)
        b = simAhead(vm, me, r);
    float haltRoom = kBlockedWithin; // predicted: it stops within 2 steps
    if (!b && onFloor) {
        pending = locoStep(vm, me, r);
        b = stepBlocker(vm, me, r, pending->x, pending->y, pending->level);
        haltRoom = 0; // the next step would be in it
        if (b && std::getenv("OS2_DEBUG_BLOCK"))
            std::fprintf(stderr, "t%llu %d step into %d\n", (unsigned long long)vm.ticks, me.id, b);
    }
    if (b && onFloor && !waitingForPerson && r.loco.moving && me.person &&
        (r.loco.kind == Object::Route::Loco::Walk || r.loco.kind == Object::Route::Loco::Start)) {
        // The walk is interrupted (InterruptActiveLocoState(2), BV 0x6b4fa0):
        // it slows to a stop with a stop clip first (StopLocomotion).
        LocoStep st = locoStep(vm, me, r, b, haltRoom);
        if (st.loco.halting) {
            if (std::getenv("OS2_DEBUG_BLOCK"))
                std::fprintf(stderr, "t%llu %d halts for %d\n", (unsigned long long)vm.ticks, me.id, b);
            if (stepBlocker(vm, me, r, st.x, st.y, st.level))
                holdStill(me, st);
            r.waiting = false;
            applyLocoStep(vm, me, r, st);
            return Step::Walking;
        }
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
            // A step-aside route (pushed on the route's own, stack > 1) that
            // is blocked fails by itself (14) and the route goes on: plan it
            // again, as when the step aside ends (inferred: what the route
            // does when the pushed one fails is not read).
            auto endAside = [&]() {
                r.aside = false, r.asideFrom = 0;
                r.blocker = 0, r.blockedTicks = 0;
                Object::Route next = r;
                RouteFail w;
                if (!planRoute(vm, me, next, w)) {
                    why = w;
                    return Step::Failed;
                }
                r.points = std::move(next.points);
                return Step::Walking;
            };
            auto routeBlocked = [&]() {
                if (r.aside)
                    return endAside();
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
            // DoPause: "CT - Route Blocked Animation" (parameter 0 the
            // pauses before, 1 the blocker: TriggerBlockedAnimation BV
            // 0x579bc0; the tree verifies parameter 1 as an object and picks
            // the animation by parameter 0, so swapped it returned at once
            // and a blocked Sim retried in the same tick: N001_Lot68's trash
            // "Dispose"/"Route To Can" recursion to a stack overflow), then
            // DoContinueAfterPause plans again.
            auto pause = [&]() {
                r.pausing = true;
                if (runWalkTree(c, "CT - Route Blocked Animation", {int16_t(r.pauses++), b, 0, 0}))
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
                if (std::getenv("OS2_DEBUG_BLOCK"))
                    std::fprintf(stderr, "t%llu %d gets out of %d's way: %s\n", (unsigned long long)vm.ticks, me.id, blocker.id,
                                 spot ? "a place" : "no place");
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
                r.asideFrom = blocker.id;
                r.blocker = 0, r.blockedTicks = 0;
                r.waiting = false;
                return true;
            };
            // An object in the way (the per-step test): not a person, so
            // "blocked by object" (DoBlockedGoalStillAvailable, §6).
            if (!blocker.person) {
                if (r.aside)
                    return endAside();
                why = RouteFail::BlockedByObject;
                return Step::Failed;
            }
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
            // Its route's blocking object: the one its locomotor stopped for
            // (CollisionCheck sets it as the stop clip starts, InterruptActive-
            // LocoState(2); DoBlocked reads it, vtable 0x1A8), not only once
            // its own DoBlocked has run: two Sims meeting head-on both halt
            // for each other a tick apart, and the first to reach DoBlocked
            // took the other for "routing, blocked by something else" and
            // gave up (Routing - Goto Head of Bed, two Sims sent to one bed).
            const auto& br = *blocker.route;
            // (Not once it walks out of the way: that is a route of its own,
            // pushed on top, with no blocker, TryGetSelfOutOfWay.)
            // A step-aside route's blocker is the Sim it gives way to
            // (TryGetSelfOutOfWay sets it, route vtable 0x140).
            const ObjectId itsBlocker = br.blocker                                         ? br.blocker
                                        : br.aside                                         ? br.asideFrom
                                        : (br.loco.halting || br.loco.haltDone) ? br.loco.halted
                                                                                             : 0;
            if (itsBlocker == me.id) { // it routes and I block it: the two block each other
                if (!base || r.aside)
                    return routeBlocked();
                if (!r.asideTried && counter < 3 && selfOutOfWay())
                    return Step::Walking;
                // It is stepping aside (its route is a step-aside one, route
                // vtable 0xd0, set by TryGetSelfOutOfWay through 0xd4): pause
                // for it without counting the try (the counter goes back
                // one). Two Sims sent to one bed in the same tick both
                // stepped aside, met again and paused alike to the 5th try.
                if (br.aside) {
                    --r.replans;
                    return pause();
                }
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
        applyLocoStep(vm, me, r, pending ? *pending : locoStep(vm, me, r));
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
        r.asideFrom = 0;
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
// thread with the parameters 0 the current goal's chair (its +0x10: the
// chair of a sitting goal, docs/seat-mac.md §A; 0 for a standing goal or
// without a goal) and 1 the route result; DoWaitingForPostRouteTree:
// its false after an arrival fails the route with 12 "post-route failed".
bool startPostRoute(Context& c, int stage, int16_t result, ObjectId chair = 0) {
    Object& me = c.me();
    if (!me.person)
        return false;
    for (const Frame& f : c.thread.stack)
        if (f.routeCallback)
            return false;
    const uint16_t tree = c.vm.functionTree(me, 0x26);
    if (!tree)
        return false;
    const std::array<int16_t, 4> params{int16_t(chair), result, 0, 0};
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
        for (const auto& [id, x] : c.vm.eachObject())
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
    if (c.frame.routeResume) { // resumed from a save: plan again from here (resumeRoutes)
        c.frame.routeResume = false;
        c.thread.rerunResult.reset();
        me.route.reset();
    }
    if (me.route && me.route->depth == depth && me.route->tree == c.frame.behavior && me.route->node == c.frame.pc) {
        RouteFail why = RouteFail::Success;
        const ObjectId blocker = me.route->blocker, chair = me.route->chair;
        switch (advance(c, me, why)) {
        case Step::Walking: return Exit::Yield;
        case Step::Call: return Exit::Call;
        case Step::Arrived:
            if (startPostRoute(c, 1, 0, chair))
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
    r.chairs = goals.chairs;
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
    // Already in the chair of one of the sitting goals (InitRoute, BV
    // 0x5737cc, after FindPath: IsPersonSittingOnChairGoal, BV 0x471984,
    // makes that goal the current one): the route arrives at once, without
    // the pre-route tree (which would stand the Sim up) or a walk; the
    // post-route tree gets the chair. A Sim on the sofa doing "Watch TV"
    // again stays seated.
    if (const ObjectId in = me.data[objdata::ContainerId]; in && me.person)
        for (size_t i = 0; i < r.chairs.size(); ++i)
            if (r.chairs[i] == in) {
                if (i < r.multi.size())
                    me.person->data[38] = r.multi[i];
                if (startPostRoute(c, 1, 0, in))
                    return Exit::Call;
                me.person->data[62] = 0;
                return Exit::True;
            }
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
    const ObjectId chair = r.chair;
    me.route = std::move(r);
    RouteFail later = RouteFail::Success;
    switch (advance(c, me, later)) {
    case Step::Walking: return Exit::Yield;
    case Step::Call: return Exit::Call;
    case Step::Arrived:
        if (startPostRoute(c, 1, 0, chair))
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
        c.frame.routeResume = false;
    }
    return e;
}


} // namespace

namespace routing_internal {

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

} // namespace routing_internal

namespace {

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
    for (const auto& [id, x] : vm.eachObject())
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

} // namespace

bool VM::placeNearFree(Object& o) {
    if (!world || !o.position.inWorld())
        return false;
    const ObjectId self[1] = {o.id};
    auto free = [&](float x, float y, int level) {
        if (!pointFree(x, y, level, self))
            return false;
        for (const ObjectId id : people())
            if (const Object* p = object(id); p && id != o.id && p->position.inWorld() && p->position.level == level &&
                                              p->data[objdata::ContainerId] == 0 &&
                                              std::hypot(p->position.x - x, p->position.y - y) < 0.5f)
                return false;
        return true;
    };
    if (o.data[objdata::ContainerId] != 0)
        leaveSlot(o);
    // FUN_00af87c8 (docs/primitives-mac.md §4g, "O lugar livre do reset"): a
    // square spiral from the person's tile (the world's centre when off the
    // lot), kept within [1, side - 2], at most 8 x the longer side steps;
    // each tile on the lot, with a floor (the partition test; inferred), in
    // the start's room on the first pass, and the person fitting at the
    // tile's centre (TSObjectCanSetPosition). The first that passes; a
    // second pass without the room; none: it stays where it is.
    const int w = world->width(), h = world->height();
    TilePos at = tileOf(o);
    if (!world->contains(at))
        at = {w / 2, h / 2, at.level};
    at.x = std::clamp(at.x, 1, std::max(1, w - 2)), at.y = std::clamp(at.y, 1, std::max(1, h - 2));
    const int16_t room = world->room(at);
    const int steps = 8 * std::max(w, h);
    for (int pass = 0; pass < 2; ++pass) {
        int x = at.x, y = at.y, dx = 1, dy = 0, run = 1;
        for (int step = 0, done = 0, turns = 0; step < steps; ++step) {
            const TilePos t{x, y, at.level};
            if (world->contains(t) && world->walkable(t) && (pass == 1 || world->room(t) == room) &&
                free(float(x) + 0.5f, float(y) + 0.5f, at.level)) {
                moveTo(*this, o, float(x) + 0.5f, float(y) + 0.5f, at.level);
                return true;
            }
            x += dx, y += dy;
            if (++done == run) { // turn; the run grows every second turn (inferred order)
                done = 0;
                std::swap(dx, dy), dx = -dx;
                if (++turns % 2 == 0)
                    ++run;
            }
        }
    }
    return false;
}

namespace {

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
// understood. The object takes the container's position and direction
// (PlaceInSlot), as the saved lots keep contained objects.
Exit snapInto(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[9];
    if (flags & ~0x07)
        throw VMError("Snap Into: flags " + hexString(flags) + " not implemented");
    // cTSObject::TrySnapInto (BV 0xe4253c; docs/seat-mac.md §B): my object
    // data 52 and 54 are cleared first; an object that does not exist is
    // error 0x17; one that does not fit (TSObjectCanPlaceInSlot: no such
    // slot, the slot taken, too heavy, its own container, the wrong height)
    // makes the primitive false with od 52 = the error + 2; else, unless
    // TEST ONLY (0x02), PlaceInSlot.
    Object& me = c.me();
    me.data[52] = 0;
    me.data[54] = 0;
    Object* x = c.vm.object(c.get(o[0], int16_t(le16(&o[1]))));
    Object* into = c.vm.object(c.get(o[3], int16_t(le16(&o[4]))));
    if (!x || !into)
        throw gameError(err::NoObject, "Snap Into");
    const int slot = flags & 0x01 ? c.thread.temps[0] : o[6];
    if (const int error = slotPlacementError(c.vm, *x, *into, slot)) {
        if (std::getenv("OS2_SNAP_FAILS"))
            std::fprintf(stderr, "t%llu snap into of %d into %d slot %d failed: %d (od 4 %d, od 27 %d, od 28 %d)\n",
                         (unsigned long long)c.vm.ticks, x->id, into->id, slot, error, x->data[4], x->data[27], into->data[28]);
        me.data[52] = int16_t(error + 2);
        return Exit::False;
    }
    if (flags & 0x02)
        return Exit::True; // TEST ONLY
    const bool inPortal = x->route && x->route->inPortal;
    placeInSlot(c.vm, *x, *into, slot);
    if (!inPortal) // a portal tree snaps the walker step by step
        x->route.reset();
    return Exit::True;
}

} // namespace

size_t resumeRoutes(VM& vm, Thread& t) {
    Object* me = vm.object(t.me);
    if (!me)
        return 0;
    size_t resumed = 0;
    for (size_t i = 0; i < t.stack.size(); ++i) {
        Frame& f = t.stack[i];
        if (!f.behavior || f.pc >= f.behavior->instructions.size())
            continue;
        const uint16_t op = f.behavior->instructions[f.pc].opcode;
        if (op != 0x2D && op != 0x1B)
            continue;
        // The routing slot noted as the walk started (noteRouteSlot: person
        // data 37), not read again from the temps.
        if (op == 0x2D && me->person)
            f.routeSlot = me->person->data[37];
        f.preRouteRan = true;
        if (i + 1 == t.stack.size()) { // walking
            f.routeResume = true;
            ++resumed;
            continue;
        }
        Frame& callee = t.stack[i + 1];
        callee.rerunCaller = true;
        callee.routeCallback = true;
        const std::string name = callee.behavior ? callee.behavior->fileName : std::string();
        const uint16_t preRoute = me->person ? vm.functionTree(*me, 0x25) : 0;
        const uint16_t postRoute = me->person ? vm.functionTree(*me, 0x26) : 0;
        const bool mine = callee.codeOwner == me->id;
        if (mine && preRoute && callee.instance == preRoute) {
            // Its result goes on as the pre-route tree's.
        } else if (name == "CT - Route Failure Feedback") {
            f.pendingRouteResult = callee.params.empty() ? int16_t(RouteFail::Unknown) : callee.params[0];
        } else if (mine && postRoute && callee.instance == postRoute) {
            const int16_t result = callee.params.size() > 1 ? callee.params[1] : int16_t(0);
            f.postRoute = result == 0 ? 1 : 2;
            if (result != 0)
                f.pendingRouteResult = result;
        } else {
            // A tree of the walk; "CT - Route Blocked ..." are run as
            // runWalkTree runs them (not route callbacks).
            if (name.rfind("CT - Route Blocked", 0) == 0)
                callee.routeCallback = false;
            f.routeResume = true;
        }
        ++resumed;
    }
    return resumed;
}

void registerRoutingPrimitives(VM& vm) {
    vm.setPrimitive(0x10, findLocationFor);
    vm.setPrimitive(0x1B, goToRelativePosition);
    vm.setPrimitive(0x2D, goToRoutingSlot);
    vm.setPrimitive(0x71, snapInto);
}

} // namespace os2::vm
