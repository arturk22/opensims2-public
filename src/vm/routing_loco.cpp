// One tick of the walk over the floor (part of the routing, routing.cpp):
// the Sims in the way (the locomotor's predicted blocking, MoveOutOfWay) and
// the locomotion step (the clips of the walker's set, the frame along the
// route's curve, the per-step placement test).
#include "vm.hpp"

#include "go_here.hpp"
#include "interactions.hpp"
#include "locomotion.hpp"
#include "routing_internal.hpp"
#include "sim_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace os2::vm {

using namespace routing_internal;

namespace routing_internal {

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

// A walker's way ahead as its locomotion will follow it: the frame's cubic
// path (LocoCurve), as GlobalLocationEstimate (cLocomotable vtable 0x3e8, BV
// 0x6af398) gives where the walker will be at a time: the path's point
// (vtable 0x54 of the path, +0x18c) at the distance completed by then
// (DistanceCompletedEstimate, vtable 0x470); without a path, where it
// stands. The curve is the one the frame is on (locoStep keeps it while
// the route is the one it was made through), else the one locoStep would
// make from the frame (from where the walker stands when it is not moving).
WayAhead::WayAhead(const Object& o) : from{o.position.x, o.position.y} {
    if (!o.route)
        return;
    const Object::Route& r = *o.route;
    const Object::Route::Loco& L = r.loco;
    size_t end = 0;
    while (end < r.points.size() && floorPoint(r.points[end]))
        ++end;
    if (end == 0)
        return;
    const bool moving = L.kind != Object::Route::Loco::None && L.moving;
    if (const LocoCurve* c = L.curve.get(); moving && L.onCurve && c && L.taken < c->points && c->points - L.taken == end) {
        const auto& p = c->point(L.taken);
        const auto& q = c->control.back();
        if (p[0] == r.points[0].x && p[1] == r.points[0].y && q[0] == r.points[end - 1].x && q[1] == r.points[end - 1].y) {
            curve = L.curve, segment = L.segment, arc = L.arc;
            return;
        }
    }
    std::vector<std::array<float, 2>> through;
    through.reserve(end);
    for (size_t k = 0; k < end; ++k)
        through.push_back({r.points[k].x, r.points[k].y});
    const float fx = moving ? L.fx : o.position.x, fy = moving ? L.fy : o.position.y;
    curve = std::make_shared<const LocoCurve>(fx, fy, through);
}

float WayAhead::left() const { return curve ? curve->remaining(segment, arc) : 0.0f; }

Point2 WayAhead::at(float tiles) const {
    if (!curve || curve->segments() == 0)
        return from;
    const LocoCurve& c = *curve;
    size_t j = segment;
    float a = arc + std::max(tiles, 0.0f);
    while (j < c.segments() && a >= c.length[j]) {
        a -= c.length[j];
        ++j;
    }
    if (j >= c.segments())
        return {c.control.back()[0], c.control.back()[1]};
    const auto p = c.at(j, c.parameterAt(j, a));
    return {p[0], p[1]};
}

} // namespace routing_internal

namespace {

// When in a window two points moving straight over it, p0 -> p1 and
// q0 -> q1, first come within r of each other: the fraction of the window
// (0 when within already), or -1 (the locomotor's swept test, vtable 0x474,
// BV 0x6b00c8: the first root of |(p - q)(t)| = r in [0, 1], or already
// within r).
float sweptWithin(Point2 p0, Point2 p1, Point2 q0, Point2 q1, float r) {
    const float rx = p0[0] - q0[0], ry = p0[1] - q0[1];
    if (rx * rx + ry * ry <= r * r)
        return 0;
    const float vx = (p1[0] - q1[0]) - rx, vy = (p1[1] - q1[1]) - ry;
    const float a = vx * vx + vy * vy;
    if (a < 1e-12f)
        return -1;
    const float b = 2 * (rx * vx + ry * vy);
    const float disc = b * b - 4 * a * (rx * rx + ry * ry - r * r);
    if (disc < 0)
        return -1;
    const float t1 = (-b - std::sqrt(disc)) / (2 * a);
    return t1 >= 0 && t1 <= 1 ? t1 : -1;
}

} // namespace

namespace routing_internal {

// Another Sim the walker is predicted to run into (CollisionCheck, BV
// 0x6b49e8), and how far along its way: the first collision within the
// horizon (10 steps of 0.05 tile at the walker's speed). Between two people
// (EstimateWhenPathWillBeBlocked, BV 0x6b3574, the human case, whatever
// their ages): the two centres come within 0.283, or the point 0.35 ahead
// of the walker along its way comes within 0.283 of the other's centre. A
// Sim is let go (CollisionCheck passes it on as the one to ignore) only when
// the two touch already (the collision is now) and are getting apart; there
// is no exception for the Sim walked to, nor for two Sims already on one
// spot (round n18: both were ours, inferred; with the second, two Sims
// 0.285 apart were never predicted and blocked each other step by step).
SimAhead simAhead(VM& vm, const Object& me, const Object::Route& r) {
    if (r.points.empty() || r.points.front().ticks > 0 || r.points.front().portal)
        return {};
    constexpr float kCentres = 0.283f, kAhead = 0.35f;
    const float mySpeed = std::max(walkTilesPerTick(vm, me), 1e-4f);
    // Where the walker will be: its frame on the cubic path its locomotion
    // follows (GlobalLocationEstimate; before, the straight lines between
    // the route points, which the frame cuts inside at the corners: a Sim
    // turning past another standing 0.33 off the way stepped within 0.283 of
    // it unpredicted, and the two blocked each other for good at
    // N001_Lot11's stair top). The horizon ends with the way (the request's
    // end: CollisionCheck limits it to the request's remaining ticks).
    (void)r;
    const WayAhead mine(me);
    const float left = mine.left();
    SimAhead best;
    for (const ObjectId id : vm.people()) {
        const Object* po = vm.object(id);
        if (!po)
            continue;
        const Object& o = *po;
        if (id == me.id || !o.person || !o.position.inWorld() || o.data[objdata::ContainerId] != 0 ||
            o.position.level != me.position.level)
            continue;
        const bool walking = o.route && !o.route->waiting;
        const float ratio = walking ? walkTilesPerTick(vm, o) / mySpeed : 0;
        const WayAhead its(o);
        const float reach = kCentres + kAhead;
        // OS2_DEBUG_AHEAD=<Sim id>: the Sims that Sim's prediction looks at (test aid).
        static const char* dbg = std::getenv("OS2_DEBUG_AHEAD");
        if (dbg && std::atoi(dbg) == me.id)
            std::fprintf(stderr, "t%llu ahead %d: %d at %.2f (walking %d, left %.2f, points %zu)\n", (unsigned long long)vm.ticks, me.id, id,
                         std::hypot(o.position.x - me.position.x, o.position.y - me.position.y), walking, left, r.points.size());
        // Out of reach within the horizon whatever the two do (the binary's
        // first test, at 0.283 + 0.35 between the centres).
        if (std::hypot(o.position.x - me.position.x, o.position.y - me.position.y) >
            reach + kStepTiles * float(kHorizonSteps) * (1 + ratio) + 0.01f)
            continue;
        for (int k = 0; k < kHorizonSteps; ++k) {
            const float t0 = kStepTiles * float(k), t1 = kStepTiles * float(k + 1);
            if ((best.id && t0 >= best.at) || (k > 0 && t0 >= left))
                break;
            const Point2 a0 = mine.at(t0), a1 = mine.at(t1);
            const Point2 b0 = walking ? its.at(t0 * ratio) : Point2{o.position.x, o.position.y};
            const Point2 b1 = walking ? its.at(t1 * ratio) : b0;
            const float ux = a1[0] - a0[0], uy = a1[1] - a0[1], ul = std::hypot(ux, uy);
            const Point2 f0 = ul > 0 ? Point2{a0[0] + ux / ul * kAhead, a0[1] + uy / ul * kAhead} : a0;
            const Point2 f1 = ul > 0 ? Point2{a1[0] + ux / ul * kAhead, a1[1] + uy / ul * kAhead} : a1;
            const float hc = sweptWithin(a0, a1, b0, b1, kCentres), ha = sweptWithin(f0, f1, b0, b1, kCentres);
            if (hc < 0 && ha < 0)
                continue;
            const float hit = hc < 0 ? ha : ha < 0 ? hc : std::min(hc, ha);
            if (k == 0 && hit == 0) {
                // Touching now: let it go if the two are getting apart.
                const float d0 = std::hypot(a0[0] - b0[0], a0[1] - b0[1]);
                const float d1 = std::hypot(a1[0] - b1[0], a1[1] - b1[1]);
                if (d1 > d0)
                    break;
            }
            const float at = t0 + hit * (t1 - t0);
            if (!best.id || at < best.at)
                best = {id, at};
            break;
        }
    }
    return best;
}

} // namespace routing_internal

namespace {

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

} // namespace

namespace routing_internal {

// The routing slot a Sim steps aside to (CreateMoveOutOfWayDestination, BV
// 0x57dcac; docs/routing-mac.md §9.3). The binary picks no point: it makes a
// destination object where the mover (`mover`, the Sim that steps aside)
// stands, with a synthetic routing slot as its slot 0, and the route to that
// slot finds the place by the common scan of the slot's cone and the A*
// (routingGoals). The slot (cTSRoutingSlot's setters: 0xD0 SetDistances(min,
// max, optimal), 0xCC SetGradient, 0xD4 SetSpacing, 0x20 AllowDirection,
// 0x40 SetFacingDirection):
// - type 0 (asked by a walker, or TryGetSelfOutOfWay): none when the two are
//   more than 4 tiles apart; one direction, 45 degrees off the asker's
//   direction d: d+7 when the mover is nearer (or as near) the point a tenth
//   of a tile to the asker's d+6 side, else d+1, absolute (inferred: it is
//   worked out from the asker's direction); 0.0625 to 0.3 tile, optimal 0.3,
//   gradient 100, spacing 0.025 (the scan takes at least 1/16): two rows,
//   0.3 and 0.2375 out, of three points each, the one 0.3 straight out
//   first; facing the asker; no random scoring, no stationary bonus, rooms
//   and walls count;
// - type 1 (the blocker stands within 1.5 tiles of the asker's goal object):
//   directions 1, 2, 6 and 7, 1 to 3 tiles, optimal 1, spacing 0.25,
//   gradient 0.625, random scoring, no stationary bonus; type 2 (MoveOutOfWay
//   without an asker): 1 to 6 tiles, rooms and walls ignored. Their
//   directions are taken relative to the asker's (the destination's
//   direction: inferred), their facing any (inferred: the 0x28 mode is not
//   decoded), and AllowOtherDirectionsAtLowerScore (0x70) is not done.
// The slot's standing multiplier is 1 (0xDC(0, 1)).
std::optional<Slots::Slot> moveOutOfWaySlot(const Object& mover, const Object& asker, int type, int& destinationDirection) {
    auto unit = [](int d) {
        const auto [sx, sy] = directionStep(((d % 8) + 8) % 8);
        const float n = std::hypot(float(sx), float(sy));
        return Point2{float(sx) / n, float(sy) / n};
    };
    Slots::Slot slot;
    slot.type = 3;
    slot.standing = 1;
    slot.flags = 0x40000; // HasStationaryBonus false
    const int d = ((asker.data[objdata::Direction] % 8) + 8) % 8;
    if (type == 0) {
        if (std::hypot(mover.position.x - asker.position.x, mover.position.y - asker.position.y) > 4)
            return std::nullopt;
        const Point2 l = unit(d + 6), rgt = unit(d + 2);
        const Point2 pl{asker.position.x + 0.1f * l[0], asker.position.y + 0.1f * l[1]};
        const Point2 pr{asker.position.x + 0.1f * rgt[0], asker.position.y + 0.1f * rgt[1]};
        const bool nearerLeft = std::hypot(mover.position.x - pl[0], mover.position.y - pl[1]) <=
                                std::hypot(mover.position.x - pr[0], mover.position.y - pr[1]);
        slot.flags |= (1u << ((nearerLeft ? d + 7 : d + 1) % 8)) | 0x200;
        slot.minDistance = 0.0625f, slot.maxDistance = 0.3f, slot.optimalDistance = 0.3f;
        slot.resolutionTiles = 0.025f, slot.gradient = 100;
        destinationDirection = 0; // the facing below is then absolute
        slot.facing = directionOf(asker.position.x - mover.position.x, asker.position.y - mover.position.y);
        if (slot.facing < 0)
            slot.facing = -3;
        return slot;
    }
    slot.flags |= 0x02 | 0x04 | 0x40 | 0x80 | 0x2000;
    if (type == 2)
        slot.flags |= 0x800 | 0x80000;
    slot.minDistance = 1, slot.maxDistance = type == 2 ? 6.0f : 3.0f, slot.optimalDistance = 1;
    slot.resolutionTiles = 0.25f, slot.gradient = 0.625f;
    slot.facing = -3;
    destinationDirection = d;
    return slot;
}

// flushDestinations, once a tick.
void flushMoveOutOfWayDestinations(VM& vm) {
    auto& last = vm.moveOutOfWayFlushedAt;
    if (last == vm.ticks + 1)
        return;
    last = vm.ticks + 1;
    flushDestinations(vm);
}

// AskBlockerToMove → the blocker's MoveOutOfWay (BV 0x57d8e8, 0x57ee3c;
// docs/routing-mac.md §8.1, §9.3): refused when a route of mine already
// asked the blocker (no cycle), when it is asked already (IsAskedToMove),
// unless its person data 159 has bit 0x02 its priority (person data 33) must
// be below mine less one (at least 1), my route must have more than one goal,
// both in the world and within 4 tiles, and its own current route must not
// have asked someone to move (0x188). Accepted (whether or not its slot will
// give a place: CreateMoveOutOfWayDestination always makes the destination),
// the move-out-of-way interaction goes on its queue with that priority, and
// my route keeps who was asked and that interaction (0x190).
bool askBlockerToMove(VM& vm, Object& me, Object::Route& r, Object& blocker) {
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
    if (r.asked == blocker.id)
        return false;
    for (const auto& outer : me.outerRoutes)
        if (outer.asked == blocker.id)
            return false;
    if (blocker.route && blocker.route->asked)
        return false;
    const int16_t priority = int16_t(std::max(1, me.person->data[persondata::Priority] - 1));
    if (!(blocker.person->data[159] & 0x02) && blocker.person->data[persondata::Priority] >= priority)
        return false;
    if (r.goals.size() < 2 || !me.position.inWorld() || !blocker.position.inWorld() ||
        blocker.position.level != me.position.level ||
        std::hypot(blocker.position.x - me.position.x, blocker.position.y - me.position.y) > 4)
        return false;
    int direction = 0;
    const auto slot = moveOutOfWaySlot(blocker, me, type, direction);
    if (!slot)
        return false;
    std::vector<ObjectId> existing;
    for (const auto& [id, o] : vm.eachObject())
        existing.push_back(id);
    Object* dest = vm.instantiateGuid(kDestinationGuid);
    if (!dest)
        return false;
    // The destination stands where the blocker is ("Interaction - Move Out
    // Of Way" walks only when it is on it), with the synthetic slot.
    const ObjectId destId = dest->id;
    auto place = [&](Object& x) {
        moveTo(vm, x, blocker.position.x, blocker.position.y, blocker.position.level);
        x.data[objdata::Direction] = int16_t(direction);
        x.syntheticSlot = *slot;
        vm.noteDestination(x);
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
                    if (const uint16_t uid = enqueue(vm, blocker, *q)) {
                        r.asked = blocker.id, r.askedUid = uid;
                        return true;
                    }
                }
    vm.removeObject(destId);
    return false;
}

// A route ends (arrived or failed; not a step aside): DoPostFailedOrSucceeded
// (BV 0x56f610) tells every router whose current route asked this Sim to
// move, for the interaction it runs now (0x188 and 0x18c against the Sim's
// current interaction), that the request is over (0x198, 0x190(0, 0)): the
// asker plans again at once (DoWaitingForPersonToMove).
void moveOutOfWayRouteEnded(VM& vm, const Object& me) {
    if (!me.person || me.activeInteraction < 0 || size_t(me.activeInteraction) >= me.queue.size())
        return;
    const uint16_t uid = me.queue[size_t(me.activeInteraction)].uid;
    for (const ObjectId id : vm.people())
        if (Object* o = vm.object(id); o && o->id != me.id && o->route && o->route->asked == me.id && o->route->askedUid == uid)
            o->route->asked = 0, o->route->askedUid = 0;
}

// A route point the walker walks to over the floor (not a stair or turn
// phase, not a portal).
bool floorPoint(const Object::Route::Point& p) { return p.ticks == 0 && !p.portal; }

} // namespace routing_internal

namespace {

constexpr float kAnimTicksPerSecond = 1000; // ANIM ticks are milliseconds

// A blend of locomotion clips with their weights: a start's or a stop's
// clip and its short one (blended by stride), or a sidestep's clips
// (blended by angle and distance, CreateStepCycle).
struct BlendedClip {
    struct Part {
        LocoPick pick;
        const LocoClip* clip = nullptr;
        float w = 1;
    };
    std::vector<Part> parts;
    float duration = 0; // seconds, when the blend sets its own (a sidestep's speed)
    explicit operator bool() const { return !parts.empty(); }
    // Seconds of a clip played at its set's speed.
    static float secondsOf(const LocoClip& c, const LocoPick& p) {
        return float(c.ticks) / kAnimTicksPerSecond / std::max(p.speed, 0.01f);
    }
    float seconds() const {
        if (duration > 0)
            return duration;
        float s = 0;
        for (const Part& p : parts)
            s += p.w * secondsOf(*p.clip, p.pick);
        return s;
    }
    float distance() const {
        float d = 0;
        for (const Part& p : parts)
            d += p.w * p.clip->distance;
        return d;
    }
    static std::array<float, 3> rootOf(const LocoClip& c, bool mirror, float t) {
        if (c.root.empty())
            return {0, 0, mirror ? -c.turn * t : c.turn * t};
        const size_t k = std::min(c.root.size() - 1, size_t(std::lround(t * float(c.root.size() - 1))));
        std::array<float, 3> r = c.root[k];
        if (mirror)
            r[0] = -r[0], r[2] = -r[2];
        return r;
    }
    // The root's offset from the frame (side, forward, yaw) at `t` (0-1).
    std::array<float, 3> root(float t) const {
        std::array<float, 3> r{0, 0, 0};
        for (const Part& p : parts) {
            const std::array<float, 3> s = rootOf(*p.clip, p.pick.mirror, t);
            for (size_t i = 0; i < 3; ++i)
                r[i] += p.w * s[i];
        }
        return r;
    }
    float turn() const {
        float t = 0;
        for (const Part& p : parts)
            t += p.w * (p.pick.mirror ? -p.clip->turn : p.clip->turn);
        return t;
    }
    const Part& heaviest() const {
        return *std::max_element(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.w < b.w; });
    }
};

bool loadLocoClip(VM& vm, const Object& me, const std::string& role, int steps, int tps, BlendedClip::Part& out) {
    if (auto p = locoClip(vm, me, role, steps))
        if (const LocoClip* c = vm.animations.loco(p->animation, tps); c && c->ticks > 0) {
            out.pick = *p;
            out.clip = c;
            return true;
        }
    return false;
}

// A start or a stop ("turnAndGo", "turnAndStop[R]"; cActiveAnimLoco::
// DetermineTurnResources, docs/routing-mac.md §6.1): the set's clip of the
// 45-degree steps nearest the angle (the ±40 degree rule of cLocoSet), no
// blend of angles (what is left over goes to the curve), blended with its
// short one of the same angle ("turnAndGoShort", "turnAndStopShortR") by
// the stride weight `shortWeight`.
BlendedClip turnClip(VM& vm, const Object& me, const std::string& role, float angle, int tps, float shortWeight) {
    BlendedClip out;
    const int steps = turnSteps(0, angle);
    const bool other = role.ends_with("R");
    const std::string plain = other ? role.substr(0, role.size() - 1) : role;
    const std::string shortRole = plain + "Short" + (other ? "R" : "");
    BlendedClip::Part a, b;
    const bool hasLong = loadLocoClip(vm, me, role, steps, tps, a);
    const bool hasShort = shortWeight > 0.001f && loadLocoClip(vm, me, shortRole, steps, tps, b);
    if (!hasLong && !hasShort) {
        if (loadLocoClip(vm, me, shortRole, steps, tps, b))
            out.parts.push_back(b);
        return out;
    }
    if (!hasShort || shortWeight <= 0.001f) {
        out.parts.push_back(a);
    } else if (!hasLong || shortWeight >= 0.999f) {
        out.parts.push_back(b);
    } else {
        a.w = 1 - shortWeight;
        b.w = shortWeight;
        out.parts.push_back(a);
        out.parts.push_back(b);
    }
    return out;
}

// A sidestep (cActiveAnimLoco::CreateStepCycle, BV 0xd2bf28): stepping
// `distance` at `angle` off where the walker faces without turning. The
// two 45-degree "step" angles around it blended by |off| / 45 degrees
// (NextLocoResourceByTurnRotation: the nearest, and the next one towards
// the angle when it is more than 0.01 off); within each angle "step" and
// "stepShort" blended by the distance, under the short one's distance
// "stepShort" with "stepInPlace" at distance 0; over the long one's the
// sidestep fails (the walker walks). Its speed is the clips' velocities
// weighed, times 1.4 (a fixed factor, 0x277d8e8) and the set's
// multiplier ("@1.5"); it lasts the distance over that speed.
BlendedClip sideStep(VM& vm, const Object& me, float angle, float distance, int tps) {
    BlendedClip out;
    const float q = float(M_PI / 4);
    const int steps = std::clamp(int(std::lround(angle / q)), -4, 4);
    const float off = std::remainder(angle - float(steps) * q, float(2 * M_PI));
    int next = steps + (off > 0 ? 1 : -1);
    if (next > 4)
        next -= 8;
    if (next < -4)
        next += 8;
    const float wNext = std::fabs(off) > 0.01f ? std::clamp(std::fabs(off) / q, 0.0f, 1.0f) : 0.0f;
    for (const auto& [s, wa] : {std::pair{steps, 1 - wNext}, std::pair{next, wNext}}) {
        if (wa <= 0)
            continue;
        BlendedClip::Part lng, shrt;
        const bool hasLong = loadLocoClip(vm, me, "step", s, tps, lng);
        const bool hasShort = loadLocoClip(vm, me, "stepShort", s, tps, shrt);
        if (!hasLong && !hasShort)
            return {};
        if (!hasLong)
            lng = shrt;
        if (!hasShort)
            shrt = lng;
        if (shrt.clip->distance > lng.clip->distance)
            std::swap(shrt, lng);
        if (distance > lng.clip->distance + 1e-3f)
            return {}; // longer than a step: walks
        if (distance >= shrt.clip->distance) {
            const float span = lng.clip->distance - shrt.clip->distance;
            const float wl = span > 1e-4f ? (distance - shrt.clip->distance) / span : 1.0f;
            lng.w = wa * wl, shrt.w = wa * (1 - wl);
            for (const auto* p : {&lng, &shrt})
                if (p->w > 0.001f)
                    out.parts.push_back(*p);
        } else {
            BlendedClip::Part still;
            const float ws = shrt.clip->distance > 1e-4f ? distance / shrt.clip->distance : 1.0f;
            if (ws < 0.999f && loadLocoClip(vm, me, "stepInPlace", 0, tps, still)) {
                shrt.w = wa * ws, still.w = wa * (1 - ws);
                out.parts.push_back(shrt);
                if (still.w > 0.001f)
                    out.parts.push_back(still);
            } else {
                shrt.w = wa;
                out.parts.push_back(shrt);
            }
        }
    }
    float sum = 0, velocity = 0, multiplier = 0;
    for (const auto& p : out.parts)
        sum += p.w, velocity += p.w * p.clip->velocity, multiplier += p.w * p.pick.speed;
    if (sum <= 0)
        return {};
    for (auto& p : out.parts)
        p.w /= sum;
    const float speed = velocity / sum * 1.4f * (multiplier / sum);
    out.duration = speed > 1e-4f && distance > 1e-4f ? distance / speed : 0;
    if (out.duration <= 0) {
        float s = 0;
        for (const auto& p : out.parts)
            s += p.w * BlendedClip::secondsOf(*p.clip, p.pick);
        out.duration = s / 1.4f;
    }
    return out;
}

} // namespace

namespace routing_internal {

// One tick of the locomotion along the floor (cLocomotable::FollowOneStep, BV
// 0x6b80e8, and the scene graph's loco request; docs/routing-mac.md §6.1):
// the phases BeginFollow lines up, each a clip of the walker's set, with a
// frame moving along the route's curve and the walker put where the clip's
// root is (LocoPlaceFromSceneGraph: its translation, and the facing its
// rotation's).
//   start  standing: "turnAndGo" of the 45-degree steps nearest the angle
//          to the first leg (the way out already leaves along them),
//          blended with "turnAndGoShort" by the stride weight. Its frame
//          covers the clip's distance (or the stretch) in the clip's length;
//          on a short way (fitStride) its share, and the stop follows it.
//          It starts now (ComputeLocoStartTicks) and blends in over the
//          request's fade in (233.3 ms, less for a short request).
//          A straight way (|chord - length| < 0.02) to a final facing the
//          walker already has (within 0.01) is a sidestep instead
//          (CreateStepCycle: "step"/"stepShort"/"stepInPlace" blended by
//          angle and distance), the body keeping its facing, if one reaches.
//   walk   the gait's cycles blended by their weights, walk and walkShort
//          blended by the stride weight: the fewest half cycles the long
//          stride reaches over what is left between the start and the stop,
//          the strides blended down to fit them (strideWeight).
//   stop   at the end of the route, once what is left is a stop's
//          distance: "turnAndStop" (and its short one by the stride weight)
//          from the way the walker comes to its final facing, of the foot
//          the fit planned (the request's cycle portion, getter 0x178, and
//          the half cycles' parity: fitStride); its frame covers what is
//          left in the clip's length. The clips of the request cross over
//          its blend distance (+0xec) at the walk's speed.
//   halt   the way is blocked ahead (InterruptActiveLocoState(2) ->
//          cActiveAnimLoco::StopLocomotion, BV 0xd2f9b4): a start plays out,
//          a walk goes on to the end of its half cycle, then a "turnAndStop"
//          of no turn of that foot, along the way; the walk reports blocked
//          once it has played (FollowOneStep state 6 -> 7). The next place
//          taken (the per-step test) blocks at once, with no clip (the
//          caller: LocoPlaceFromSceneGraph fails -> EndFollow(1)).
// The frame follows the route's B-spline (LocoCurve: cut every 0.2 tile by
// shapeWalk, the ends tripled). A set without these clips (tests, private
// sets without turns) walks at the gait's speed from the first tick to the
// last.
// `haltFor`: the blocker the walk stops for (0: none); `haltRoom`: how far
// its frame may go on (tiles; inferred).
LocoStep locoStep(VM& vm, const Object& me, const Object::Route& r, ObjectId haltFor, float haltRoom) {
    using Loco = Object::Route::Loco;
    LocoStep out;
    Loco& L = out.loco;
    L = r.loco;
    const float tps = float(vm.clock.ticksPerSecond);
    const int itps = int(vm.clock.ticksPerSecond);
    // The stretch over the floor from here: its points.
    size_t end = 0;
    while (end < r.points.size() && floorPoint(r.points[end]))
        ++end;
    const bool final = end == r.points.size();
    if (L.kind == Loco::None || !L.moving) {
        L.fx = me.position.x, L.fy = me.position.y;
        L.onCurve = false;
    }
    // The curve from the frame through the stretch's points, kept while the
    // route is the one it was made through (planned again: a new one from
    // the frame).
    bool rebuilt = false;
    auto ensureCurve = [&] {
        if (end == 0) {
            L.curve.reset();
            L.onCurve = false;
            return;
        }
        if (const LocoCurve* c = L.curve.get(); L.onCurve && c && L.taken < c->points && c->points - L.taken == end) {
            const auto& p = c->point(L.taken);
            const auto& q = c->control.back();
            if (p[0] == r.points[0].x && p[1] == r.points[0].y && q[0] == r.points[end - 1].x && q[1] == r.points[end - 1].y)
                return;
        }
        std::vector<std::array<float, 2>> through;
        through.reserve(end);
        for (size_t k = 0; k < end; ++k)
            through.push_back({r.points[k].x, r.points[k].y});
        L.curve = std::make_shared<const LocoCurve>(L.fx, L.fy, through);
        L.segment = 0, L.taken = 0, L.arc = 0;
        L.onCurve = true;
        rebuilt = true;
        // The overlap of the phases (FollowOneStep): by the way's beginning
        // as BeginFollow measures it, the legs summed until past 0.75 (or
        // until it changes level; the room change it also stops at is not
        // checked here): 5 ms when that is at most 0.25, else 100 ms.
        float begun = 0, x = L.fx, y = L.fy;
        for (size_t k = 0; k < end && begun <= 0.75f; ++k) {
            if (r.points[k].level != me.position.level)
                break;
            begun += std::hypot(r.points[k].x - x, r.points[k].y - y);
            x = r.points[k].x, y = r.points[k].y;
        }
        L.overlap = begun <= 0.25f ? 0.005f : 0.1f;
    };
    ensureCurve();
    auto curveLeft = [&] { return L.curve && L.onCurve ? L.curve->remaining(L.segment, L.arc) : 0.0f; };
    const float stretch = curveLeft();
    const auto& first = r.points.front();
    const float legX = first.x - L.fx, legY = first.y - L.fy;
    const float legHeading = std::hypot(legX, legY) > 1e-5f ? headingOfMotion(legX, legY) : facingOf(me);
    // The way the walker comes to the end: the last leg's.
    auto lastHeading = [&] {
        if (end == 0)
            return legHeading;
        const float px = end >= 2 ? r.points[end - 2].x : L.fx, py = end >= 2 ? r.points[end - 2].y : L.fy;
        const float dx = r.points[end - 1].x - px, dy = r.points[end - 1].y - py;
        return std::hypot(dx, dy) > 1e-5f ? headingOfMotion(dx, dy) : legHeading;
    };
    auto setLayers = [&](const BlendedClip& c) {
        const float secs = c.seconds();
        L.layers.clear();
        for (const auto& p : c.parts)
            L.layers.push_back({p.pick.animation, p.w, p.pick.mirror,
                                secs > 0 ? BlendedClip::secondsOf(*p.clip, p.pick) / secs * p.pick.speed : p.pick.speed});
        // The heaviest first.
        std::stable_sort(L.layers.begin(), L.layers.end(), [](const LocoLayer& a, const LocoLayer& b) { return a.weight > b.weight; });
    };
    auto begin = [&](Loco::Kind kind, const BlendedClip& c, float distance, float heading) {
        L.kind = kind;
        L.animation = c.heaviest().pick.animation;
        L.mirror = c.heaviest().pick.mirror;
        L.tick = 0;
        L.lead = 0;
        L.ticks = std::max(1, int(std::lround(c.seconds() * tps)));
        L.distance = distance;
        L.heading = heading;
        L.turn = c.turn();
        // A stop or a sidestep leaves the body turned off the frame (a
        // start's turn ends on the way, the walk going on from its sway).
        L.endTurn = kind == Loco::Stop || L.sidestep ? c.root(1)[2] : 0;
        setLayers(c);
        ++L.serial;
    };
    // The clip now playing (a start or a stop), as begun: its layers.
    auto current = [&]() {
        BlendedClip c;
        for (const LocoLayer& l : L.layers)
            if (const LocoClip* clip = vm.animations.loco(l.animation, itps); clip && l.weight > 0)
                c.parts.push_back({LocoPick{l.animation, l.mirror, 1}, clip, l.weight});
        return c;
    };
    // A stop's clip for the way the walker comes and its final facing, of
    // a foot ("R": the other one).
    auto stopClip = [&](float heading, bool turnToFacing, float shortWeight, bool otherFoot) {
        const float facing = turnToFacing && r.facing >= 0 ? headingOfDirection(r.facing) : heading;
        return turnClip(vm, me, otherFoot ? "turnAndStopR" : "turnAndStop", std::remainder(facing - heading, float(2 * M_PI)),
                        itps, shortWeight);
    };
    auto plannedStop = [&](float heading) { return stopClip(heading, true, L.stopWeight, L.stopOtherFoot); };
    auto walkGait = [&] { return blendGaits(gaitOf(vm, me), gaitOf(vm, me, true), L.strideWeight); };
    // The request's cycle portion as the walk starts standing (+0x1bc: 0.5,
    // or the last blocking primitive's final one in the start window).
    float standPortion = 0.5f;
    // When the route's first phase starts (cLocomotable::ComputeLocoStartTicks,
    // BV 0x6ad69c; docs/anim-mac.md §13.6), against the last blocking
    // primitive's request: with its action's end eA kept (none: now), the
    // last route's stop still playing starts it at that stop's end; else
    // not a short step and |now - eA| < 140 ms starts it at eA + 70; else,
    // within W (40 for a short step, 140) of that request's end e and
    // before it, at e - W/2. In those cases the request's cycle portion is
    // the kept one. VM ticks after now (negative: before).
    auto followStartTicks = [&](bool shortStep) {
        const AnimationState& a = me.animation;
        const double now = animationClockMs(vm);
        const double eA = a.previousActionEndMs, e = a.previousEndMs;
        if (eA == AnimationState::kNoTicks)
            return 0.0f;
        constexpr double kWindow = 140; // DAT_02c11c04
        double start = now;
        if (now < a.locoEndingUntilMs) {
            if (e != AnimationState::kNoTicks && start < e)
                start = e;
            standPortion = a.previousPortion;
        } else if (!shortStep && now - eA > -kWindow && now - eA < kWindow) {
            start = eA + kWindow / 2;
            standPortion = a.previousPortion;
        } else if (e != AnimationState::kNoTicks) {
            const double w = shortStep ? 40 : kWindow, d = now - e;
            if (d > -w && d < w) {
                if (d < 0)
                    start = e - w / 2;
                standPortion = a.previousPortion;
            }
        }
        return float((start - now) * tps / 1000.0);
    };
    // The stride fit (fitStride) for what is left from the frame: the stop
    // ahead by its foot, the start's distances (0: none), what is left of
    // the current half cycle when walking on. The request's cycle portion
    // (getter 0x178) is 0.5 standing (BeginFollow), the walk's when walking
    // on: the stop's first foot by it (stopFootOther).
    auto fit = [&](float startLong, float startShort, bool walkingOn) {
        const Gait full = gaitOf(vm, me), shortGait = gaitOf(vm, me, true);
        const float Lc = full.speed * full.seconds;
        const float Ls = shortGait.cycles.empty() ? Lc : shortGait.speed * shortGait.seconds;
        const float facing = r.facing >= 0 ? headingOfDirection(r.facing) : lastHeading();
        const float angle = std::remainder(facing - lastHeading(), float(2 * M_PI));
        std::function<std::pair<float, float>(bool)> stops = [&](bool otherFoot) {
            const char* role = otherFoot ? "turnAndStopR" : "turnAndStop";
            const BlendedClip l = turnClip(vm, me, role, angle, itps, 0), s = turnClip(vm, me, role, angle, itps, 1);
            return std::pair<float, float>{l ? l.distance() : 0, s ? s.distance() : (l ? l.distance() : 0)};
        };
        float f0 = 0;
        const float portion = walkingOn ? L.cycle : standPortion;
        if (walkingOn) {
            f0 = 1 - L.cycle;
            if (f0 >= 0.5f)
                f0 -= 0.5f;
        }
        const StrideFit f =
            fitStride(stretch, Lc, Ls, startLong, startShort, final ? &stops : nullptr, stopFootOther(portion), f0, 0);
        L.strideWeight = shortGait.cycles.empty() ? 0 : f.weight;
        L.stopWeight = shortGait.cycles.empty() ? 0 : f.stopWeight;
        L.stopOtherFoot = f.stopOtherFoot;
        L.shortWay = f.shortWay;
        return f;
    };
    auto walkOn = [&](const Gait& g) {
        // The poses blend in over locoPoseBlend: the start just played and the stop ahead.
        const float startDistance = L.kind == Loco::Start ? L.distance : 0;
        const BlendedClip stopAhead = final ? plannedStop(lastHeading()) : BlendedClip{};
        const float poseFade = locoPoseBlend(startDistance, stopAhead ? stopAhead.distance() : 0, g.speed);
        // The clips of one request cross over the same BlendDistance (+0xec,
        // PushLocoCycle): the start into the walk now, the walk into the stop.
        L.fade = poseFade;
        L.crossover = true;
        L.kind = Loco::Walk;
        L.animation = g.animation;
        L.mirror = false;
        L.turn = 0;
        L.layers = g.layers();
        for (size_t k = g.cycles.size(); k < L.layers.size(); ++k)
            L.layers[k].fade = poseFade;
        L.cycleStart = L.cycle; // the layers start at the portion the walk is at
        ++L.serial;
    };
    // The stride weight for what is left from here (a walk planned again):
    // the stop alone between.
    // A new request (BeginFollow): its length and played ticks from here.
    auto newRequest = [&] {
        L.requestTicks = 0;
        L.requestLength = curveLeft();
    };
    auto refitStride = [&] {
        newRequest();
        (void)fit(0, 0, true);
    };
    // A request's fades (FollowOneStep / InitChannelNetwork's FUN_00a46ea8):
    // in over 233.3 ms, out over 233.3 ms with a stop clip and 0.6 of it
    // (140) without, both shrunk in proportion (times 0.9999) when the
    // request is shorter than the two. Its first clip blends in over the
    // fade in.
    auto requestFadeIn = [&](float seconds, bool withStop) {
        const float in = kBlendTicks / 1000, out = withStop ? in : 0.6f * in;
        return seconds > 0 && seconds < in + out ? in * seconds / (in + out) * 0.9999f : in;
    };
    if (L.kind == Loco::None || (!L.moving && L.kind != Loco::Start)) {
        const bool wasMoving = L.moving;
        L.kind = Loco::None;
        L.halting = false, L.halted = 0;
        L.sidestep = false;
        if (!L.moving && me.person) {
            const float facing = headingOfDirection(me.data[objdata::Direction]);
            const float angle = std::remainder(legHeading - facing, float(2 * M_PI));
            // A straight way to a final facing the walker already has.
            if (final && end > 0 && r.facing >= 0 &&
                std::fabs(std::remainder(headingOfDirection(r.facing) - facing, float(2 * M_PI))) < 0.01f) {
                const float dx = r.points[end - 1].x - L.fx, dy = r.points[end - 1].y - L.fy;
                const float chord = std::hypot(dx, dy);
                float length = 0, x = L.fx, y = L.fy;
                for (size_t k = 0; k < end; ++k) {
                    length += std::hypot(r.points[k].x - x, r.points[k].y - y);
                    x = r.points[k].x, y = r.points[k].y;
                }
                if (chord > 1e-3f && std::fabs(chord - length) < 0.02f) {
                    const float way = headingOfMotion(dx, dy);
                    if (const BlendedClip step = sideStep(vm, me, std::remainder(way - facing, float(2 * M_PI)), chord, itps)) {
                        newRequest();
                        L.sidestep = true;
                        begin(Loco::Start, step, stretch, way);
                        L.cycleStart = L.cycle = 0;
                        L.fade = requestFadeIn(step.seconds(), false);
                        L.crossover = false;
                        // Standing at its start this tick (-1), or the
                        // window's ticks later or earlier (a short step).
                        L.lead = -1 - followStartTicks(true);
                        out.beganFollow = true;
                    }
                }
            }
            if (!L.sidestep) {
                newRequest();
                const float ahead = wasMoving ? 0.0f : followStartTicks(false);
                // The start of the request's portion: the plain clip from
                // 0.5 (standing), the "R" one from 0 (InitChannelNetwork, §8.5).
                const char* goRole = stopFootOther(standPortion) ? "turnAndGoR" : "turnAndGo";
                if (!turnClip(vm, me, goRole, angle, itps, 0))
                    goRole = "turnAndGo";
                const BlendedClip goLong = turnClip(vm, me, goRole, angle, itps, 0);
                const BlendedClip goShort = turnClip(vm, me, goRole, angle, itps, 1);
                StrideFit f;
                if (goLong) {
                    L.cycle = goLong.heaviest().clip->cycle; // the portion the start ends at
                    f = fit(goLong.distance(), goShort ? goShort.distance() : goLong.distance(), false);
                } else {
                    f = fit(0, 0, false);
                }
                if (const BlendedClip go = turnClip(vm, me, goRole, angle, itps, L.strideWeight)) {
                    // A short way's start covers its share (squeezed under
                    // the short clips' own distance).
                    begin(Loco::Start, go, std::min(f.shortWay ? f.startDistance : go.distance(), stretch), legHeading);
                    L.cycleStart = L.cycle = go.heaviest().clip->cycle;
                    // The first phase starts now (ComputeLocoStartTicks: no
                    // earlier primitive's end is kept here; docs/anim-mac.md
                    // §8.2), blending in over the request's fade in: in the
                    // tick BeginFollow runs, FollowOneStep creates the
                    // request at now and updates it to now, so the Sim
                    // stands at the clip's start, and moves from the next
                    // tick on (lead -1 tick). The old 5/100 ms head start
                    // was a misreading.
                    const BlendedClip stopAhead = final ? plannedStop(lastHeading()) : BlendedClip{};
                    L.fade = requestFadeIn(go.seconds() + f.cycles * walkGait().seconds + (stopAhead ? stopAhead.seconds() : 0),
                                           bool(stopAhead));
                    L.crossover = false;
                    if (!wasMoving) {
                        // Standing at its start this tick (-1: it starts
                        // now), or as many ticks later or earlier as
                        // ComputeLocoStartTicks puts it.
                        L.lead = -1 - ahead;
                        out.beganFollow = true;
                    }
                }
            }
        } else if (!wasMoving || rebuilt) {
            refitStride();
        }
        L.moving = true;
    }
    // The stop clip of a halt (no turn, of the foot found), its frame going
    // on along the way as far as the room left; no clip: the walk just ends
    // (the halt is done).
    auto beginHalt = [&] {
        if (const BlendedClip halt = stopClip(L.heading, false, L.strideWeight, L.haltOtherFoot); halt && me.person) {
            // It crosses over from what played over the request's blend
            // distance (+0xec: the start's and the planned stop's).
            const bool fromStart = L.kind == Loco::Start;
            const float startDistance = fromStart ? L.distance : 0;
            const float speed = fromStart ? (L.ticks > 0 ? L.distance * tps / float(L.ticks) : 0) : L.speed * tps;
            const BlendedClip planned = final ? plannedStop(lastHeading()) : BlendedClip{};
            L.sidestep = false;
            begin(Loco::Stop, halt, std::max(0.0f, std::min(L.haltRoom, halt.distance())), L.heading);
            L.fade = locoPoseBlend(startDistance, planned ? planned.distance() : 0, speed);
            L.crossover = true;
        } else {
            L.kind = Loco::None;
        }
        L.haltWalk = 0;
    };
    // (A sidestep, like a turn in place, plays out: StopLocomotion leaves
    // a started request of sequence other than 5/6 alone.)
    if (haltFor && !L.halting && (L.kind == Loco::Walk || (L.kind == Loco::Start && !L.sidestep))) {
        // The way is blocked ahead (InterruptActiveLocoState(2) ->
        // StopLocomotion with the stop clip, BV 0xd2f9b4; docs/anim-mac.md
        // §8.7): a start plays out and the stop follows it, of the start's
        // foot; a walk goes on to the end of its half cycle (portion 0.5 or
        // 1) and the stop of that foot follows. Unless the planned stop is
        // within that half cycle: then it is the stop (nothing changes).
        // CollisionCheck then ends the walk at once (EndFollow) unless the
        // request, so stopped, ends 2 steps before the collision: `haltRoom`
        // is that room (round n18; before, the stop was squeezed into the 2
        // steps, inferred).
        const float p = L.kind == Loco::Walk ? L.cycle : L.cycleStart;
        float h = 0, stride = 0, rest = 0;
        if (L.kind == Loco::Walk) {
            const Gait g = walkGait();
            stride = g.speed * g.seconds;
            h = p > 0 ? (p < 0.5f ? 0.5f : 1.0f) - p : 0.0f;
            rest = h * stride;
        } else {
            rest = L.distance * std::max(0.0f, 1 - (float(L.tick) + L.lead) / float(std::max(L.ticks, 1)));
        }
        const BlendedClip planned = final ? plannedStop(lastHeading()) : BlendedClip{};
        const bool foot = stopFootOther(p);
        if (L.kind == Loco::Walk && planned && stretch <= planned.distance() + h * stride + 1e-4f) {
            // The planned stop is the stop: the walk goes on if its way ends
            // in the room.
            out.walkOn = stretch <= haltRoom + 1e-4f;
        } else if (const BlendedClip halt = stopClip(L.heading, false, L.strideWeight, foot); halt && me.person &&
                   rest + halt.distance() <= haltRoom + 1e-4f) {
            L.halting = true;
            L.halted = haltFor;
            L.haltOtherFoot = foot;
            L.haltRoom = rest + halt.distance();
            L.haltWalk = rest;
            if (L.kind == Loco::Walk && L.haltWalk <= 1e-4f)
                beginHalt();
        }
    }
    std::optional<Gait> gait;
    if ((L.kind == Loco::None && !L.halting) || L.kind == Loco::Walk) {
        if (rebuilt && L.kind == Loco::Walk && !L.halting)
            refitStride();
        // (A stop covers less than a tile, a stride at most a run's 4.4 m.)
        const BlendedClip stop = final && stretch < 6 && !L.halting ? plannedStop(legHeading) : BlendedClip{};
        gait = walkGait();
        const Gait& g = *gait;
        if (L.kind == Loco::None || L.animation != g.animation || (rebuilt && L.layers.size() != g.cycles.size() + g.poses.size())) {
            walkOn(g);
        } else if (rebuilt) {
            L.layers = g.layers(); // the blend's weights for the way planned again
        }
        L.speed = (g.speed > 0 ? g.speed : vm.walkTilesPerTick * tps) / tps;
        // The stop, once what is left is a stop's distance; halting, the
        // halt's stop once the half cycle is walked.
        if (L.halting) {
            if (L.haltWalk < L.speed)
                beginHalt();
        } else if (final && stop && stretch - L.speed <= stop.distance()) {
            begin(Loco::Stop, stop, stretch, legHeading);
            L.endX = r.points[end - 1].x, L.endY = r.points[end - 1].y;
        }
    }
    if (L.kind == Loco::Stop && !L.halting && final && end > 0 &&
        (r.points[end - 1].x != L.endX || r.points[end - 1].y != L.endY)) {
        // Planned again: walk on.
        L.sidestep = false;
        refitStride();
        const Gait g = walkGait();
        walkOn(g);
        L.speed = std::max(g.speed, 0.1f) / tps;
    }
    // The frame's way this tick, along the curve.
    ensureCurve();
    float step = L.speed;
    if (L.kind != Loco::Walk) {
        // The frame's share of the clip's way this tick, with the phase
        // begun `lead` ticks before its first tick (-1: at its start; less:
        // later, standing until then).
        const float n = float(std::max(L.ticks, 1));
        auto played = [&](float k) { return std::clamp(k + L.lead, 0.0f, n); };
        step = L.distance * (played(float(L.tick) + 1) - played(float(L.tick))) / n;
    }
    if (L.halting)
        step = std::min(step, L.haltRoom);
    float x = L.fx, y = L.fy;
    float heading = L.heading;
    size_t passed = L.taken;
    if (L.curve && L.onCurve && step > 1e-7f) {
        const LocoCurve& c = *L.curve;
        L.arc += step;
        while (L.segment < c.segments() && L.arc >= c.length[L.segment]) {
            L.arc -= c.length[L.segment];
            passed = std::max(passed, c.passedAfter(L.segment));
            ++L.segment;
        }
        if (L.segment >= c.segments()) {
            x = c.control.back()[0], y = c.control.back()[1];
            L.arc = 0;
            passed = c.points;
        } else {
            const float u = c.parameterAt(L.segment, L.arc);
            const auto p = c.at(L.segment, u);
            x = p[0], y = p[1];
            auto d = c.slope(L.segment, u);
            if (std::hypot(d[0], d[1]) < 1e-4f) { // a tripled end: the way it leaves
                const auto q = u < 0.95f ? c.at(L.segment, u + 0.05f)
                                         : L.segment + 1 < c.segments() ? c.at(L.segment + 1, 0.05f) : p;
                d = {q[0] - p[0], q[1] - p[1]};
            }
            if (std::hypot(d[0], d[1]) > 1e-6f)
                heading = headingOfMotion(d[0], d[1]);
        }
    }
    // The frame faces along the curve while walking and while starting (the
    // start's turn is the clip's, what is left over the curve's: inferred,
    // the frame's orientation is the path's); a stop or a sidestep keeps
    // the way it began with (it ends on the final facing).
    if (L.kind == Loco::Walk || (L.kind == Loco::Start && !L.sidestep))
        L.heading = heading;
    if (L.kind == Loco::Walk) {
        // The cycle goes on at the blend's speed (one cycle per its length).
        if (!gait)
            gait = walkGait();
        float secs = gait->seconds;
        if (secs <= 0)
            if (const LocoClip* c = vm.animations.loco(L.animation, itps); c && c->ticks > 0)
                secs = float(c->ticks) / kAnimTicksPerSecond;
        if (secs > 0)
            L.cycle = std::fmod(L.cycle + 1 / tps / secs, 1.0f);
        if (L.halting)
            L.haltWalk = std::max(0.0f, L.haltWalk - step);
    }
    if (L.halting)
        L.haltRoom = std::max(0.0f, L.haltRoom - step);
    L.fx = x, L.fy = y;
    out.consumed = passed - L.taken;
    L.taken = passed;
    out.level = out.consumed > 0 ? r.points[out.consumed - 1].level : me.position.level;
    // The root: the clip's offset from the frame at this tick.
    float rx = 0, ry = 0, yaw = 0;
    if (L.moving)
        ++L.requestTicks;
    if (L.kind == Loco::Start || L.kind == Loco::Stop) {
        ++L.tick;
        if (const BlendedClip c = current()) {
            const auto rt = c.root(std::min(1.0f, (float(L.tick) + L.lead) / float(std::max(L.ticks, 1))));
            rx = rt[0], ry = rt[1], yaw = rt[2];
        }
        // The way's last clip (the stop, or a sidestep's step): the walk is
        // done as the request starts to fade out, 233.3 ms before its end
        // with a stop clip, 140 without (both shrunk for a short request),
        // unless the way is at most 0.5 long with 40 ms or more left; the
        // Sim goes to the way's end with the final facing, and the clip
        // plays on where it is in the world (FollowOneStep: the hand-off to
        // the ending state, then MarkActiveLocoStateAsCompleted; BV
        // 0x6b80e8, docs/anim-mac.md §8.3).
        if (final && !L.halting && (L.kind == Loco::Stop || L.sidestep)) {
            const float left = (float(L.ticks) - float(L.tick) - L.lead) / tps;
            const float in = kBlendTicks / 1000, full = L.kind == Loco::Stop ? in : 0.6f * in;
            const float total = float(L.requestTicks) / tps + left;
            const float fadeOut = total > 0 && total < in + full ? full * total / (in + full) * 0.9999f : full;
            if (left > 1e-4f && left <= fadeOut && (L.requestLength > 0.5f || left < 0.04f)) {
                // The request's end, its blend out's start, the portion its
                // clip ends at (FollowOneStep: SetEndTicks, SetEndActionTicks,
                // SetFinalCyclePortionCompleted, BV 0x6b8848).
                const double now = animationClockMs(vm);
                out.endMs = now + double(left) * 1000.0;
                out.actionEndMs = out.endMs - double(fadeOut) * 1000.0;
                if (const BlendedClip c = current())
                    out.endPortion = c.heaviest().clip->cycle;
                L.tick = L.ticks;
                L.lead = 0;
                out.handOff = true;
            }
        }
        if (float(L.tick) + L.lead >= float(L.ticks)) {
            if (L.halting && L.kind == Loco::Start) { // the start played out: the halt's stop
                beginHalt();
                rx = ry = 0;
                yaw = 0;
            } else if (L.halting) { // stopped there
                rx = ry = 0;
                yaw = L.endTurn;
            } else if (L.kind == Loco::Start && L.shortWay && final) {
                // A short way: the stop follows the start at once (no
                // cycles: sequence 5), over the rest of the way, the two
                // crossing over the request's blend distance.
                if (const BlendedClip stop = plannedStop(legHeading)) {
                    const float startDistance = L.distance;
                    const float speed = L.ticks > 0 ? startDistance * tps / float(L.ticks) : 0;
                    begin(Loco::Stop, stop, curveLeft(), legHeading);
                    L.endX = r.points[end - 1].x, L.endY = r.points[end - 1].y;
                    L.fade = locoPoseBlend(startDistance, stop.distance(), speed);
                    L.crossover = true;
                } else {
                    L.cycle = L.cycleStart;
                    walkOn(walkGait());
                }
                rx = ry = 0;
                yaw = 0;
            } else if (L.kind == Loco::Stop || L.sidestep) { // the end of the way (a stop, a sidestep): there
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
                L.cycle = L.cycleStart;
                walkOn(walkGait());
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

} // namespace routing_internal

namespace {

// The object whose footprint the walker's body would stand in at (x, y),
// as the planner sees the objects (addObstacles: not a Sim, not in a slot,
// blocking routing, not one of the route's own, nor one that lets the
// walker through, nor on a stair landing), or 0. The body is a person's
// footprint box (TSObjectBaseFootprintWidth: 0.2 tile), the objects' are
// their routing footprints (the model's, or the OBJD box), on the 3x3 tiles
// around (no footprint reaches further). OS2_TILE_FOOTPRINTS=1: the whole
// tile the walker steps onto, as before.
ObjectId footprintBlocker(VM& vm, const Object& me, const Object::Route& r, float x, float y, int level) {
    std::set<ObjectId> skip;
    for (ObjectId id : r.ignore)
        if (const Object* o = vm.object(id))
            skip.insert(multiTileGroup(*o));
    if (const Object* target = vm.object(r.target))
        skip.insert(multiTileGroup(*target));
    static const bool wholeTiles = std::getenv("OS2_TILE_FOOTPRINTS") != nullptr;
    const TilePos at{int(std::floor(x)), int(std::floor(y)), level};
    constexpr float kHalf = 0.1f;
    // The 3x3 tiles' objects in one walk; once an object's "allow
    // intersection" tree has run (it could move something), the next tiles'
    // are taken again, as before.
    const auto around = vm.objectsAround(at, 1);
    const uint64_t runs = vm.treeRuns;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            if (wholeTiles && (dx || dy))
                continue;
            const TilePos t{at.x + dx, at.y + dy, level};
            const std::vector<ObjectId> on =
                vm.treeRuns == runs ? around[size_t(dx + 1) * 3 + size_t(dy + 1)] : vm.objectsOnTile(t);
            for (ObjectId id : on) {
                const Object* o = vm.object(id);
                if (!o || id == me.id || o->person || !VM::blocksRouting(*o) || skip.count(multiTileGroup(*o)) ||
                    intersectionAllowed(vm, *o, me))
                    continue;
                if (vm.world && std::any_of(vm.world->stairs().begin(), vm.world->stairs().end(),
                                            [&](const Stair& s) { return s.bottom == t || s.top == t; }))
                    continue;
                if (wholeTiles)
                    return id;
                for (const auto& f : vm.routingFootprint(*o))
                    if (f[0] < x + kHalf && x - kHalf < f[2] && f[1] < y + kHalf && y - kHalf < f[3])
                        return id;
            }
        }
    return 0;
}

} // namespace

namespace routing_internal {

bool peopleMayOverlap(VM& vm, const Object& a, const Object& b) {
    // Either one may intersect people (object data 8 "flags" 0x04 | 0x10:
    // zero extent, allow person intersection), or either one's "allow
    // intersection" tree (OBJf 5) lets the two overlap. A person's
    // CanIntersectPeople is also bit 0x08 of its +0x72a field (inferred:
    // person data 68, as bit 0x02 there).
    auto ghostly = [](const Object& p) { return p.person && p.person->data.size() > 68 && (p.person->data[68] & 0x08); };
    return (a.data[objdata::Flags] & 0x14) || (b.data[objdata::Flags] & 0x14) || ghostly(a) || ghostly(b) ||
           intersectionAllowed(vm, b, a);
}

// What the walker would stand in at `to` (TSObjectCanSetPosition with error
// 0x0C "intersects object", whose object LocoPlaceFromSceneGraph makes the
// blocker), or 0: another Sim, or an object whose footprint it steps into.
// Between two Sims (TestPlacementWithRespectToObject, BV 0x8ac158) a body is
// a point: the step is refused when the two centres come within 0.283
// (0x274b968), whatever their ages; the Sim walked to has no exception.
// Person data 68 ("Ghost Flags") with bit 0x02 skips the test. The objects:
// the person's footprint box against their routing footprints
// (footprintBlocker). LocoPlaceFromSceneGraph (BV 0x6b2ee0, round n30): when
// the new place fails, the walker's current place is tested the same way,
// and if it fails too, for whatever reason (another Sim within 0.283, a
// footprint), the step goes; until round n30 the VM let a walker on only
// past the Sim (or out of the footprints) it already stood in, one by one.
// (The walls are left to the planner here.)
ObjectId stepBlocker(VM& vm, const Object& me, const Object::Route& r, float x, float y, int level) {
    if (me.person && me.person->data.size() > 68 && (me.person->data[68] & 0x02))
        return 0;
    auto personAt = [&](float px, float py, int lv) -> ObjectId {
        for (const ObjectId id : vm.people()) { // by ID, as objects() is
            const Object* po = vm.object(id);
            if (!po)
                continue;
            const Object& o = *po;
            if (id == me.id || !o.person || !o.position.inWorld() || o.data[objdata::ContainerId] != 0 ||
                o.position.level != lv)
                continue;
            if (std::hypot(o.position.x - px, o.position.y - py) < kPeopleApart && !peopleMayOverlap(vm, me, o))
                return id;
        }
        return 0;
    };
    static const bool wholeTiles = std::getenv("OS2_TILE_FOOTPRINTS") != nullptr;
    const TilePos to{int(std::floor(x)), int(std::floor(y)), level};
    const TilePos from = VM::tileOf(me);
    const bool footprints = me.position.inWorld() && (!wholeTiles || !(to == from));
    ObjectId id = personAt(x, y, level);
    if (!id && footprints)
        id = footprintBlocker(vm, me, r, x, y, level);
    if (!id || !me.position.inWorld())
        return id;
    // The current place fails too: the step goes.
    if (personAt(me.position.x, me.position.y, me.position.level) ||
        (footprints && footprintBlocker(vm, me, r, me.position.x, me.position.y, me.position.level)))
        return 0;
    return id;
}

// Puts the walker where this tick's locomotion step puts it.
void applyLocoStep(VM& vm, Object& me, Object::Route& r, const LocoStep& st) {
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
                     "loco t%llu %d %-5s %-32s %s tick %d/%d lead %.2f frame (%.3f,%.3f) h %6.1f root (%.3f,%.3f) face %6.1f "
                     "speed %.3f clip %.3f (own %.3f) slide %.3f%s%s%s\n",
                     (unsigned long long)vm.ticks, me.id, kinds[st.loco.kind], st.loco.animation.c_str(),
                     st.loco.mirror ? "M" : "-", st.loco.tick, st.loco.ticks, st.loco.lead, st.loco.fx, st.loco.fy,
                     st.loco.heading * 180 / float(M_PI), st.x, st.y, st.facing * 180 / float(M_PI), frame, natural,
                     own, std::fabs(frame - natural), st.loco.layers.size() > 1 ? " blend" : "",
                     [&] {
                         static std::string b;
                         b.clear();
                         if (st.loco.layers.size() > 1)
                             for (const auto& l : st.loco.layers)
                                 b += " " + l.animation + (l.mirror ? "(M)" : "") + "*" + std::to_string(l.weight).substr(0, 4);
                         return b.c_str();
                     }(),
                     st.loco.halting ? " halting" : "");
    }
    r.loco = st.loco;
    AnimationState& a = me.animation;
    if (st.beganFollow) // ComputeLocoStartTicks used them up
        a.previousEndMs = a.previousActionEndMs = AnimationState::kNoTicks;
    if (st.handOff) {
        ++me.locoHandOff;
        a.previousEndMs = st.endMs;
        a.previousActionEndMs = st.actionEndMs;
        a.previousPortion = st.endPortion;
        a.locoEndingUntilMs = st.endMs;
    }
    r.points.erase(r.points.begin(), r.points.begin() + std::ptrdiff_t(std::min(st.consumed, r.points.size())));
    moveTo(vm, me, st.x, st.y, st.level);
    setDirection(me, directionOf(-std::sin(st.facing), std::cos(st.facing)));
    if (r.loco.kind == Object::Route::Loco::Stop && !r.loco.halting && r.loco.tick >= r.loco.ticks) {
        // Stopped, turned to the final facing (BeginCachedEndTurnPath
        // is not needed).
        r.endTurned = true;
        r.loco.kind = Object::Route::Loco::None;
        r.loco.moving = false;
    }
}

} // namespace routing_internal

} // namespace os2::vm
