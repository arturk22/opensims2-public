// Starting a lot's objects from their saved state (lot_resume.hpp).

#include "lot_resume.hpp"

#include "formats/lot_objects.hpp"
#include "interactions.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace os2::vm {

namespace {

// OBJD field "object version" (label 49 of STR# 0xCC) and object data 58
// "object version" (STR# 0x8D).
constexpr size_t kDefinitionObjectVersion = 0x40 + 2 * 49;
constexpr int16_t kObjectDataObjectVersion = 58;
constexpr size_t kLoadFunction = 2;

int16_t motiveValue(float v) { return int16_t(std::lround(std::clamp(v, -100.0f, 100.0f))); }

void restoreMotives(Object& o, const LotObjects::State& s) {
    if (!o.person || !s.personRead)
        return;
    for (size_t k = 0; k < kMotiveCount && k < s.motives.size(); ++k)
        o.person->motives[k] = motiveValue(s.motives[k]);
}

void restoreState(Object& o, const LotObjects::State& s) {
    if (o.attributes.size() < s.attributes.size())
        o.attributes.resize(s.attributes.size());
    std::copy(s.attributes.begin(), s.attributes.end(), o.attributes.begin());
    if (o.semiAttributes.size() < s.semiAttributes.size())
        o.semiAttributes.resize(s.semiAttributes.size());
    std::copy(s.semiAttributes.begin(), s.semiAttributes.end(), o.semiAttributes.begin());
    for (size_t i = 0; i < s.data.size() && i < o.data.size(); ++i)
        o.data[i] = s.data[i];
    o.data[objdata::ObjectId] = o.id;
    if (o.person && s.personRead)
        for (size_t i = 0; i < s.personData.size() && i < o.person->data.size(); ++i)
            o.person->data[i] = s.personData[i];
    restoreMotives(o, s);
    o.arrays.clear();
    for (size_t i = 0; i < s.arrays.size() && i < kMaxObjectArrays; ++i)
        o.arrays.push_back({s.arrays[i], 0});
    // The temporary token: its fields as data owner 0x27 numbers them (kind,
    // inventory ID, counted, index, GUID, flags, count) and the token itself.
    const auto& f = s.tempTokenFields;
    TempToken& t = o.tempToken;
    t.kind = f[0] >= 0 && f[0] <= 3 ? InventoryKind(f[0]) : InventoryKind::Global;
    t.inventoryId = f[1];
    t.counted = f[2] != 0;
    t.index = f[3];
    t.token.guid = s.token.guid;
    t.token.flags = int16_t(s.token.flags);
    t.token.properties = s.token.properties;
    t.loaded = s.token.guid != 0;
    // Timers: (sequence, tree, owner, flags, ticks, ticks left, parameters);
    // flags 0 in every saved timer, taken as "looping" in bit 0 (inferred).
    o.timers.clear();
    for (const auto& st : s.timers) {
        if (o.timers.size() >= kMaxTimersPerObject)
            break;
        ObjectTimer x;
        x.id = int16_t(st.a);
        x.tree = uint16_t(st.b);
        x.period = st.e;
        x.ticks = std::max(st.f, 1);
        x.looping = st.d & 1;
        x.params = st.params;
        o.timers.push_back(x);
    }
}

// The saved interactions of a Sim (cTSPerson: the interaction in its slot
// at +0x5D4, which holds the running one, then the queue at +0x584; in the
// game's lots the queues are empty and 3 of the 55 Sims have one running):
// each record becomes a queue entry of the TTAB entry of its stack object
// whose English TTA string is the record's name (else whose TTA index is the
// record's tree table entry), with its priority and parameters. When the
// running one's object is the code owner of a frame of the resumed Main,
// that frame (the first such) is its action tree's root: it is marked as the
// interaction, so it ends the entry when it returns. Records whose entry
// cannot be found are dropped.
// The queue entry a saved interaction record names (see restoreQueue).
std::optional<QueuedInteraction> entryOf(VM& vm, Object& sim, const LotObjects::SavedInteraction& rec) {
    const ObjectId leader = interactionLeader(vm, rec.stackObject);
    const Object* o = vm.object(leader);
    const auto ref = o ? interactionTable(vm, *o) : std::nullopt;
    if (!ref)
        return std::nullopt;
    std::optional<QueuedInteraction> q;
    auto english = [&](uint32_t i) { return ref->strings ? ref->strings->get(i, lang::EnglishUS) : std::string(); };
    if (rec.ttabEntry < 0x10000 && (rec.name.empty() || english(rec.ttabEntry) == rec.name))
        q = makeInteraction(vm, leader, uint16_t(rec.ttabEntry));
    if (!q && ref->strings && !rec.name.empty())
        for (const auto& e : ref->table->interactions)
            if (english(e.stringIndex) == rec.name) {
                q = makeInteraction(vm, leader, uint16_t(e.stringIndex));
                break;
            }
    if (!q)
        return std::nullopt;
    q->priority = int16_t(std::clamp<int64_t>(rec.priority, 0, kPriorityMaximum));
    q->pushedBy = rec.initiator > 0 ? rec.initiator : sim.id;
    q->args = rec.params;
    if (sim.nextInteractionUid == 0)
        ++sim.nextInteractionUid;
    q->uid = sim.nextInteractionUid++;
    return q;
}

// The saved sub-queue (+0x5AC) and current sub-action (+0x5DC), as the
// queue's: the sub-action's frame is the first frame of the resumed Main,
// above the running interaction's, whose code owner is its object.
size_t restoreSubQueue(VM& vm, Object& sim, const LotObjects::State& s) {
    sim.subQueue.clear();
    sim.subAction.reset();
    for (const auto& rec : s.list5AC)
        if (auto q = entryOf(vm, sim, rec); q && sim.subQueue.size() < kMaxSubQueue)
            sim.subQueue.push_back(std::move(*q));
    if (s.pending && s.pending->ttabEntry != 0xFFFFFFFF && s.pending->stackObject > 0)
        if (auto q = entryOf(vm, sim, *s.pending)) {
            if (auto t = vm.threads.find(sim.id); t != vm.threads.end()) {
                bool above = false;
                for (Frame& f : t->second.stack) {
                    above = above || f.interaction;
                    if (above && !f.interaction && f.codeOwner != sim.id &&
                        (f.codeOwner == s.pending->stackObject || f.codeOwner == q->object)) {
                        f.subInteraction = true;
                        sim.subAction = std::move(*q);
                        break;
                    }
                }
            }
        }
    syncQueueData(sim);
    return sim.subQueue.size() + (sim.subAction ? 1 : 0);
}

size_t restoreQueue(VM& vm, Object& sim, const LotObjects::State& s) {
    if (!sim.person)
        return 0;
    std::vector<const LotObjects::SavedInteraction*> records;
    const bool running = s.slotA.ttabEntry != 0xFFFFFFFF && s.slotA.stackObject > 0;
    if (running)
        records.push_back(&s.slotA);
    for (const auto& q : s.queue)
        records.push_back(&q);
    if (records.empty())
        return 0;
    sim.queue.clear();
    sim.activeInteraction = -1;
    for (const auto* rec : records) {
        const ObjectId leader = interactionLeader(vm, rec->stackObject);
        const Object* o = vm.object(leader);
        const auto ref = o ? interactionTable(vm, *o) : std::nullopt;
        if (!ref)
            continue;
        // The tree table entry when its English string is the record's name
        // (or the record has none), else the entry with that name.
        std::optional<QueuedInteraction> q;
        auto english = [&](uint32_t i) { return ref->strings ? ref->strings->get(i, lang::EnglishUS) : std::string(); };
        if (rec->ttabEntry < 0x10000 && (rec->name.empty() || english(rec->ttabEntry) == rec->name))
            q = makeInteraction(vm, leader, uint16_t(rec->ttabEntry));
        if (!q && ref->strings && !rec->name.empty())
            for (const auto& e : ref->table->interactions)
                if (english(e.stringIndex) == rec->name) {
                    q = makeInteraction(vm, leader, uint16_t(e.stringIndex));
                    break;
                }
        if (!q || sim.queue.size() >= kMaxQueuedInteractions)
            continue;
        q->priority = int16_t(std::clamp<int64_t>(rec->priority, 0, kPriorityMaximum));
        q->pushedBy = rec->initiator > 0 ? rec->initiator : sim.id;
        q->args = rec->params;
        q->uid = newInteractionId(vm, sim);
        sim.queue.push_back(std::move(*q));
    }
    if (sim.queue.empty())
        return 0;
    if (auto t = vm.threads.find(sim.id); running && t != vm.threads.end() && sim.queue[0].object == interactionLeader(vm, s.slotA.stackObject))
        for (Frame& f : t->second.stack)
            if ((f.codeOwner == s.slotA.stackObject || f.codeOwner == sim.queue[0].object) && f.codeOwner != sim.id) {
                f.interaction = true;
                sim.activeInteraction = 0;
                break;
            }
    syncQueueData(sim);
    return sim.queue.size();
}

} // namespace

void applySavedSimulator(VM& vm, const LotObjects& saved) {
    // The base game's lots hold 54 globals (0x94/0x9F: 46; the rest keep
    // what the VM has).
    if (saved.globals.size() <= global::Year)
        return;
    std::copy_n(saved.globals.begin(), std::min(saved.globals.size(), kGlobalCount), vm.globals.begin());
    const auto& g = saved.globals;
    auto& c = vm.clock;
    c.hour = std::clamp<int>(g[global::Hour], 0, 23);
    c.minute = std::clamp<int>(g[global::Minute], 0, 59);
    c.tick = std::clamp<int>(g[global::Second], 0, 59) * c.ticksPerMinute / 60;
    c.day = std::max<int>(g[global::DayOfMonth], 1);
    c.month = std::clamp<int>(g[global::Month], 1, 12);
    c.year = g[global::Year];
    vm.setClockGlobals();
}

void applySavedMotives(VM& vm, const LotLoad& load, const LotObjects& saved) {
    std::map<int16_t, const LotObjects::State*> states;
    for (const auto& s : saved.objects)
        states[s.id] = &s;
    for (const auto& e : load.objects)
        if (Object* o = e.created ? vm.object(e.id) : nullptr)
            if (auto it = states.find(e.id); it != states.end())
                restoreMotives(*o, *it->second);
}

ResumeResult resumeLotObjects(VM& vm, const LotLoad& load, const LotObjects& saved) {
    ResumeResult out;
    std::map<int16_t, const LotObjects::State*> states;
    for (const auto& s : saved.objects)
        states[s.id] = &s;
    std::vector<ObjectId> ids;
    for (const auto& e : load.objects)
        if (e.created && vm.object(e.id))
            ids.push_back(e.id);
    // Restore every object first: the tree stacks name other objects.
    for (ObjectId id : ids)
        if (auto it = states.find(id); it != states.end()) {
            restoreState(*vm.object(id), *it->second);
            ++out.restored;
        }
    for (ObjectId id : ids) {
        auto it = states.find(id);
        const LotObjects::State* s = it == states.end() ? nullptr : it->second;
        std::string problem;
        if (!s || s->stack.empty()) {
            problem = "no saved tree stack";
        } else {
            Thread t;
            t.me = id;
            std::copy(s->temps.begin(), s->temps.end(), t.temps.begin());
            for (const auto& f : s->stack) {
                // A frame runs the code of "running on" (0 in the older layout: me).
                const ObjectId ownerId = f.runningOn ? ObjectId(f.runningOn) : id;
                const Object* owner = vm.object(ownerId);
                if (!owner) {
                    problem = "a frame runs the code of a missing object";
                    break;
                }
                auto [group, behavior] = vm.resolveTree(*owner, f.treeId);
                if (!behavior || behavior->instructions.empty()) {
                    problem = "a frame's tree is missing";
                    break;
                }
                if (f.node >= behavior->instructions.size()) {
                    problem = "a frame's node is past its tree";
                    break;
                }
                Frame fr;
                fr.behavior = behavior;
                fr.group = group;
                fr.instance = f.treeId;
                fr.codeOwner = ownerId;
                fr.pc = f.node;
                fr.stackObject = ObjectId(f.objectId);
                fr.params = f.params;
                fr.params.resize(std::max<size_t>({fr.params.size(), behavior->argCount, 4}), 0);
                fr.locals = f.locals;
                fr.locals.resize(std::max<size_t>(fr.locals.size(), behavior->localCount), 0);
                t.stack.push_back(std::move(fr));
            }
            if (problem.empty() && t.stack.size() <= kMaxStackDepth) {
                vm.threads.insert_or_assign(id, std::move(t));
                ++out.stacks;
            } else if (problem.empty()) {
                problem = "the saved tree stack is too deep";
            }
        }
        if (!problem.empty()) {
            ++out.problems[problem];
            out.freshMains += vm.startMain(id);
        }
    }
    // The Sims' saved interaction queues, now that their stacks are back.
    for (ObjectId id : ids)
        if (auto it = states.find(id); it != states.end())
            if (Object* o = vm.object(id)) {
                out.queued += restoreQueue(vm, *o, *it->second);
                if (o->person)
                    out.queued += restoreSubQueue(vm, *o, *it->second);
            }
    // The load entry point runs only when the object's version differs from
    // its definition's (the editor's description of OBJf entry 2).
    for (ObjectId id : ids) {
        Object* o = vm.object(id);
        if (!o || !o->definition || !states.count(id))
            continue;
        if (uint16_t(o->data[kObjectDataObjectVersion]) == o->definition->field(kDefinitionObjectVersion))
            continue;
        if (auto t = vm.runFunction(id, kLoadFunction)) {
            ++out.loads;
            if (t->status == Thread::Status::Failed)
                out.failedLoads.push_back(std::move(*t));
        }
    }
    return out;
}

ResumeResult startSavedLot(VM& vm, const LotLoad& load, const LotObjects& saved, bool fromInit) {
    applySavedSimulator(vm, saved);
    if (!fromInit)
        return resumeLotObjects(vm, load, saved);
    for (const auto& e : load.objects)
        if (e.created && vm.object(e.id))
            vm.runFunction(e.id, 0);
    applySavedMotives(vm, load, saved);
    ResumeResult out;
    for (const auto& e : load.objects)
        out.freshMains += e.created && vm.startMain(e.id);
    return out;
}

} // namespace os2::vm
