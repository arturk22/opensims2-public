// On Timer (0x7A): event trees that run on an object after a number of ticks.
//
// The Mac binary (TryOnTimer, OnTimerParam; docs/primitives-mac.md §4) and
// the strings its editor's disassembler left in Sims2.exe
// (cEdithLanguage::CatOnTimerParam: "( Start Timer", "( Modify Timer", "(
// Delete Timer", ", Getting ticks till fire from temp 1", ", Ticks till fire
// %d", ", Looping", ", using current params as event tree params", ",
// passing in params found in ", ", Reset Timer Ticks") give the operand:
//   bytes 0-1  ticks till fire (u16), or temp 1 with flag 0x08
//   byte  2    passed to the timer manager (not known; 0, 1, 2 in the game)
//   bytes 3-4  the event tree (a call opcode of the object's code)
//   byte  5    flags: 0x01 the event tree gets the current tree's
//              parameters, 0x02 looping (inferred), 0x04 "reset timer ticks"
//              when modifying (inferred), 0x08 ticks in temp 1
//   bytes 6-14 three (owner u8, data u16) parameters for the event tree
//   byte  15   mode: 0 start a timer (its ID goes to temp 0), 1 modify the
//              timer whose ID is in temp 0, 2 delete it
// The result is always true. Every use in the game starts a timer (byte 15
// is 0 in all 16). The binary keeps the timers in the lot's timer manager
// (no manager: error 0x17); here they belong to the object whose code is
// running (inferred: a "CT - Delete Self" a Sim runs on a burglar's sack
// removes the sack, and "Timer Event - Hide Professor" tests that "me" is
// the professor), saved and restored with it (cTSObject::WriteTimers).
// Timer IDs count up from the largest one in use.
//
// Inferred: the event tree runs to completion on the tick it is due, before
// the Main threads, as a check tree, with no stack object; 0 ticks fire on
// the next tick.

#include "vm.hpp"

#include "errors.hpp"

#include "formats/binreader.hpp"

#include <algorithm>

namespace os2::vm {

namespace {

constexpr uint8_t kFlagCurrentParams = 0x01;
constexpr uint8_t kFlagLooping = 0x02;
constexpr uint8_t kFlagResetTicks = 0x04;
constexpr uint8_t kFlagTicksInTemp1 = 0x08;
constexpr size_t kMaxFailedTimerEvents = 64;

int16_t nextTimerId(VM& vm) {
    int16_t top = 0;
    for (const auto& [id, o] : vm.objects())
        for (const ObjectTimer& t : o.timers)
            top = std::max(top, t.id);
    return int16_t(top + 1);
}

ObjectTimer* timerById(VM& vm, int16_t id, Object** ownerOut) {
    for (const auto& [oid, o] : vm.objects()) {
        Object* x = vm.object(oid);
        for (ObjectTimer& t : x->timers)
            if (t.id == id) {
                *ownerOut = x;
                return &t;
            }
    }
    return nullptr;
}

Exit onTimer(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[5], mode = o[15];
    const int32_t ticks = (flags & kFlagTicksInTemp1) ? c.thread.temps[1] : int16_t(le16(&o[0]));
    if (mode == 1 || mode == 2) {
        Object* owner = nullptr;
        ObjectTimer* t = timerById(c.vm, c.thread.temps[0], &owner);
        if (!t)
            return Exit::True;
        if (mode == 2) {
            owner->timers.erase(owner->timers.begin() + (t - owner->timers.data()));
            return Exit::True;
        }
        t->tree = le16(&o[3]);
        t->period = ticks;
        t->looping = flags & kFlagLooping;
        if (flags & kFlagResetTicks)
            t->ticks = std::max(ticks, 1);
        return Exit::True;
    }
    if (mode != 0)
        throw VMError("On Timer: mode " + std::to_string(mode) + " not implemented");
    Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        throw gameError(err::NoObject, "On Timer: code owner " + std::to_string(c.frame.codeOwner));

    ObjectTimer t;
    t.id = nextTimerId(c.vm);
    t.tree = le16(&o[3]);
    t.period = ticks;
    t.ticks = std::max(t.period, 1);
    t.looping = flags & kFlagLooping;
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
        // Count down first, then run the due ones: an event tree may set or
        // change timers of its own object (the motive increment re-arms itself).
        std::vector<ObjectTimer> due;
        for (auto it = o->timers.begin(); it != o->timers.end();) {
            if (--it->ticks > 0) {
                ++it;
                continue;
            }
            due.push_back(*it);
            if (it->looping) {
                it->ticks = std::max(it->period, 1);
                ++it;
            } else {
                it = o->timers.erase(it);
            }
        }
        for (const ObjectTimer& t : due) {
            if (!object(id))
                break;
            Thread th = start(id, t.tree, std::span<const int16_t>(t.params.data(), 4), 0, id);
            for (size_t turn = 0; th.status == Thread::Status::Running && turn < 1000; ++turn)
                run(th, instructionsPerTurn);
            if (th.status == Thread::Status::Failed) {
                if (failedTimerEvents.size() >= kMaxFailedTimerEvents)
                    failedTimerEvents.pop_front();
                failedTimerEvents.push_back(std::move(th));
            }
        }
    }
}

void registerTimerPrimitives(VM& vm) { vm.setPrimitive(0x7A, onTimer); }

} // namespace os2::vm
