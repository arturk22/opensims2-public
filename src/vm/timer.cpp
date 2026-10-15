// On Timer (0x7A): event trees that run on an object after a number of ticks.
//
// The Mac binary (TryOnTimer, OnTimerParam; docs/primitives-mac.md §4) and
// the strings its editor's disassembler left in Sims2.exe
// (cEdithLanguage::CatOnTimerParam: "( Start Timer", "( Modify Timer", "(
// Delete Timer", ", Getting ticks till fire from temp 1", ", Ticks till fire
// %d", ", Looping", ", using current params as event tree params", ",
// passing in params found in ", ", Reset Timer Ticks") give the operand:
//   bytes 0-1  ticks till fire (u16), or temp 1 with flag 0x08
//   byte  2    not read by TryOnTimer (0, 1, 2 in the game)
//   bytes 3-4  the event tree (a call opcode of the object's code)
//   byte  5    flags: 0x01 the event tree gets the current tree's
//              parameters, 0x02 repeat, 0x04 "reset timer ticks" when
//              modifying, 0x08 ticks in temp 1
//   bytes 6-14 three (owner u8, data u16) parameters for the event tree
//   byte  15   mode: 0 start a timer (its ID goes to temp 0), 1 modify the
//              timer whose ID is in temp 0, 2 delete it
// The result is always true. Every use in the game starts a timer (byte 15
// is 0 in all 16). The binary (TryOnTimer BV 0xe26308) keeps the timers on
// the object whose code the frame runs (the frame's object, the code owner;
// no such object: error 0x17), saved and restored with it
// (cTSObject::WriteTimers, docs/save-mac.md §8):
// - start (cTSObject::AddTimer, vtable 0x4A4): ticks left = period = the
//   ticks, flags = repeat (operand 0x02), the object that runs the
//   primitive (vtable 0xB8) as the event tree's stack object, the ID from
//   the simulator's GUID counter (cTSSimulator vtable 0xAC, BV 0x4989e0:
//   global 36, as AssignUniqueGUID gives interactions; in the game's lots
//   the timers' IDs sit just below the saved global 36), back in temp 0;
// - modify (vtable 0x4A8, BV 0xb02a94): the timer of the ID in temp 0 among
//   the code owner's gets the tree, the period and flags = repeat | 2 with
//   operand 0x04, which also sets the ticks left to the period;
// - delete (vtable 0x4AC, BV 0xb04474): the first of the code owner's
//   timers with that ID goes.
// Each tick (BV 0xb02b8c), per object in list order (the count taken
// before the loop): ticks left - 1; at 0 or below the event tree runs at
// once (cTSObject::RunTree, the object's code, the setter as stack object,
// the parameters); a repeating timer whose tree returned true starts over
// with the period, any other is deleted by its ID.
//
// Inferred: the event tree runs to completion on the tick it is due, before
// the Main threads, as a check tree (RunTree's own way of running is not
// read).

#include "vm.hpp"

#include "errors.hpp"
#include "interactions.hpp"

#include "formats/binreader.hpp"

#include <algorithm>

namespace os2::vm {

namespace {

constexpr uint8_t kFlagCurrentParams = 0x01;
constexpr uint8_t kFlagLooping = 0x02;
constexpr uint8_t kFlagResetTicks = 0x04;
constexpr uint8_t kFlagTicksInTemp1 = 0x08;
constexpr size_t kMaxFailedTimerEvents = 64;

ObjectTimer* timerById(Object& owner, int16_t id) {
    for (ObjectTimer& t : owner.timers)
        if (t.id == id)
            return &t;
    return nullptr;
}

Exit onTimer(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[5], mode = o[15];
    const int32_t ticks = (flags & kFlagTicksInTemp1) ? c.thread.temps[1] : int32_t(le16(&o[0]));
    Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        throw gameError(err::NoObject, "On Timer: code owner " + std::to_string(c.frame.codeOwner));
    if (mode == 1 || mode == 2) {
        ObjectTimer* t = timerById(*owner, c.thread.temps[0]);
        if (!t)
            return Exit::True;
        if (mode == 2) {
            owner->timers.erase(owner->timers.begin() + (t - owner->timers.data()));
            return Exit::True;
        }
        t->tree = le16(&o[3]);
        t->period = ticks;
        t->flags = ((flags & kFlagLooping) ? 1u : 0u) | ((flags & kFlagResetTicks) ? 2u : 0u);
        if (flags & kFlagResetTicks)
            t->ticks = ticks;
        return Exit::True;
    }
    if (mode != 0)
        throw VMError("On Timer: mode " + std::to_string(mode) + " not implemented");

    ObjectTimer t;
    t.id = int16_t(newInteractionId(c.vm, *owner));
    t.tree = le16(&o[3]);
    t.period = ticks;
    t.ticks = ticks;
    t.flags = (flags & kFlagLooping) ? 1u : 0u;
    t.object = c.me().id;
    if (flags & kFlagCurrentParams) {
        for (size_t i = 0; i < c.frame.params.size() && i < t.params.size(); ++i)
            t.params[i] = c.frame.params[i];
    } else {
        for (int i = 0; i < 3; ++i)
            t.params[size_t(i)] = c.get(o[size_t(6 + 3 * i)], int16_t(le16(&o[size_t(7 + 3 * i)])));
    }
    auto& timers = owner->timers;
    if (timers.size() >= kMaxTimersPerObject)
        throw VMError("On Timer: too many timers on object " + std::to_string(owner->id));
    timers.push_back(t);
    c.thread.temps[0] = t.id;
    return Exit::True;
}

} // namespace

void VM::handleTimers() {
    std::vector<ObjectId> ids;
    for (const Object* o : byId_) // by ID, as objects_ is
        if (o && !o->timers.empty())
            ids.push_back(o->id);
    for (ObjectId id : ids) {
        Object* o = object(id);
        if (!o)
            continue;
        size_t count = o->timers.size();
        for (size_t i = 0; i < count; ++i) {
            if (--o->timers[i].ticks > 0)
                continue;
            const ObjectTimer t = o->timers[i];
            // The event tree may set, change or delete timers of its own
            // object, or delete the object.
            const ObjectId stack = t.object && object(t.object) ? t.object : ObjectId(0);
            Thread th = start(id, t.tree, std::span<const int16_t>(t.params.data(), 4), stack, id);
            for (size_t turn = 0; th.status == Thread::Status::Running && turn < 1000; ++turn)
                run(th, instructionsPerTurn);
            const bool returnedTrue = th.status == Thread::Status::Finished && th.result;
            if (th.status == Thread::Status::Failed) {
                if (failedTimerEvents.size() >= kMaxFailedTimerEvents)
                    failedTimerEvents.pop_front();
                failedTimerEvents.push_back(std::move(th));
            }
            o = object(id);
            if (!o)
                break;
            if (i < o->timers.size() && t.repeats() && returnedTrue) {
                o->timers[i].ticks = o->timers[i].period;
                continue;
            }
            // DeleteTimer(ID): the first of the object's timers with it.
            for (size_t k = 0; k < o->timers.size(); ++k)
                if (o->timers[k].id == t.id) {
                    o->timers.erase(o->timers.begin() + std::ptrdiff_t(k));
                    if (k <= i) {
                        --i;
                        --count;
                    }
                    break;
                }
            count = std::min(count, o->timers.size());
        }
    }
}

void registerTimerPrimitives(VM& vm) { vm.setPrimitive(0x7A, onTimer); }

} // namespace os2::vm
