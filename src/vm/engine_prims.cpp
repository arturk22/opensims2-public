// Primitives whose work is the engine's or the UI's, and the small ones
// next to them, from the Mac binary (BV 1.10, Ghidra headless):
//
// - 0x07 Refresh (cTSObject::TryUpdate): who u16 0 (0 me, 1 the stack
//   object: none is error 0x15), what u16 2 (< 3, else error 3 like any
//   other "who"); it only asks the renderer to refresh: true.
// - 0x73 Debug (cTSObject::TryDebug): true at once unless the game runs
//   with its debugging switch (then it prints or opens windows); the
//   shipped game does not.
// - 0x23 Camera Control (TryUserEvent), 0x22 UI Effect (TryUIEffect) and
//   0x7B Cinematic (TryCinematic): camera moves, UI highlights and
//   cinematics. Headless they do nothing and are true (the binary returns 1,
//   or 2 while a camera move with a timeout runs); the host may look at
//   `uiEvents()` to act on them, and at `cinematicRequests()` for the
//   cinematics with their scene and actors resolved (docs/cinematics-mac.md).
// - 0x25 Test Sim Interacting With (cTSPerson::TryTestInteractingWith):
//   true if the stack object is one of the objects I hold a reference to
//   (cTSPerson::HasObjectReference: a list of (?, object) entries; taken as
//   the objects of my interaction queue, inferred).

#include "engine.hpp"
#include "errors.hpp"
#include "formats/binreader.hpp"
#include "vm.hpp"
#include "formats/strings.hpp"

#include <deque>

namespace os2::vm {

namespace {

std::deque<UiEvent> events;
std::deque<CinematicRequest> cinematics;
constexpr uint16_t kCinematicsTable = 0x97;

void record(Context& c) {
    if (events.size() >= 256)
        events.pop_front();
    events.push_back({uint8_t(c.instruction.opcode), c.thread.me, c.frame.stackObject, c.instruction.operands});
}

Exit cinematic(Context& c) {
    record(c);
    const auto& o = c.instruction.operands;
    CinematicRequest r;
    r.flags = o[5];
    r.temp0 = c.thread.temps[0];
    r.me = c.thread.me;
    r.tick = c.vm.ticks;
    try {
        const int index = (r.flags & 0x10) ? c.get(o[6], int16_t(le16(&o[7]))) : int(le16(&o[0]));
        // The table the flags name, else the running tree's own (every use in
        // the game has its scenes there).
        uint32_t named = c.frame.group;
        if (r.flags & 0x20)
            named = kGlobalGroup;
        else if (r.flags & 0x40)
            named = c.vm.code().semiGlobalFor(c.frame.group);
        for (uint32_t group : {named, c.frame.group}) {
            const StringTable* t = group ? c.vm.code().strings(group, kCinematicsTable) : nullptr;
            if (t && index >= 0 && size_t(index) < t->count(lang::EnglishUS)) {
                r.scene = t->get(size_t(index), lang::EnglishUS);
                break;
            }
        }
        if (Object* holder = c.vm.object(c.get(o[9], int16_t(le16(&o[10])))))
            if (!holder->arrays.empty())
                for (int16_t v : holder->arrays[0].values)
                    r.actors.push_back(ObjectId(v));
    } catch (const std::exception&) {
        // An unreadable variable: the request goes out without what it names.
    }
    if (cinematics.size() >= 32)
        cinematics.pop_front();
    cinematics.push_back(std::move(r));
    return Exit::True;
}

Exit refresh(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t who = le16(&o[0]);
    if (who == 1 && !c.vm.object(c.frame.stackObject))
        throw gameError(err::NoStackObject, "Refresh");
    if (who > 1 || le16(&o[2]) >= 3)
        throw gameError(err::BadUpdateParameter, "Refresh");
    return Exit::True;
}

Exit debug(Context&) {
    return Exit::True;
}

Exit uiPrimitive(Context& c) {
    record(c);
    return Exit::True;
}

Exit testSimInteractingWith(Context& c) {
    const Object& me = c.me();
    if (!me.person)
        throw gameError(err::NotSupported, "Test Sim Interacting With");
    const ObjectId target = c.frame.stackObject;
    for (const auto& q : me.queue)
        if (q.object == target)
            return Exit::True;
    return Exit::False;
}

} // namespace

int16_t useCount(VM& vm, const Object& o) {
    const ObjectId lead = o.data[objdata::LeadTileObjectId] ? o.data[objdata::LeadTileObjectId] : o.id;
    auto part = [&](ObjectId id) {
        if (id == o.id)
            return true;
        const Object* x = vm.object(id);
        return x && (x->data[objdata::LeadTileObjectId] ? x->data[objdata::LeadTileObjectId] : x->id) == lead;
    };
    int16_t users = 0;
    for (const Object* simp : vm.objectTable()) {
        if (!simp || !simp->person || simp->id == o.id)
            continue;
        const Object& sim = *simp;
        const ObjectId id = sim.id;
        bool uses = false;
        for (int i = 0; i <= sim.activeInteraction && size_t(i) < sim.queue.size() && !uses; ++i)
            uses = part(sim.queue[size_t(i)].object);
        if (auto it = vm.threads.find(id); !uses && it != vm.threads.end())
            for (const Frame& f : it->second.stack)
                if ((uses = part(f.codeOwner)))
                    break;
        users = int16_t(users + uses);
    }
    return users;
}

std::deque<UiEvent>& uiEvents() {
    return events;
}

std::deque<CinematicRequest>& cinematicRequests() {
    return cinematics;
}

void registerEnginePrimitives(VM& vm) {
    vm.setPrimitive(0x07, refresh);
    vm.setPrimitive(0x73, debug);
    vm.setPrimitive(0x22, uiPrimitive);
    vm.setPrimitive(0x23, uiPrimitive);
    vm.setPrimitive(0x7B, cinematic);
    vm.setPrimitive(0x25, testSimInteractingWith);
}

} // namespace os2::vm
