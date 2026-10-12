// Message (0x77), as the Mac binary's cTSObject::TryMessage, PassFilter,
// RunMessageTree and RunOneTickTree do it (Ghidra headless, BV 1.10).
//
// Operands (MessageParam: u16 at 1, 6, 10, 13): message ID (owner byte 15,
// data u16 1), priority u8 at 8, a target by variable (owner byte 5, data
// u16 6) when byte 4 has 0x02, else the set of receivers u8 at 3 and the
// filter u8 at 0; with byte 4 & 0x01 the filter's room is that variable
// instead of mine. Two data values: (owner byte 9, data u16 10) and (owner
// byte 12, data u16 13).
//
// The message is six values: ID, the frame's stack object (stack element
// field +10), priority, data 1, data 2 and my temp 0. Each receiver runs its
// OBJf function 36 (the "message handler": "Function - Message",
// "Function - Message Handler", ...) at once, as a one-tick tree
// (RunOneTickTree, on my thread): me = the sender, the receiver's code with
// the receiver as the stack object, the six values as its parameters; a handler that would wait is the game's error
// 0x451 "Check tree primitive blocked completion.". Receivers without a
// handler count as reached. True if anything was reached; the explicit
// target that does not exist is error 0x17.
//
// Sets (editor strings): 0 selectable Sims, 1 selectable Sims + neighbors,
// 2 selectable Sims + NPCs, 3 neighbors only, 4 NPCs only, 5 all Sims, 6
// objects that are not Sims, 7 everything (the binary walks the person
// list for 0-5 and every object for 6/7, skipping Sims for 6). The binary
// tells the kinds apart by person data 32 "Person Type" (0, 1, 2 here taken
// as selectable, neighbor, NPC; inferred from the set names: the
// decompiled branches for 1-4 do not quite match them, the names are
// followed). Filters (PassFilter): 0 same room, 1 same level, 2 the lot, 3
// inside, 4 outside ("outside" = room value 1 of the receiver's room).

#include "engine.hpp"
#include "errors.hpp"
#include "formats/binreader.hpp"
#include "vm.hpp"

#include <array>
#include <string>
#include <vector>

namespace os2::vm {

namespace {

constexpr size_t kMessageHandlerFunction = 36;
constexpr int16_t kPersonType = 32;
constexpr int kMaxMessageDepth = 8; // handlers that send messages (inferred bound)
int messageDepth = 0;

bool roomOutside(VM& vm, int16_t room) {
    if (room <= 0)
        return true; // no room: outside (inferred)
    auto it = vm.rooms.find(room);
    return it != vm.rooms.end() && it->second.values[1] != 0;
}

bool passFilter(VM& vm, const Object& o, uint8_t filter, int16_t room, int level) {
    switch (filter) {
    case 0: return o.data[objdata::Room] == room;
    case 1: return o.position.level == level;
    case 3: return !roomOutside(vm, o.data[objdata::Room]);
    case 4: return roomOutside(vm, o.data[objdata::Room]);
    default: return true;
    }
}

// Runs the receiver's message handler at once: the receiver's code, with it
// as the stack object, in my thread (RunOneTickTree on my tree sim, so the
// temps are mine; "Function - Message" wakes the stack object with Notify
// the Stack Object out of Idle).
void deliver(Context& c, ObjectId target, const std::array<int16_t, 6>& msg) {
    VM& vm = c.vm;
    const Object* o = vm.object(target);
    if (!o)
        return;
    const uint16_t tree = vm.functionTree(*o, kMessageHandlerFunction);
    if (!tree)
        return;
    runOneTickTree(vm, c.thread.temps, c.thread.me, target, target, tree, msg);
}

enum class Kind { Selectable, Neighbor, Npc };

Kind kindOf(const Object& sim) {
    switch (sim.person->data[kPersonType]) {
    case 0: return Kind::Selectable;
    case 2: return Kind::Npc;
    default: return Kind::Neighbor;
    }
}

bool inSet(uint8_t set, Kind k) {
    switch (set) {
    case 0: return k == Kind::Selectable;
    case 1: return k != Kind::Npc;
    case 2: return k != Kind::Neighbor;
    case 3: return k == Kind::Neighbor;
    case 4: return k == Kind::Npc;
    default: return true; // 5 all Sims
    }
}

Exit message(Context& c) {
    const auto& o = c.instruction.operands;
    std::array<int16_t, 6> msg{};
    msg[0] = c.get(o[15], int16_t(le16(&o[1])));
    msg[1] = c.frame.stackObject;
    msg[2] = o[8];
    msg[3] = c.get(o[9], int16_t(le16(&o[10])));
    msg[4] = c.get(o[12], int16_t(le16(&o[13])));
    msg[5] = c.thread.temps[0];
    VM& vm = c.vm;
    if (o[4] & 0x02) {
        const ObjectId target = c.get(o[5], int16_t(le16(&o[6])));
        if (!vm.object(target))
            throw gameError(err::NoObject, "message target " + std::to_string(target));
        deliver(c, target, msg);
        return Exit::True;
    }
    const Object& me = c.me();
    const int level = me.position.level;
    int16_t room = me.data[objdata::Room];
    if (o[4] & 0x01)
        room = c.get(o[5], int16_t(le16(&o[6])));
    const uint8_t set = o[3], filter = o[0];
    std::vector<ObjectId> receivers;
    for (const auto& [id, x] : vm.eachObject()) {
        if (set >= 6) {
            if (set == 6 && x.person)
                continue;
        } else if (!x.person || !inSet(set, kindOf(x))) {
            continue;
        }
        if (passFilter(vm, x, filter, room, level))
            receivers.push_back(id);
    }
    for (ObjectId id : receivers)
        deliver(c, id, msg);
    return receivers.empty() ? Exit::False : Exit::True;
}

} // namespace

bool runOneTickTree(VM& vm, std::array<int16_t, kTempCount>& temps, ObjectId me, ObjectId codeOwner,
                    ObjectId stackObject, uint16_t tree, std::span<const int16_t> params,
                    std::vector<ActionString>* actionStrings) {
    if (messageDepth >= kMaxMessageDepth)
        throw gameError(0x452 /* Max stack depth exceeded. */, "one-tick trees");
    Thread t = vm.start(me, tree, params.first(std::min<size_t>(params.size(), 4)), stackObject, codeOwner);
    t.temps = temps;
    t.actionStrings = actionStrings;
    if (t.status == Thread::Status::Running) {
        auto& p = t.stack.back().params;
        for (size_t i = 4; i < params.size() && i < p.size(); ++i)
            p[i] = params[i];
        ++messageDepth;
        try {
            vm.run(t, vm.instructionsPerTurn);
        } catch (...) {
            --messageDepth;
            throw;
        }
        --messageDepth;
    }
    temps = t.temps;
    if (t.status == Thread::Status::Failed)
        throw VMError(t.error.substr(0, t.error.find('\n')) + " (in one-tick tree " + hexString(tree) + " of object " +
                      std::to_string(codeOwner) + ")");
    if (t.status == Thread::Status::Running)
        throw VMError("Check tree primitive blocked completion. (one-tick tree " + hexString(tree) + " of object " +
                      std::to_string(codeOwner) + ")");
    return t.result;
}

void registerMessagePrimitives(VM& vm) {
    vm.setPrimitive(0x77, message);
}

} // namespace os2::vm
