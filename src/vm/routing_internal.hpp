#pragma once

// What the routing sources share: routing.cpp, routing_plan.cpp,
// routing_loco.cpp and routing_walk.cpp (internal to them, not an API; each
// function is described where it is defined).

#include "path_planner.hpp"
#include "vm.hpp"

#include <optional>
#include <span>
#include <vector>

namespace os2::vm::routing_internal {

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

// routing.cpp: either one's "allow intersection" tree lets the two overlap.
bool intersectionAllowed(VM& vm, const Object& a, const Object& b);

// routing_plan.cpp
// The lot's path planner (made again when the world changed).
PathPlanner& plannerOf(VM& vm);
// The planner's obstacles for `me`'s plan (the objects that block, the Sims).
void addObstacles(VM& vm, PathPlanner& planner, const Object& me, std::span<const ObjectId> ignore);
// cLocomotable::ComputeTurn: the 45-degree steps from heading `from` to `to`.
int turnSteps(float from, float to);
// A turn in place of `steps` 45-degree steps where the walker stands.
std::optional<Object::Route::Point> turnPoint(VM& vm, const Object& me, int steps);
// Where the walker faces now (radians, 0 = +Y).
float facingOf(const Object& me);
// Plans the route's points from where the walker stands to its goals.
bool planRoute(VM& vm, Object& me, Object::Route& r, RouteFail& why);

// routing_loco.cpp
// Where a walker will be `tiles` further along its route points.
Point2 alongRoute(const Object& o, float tiles);
// Another Sim the walker is predicted to run into (CollisionCheck), or 0.
ObjectId simAhead(VM& vm, const Object& me, const Object::Route& r);
// Where a blocker steps aside to (CreateMoveOutOfWayDestination).
std::optional<Point2> moveOutOfWayPoint(VM& vm, const Object& me, const Object& blocker, int type);
// flushDestinations, once a tick.
void flushMoveOutOfWayDestinations(VM& vm);
// AskBlockerToMove: the blocker's MoveOutOfWay queued, or false.
bool askBlockerToMove(VM& vm, Object& me, const Object::Route& r, Object& blocker);
// A route point the walker walks to over the floor.
bool floorPoint(const Object::Route::Point& p);

struct LocoStep {
    Object::Route::Loco loco;
    size_t consumed = 0; // route points reached
    float x = 0, y = 0;  // where the walker (the root) stands
    int level = 0;
    float facing = 0; // the body's heading
};

// One tick of the locomotion along the floor (FollowOneStep).
LocoStep locoStep(VM& vm, const Object& me, const Object::Route& r, ObjectId haltFor = 0, float haltRoom = 0);
// What the walker would stand in at (x, y, level), or 0.
ObjectId stepBlocker(VM& vm, const Object& me, const Object::Route& r, float x, float y, int level);
// A halting walker whose next place is taken stays where it stands.
void holdStill(const Object& me, LocoStep& st);
// Puts the walker where this tick's locomotion step puts it.
void applyLocoStep(VM& vm, Object& me, Object::Route& r, const LocoStep& st);

// routing_walk.cpp
void setDirection(Object& o, int dir);
// Puts an object at a point of a level and keeps its room up to date.
void moveTo(VM& vm, Object& o, float x, float y, int level);
// The routing slots (type 3) of an object's SLOT, in order.
std::vector<const Slots::Slot*> routingSlots(const Slots* table);
const Slots* slotTable(VM& vm, const Object& o);

} // namespace os2::vm::routing_internal
