// Run Functional Tree (0x14) and Reach/Put (0x74). Operand layouts and
// enumerations come from the game's own data: the SimAntics editor's labels
// in objects.package (group 0x7FE59FD0: STR# 0xC9 "Function Table Names",
// STR# 0xF5 "entry points"), the editor's disassembly strings left in
// Sims2.exe (cEdithLanguage, in reverse order of the code) and how the
// game's trees use each byte (`s2pkg prim-usage <Res> 14|74`). What is
// inferred is said where it is used; TODO.md (M5) keeps the list.

#include "vm.hpp"

#include "animation.hpp"

#include "formats/binreader.hpp"
#include "formats/anim.hpp"

#include <algorithm>
#include <cmath>

namespace os2::vm {

namespace {


// --- Run Functional Tree -------------------------------------------------------

// Function numbers (STR# 0xC9 "Function Table Names") to OBJf entries (STR#
// 0xF5 "entry points"), matched by name: prepare food -> "prep" (18), ...,
// serving surface (10) -> 14, garden (12) -> "gardening" (16), dry/wash
// clothes -> 31/32, message handler -> 36, ..., along route callback -> 41,
// awareness -> 42, reset -> 43, lookatTarget -> 44, Utility State Change ->
// 46, Global Awareness -> 53. The first 15 are The Sims 1's table (FreeSO's
// VMFindBestObjectForFunction.FunctionToEntryPoint); the OBJf actions at
// those entries are the objects' "Function - Sit", "Function - Dispose", ...
constexpr uint8_t kFunctionEntry[] = {18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 14, 28, 16, 17,
                                      29, 31, 32, 36, 37, 38, 39, 40, 41, 42, 43, 44, 46, 53};

const ObjectFunctions::Entry* functionEntry(VM& vm, const Object& o, uint16_t function) {
    if (function >= std::size(kFunctionEntry))
        return nullptr;
    const ObjectFunctions* f = vm.code().functions(o.group, o.definitionInstance);
    const size_t entry = kFunctionEntry[function];
    return f && entry < f->functions.size() ? &f->functions[entry] : nullptr;
}

// Runs `guard` of `owner`'s code for `me` as a check tree, to the end, in a
// thread of its own (with the running thread's temps, as interaction checks
// get the Sim's): true if it returns true. A guard that does not exist
// passes (as FreeSO's "else Execute = true").
bool runCheck(Context& c, ObjectId owner, uint16_t guard, std::array<int16_t, 4> args) {
    const Object* o = c.vm.object(owner);
    if (!o || !c.vm.resolveTree(*o, guard).second)
        return true;
    Thread t = c.vm.start(c.thread.me, guard, args, owner, owner);
    t.temps = c.thread.temps;
    constexpr size_t kMaxTurns = 1000; // sleeps in a check tree count down without waiting
    for (size_t turn = 0; t.status == Thread::Status::Running && turn < kMaxTurns; ++turn)
        c.vm.run(t, c.vm.instructionsPerTurn);
    if (t.status == Thread::Status::Failed)
        throw VMError("Run Functional Tree: check tree failed: " + t.error.substr(0, t.error.find('\n')));
    return t.status == Thread::Status::Finished && t.result;
}

// Run Functional Tree (0x14): function u16 at 0 (STR# 0xC9), flags u8 at 2.
// The flags are named by the editor's disassembly strings, in code order
// ", running check tree only", ", passing parameters from calling tree",
// ", change icon", and the trees agree: 0x04 (6 uses: "Set to Next - Good
// Dining Chair", "Get in Temp 0 - Bar Stool ID") only asks whether the
// object can do it, 0x02 (9 uses, all in relays: "Function - Message
// Handler", "Function - Pre Route", "Function - Awareness - Private") passes
// the running tree's parameters on, 0x01 changes the interaction's icon (as
// FreeSO's only flag; there is no icon here). As in FreeSO: the stack
// object's OBJf entry for the function runs its guard as a check tree, then
// its action as a subroutine in my thread with the stack object as the code
// owner; the instruction branches on its result. False when the stack
// object is missing, has no action for the function or its guard fails;
// 0xFFFF (one use, "zzz") is FreeSO's invalid operand, false too. Bytes 8+
// (non-zero in 8 uses) look like editor leftovers.
Exit runFunctionalTree(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t function = le16(&o[0]);
    const uint8_t flags = o[2];
    if (function == 0xFFFF)
        return Exit::False;
    if (function >= std::size(kFunctionEntry))
        throw VMError("Run Functional Tree: function " + std::to_string(function) + " out of range");
    const Object* target = c.vm.object(c.frame.stackObject);
    if (!target)
        return Exit::False;
    const ObjectFunctions::Entry* e = functionEntry(c.vm, *target, function);
    if (!e || !e->action)
        return Exit::False;
    std::array<int16_t, 4> args{};
    if (flags & 0x02)
        std::copy_n(c.frame.params.begin(), std::min<size_t>(c.frame.params.size(), 4), args.begin());
    const ObjectId id = target->id;
    const uint16_t action = e->action, guard = e->guard;
    if (guard && !runCheck(c, id, guard, args))
        return Exit::False;
    if (flags & 0x04)
        return Exit::True;
    if (!c.vm.object(id) || !c.vm.resolveTree(*c.vm.object(id), action).second)
        return Exit::False; // the check tree removed it, or the action does not exist
    c.vm.call(c.thread, id, action, args);
    c.thread.stack.back().stackObject = id;
    return Exit::Call;
}

// --- Reach/Put -----------------------------------------------------------------

const Slots* slotTable(VM& vm, const Object& o) {
    return o.definition ? vm.code().slots(o.group, o.definition->slotId()) : nullptr;
}

// Containment slot `index` of `o`: the SLOT's type 0 entries, in order (as
// Snap Into and the slot data owners count them); null if there is none.
const Slots::Slot* containmentSlot(VM& vm, const Object& o, int index) {
    if (index < 0)
        return nullptr;
    if (const Slots* t = slotTable(vm, o))
        for (const auto& s : t->slots)
            if (s.type == 0 && index-- == 0)
                return &s;
    return nullptr;
}

size_t containmentSlotCount(VM& vm, const Object& o) {
    size_t n = 0;
    if (const Slots* t = slotTable(vm, o))
        for (const auto& s : t->slots)
            n += s.type == 0;
    return n;
}

// The surface of the generic reach animations ("a2o-<surface>-get/put") for
// a slot's height. The heights are The Sims 1's enumeration (FreeSO's
// SLOT.HeightOffsets: 1 floor, 2 low table, 3 table, 4 counter, 5
// non-standard, 6 in hand, 7 sitting, 8 end table), which the game's SLOTs
// keep: coffee tables and the chessboard's low slot use 2, desks and dining
// tables 3, counters 4, end tables 8. Inferred: 8 reaches like a table, 9-11
// (counters' extra slots) and 5 (appliances, accessories) like a counter,
// the rest (in hand, sitting) like the floor, as FreeSO does for a Sim.
std::string surfaceFor(int height) {
    switch (height) {
    case 2: return "tableCoffee";
    case 3:
    case 8: return "table";
    case 4:
    case 5:
    case 9:
    case 10:
    case 11: return "counter";
    default: return "floor";
    }
}

// Where an object is reached at: the height of the containment slot it is
// in, or the floor.
std::string surfaceOf(VM& vm, const Object& x) {
    const Object* container = vm.object(x.data[objdata::ContainerId]);
    if (!container || container->person)
        return "floor";
    const Slots::Slot* s = containmentSlot(vm, *container, x.data[objdata::SlotNumber]);
    return surfaceFor(s ? s->height : 1);
}

// The reach animation for `sim`: "<age>2o-<surface>-<get|put>" ("a", "c"),
// falling back to the adult one (teens and elders use the adult reaches;
// only adults and children have them in the game).
AnimationLibrary& library(VM& vm) {
    if (!vm.animations.index())
        vm.animations.setIndex(vm.code().index());
    return vm.animations;
}

std::string reachAnimation(VM& vm, const Object& sim, const std::string& surface, bool get) {
    const std::string rest = "2o-" + surface + (get ? "-get" : "-put");
    const std::string own = agePrefix(sim) + rest;
    if (library(vm).exists(own))
        return own;
    return "a" + rest;
}

// Tick of the animation's event 0 ("transfer object to hand" / "... to
// slot"), the moment the object changes hands, in VM ticks; nothing if the
// animation has none.
std::optional<int> transferTick(VM& vm, const std::string& name) {
    const auto t = library(vm).eventTicks(name, "event=0");
    if (!t)
        return std::nullopt;
    return int(std::lround(double(*t) / Animation::kTicksPerSecond * vm.clock.ticksPerMinute));
}

int ticksFor(VM& vm, float seconds) {
    return int(std::ceil(double(seconds) * vm.clock.ticksPerMinute - 1e-6));
}

// Entry `index` of the adult animation list (STR# 0x81) for the running code:
// the tree's group, the code owner's, its semi-global, then the global one
// (as the animation primitives look lists up). Empty if none has it.
std::string adultListEntry(Context& c, int index) {
    CodeLibrary& code = c.vm.code();
    std::vector<uint32_t> groups{c.frame.group};
    if (const Object* owner = c.vm.object(c.frame.codeOwner))
        groups.push_back(owner->group), groups.push_back(owner->semiGroup);
    groups.push_back(kGlobalGroup);
    for (uint32_t g : groups)
        if (const StringTable* t = g ? code.strings(g, animlist::Adult) : nullptr)
            if (index >= 0 && size_t(index) < t->count(lang::EnglishUS))
                return t->get(size_t(index), lang::EnglishUS);
    return {};
}

// The grasp and carry overlays of a list entry "a2o-newspaper-grasp,
// a2o-newspaper-carry" or "a2o-phoneWall-grasp, a2o-phoneTable-carry(carry)":
// each name without its role and speed, resolved for the Sim's age.
std::vector<std::string> carryOverlays(VM& vm, const Object& sim, const std::string& entry) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t comma = entry.find(',', start);
        std::string item = entry.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (const size_t cut = item.find_first_of("(@"); cut != std::string::npos)
            item.resize(cut);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.erase(item.begin());
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.pop_back();
        if (!item.empty() && item != ".")
            out.push_back(resolveListAnimation(vm, sim, item));
        if (comma == std::string::npos)
            return out;
        start = comma + 1;
    }
}

void putInSlot(Object& x, const Object& container, int slot) {
    x.data[objdata::ContainerId] = container.id;
    x.data[objdata::SlotNumber] = int16_t(slot);
    x.position = container.position;
    x.data[objdata::Room] = container.data[objdata::Room];
    x.route.reset();
}

// The hands are a Sim's containment slots 0 and 1 ("Object in Hand - Get
// ID(tmp 0) & Hand(tmp 3)" reads my slot 0, then my slot 1, into temp 3).
constexpr int kHands = 2;

// Reach/Put (0x74), a Sims 2 primitive ("Reach/Put" in STR# 0x8B; The Sims
// 1's Reach, 0x2F, is "~(old)Reach"). The editor's disassembly strings, in
// code order: "Drop object onto Floor " / "Pick up Object in " / "Drop onto
// Object in ", ", object anim : ", ", using Sim age as filter on object
// Anim", ", grasp anim : ", ", expecting handedness in temp 3"; its errors:
// "Cant run Reach/Drop on non Sim object.", "No Object for Reach to work
// on.", "Missing reach animation.", "Reach animation does not contain event
// 0." Operands, from how the 188 uses in the game agree with that: the
// object (owner u8 at 3, data u16 at 4), mode u8 at 10 (0 pick up: every
// "CT - Reach - Grab"; 1 drop onto the object; 2 drop onto the floor: "CT -
// Reach - Drop" drops onto the object in parameter 0 with mode 1, and with
// mode 2 when parameter 0 is 0), flags u8 at 9 and the grasp animation u16
// at 11: an entry of the adult animation list ("a2o-newspaper-grasp,
// a2o-newspaper-carry"). Flag 0x01: the slot to drop into is in temp 0
// (inferred: 47 of its 51 uses set temp 0 in the same tree, 33 of the other
// 137 do; "CT - Reach - Drop" copies its slot parameter there); 0x02: the
// hand is in temp 3 (inferred, "expecting handedness in temp 3": 20 of its
// 50 uses set temp 3, 12 of the other 138). Not understood: the object
// animation (byte 7, with byte 8: 0x57/1, 0xE7/3, ...), flags 0x04-0x10, the
// u16 at 13 and byte 15 (the chessboard's reaches give 2, 3, 8-11 there).
//
// The Sim plays the generic reach animation for the height where the object
// is or goes ("a2o-floor-get", "a2o-counter-put", "a2o-tableCoffee-get",
// ...: surfaceFor) and the object changes hands at the animation's event 0;
// the instruction returns true when the animation ends. Picking up puts the
// object in a free hand (slot 0, else 1; false with both full) and adds the
// grasp and carry animations as overlays on the Sim; dropping takes them
// off and puts what the hand holds into the object's containment slot
// (temp 0 with flag 0x01, else the first free one; false if it is taken) or
// on the floor tile in front of the Sim (else its own tile), facing as the
// Sim. Dropping with empty hands is false.
Exit reachPut(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    if (!me.person)
        throw VMError("Cant run Reach/Drop on non Sim object.");
    const uint8_t mode = o[10], flags = o[9];
    if (mode > 2)
        throw VMError("Reach/Put: mode " + std::to_string(mode) + " not implemented");
    const size_t depth = c.thread.stack.size();
    AnimationState& anim = me.animation;

    if (me.reach && me.reach->depth == depth && me.reach->tree == c.frame.behavior && me.reach->node == c.frame.pc) {
        Object::Reach& r = *me.reach;
        ++r.tick;
        if (!r.transferred && r.tick >= r.transferAt) {
            r.transferred = true;
            Object* x = c.vm.object(r.object);
            Object* to = c.vm.object(r.target);
            if (r.mode == 0) {
                if (x && x->data[objdata::ContainerId] != me.id) {
                    putInSlot(*x, me, r.hand);
                    me.carryOverlays = r.overlays;
                    for (const auto& name : r.overlays)
                        anim.overlays.push_back(name);
                    ++anim.overlaySerial;
                }
            } else if (x) {
                for (const auto& name : me.carryOverlays)
                    if (auto it = std::find(anim.overlays.begin(), anim.overlays.end(), name); it != anim.overlays.end())
                        anim.overlays.erase(it);
                me.carryOverlays.clear();
                ++anim.overlaySerial;
                if (r.mode == 1 && to) {
                    putInSlot(*x, *to, r.slot);
                } else {
                    x->data[objdata::ContainerId] = 0;
                    x->data[objdata::SlotNumber] = -1;
                    x->position = {float(r.floor.x) + 0.5f, float(r.floor.y) + 0.5f, r.floor.level};
                    x->data[objdata::Direction] = me.data[objdata::Direction];
                    if (c.vm.world)
                        x->data[objdata::Room] = c.vm.world->room(r.floor);
                }
            }
        }
        if (r.tick < r.end)
            return Exit::Yield;
        me.reach.reset();
        if (anim.name == r.animation) {
            anim.name.clear();
            anim.tree = nullptr;
        }
        return Exit::True;
    }
    me.reach.reset();

    Object::Reach r;
    r.mode = mode;
    r.depth = depth, r.tree = c.frame.behavior, r.node = c.frame.pc;
    auto held = [&](int hand) { return c.vm.containedIn(me.id, int16_t(hand)); };
    std::string surface;
    if (mode == 0) {
        Object* x = c.vm.object(c.get(o[3], int16_t(le16(&o[4]))));
        if (!x)
            throw VMError("No Object for Reach to work on.");
        if (x->data[objdata::ContainerId] == me.id)
            return Exit::True; // already in my hand
        r.object = x->id;
        r.hand = -1;
        if (flags & 0x02) {
            const int h = c.thread.temps[3];
            if (h >= 0 && h < kHands && !held(h))
                r.hand = h;
        } else {
            for (int h = 0; h < kHands && r.hand < 0; ++h)
                if (!held(h))
                    r.hand = h;
        }
        if (r.hand < 0)
            return Exit::False;
        surface = surfaceOf(c.vm, *x);
        if (const std::string entry = adultListEntry(c, le16(&o[11])); !entry.empty())
            r.overlays = carryOverlays(c.vm, me, entry);
    } else {
        // What the hand holds: the hand in temp 3 with flag 0x02, else the first that holds something.
        ObjectId x = 0;
        if (flags & 0x02) {
            const int h = c.thread.temps[3];
            x = h >= 0 && h < kHands ? held(h) : 0;
        } else {
            for (int h = 0; h < kHands && !x; ++h)
                x = held(h);
        }
        if (!x)
            return Exit::False;
        r.object = x;
        if (mode == 1) {
            const Object* to = c.vm.object(c.get(o[3], int16_t(le16(&o[4]))));
            if (!to)
                throw VMError("No Object for Reach to work on.");
            r.target = to->id;
            const size_t slots = containmentSlotCount(c.vm, *to);
            if (flags & 0x01) {
                r.slot = c.thread.temps[0];
                if (r.slot < 0 || size_t(r.slot) >= slots)
                    throw VMError("Slot number out of range. (slot " + std::to_string(r.slot) + " of " +
                                  std::to_string(slots) + ")");
                if (const ObjectId there = c.vm.containedIn(to->id, int16_t(r.slot)); there && there != x)
                    return Exit::False;
            } else {
                r.slot = -1;
                for (size_t s = 0; s < slots && r.slot < 0; ++s)
                    if (!c.vm.containedIn(to->id, int16_t(s)))
                        r.slot = int(s);
                if (r.slot < 0)
                    return Exit::False;
            }
            const Slots::Slot* s = containmentSlot(c.vm, *to, r.slot);
            surface = surfaceFor(s ? s->height : 1);
        } else {
            if (!me.position.inWorld())
                throw VMError("Reach/Put: dropping onto the floor while out of world");
            const TilePos at = VM::tileOf(me);
            const auto [dx, dy] = directionStep(((me.data[objdata::Direction] % 8) + 8) % 8);
            const TilePos front{at.x + dx, at.y + dy, at.level};
            const ObjectId ignore[] = {x, me.id};
            r.floor = c.vm.world && c.vm.tileFree(front, ignore) ? front : at;
            surface = "floor";
        }
    }

    r.animation = reachAnimation(c.vm, me, surface, mode == 0);
    const auto seconds = library(c.vm).seconds(r.animation);
    if (!seconds)
        throw VMError("Missing reach animation. (" + r.animation + ")");
    const auto transfer = transferTick(c.vm, r.animation);
    if (!transfer)
        throw VMError("Reach animation does not contain event 0. (" + r.animation + ")");
    r.end = std::max(1, ticksFor(c.vm, *seconds));
    r.transferAt = std::clamp(*transfer, 0, r.end);
    anim.name = r.animation;
    ++anim.serial;
    anim.ticksLeft = r.end;
    anim.depth = depth, anim.tree = c.frame.behavior, anim.node = c.frame.pc;
    me.reach = std::move(r);
    return Exit::Yield;
}

} // namespace

void registerReachPrimitives(VM& vm) {
    vm.setPrimitive(0x14, runFunctionalTree);
    vm.setPrimitive(0x74, reachPut);
}

} // namespace os2::vm
