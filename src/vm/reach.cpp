// Find Best Object for Function (0x0E), Run Functional Tree (0x14) and
// Reach/Put (0x74). Operand layouts and
// enumerations come from the game's own data: the SimAntics editor's labels
// in objects.package (group 0x7FE59FD0: STR# 0xC9 "Function Table Names",
// STR# 0xF5 "entry points"), the editor's disassembly strings left in
// Sims2.exe (cEdithLanguage, in reverse order of the code) and how the
// game's trees use each byte (`s2pkg prim-usage <Res> 14|74`). What is
// inferred is said where it is used; TODO.md (M5) keeps the list.

#include "vm.hpp"

#include "animation.hpp"
#include "errors.hpp"

#include "formats/binreader.hpp"
#include "formats/anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

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
// The Mac binary has the same fixed table, except that 24 "reset" has no
// entry (0 here) and 27 "Global Awareness" (0x35) is Bon Voyage only
// (docs/primitives-mac.md §4/§5b): both give false.
constexpr uint8_t kFunctionEntry[] = {18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 14, 28, 16, 17,
                                      29, 31, 32, 36, 37, 38, 39, 40, 41, 42, 0, 44, 46, 0};

const ObjectFunctions::Entry* functionEntry(VM& vm, const Object& o, uint16_t function) {
    if (function >= std::size(kFunctionEntry) || kFunctionEntry[function] == 0)
        return nullptr;
    const ObjectFunctions* f = vm.code().functions(o.group, o.definitionInstance);
    const size_t entry = kFunctionEntry[function];
    return f && entry < f->functions.size() ? &f->functions[entry] : nullptr;
}

// Runs `guard` of `owner`'s code for `me` as a check tree, to the end, in a
// thread of its own: true if it returns true. A guard that does not exist
// passes (as FreeSO's "else Execute = true"). The binary runs it in my own
// tree sim (cTSObject::RunOneTickTree with no parameters: they default to
// my temps) and does not restore my temps afterwards, so what the tree
// writes to them is what the caller reads next: "Function - Prep - TEST"
// returns false for a free counter in autonomy and leaves the kinds of food
// it can prepare in temp 2, which "Function Test - Prep" hands to the
// fridge's meal check trees.
bool runCheck(Context& c, ObjectId owner, uint16_t guard, std::array<int16_t, 4> args) {
    const Object* o = c.vm.object(owner);
    if (!o || !c.vm.resolveTree(*o, guard).second)
        return true;
    Thread t = c.vm.start(c.thread.me, guard, args, owner, owner);
    t.temps = c.thread.temps;
    constexpr size_t kMaxTurns = 1000; // sleeps in a check tree count down without waiting
    for (size_t turn = 0; t.status == Thread::Status::Running && turn < kMaxTurns; ++turn)
        c.vm.run(t, c.vm.instructionsPerTurn);
    c.thread.temps = t.temps;
    if (t.status == Thread::Status::Failed)
        throw VMError("Run Functional Tree: check tree failed: " + t.error.substr(0, t.error.find('\n')));
    return t.status == Thread::Status::Finished && t.result;
}

// Run Functional Tree (0x14), as the Mac binary has it
// (TryCallFunctionalTree; docs/primitives-mac.md §4): function u16 at 0
// (STR# 0xC9), flags u16 at 2: 0x01 changes the interaction's icon (there is
// no icon here), 0x02 passes the running tree's parameters on, 0x04 runs
// the entry's check tree instead of its action tree. The target is the stack
// object (none: error 0x15); the function goes through the fixed table above
// (24 "reset" and, in the base game, 27 "Global Awareness" have no entry:
// false), and that OBJf entry's action tree (with 0x04, its check tree)
// runs as a subroutine in my thread with the stack object as "me" for its
// code (GosubObjectTree); the instruction branches on its result. Tree 0,
// one that does not exist or a function past the table: false. The binary
// does not run the check tree before the action tree.
Exit runFunctionalTree(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t function = le16(&o[0]);
    const uint16_t flags = le16(&o[2]);
    const Object* target = c.vm.object(c.frame.stackObject);
    if (!target)
        throw gameError(err::NoStackObject, "Run Functional Tree");
    const ObjectFunctions::Entry* e = functionEntry(c.vm, *target, function);
    if (!e)
        return Exit::False;
    const uint16_t tree = flags & 0x04 ? e->guard : e->action;
    if (!tree || !c.vm.resolveTree(*target, tree).second)
        return Exit::False;
    // With 0x02 all of the current tree's parameters go along, not only the
    // first four ("Function - Message Handler", 8 arguments, hands its
    // parameter 7 to the "reaction Handler" this way); without it the
    // binary passes no parameters, and the tree sim's default ones are the
    // object's temps (cTSTreeSim::Gosub, cTSObject::OnInit).
    std::array<int16_t, 4> args{};
    std::vector<int16_t> params;
    if (flags & 0x02)
        params = c.frame.params;
    else
        params.assign(c.thread.temps.begin(), c.thread.temps.end());
    std::copy_n(params.begin(), std::min<size_t>(params.size(), 4), args.begin());
    const ObjectId id = target->id;
    c.vm.call(c.thread, id, tree, args);
    Frame& callee = c.thread.stack.back();
    callee.stackObject = id;
    for (size_t i = 4; i < params.size() && i < callee.params.size(); ++i)
        callee.params[i] = params[i];
    return Exit::Call;
}

// Find Best Object for Function (0x0E), as the Mac binary's
// cTSObject::TryFindFunctionalObject (docs/primitives-mac.md §4): function
// u16 at 0 (STR# 0xC9) -> the object data with the object's value for it
// and its OBJf entry: prep 31, cook 32, flat surface 33, dispose 36, wash
// dish 37, eating surface 38, sit 26 ("Sitting Value"), serving surface 61,
// clean 39 ("dirty level"), garden 64, wash hands 65, repair 15 ("Repair
// Level"); dry clothes, awareness and lookatTarget have no value (50); the
// other functions are false at once. Flags u8 at 2: 0x01 only objects
// outside, 0x02 only inside, 0x04 only in the reference object's room, 0x20
// only outside it, 0x40 only in my room, 0x08 the reference object is
// (owner u8 at 3, data u16 at 4) instead of me (missing: error 0x17), 0x10
// only in its line of sight, 0x80 also objects locked out (object data 25
// "lockout count"). For every other object with that OBJf entry: my object
// data 66 "function score" := its value (0: skipped); the entry's check
// tree, if any, runs and may change it (false or 0: skipped); score = that
// x 1 / (1 + distance / 5) ("functional half value distance"; distance in
// tiles plus 10 per level, from the reference object). The best becomes the
// stack object (true); none: false. Inferred: the Sim's per-object biases
// (`local_124` 0x134/0x138 in the binary) are 1; "outside" = the room's
// "outside" value (or no room); objects in use are not told apart.
Exit findBestObjectForFunction(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t function = le16(&o[0]);
    const uint8_t flags = o[2];
    constexpr int16_t kNoValue = -1, kNotDone = -2;
    constexpr int16_t kValue[] = {31, 32, 33, 36, kNotDone, kNotDone, 37, 38, 26, kNotDone, 61, 39, 64, 65,
                                  15, kNoValue, kNotDone, kNotDone, kNotDone, kNotDone, kNotDone, kNotDone,
                                  kNotDone, kNoValue, kNotDone, kNoValue, kNotDone, kNotDone};
    if (function >= std::size(kValue) || kValue[function] == kNotDone)
        return Exit::False;
    const int16_t valueField = kValue[function];
    const bool awareness = function == 23 || function == 25;
    constexpr int16_t kLockoutCount = 25, kFunctionScore = 66;
    constexpr float kHalfValueDistance = 5.0f, kFloorToFloor = 10.0f;
    VM& vm = c.vm;
    const ObjectId meId = c.thread.me;
    ObjectId refId = meId;
    if (flags & 0x08) {
        refId = c.get(o[3], int16_t(le16(&o[4])));
        if (!vm.object(refId))
            throw gameError(err::NoObject, "Find Best Object for Function: reference object " + std::to_string(refId));
    }
    auto roomOf = [&](const Object& x) { return x.data[objdata::Room]; };
    auto outside = [&](const Object& x) {
        const int16_t room = roomOf(x);
        auto it = vm.rooms.find(room);
        return room <= 0 || (it != vm.rooms.end() && (it->second.known & 0x02) && it->second.values[1] != 0);
    };
    std::vector<ObjectId> ids;
    for (const auto& [id, x] : vm.objects())
        ids.push_back(id);
    ObjectId best = 0;
    float bestScore = -std::numeric_limits<float>::max();
    for (ObjectId id : ids) {
        const Object* x = vm.object(id);
        if (!x || id == meId || x->person || !x->position.inWorld())
            continue;
        if (!(flags & 0x80) && x->data[kLockoutCount] > 0)
            continue;
        const ObjectFunctions::Entry* e = functionEntry(vm, *x, function);
        if (!e || (!e->action && !e->guard))
            continue;
        Object& me = c.me();
        me.data[kFunctionScore] = valueField == kNoValue ? int16_t(50) : x->data[size_t(valueField)];
        if (me.data[kFunctionScore] == 0)
            continue;
        if (e->guard) {
            std::array<int16_t, 4> temps{};
            std::copy_n(c.thread.temps.begin(), std::min<size_t>(4, c.thread.temps.size()), temps.begin());
            if (!runCheck(c, id, e->guard, temps) || !vm.object(meId) || c.me().data[kFunctionScore] == 0)
                continue;
            x = vm.object(id);
            if (!x)
                continue;
        }
        const Object* ref = vm.object(refId);
        if (!ref)
            ref = &c.me();
        if ((flags & 0x01) && !outside(*x))
            continue;
        if ((flags & 0x02) && outside(*x))
            continue;
        if ((flags & 0x04) && roomOf(*x) != roomOf(*ref))
            continue;
        if ((flags & 0x20) && roomOf(*x) == roomOf(*ref))
            continue;
        if ((flags & 0x40) && roomOf(*x) != roomOf(c.me()))
            continue;
        const float dx = x->position.x - ref->position.x, dy = x->position.y - ref->position.y;
        const float distance = std::sqrt(dx * dx + dy * dy) + kFloorToFloor * float(std::abs(x->position.level - ref->position.level));
        const float score = float(c.me().data[kFunctionScore]) / (distance / kHalfValueDistance + 1.0f);
        if (score <= bestScore)
            continue;
        if ((flags & 0x10) && vm.world && !vm.world->lineOfSight(VM::tileOf(*ref), VM::tileOf(*x)))
            continue;
        best = id, bestScore = score;
    }
    if (std::getenv("OS2_FBOF_FAILS") && !best) // what Find Best Object for Function found nothing for, for looking into
        std::fprintf(stderr, "t%llu FBOF %u flags %02X of %d in \"%s\" #%u found nothing\n", (unsigned long long)vm.ticks, function, flags, meId,
                     c.frame.behavior->fileName.c_str(), c.frame.pc);
    if (!best)
        return Exit::False;
    c.frame.stackObject = best;
    return Exit::True;
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
    return int(std::lround(double(*t) / Animation::kTicksPerSecond * vm.clock.ticksPerSecond));
}

int ticksFor(VM& vm, float seconds) {
    return int(std::ceil(double(seconds) * vm.clock.ticksPerSecond - 1e-6));
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
// 1's Reach, 0x2F, is "~(old)Reach"), checked against the Mac binary
// (nTSSG::cAnimatable::try_new_reach, NewReachParam; docs/primitives-mac.md
// §4). Operands: u16 at 1 and u16 at 7 (not read), the object (owner u8 at
// 3, data u16 at 4), the containment slot u8 at 6 (mode 1), flags u8 at 9
// (0x01 the slot is in temp 0, 0x02 the hand is in temp 3, 0x04 the object
// animation is filtered by the Sim's age), mode u8 at 10 (0 pick up the
// object, 1 put what the hand holds into the object's slot, 2 put it on the
// floor), the grasp animation u16 at 11 (an entry of the adult list STR#
// 0x81, 0xFFFF none) and the object animation u16 at 13 (STR# 0x86, 0xFFFF
// none; not played here). The doc reads both as index + 1, but the game's
// lists fit 0-based indices ("CT - Reach - Grab" of the toasting glass gives
// 0 = "a2o-toastingGlass-grasp, a2o-toastingGlass-carry" and object
// animation 1 = "o2a-toastingGlass-pose"), so they are taken as is. Only
// Sims reach (error 0x69). Picking up an object that does not exist is
// false; one already in that hand is true at once. Putting: an out-of-range
// slot is error 0x1B; a target that does not exist is error 0x6A
// (inferred); an occupied slot or empty hands are false (inferred). The
// animation errors are the game's (0x71 missing, 0x6E without event 0).
//
// The Sim plays the generic reach animation for the height where the object
// is or goes ("a2o-floor-get", "a2o-counter-put", "a2o-tableCoffee-get",
// ...: surfaceFor) and the object changes hands at the animation's event 0;
// the instruction returns true when the animation ends. Picking up puts the
// object in the hand (temp 3 with 0x02, else the first free one; false with
// both full) and adds the grasp and carry animations as overlays on the Sim;
// putting takes them off and puts what the hand holds into the slot, or on
// the floor tile in front of the Sim (else its own tile), facing as the Sim.
// Not done: the binary's "reach object ID" / "reach surface object ID" of
// the Sim (cTSPerson::SetReachObjectID; which person data the trees read
// them through is not known), the object animation.
Exit reachPut(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    if (!me.person)
        throw gameError(err::ReachOnNonSim);
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
                        anim.addOverlay(name).oeType = oetype::Carry;
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
            return Exit::False; // the binary: no such object is false
        if (x->data[objdata::ContainerId] == me.id &&
            (!(flags & 0x02) || x->data[objdata::SlotNumber] == c.thread.temps[3]))
            return Exit::True; // already in (that) hand: true without animating
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
        if (const uint16_t grasp = le16(&o[11]); grasp != 0xFFFF)
            if (const std::string entry = adultListEntry(c, grasp); !entry.empty())
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
                throw gameError(err::NoReachObject);
            r.target = to->id;
            const size_t slots = containmentSlotCount(c.vm, *to);
            r.slot = flags & 0x01 ? c.thread.temps[0] : o[6];
            if (r.slot < 0 || size_t(r.slot) >= slots)
                throw gameError(err::SlotOutOfRange, "slot " + std::to_string(r.slot) + " of " + std::to_string(slots));
            if (const ObjectId there = c.vm.containedIn(to->id, int16_t(r.slot)); there && there != x)
                return Exit::False;
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
        throw gameError(err::MissingReachAnimation, r.animation);
    const auto transfer = transferTick(c.vm, r.animation);
    if (!transfer)
        throw gameError(err::NoReachEvent0, r.animation);
    r.end = std::max(1, ticksFor(c.vm, *seconds));
    r.transferAt = std::clamp(*transfer, 0, r.end);
    anim.name = r.animation;
    ++anim.serial;
    anim.ticksLeft = r.end;
    anim.depth = depth, anim.tree = c.frame.behavior, anim.node = c.frame.pc;
    me.reach = std::move(r);
    return Exit::Yield;
}

// --- Snap ----------------------------------------------------------------------

// Snap (0x2E): moves me at once (no walking) to a routing slot of the stack
// object (TrySnap in the Mac binary; docs/primitives-mac.md §4/§5): slot u16
// at 0, mode u16 at 2 (STR# 0xCF: 0 the routing slot whose number is in the
// parameter of u16 0, 1 be contained: into the stack object's containment
// slot 0, 2 in front of it, 3 the literal routing slot, 4 the global SLOT),
// flags u16 at 4: 0x02 ask a person in the way to move (nobody is asked
// here: the snap just fails), 0x08 the slot number is in temp 0, 0x10 test
// only; u16 8 = 1 "from slot in temp 1" (not done). No stack object: error
// 0x15; a slot that does not exist: error 0x1B. A failure is false and
// writes my object data 52 "primitive result": 1 when a Sim in the way was
// asked to move (0x02), else the placement error + 2 (TrySnap: the first
// error of the placement test's list): 0x0C "Can't intersect other
// objects" + 2 = 0x0E for a goal taken, 2 "Location out of bounds" + 2 = 4
// when every goal is off the lot (the carpool's "Drive - Drive Leave" waits
// for 4 to know the car has reached the end of the street). The place is the slot's goal
// with the lowest cost whose tile is free (the Sim's own and, for a slot on
// the object, the object's tile do not count), with the slot's facing. The
// Sims 2's SLOTs have no snap target slot (-1 everywhere): sitting is a Snap
// followed by a Snap Into.
Exit snap(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t index = le16(&o[0]), where = le16(&o[2]);
    const uint16_t flags = le16(&o[4]);
    Object& me = c.me();
    if (!c.vm.object(c.frame.stackObject))
        throw gameError(err::NoStackObject, "Snap");
    Object& target = c.stackObject();
    constexpr int16_t kPrimitiveResult = 52;
    auto fail = [&](int placementError = 0x0C) {
        if (std::getenv("OS2_SNAP_FAILS")) // why snaps fail, for looking into
            std::fprintf(stderr, "t%llu snap of %d to %d at (%.2f, %.2f, %d) (mode %u slot %u flags %X) failed: %d\n", (unsigned long long)c.vm.ticks,
                         me.id, c.frame.stackObject, target.position.x, target.position.y, target.position.level, where, index, flags, placementError);
        me.data[kPrimitiveResult] = int16_t(placementError == 0x0C && (flags & 0x02) ? 1 : placementError + 2);
        return Exit::False;
    };
    if (where > 4)
        throw VMError("Snap: slot scope " + std::to_string(where) + " not implemented");
    if (where == 1) {
        if (c.vm.containedIn(target.id, 0) && c.vm.containedIn(target.id, 0) != me.id)
            return fail();
        if (!(flags & 0x10))
            putInSlot(me, target, 0);
        return Exit::True;
    }
    Slots::Slot slot;
    if (where == 2) {
        slot.type = 3;
        slot.flags = 0x01; // in front, relative to the object
        slot.minDistance = slot.optimalDistance = slot.maxDistance = 1;
        slot.facing = -2;
    } else {
        int n = flags & 0x08 ? c.thread.temps[0] : where == 0 ? c.get(0x09, int16_t(index)) : int(index);
        const Slots* table = where == 4 ? c.vm.code().slots(kGlobalGroup, 0x64) : slotTable(c.vm, target);
        const Slots::Slot* found = nullptr;
        if (table)
            for (const auto& s : table->slots)
                if (s.type == 3 && n-- == 0) {
                    found = &s;
                    break;
                }
        if (!found)
            throw gameError(err::SlotOutOfRange, "routing slot " + std::to_string(index));
        slot = *found;
    }
    if (!target.position.inWorld())
        return fail();
    const VM::RouteGoals goals = c.vm.routingGoals(slot, target);
    // The target's own footprint does not count: a snap to its slot puts me
    // against it or on it (standing up from a chair to the place beside it,
    // which here falls on the chair's tile or across a wall for want of the
    // model's footprint; inferred).
    std::vector<ObjectId> ignore{me.id, target.id};
    size_t best = goals.goals.size();
    for (size_t i = 0; i < goals.goals.size(); ++i) {
        const TilePos t{goals.goals[i].x, goals.goals[i].y, goals.level};
        const bool free = !c.vm.world || c.vm.tileFree(t, ignore);
        if (free && (best == goals.goals.size() || goals.goals[i].cost < goals.goals[best].cost))
            best = i;
    }
    if (best == goals.goals.size() && std::getenv("OS2_SNAP_FAILS"))
        for (size_t i = 0; i < goals.goals.size(); ++i)
            for (const auto& [id, x] : c.vm.objects())
                if (id != me.id && id != target.id && VM::blocksRouting(x) &&
                    VM::tileOf(x) == TilePos{goals.goals[i].x, goals.goals[i].y, goals.level})
                    std::fprintf(stderr, "  goal (%d, %d, %d) taken by %d %s\n", goals.goals[i].x, goals.goals[i].y, goals.level, id,
                                 x.definition ? x.definition->fileName.c_str() : "?");
    if (best == goals.goals.size()) {
        bool onLot = goals.goals.empty() || !c.vm.world;
        for (const auto& g : goals.goals)
            onLot = onLot || c.vm.world->contains(TilePos{g.x, g.y, goals.level});
        return fail(onLot ? 0x0C : 2);
    }
    if (flags & 0x10)
        return Exit::True;
    me.data[objdata::ContainerId] = 0;
    me.data[objdata::SlotNumber] = -1;
    if (!(me.route && me.route->inPortal)) // a portal tree moves the walker
        me.route.reset();
    const Position from = me.position;
    const int fromDir = me.data[objdata::Direction];
    me.position = {goals.points[best][0], goals.points[best][1], goals.level};
    if (goals.facings[best] >= 0)
        me.data[objdata::Direction] = int16_t(goals.facings[best]);
    if (std::getenv("OS2_DEBUG_SNAP"))
        std::fprintf(stderr, "t%llu snap of %d to %d (mode %u slot %u flags %X): goal %zu of %zu at (%.2f, %.2f) dir %d -> %d\n",
                     (unsigned long long)c.vm.ticks, me.id, target.id, where, index, flags, best, goals.goals.size(),
                     me.position.x, me.position.y, fromDir, me.data[objdata::Direction]);
    // A multi-tile object moves whole: its other parts keep their places
    // around me, turned with me (the carpool's car snapping down the street
    // to its "slot_travel_drive" bone; inferred, as placement moves groups).
    if (const ObjectId group = multiTileGroup(me); group && !me.person && from.inWorld()) {
        const int turn = ((me.data[objdata::Direction] - fromDir) % 8 + 8) % 8;
        const double a = -turn * M_PI / 4; // directions go clockwise
        std::vector<ObjectId> parts;
        for (const auto& [id, x] : c.vm.objects())
            if (id != me.id && multiTileGroup(x) == group && x.position.inWorld() && !x.person)
                parts.push_back(id);
        for (ObjectId id : parts) {
            Object& part = *c.vm.object(id);
            const float dx = part.position.x - from.x, dy = part.position.y - from.y;
            part.position.x = me.position.x + float(dx * std::cos(a) - dy * std::sin(a));
            part.position.y = me.position.y + float(dx * std::sin(a) + dy * std::cos(a));
            part.position.level = me.position.level;
            part.data[objdata::Direction] = int16_t(((part.data[objdata::Direction] + turn) % 8 + 8) % 8);
            if (c.vm.world)
                part.data[objdata::Room] = c.vm.world->room(VM::tileOf(part));
        }
    }
    if (c.vm.world)
        me.data[objdata::Room] = c.vm.world->room(VM::tileOf(me));
    return Exit::True;
}

} // namespace

void registerReachPrimitives(VM& vm) {
    vm.setPrimitive(0x2E, snap);
    vm.setPrimitive(0x0E, findBestObjectForFunction);
    vm.setPrimitive(0x14, runFunctionalTree);
    vm.setPrimitive(0x74, reachPut);
}

} // namespace os2::vm
