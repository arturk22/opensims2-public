// On Timer (0x7A): event trees that run on an object after a number of ticks.
//
// Operand, from the 16 uses in the game's trees and the strings the editor's
// disassembler left in Sims2.exe (cEdithLanguage::CatOnTimerParam: "( Start
// Timer", "( Modify Timer", "( Delete Timer", ", Getting ticks till fire from
// temp 1", ", Ticks till fire %d", ", Looping", ", using current params as
// event tree params", ", passing in params found in ", ", Reset Timer Ticks"):
//   bytes 0-1  ticks till fire (u16)
//   byte  2    operation: 0 and 2 add a timer, 1 changes the object's timer
//              for that tree (or adds it)
//   bytes 3-4  the event tree (a call opcode of the object's code)
//   byte  5    flags: 0x01 the event tree gets the current tree's parameters,
//              0x08 the ticks are in temp 1
//   bytes 6-14 three (owner u8, data u16) parameters for the event tree
//
// What the trees show: "Memory - Balloon Sequence Timed" sets a sequence of
// timers (op 2, flag 0x08) with the delay accumulated in temp 1, two of them
// with the same tree; "Set Motive Delta" and "Event - Motive Increment" (op 1,
// flag 0x01) re-arm the motive increment with the tree's own parameters
// (motive, delta), which the event reads as parameters 0-1; "Event - Flush"
// pushes its "Stop Flush" timer back (op 1) and passes the stack object
// explicitly, its own parameter 0 being an event code; "Function - Load"
// wakes the object up a tick later through "Event - Timer - Notify". The
// Mac binary names cTSObject::AddTimer, ModifyTimer, DeleteTimer and
// HandleTimers; the timers are saved with the object (WriteTimers).
//
// Inferred: the operation numbers (all three seen ops start or restart a
// timer; "Delete" is not used by any tree, so it is not here), that op 1
// replaces the timer of the same tree, that the timer belongs to the object
// whose code is running (a "CT - Delete Self" a Sim runs on a burglar's sack
// removes the sack, and "Timer Event - Hide Professor" tests that "me" is the
// professor), that the event tree runs to completion on the tick it is due,
// before the Main threads, as a check tree, with no stack object, and that
// 0 ticks fire on the next tick.

#include "vm.hpp"

#include "formats/binreader.hpp"

#include <algorithm>

namespace os2::vm {

namespace {

constexpr uint8_t kFlagCurrentParams = 0x01;
constexpr uint8_t kFlagTicksInTemp1 = 0x08;
constexpr size_t kMaxFailedTimerEvents = 64;

Exit onTimer(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t op = o[2];
    const uint8_t flags = o[5];
    if (op > 2)
        throw VMError("On Timer: operation " + std::to_string(op) + " not implemented");
    Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        throw VMError("On Timer: code owner " + std::to_string(c.frame.codeOwner) + " does not exist");

    ObjectTimer t;
    t.tree = le16(&o[3]);
    t.period = (flags & kFlagTicksInTemp1) ? c.thread.temps[1] : int16_t(le16(&o[0]));
    t.ticks = std::max(t.period, 1);
    if (flags & kFlagCurrentParams) {
        for (size_t i = 0; i < c.frame.params.size() && i < t.params.size(); ++i)
            t.params[i] = c.frame.params[i];
    } else {
        for (int i = 0; i < 3; ++i)
            t.params[size_t(i)] = c.get(o[size_t(6 + 3 * i)], int16_t(le16(&o[size_t(7 + 3 * i)])));
    }
    auto& timers = owner->timers;
    if (op == 1) {
        auto it = std::find_if(timers.begin(), timers.end(), [&](const ObjectTimer& x) { return x.tree == t.tree; });
        if (it != timers.end()) {
            *it = t;
            return Exit::True;
        }
    }
    if (timers.size() >= kMaxTimersPerObject)
        throw VMError("On Timer: too many timers on object " + std::to_string(owner->id));
    timers.push_back(t);
    return Exit::True;
}

} // namespace

void VM::handleTimers() {
    std::vector<ObjectId> ids;
    for (const auto& [id, o] : objects_)
        if (!o.timers.empty())
            ids.push_back(id);
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
