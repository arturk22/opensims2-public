// The VM's own state of a resumed thread (thread_state.hpp).

#include "thread_state.hpp"

#include "locomotion.hpp"

#include <algorithm>

#include "formats/binreader.hpp"
#include "formats/binwriter.hpp"

namespace os2::vm {

namespace {

// 2: a route's asideFrom (the Sim a step-aside route gives way to).
// 3: the locomotion's planned stop foot, short way, fade and halt walk.
// 4: the route's remembered blocker (DoBlockedGoalStillAvailable's +0x3e).
constexpr uint32_t kVersion = 5;
uint32_t readingVersion = kVersion; // of the state being read
bool knownVersion(uint32_t v) {
    readingVersion = v;
    return v >= 1 && v <= kVersion;
}

enum FrameFlag : uint8_t {
    RerunCaller = 0x01,
    RouteCallback = 0x02,
    PreRouteRan = 0x04,
    RouteResume = 0x08,
    HasCallerResult = 0x10,
    CallerResult = 0x20,
    FoundAction = 0x40,
};

uint8_t flagsOf(const Frame& f) {
    uint8_t b = 0;
    b |= f.rerunCaller ? RerunCaller : 0;
    b |= f.routeCallback ? RouteCallback : 0;
    b |= f.preRouteRan ? PreRouteRan : 0;
    b |= f.routeResume ? RouteResume : 0;
    b |= f.callerResult ? HasCallerResult : 0;
    b |= f.callerResult.value_or(false) ? CallerResult : 0;
    b |= f.foundAction ? FoundAction : 0;
    return b;
}

bool hasState(const Frame& f) {
    return flagsOf(f) || f.postRoute || f.pendingRouteResult != -1 || f.routeSlot != -1;
}

void writeRoute(BinWriter& w, const Object::Route& r, size_t frame) {
    w.u32(uint32_t(frame));
    w.u32(uint32_t(r.points.size()));
    for (const auto& p : r.points) {
        w.f32(p.x), w.f32(p.y), w.i32(p.level), w.i32(p.ticks), w.f32(p.z), w.i32(p.direction);
        w.str7(p.animation);
        w.i16(p.door), w.i16(p.portal), w.i16(p.portalOwner);
        for (int16_t v : p.portalParams)
            w.i16(v);
        w.u8(p.viaPortal), w.i32(p.turnSteps), w.u8(p.mirror);
    }
    w.i32(r.level), w.i32(r.facing), w.i16(r.target);
    w.u32(uint32_t(r.goals.size()));
    for (const auto& g : r.goals)
        w.f32(g[0]), w.f32(g[1]), w.f32(g[2]);
    w.u32(uint32_t(r.facings.size()));
    for (int f : r.facings)
        w.i32(f);
    w.u32(uint32_t(r.multi.size()));
    for (int16_t m : r.multi)
        w.i16(m);
    w.u32(uint32_t(r.ignore.size()));
    for (ObjectId i : r.ignore)
        w.i16(i);
    w.i32(r.blockedTicks), w.i32(r.replans), w.f32(r.blockerX), w.f32(r.blockerY);
    w.u8(r.waiting), w.u8(r.failureTrees), w.i16(r.blocker);
    w.u8(r.aside), w.u8(r.asideTried), w.u8(r.pausing), w.u8(r.blockedTree);
    w.i16(r.asideFrom);
    w.i32(r.pauses), w.u8(r.inPortal), w.i32(r.check);
    w.u32(uint32_t(r.lastCheck)), w.u32(uint32_t(r.lastCheck >> 32));
    const auto& c = r.climb;
    w.str7(c.animation), w.u32(c.serial), w.f32(c.z), w.i32(c.ticksLeft), w.i32(c.turnSteps), w.u8(c.mirror);
    w.u8(r.endTurned);
    const auto& l = r.loco;
    w.u8(uint8_t(l.kind)), w.u8(l.moving);
    w.str7(l.animation), w.u32(l.serial), w.u8(l.mirror);
    w.f32(l.strideWeight), w.f32(l.stopWeight), w.f32(l.overlap), w.u8(l.sidestep);
    w.i32(l.tick), w.i32(l.ticks), w.f32(l.lead), w.f32(l.distance), w.f32(l.speed);
    w.f32(l.fx), w.f32(l.fy), w.f32(l.heading), w.f32(l.turn), w.f32(l.endTurn), w.f32(l.cycle), w.f32(l.cycleStart);
    w.f32(l.endX), w.f32(l.endY);
    w.u32(uint32_t(l.layers.size()));
    for (const auto& y : l.layers)
        w.str7(y.animation), w.f32(y.weight), w.u8(y.mirror), w.f32(y.rate), w.f32(y.fade);
    // The curve, by what it was made from (LocoCurve's constructor).
    w.u8(l.curve != nullptr);
    if (l.curve) {
        const auto& k = *l.curve;
        w.f32(k.control.empty() ? 0.0f : k.control[0][0]);
        w.f32(k.control.empty() ? 0.0f : k.control[0][1]);
        w.u32(uint32_t(k.points));
        for (size_t i = 0; i < k.points; ++i)
            w.f32(k.control[3 + i][0]), w.f32(k.control[3 + i][1]);
    }
    w.u32(uint32_t(l.segment)), w.u32(uint32_t(l.taken)), w.f32(l.arc), w.u8(l.onCurve);
    w.u8(l.halting), w.u8(l.haltDone), w.i16(l.halted);
    w.u8(l.stopOtherFoot), w.u8(l.shortWay), w.f32(l.fade), w.f32(l.haltWalk), w.u8(l.haltOtherFoot), w.u8(l.crossover), w.f32(l.haltRoom), w.i32(l.requestTicks), w.f32(l.requestLength);
    w.i16(r.lastBlocker);
    w.u32(r.plans), w.u32(r.checkPlans), w.i32(r.checkRoom), w.i32(r.checkLevel), w.f32(r.checkWalked);
    w.u8(r.blockCheckDone);
}

Object::Route readRoute(BinReader& r, size_t& frame) {
    Object::Route x;
    frame = r.u32();
    for (uint32_t n = r.u32(); n; --n) {
        Object::Route::Point p;
        p.x = r.f32(), p.y = r.f32(), p.level = r.i32(), p.ticks = r.i32(), p.z = r.f32(), p.direction = r.i32();
        p.animation = r.str7();
        p.door = r.i16(), p.portal = r.i16(), p.portalOwner = r.i16();
        for (int16_t& v : p.portalParams)
            v = r.i16();
        p.viaPortal = r.u8(), p.turnSteps = r.i32(), p.mirror = r.u8();
        x.points.push_back(std::move(p));
    }
    x.level = r.i32(), x.facing = r.i32(), x.target = r.i16();
    for (uint32_t n = r.u32(); n; --n) {
        const float a = r.f32(), b = r.f32(), c = r.f32();
        x.goals.push_back({a, b, c});
    }
    for (uint32_t n = r.u32(); n; --n)
        x.facings.push_back(r.i32());
    for (uint32_t n = r.u32(); n; --n)
        x.multi.push_back(r.i16());
    for (uint32_t n = r.u32(); n; --n)
        x.ignore.push_back(r.i16());
    x.blockedTicks = r.i32(), x.replans = r.i32(), x.blockerX = r.f32(), x.blockerY = r.f32();
    x.waiting = r.u8(), x.failureTrees = r.u8(), x.blocker = r.i16();
    x.aside = r.u8(), x.asideTried = r.u8(), x.pausing = r.u8(), x.blockedTree = r.u8();
    if (readingVersion >= 2)
        x.asideFrom = r.i16();
    x.pauses = r.i32(), x.inPortal = r.u8(), x.check = r.i32();
    x.lastCheck = r.u32();
    x.lastCheck |= uint64_t(r.u32()) << 32;
    auto& c = x.climb;
    c.animation = r.str7(), c.serial = r.u32(), c.z = r.f32(), c.ticksLeft = r.i32(), c.turnSteps = r.i32(),
    c.mirror = r.u8();
    x.endTurned = r.u8();
    auto& l = x.loco;
    l.kind = Object::Route::Loco::Kind(r.u8()), l.moving = r.u8();
    l.animation = r.str7(), l.serial = r.u32(), l.mirror = r.u8();
    l.strideWeight = r.f32(), l.stopWeight = r.f32(), l.overlap = r.f32(), l.sidestep = r.u8();
    l.tick = r.i32(), l.ticks = r.i32(), l.lead = r.f32(), l.distance = r.f32(), l.speed = r.f32();
    l.fx = r.f32(), l.fy = r.f32(), l.heading = r.f32(), l.turn = r.f32(), l.endTurn = r.f32(), l.cycle = r.f32(),
    l.cycleStart = r.f32();
    l.endX = r.f32(), l.endY = r.f32();
    for (uint32_t n = r.u32(); n; --n) {
        LocoLayer y;
        y.animation = r.str7(), y.weight = r.f32(), y.mirror = r.u8(), y.rate = r.f32(), y.fade = r.f32();
        l.layers.push_back(std::move(y));
    }
    if (r.u8()) {
        const float cx = r.f32(), cy = r.f32();
        std::vector<std::array<float, 2>> through(r.u32());
        for (auto& p : through)
            p[0] = r.f32(), p[1] = r.f32();
        l.curve = std::make_shared<const LocoCurve>(cx, cy, through);
    }
    l.segment = r.u32(), l.taken = r.u32(), l.arc = r.f32(), l.onCurve = r.u8();
    l.halting = r.u8(), l.haltDone = r.u8(), l.halted = r.i16();
    if (readingVersion >= 3)
        l.stopOtherFoot = r.u8(), l.shortWay = r.u8(), l.fade = r.f32(), l.haltWalk = r.f32(), l.haltOtherFoot = r.u8(),
        l.crossover = r.u8(), l.haltRoom = r.f32(), l.requestTicks = r.i32(), l.requestLength = r.f32();
    if (readingVersion >= 4)
        x.lastBlocker = r.i16();
    if (readingVersion >= 5)
        x.plans = r.u32(), x.checkPlans = r.u32(), x.checkRoom = r.i32(), x.checkLevel = r.i32(), x.checkWalked = r.f32(),
        x.blockCheckDone = r.u8();
    return x;
}

} // namespace

std::optional<std::vector<uint8_t>> writeThreadState(VM& vm, const Object& o) {
    auto t = vm.threads.find(o.id);
    const bool running = t != vm.threads.end() && t->second.status == Thread::Status::Running;
    static const std::vector<Frame> none;
    const auto& stack = running ? t->second.stack : none;
    // The route's instruction: its frame (Route::depth is the stack depth
    // with it on top).
    const bool route = o.route && o.route->depth >= 1 && o.route->depth <= stack.size() &&
                       stack[o.route->depth - 1].behavior == o.route->tree &&
                       stack[o.route->depth - 1].pc == o.route->node;
    bool thread = route || (running && o.eyeAttractor);
    for (const Frame& f : stack)
        thread = thread || hasState(f);
    // An engine's destination (Move Out Of Way's synthetic routing slot, a
    // Go Here destination the Sims manage).
    const bool destination = o.syntheticSlot || o.managedDestination || o.destinationHeld;
    if (!thread && !destination)
        return std::nullopt;
    BinWriter w;
    w.u32(kVersion);
    w.u8(uint8_t((thread ? 1 : 0) | (destination ? 2 : 0)));
    if (thread) {
        w.u32(uint32_t(stack.size()));
        for (const Frame& f : stack) {
            w.u16(f.instance), w.u16(f.pc);
            w.u8(flagsOf(f)), w.u8(f.postRoute), w.i16(f.pendingRouteResult), w.i16(f.routeSlot);
        }
        w.i16(o.eyeAttractor);
        w.u8(route);
        if (route)
            writeRoute(w, *o.route, o.route->depth - 1);
    }
    if (destination) {
        w.u8(o.managedDestination), w.u8(o.destinationHeld), w.u8(o.syntheticSlot.has_value());
        if (o.syntheticSlot) {
            const auto& slot = *o.syntheticSlot;
            w.u16(slot.type), w.f32(slot.offset[0]), w.f32(slot.offset[1]), w.f32(slot.offset[2]), w.i32(slot.facing);
        }
    }
    return std::move(w.bytes());
}

bool applyThreadState(VM& vm, Object& o, std::span<const uint8_t> bytes) {
    auto t = vm.threads.find(o.id);
    try {
        BinReader r(bytes);
        if (!knownVersion(r.u32()))
            return false;
        const uint8_t parts = r.u8();
        struct Saved {
            uint8_t flags, postRoute;
            int16_t pending, slot;
        };
        bool fits = false;
        std::vector<Saved> saved;
        ObjectId eye = 0;
        std::optional<Object::Route> route;
        size_t frame = 0;
        if (parts & 1) {
            const uint32_t n = r.u32();
            fits = t != vm.threads.end() && n == t->second.stack.size();
            for (uint32_t i = 0; i < n; ++i) {
                const uint16_t tree = r.u16(), node = r.u16();
                Saved s;
                s.flags = r.u8(), s.postRoute = r.u8(), s.pending = r.i16(), s.slot = r.i16();
                saved.push_back(s);
                if (fits && (tree != t->second.stack[i].instance || node != t->second.stack[i].pc))
                    fits = false; // written for another stack
            }
            eye = r.i16();
            if (r.u8())
                route = readRoute(r, frame);
            fits = fits && (!route || frame < saved.size());
        }
        if (parts & 2) {
            o.managedDestination = r.u8();
            o.destinationHeld = r.u8();
            if (r.u8()) {
                Slots::Slot slot;
                slot.type = r.u16();
                slot.offset = {r.f32(), r.f32(), r.f32()};
                slot.facing = r.i32();
                o.syntheticSlot = slot;
            }
            vm.noteDestination(o);
        }
        if (!fits)
            return false;
        auto& stack = t->second.stack;
        for (size_t i = 0; i < stack.size(); ++i) {
            Frame& f = stack[i];
            const Saved& s = saved[i];
            f.rerunCaller = s.flags & RerunCaller;
            f.routeCallback = s.flags & RouteCallback;
            f.preRouteRan = s.flags & PreRouteRan;
            f.routeResume = s.flags & RouteResume;
            f.callerResult = (s.flags & HasCallerResult) ? std::optional<bool>((s.flags & CallerResult) != 0)
                                                         : std::nullopt;
            f.foundAction = f.foundAction || (s.flags & FoundAction);
            f.postRoute = s.postRoute;
            f.pendingRouteResult = s.pending;
            f.routeSlot = s.slot;
        }
        o.eyeAttractor = vm.object(eye) ? eye : 0;
        if (route) {
            route->depth = frame + 1;
            route->tree = stack[frame].behavior;
            route->node = stack[frame].pc;
            o.route = std::move(route);
        }
        return true;
    } catch (const FormatError&) {
        return false;
    }
}

std::vector<uint8_t> writeLotState(VM& vm) {
    BinWriter w;
    w.u32(kVersion);
    w.u32(plannerNoise(vm));
    // The VM's tick count (what the routes' checks and the like are timed
    // by), low and high words.
    w.u32(uint32_t(vm.ticks));
    w.u32(uint32_t(vm.ticks >> 32));
    return std::move(w.bytes());
}

void applyLotState(VM& vm, std::span<const uint8_t> bytes) {
    try {
        BinReader r(bytes);
        if (!knownVersion(r.u32()))
            return;
        restorePlannerNoise(vm, r.u32());
        uint64_t ticks = r.u32();
        ticks |= uint64_t(r.u32()) << 32;
        vm.ticks = std::max(vm.ticks, ticks);
    } catch (const FormatError&) {
    }
}

} // namespace os2::vm
