// Saving a lot from the VM (lot_save.hpp).

#include <set>
#include "lot_save.hpp"

#include "placement.hpp"

#include "formats/lot_objects.hpp"
#include "formats/binreader.hpp"
#include "interactions.hpp"
#include "save_types.hpp"
#include "thread_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace os2::vm {

namespace {

constexpr int16_t kPersonObjectType = 2;

float savedMotive(const Object& o, size_t k, float before, bool known) {
    const Person& p = *o.person;
    // The motive engine's float, fractions and all (the game keeps floats;
    // the trees see integers); else, still the integer it was read as, the
    // float read.
    if (p.exactMotivesValid && p.writtenMotives[k] == p.motives[k])
        return p.exactMotives[k];
    if (known && std::lround(std::clamp(before, -100.0f, 100.0f)) == p.motives[k])
        return before;
    return float(p.motives[k]);
}

void captureFrames(VM& vm, const LotObjects& saved, const Thread& t, LotObjects::State& s) {
    const auto before = s.stack;
    s.stack.clear();
    for (size_t i = 0; i < t.stack.size(); ++i) {
        const Frame& f = t.stack[i];
        LotObjects::State::Frame x;
        x.objectId = uint16_t(f.stackObject);
        x.treeId = f.instance;
        x.node = f.pc;
        x.runningObject = uint16_t(t.me);
        x.runningOn = uint16_t(f.codeOwner);
        auto st = saved.saveTypeOf.find(int16_t(f.codeOwner));
        x.behaviorSaveType = st == saved.saveTypeOf.end() ? 0 : uint16_t(st->second);
        // The resume pads parameters and locals to the tree's counts (and 4
        // parameters): saved as many as the frame had when it was read (the
        // tree's counts for a new frame), more only to keep nonzero values.
        const LotObjects::State::Frame* was = nullptr;
        if (i < before.size() && before[i].treeId == x.treeId &&
            (before[i].runningOn ? before[i].runningOn : uint16_t(t.me)) == x.runningOn)
            was = &before[i];
        auto fit = [](std::vector<int16_t>& out, const std::vector<int16_t>& vm, size_t n) {
            for (size_t k = n; k < vm.size(); ++k)
                if (vm[k])
                    n = k + 1;
            out.assign(n, 0);
            std::copy_n(vm.begin(), std::min(n, vm.size()), out.begin());
        };
        fit(x.params, f.params, was ? was->params.size() : f.behavior ? f.behavior->argCount : 0);
        fit(x.locals, f.locals, was ? was->locals.size() : f.behavior ? f.behavior->localCount : 0);
        // The stack element's primitive state (an Animate Sim's request ID,
        // Generic Sims Call's family, ...): the VM keeps it per frame.
        x.primitiveState = f.primitiveState;
        if (was) {
            // What the VM does not keep comes from the frame as read.
            x.behaviorSaveType = was->behaviorSaveType;
            if (!was->runningOn && !was->runningObject)
                x.runningOn = x.runningObject = 0; // a version without them
        }
        s.stack.push_back(std::move(x));
    }
}

LotObjects::SavedInteraction toRecord(VM& vm, const Object& sim, const QueuedInteraction& q,
                                      const LotObjects::SavedInteraction* before) {
    LotObjects::SavedInteraction r = before ? *before : LotObjects::SavedInteraction{};
    r.u0C = int16_t(sim.id);
    r.stackObject = int16_t(q.object);
    if (!before)
        r.target = -1;
    r.initiator = q.pushedBy == sim.id ? (before && before->initiator <= 0 ? before->initiator : int16_t(-1)) : int16_t(q.pushedBy);
    r.ttabEntry = q.interaction;
    r.params = q.args;
    r.priority = uint32_t(std::max<int16_t>(q.priority, 0));
    // The name as the record keeps it: the English TTA string.
    if (const Object* o = vm.object(q.object))
        if (const auto ref = interactionTable(vm, *o); ref && ref->strings)
            r.name = ref->strings->get(q.interaction, lang::EnglishUS);
    if (!r.uid)
        r.uid = q.uid;
    // The interaction's ID and link (cTSInteraction +0x40/+0x42; the IDs
    // come from global 36, saved with the globals).
    r.u40 = q.uid;
    r.u42 = q.link;
    return r;
}

void capturePerson(VM& vm, const Object& o, LotObjects::State& s, bool known) {
    const Person& p = *o.person;
    if (!s.personRead) {
        s.personTag = 0xDEADBEEF;
        s.personVersion = 2;
        s.personRead = true;
        s.slotA.ttabEntry = s.slotB.ttabEntry = 0xFFFFFFFF;
    }
    const auto motivesBefore = s.motives;
    for (size_t k = 0; k < kMotiveCount && k < s.motives.size(); ++k) {
        s.motives[k] = savedMotive(o, k, motivesBefore[k], known);
        if (std::lround(std::clamp(s.motives2[k], -100.0f, 100.0f)) != std::lround(std::clamp(s.motives[k], -100.0f, 100.0f)))
            s.motives2[k] = std::clamp(s.motives[k], -100.0f, 100.0f);
    }
    // The Set Motive Changes running (cTSPerson +0x618, cMotiveInc::Write:
    // u32 motive, f32 change per tick, f32 limit; Mac BV 0xad5c44).
    s.motiveIncs.clear();
    for (const MotiveChange& ch : p.motiveChanges)
        s.motiveIncs.push_back({uint32_t(uint16_t(ch.motive)), ch.perTick, ch.limit});
    if (s.personData.size() < p.data.size())
        s.personData.resize(p.data.size(), 0);
    std::copy(p.data.begin(), p.data.end(), s.personData.begin());
    // The running interaction in the slot at +0x5D4 (empty: tree table
    // entry -1), the rest in the queue.
    const LotObjects::SavedInteraction* runningBefore =
        s.slotA.ttabEntry != 0xFFFFFFFF && s.slotA.stackObject > 0 ? &s.slotA : nullptr;
    const auto queueBefore = s.queue;
    s.queue.clear();
    size_t first = 0;
    if (o.activeInteraction == 0 && !o.queue.empty()) {
        const auto& q = o.queue[0];
        const bool same = runningBefore && interactionLeader(vm, runningBefore->stackObject) == q.object;
        s.slotA = toRecord(vm, o, q, same ? runningBefore : nullptr);
        if (same)
            s.slotA.stackObject = runningBefore->stackObject;
        first = 1;
    } else if (runningBefore) {
        s.slotA.ttabEntry = 0xFFFFFFFF;
        s.slotA.name.clear();
    }
    for (size_t i = first; i < o.queue.size(); ++i) {
        const size_t j = i - first;
        s.queue.push_back(toRecord(vm, o, o.queue[i], j < queueBefore.size() ? &queueBefore[j] : nullptr));
    }
    // The current sub-action (+0x5DC) and the sub-queue (+0x5AC).
    const auto pendingBefore = s.pending;
    s.pending.reset();
    if (o.subAction)
        s.pending = toRecord(vm, o, *o.subAction,
                             pendingBefore && interactionLeader(vm, pendingBefore->stackObject) == o.subAction->object
                                 ? &*pendingBefore
                                 : nullptr);
    const auto subBefore = s.list5AC;
    s.list5AC.clear();
    for (size_t i = 0; i < o.subQueue.size(); ++i)
        s.list5AC.push_back(toRecord(vm, o, o.subQueue[i], i < subBefore.size() ? &subBefore[i] : nullptr));
}

void captureObject(VM& vm, const LotObjects& saved, const Object& o, LotObjects::State& s, bool known) {
    s.x = o.position.x;
    s.y = o.position.y;
    s.level = o.position.level;
    if (o.position.hasHeight())
        s.elevation = o.position.elevation;
    // All of them, as cTSObject::Write writes its counts (the attributes:
    // the OBJD's count, at least 8, or the count read from the save; the
    // semi-attributes: the semi-global's label count, grown to the saved
    // one). Trimming the trailing zeros left a created object with fewer
    // attributes after the reload, and a tree reading the last one failed
    // (N001_Lot36's kicked-over trash pile, attribute 7 of 7).
    s.attributes = o.attributes;
    s.semiAttributes = o.semiAttributes;
    if (s.data.size() < o.data.size())
        s.data.resize(o.data.size(), 0);
    std::copy(o.data.begin(), o.data.end(), s.data.begin());
    s.arrays.clear();
    for (const auto& a : o.arrays)
        s.arrays.push_back(a.values);
    const TempToken& t = o.tempToken;
    s.tempTokenFields[0] = int16_t(t.kind);
    s.tempTokenFields[1] = t.inventoryId;
    s.tempTokenFields[2] = t.counted;
    s.tempTokenFields[3] = t.index;
    s.token.guid = t.token.guid;
    s.token.flags = uint16_t(t.token.flags);
    s.token.properties = t.token.properties;
    const auto timersBefore = s.timers;
    s.timers.clear();
    for (size_t i = 0; i < o.timers.size(); ++i) {
        const ObjectTimer& x = o.timers[i];
        LotObjects::State::Timer st = i < timersBefore.size() ? timersBefore[i] : LotObjects::State::Timer{};
        st.a = uint32_t(x.id);
        st.b = x.tree;
        st.e = x.period;
        st.f = x.ticks;
        st.d = (st.d & ~1u) | (x.looping ? 1u : 0u);
        st.params = x.params;
        s.timers.push_back(st);
    }
    if (auto th = vm.threads.find(o.id); th != vm.threads.end() && th->second.status == Thread::Status::Running) {
        std::copy_n(th->second.temps.begin(), s.temps.size(), s.temps.begin());
        captureFrames(vm, saved, th->second, s);
    } else {
        s.stack.clear();
    }
    if (o.person)
        capturePerson(vm, o, s, known);
}

} // namespace

CaptureResult captureLot(VM& vm, const LotObjects& saved, const std::map<uint32_t, LotObjects::SaveType>* moreSaveTypes) {
    // The save's "about to save" message (0xCD94C8EE, cTSSaveLotController::
    // SaveLot step 7): cTSLot::ComputeAndStoreLotData refreshes the lot's
    // values (globals 46-49) before the simulator writes them
    // (docs/neighborhood-mac.md §11.1).
    computeLotData(vm);
    CaptureResult out;
    LotObjects& lot = out.lot;
    lot = saved;
    lot.objects.clear();
    lot.pairs.clear();
    lot.saveTypeOf.clear();
    std::map<uint32_t, int16_t> saveTypeOfGuid;
    for (const auto& [st, t] : saved.saveTypes)
        saveTypeOfGuid.emplace(t.guid, st);
    std::map<int16_t, const LotObjects::State*> before;
    for (const auto& s : saved.objects)
        before[s.id] = &s;
    for (const auto& [id, o] : vm.eachObject()) {
        auto it = before.find(int16_t(id));
        LotObjects::State s;
        // The same object as the one saved with this ID (an ID freed and
        // given to a new object is a new object).
        // A Sim that arrived on this load (formats/lot_arrivals.hpp) has a
        // state with no person part read: written as a new object.
        const bool known = it != before.end() && saved.guidOf(int16_t(id)) == o.guid() &&
                           !(it->second->person && !it->second->personRead);
        int16_t saveType = 0;
        if (known) {
            s = *it->second;
            saveType = saved.saveTypeOf.at(int16_t(id));
        } else {
            auto st = saveTypeOfGuid.find(o.guid());
            if (st == saveTypeOfGuid.end()) {
                // A record built from the definition as the game writes it
                // (save_types.hpp), else one from another lot's table.
                std::optional<LotObjects::SaveType> record = saveTypeRecord(vm.code(), o.guid());
                if (!record && moreSaveTypes)
                    if (auto m = moreSaveTypes->find(o.guid()); m != moreSaveTypes->end())
                        record = m->second;
                if (record) {
                    int16_t next = 1;
                    for (const auto& [k, _] : lot.saveTypes)
                        next = std::max<int16_t>(next, int16_t(k + 1));
                    lot.saveTypes[next] = *record;
                    lot.saveTypeOrder.push_back(next);
                    st = saveTypeOfGuid.emplace(o.guid(), next).first;
                    ++out.newSaveTypes;
                }
            }
            if (st == saveTypeOfGuid.end()) {
                out.warnings.push_back("object " + std::to_string(id) + " (GUID " + hexString(o.guid()) +
                                       ") has no save type in the lot: not saved");
                continue;
            }
            saveType = st->second;
            const auto& t = lot.saveTypes.at(saveType);
            s.id = int16_t(id);
            s.kind = t.objectType == kPersonObjectType ? LotObjects::Kind::Person
                     : (t.flags & 1)                   ? LotObjects::Kind::MultiTile
                                                       : LotObjects::Kind::Object;
            s.person = s.kind == LotObjects::Kind::Person;
            s.data.assign(lotObjectDataCount(saved.version), 0);
            // Empty parts as the game writes them: no relationships, the
            // slots marker (-101) with no slots, no effects, no overrides.
            s.relationshipTable = {0, 0, 0, 0};
            s.slotsRaw = {0x9B, 0xFF, 0, 0};
            s.effectsRaw = {0, 0};
            if (saved.version > 0x94)
                s.overridesRaw = {0, 0};
            if (s.kind == LotObjects::Kind::MultiTile)
                s.leadObject = multiTileGroup(o);
            ++out.added;
        }
        if (!s.tailError.empty()) {
            out.warnings.push_back("object " + std::to_string(id) + ": its saved state did not read through; only "
                                   "its position, attributes and object data are saved");
        }
        captureObject(vm, saved, o, s, known);
        if (s.kind == LotObjects::Kind::MultiTile)
            s.leadObject = int16_t(multiTileGroup(o));
        lot.pairs.emplace_back(uint32_t(uint16_t(id)), saveType);
        lot.saveTypeOf[int16_t(id)] = saveType;
        lot.objects.push_back(std::move(s));
        out.updated += known;
    }
    // Objects the VM could not create (their GUID is in no object
    // definition of the game: TV channels, the tutorials' import
    // controllers): kept as they were, as the game keeps what it skips.
    for (const auto& s : saved.objects)
        if (!vm.object(s.id) && !vm.code().definitionByGuid(saved.guidOf(s.id))) {
            const int16_t st = saved.saveTypeOf.at(s.id);
            lot.pairs.emplace_back(uint32_t(uint16_t(s.id)), st);
            lot.saveTypeOf[s.id] = st;
            lot.objects.push_back(s);
            ++out.kept;
        }
    // Save types the load added (a Sim that just moved in, formats/
    // lot_arrivals.hpp) are not in the table yet: written now, as the game
    // builds them from the definition (save_types.hpp) when it can.
    for (const auto& [id, st] : lot.saveTypeOf)
        if (std::find(lot.saveTypeOrder.begin(), lot.saveTypeOrder.end(), st) == lot.saveTypeOrder.end()) {
            if (auto record = saveTypeRecord(vm.code(), lot.saveTypes.at(st).guid))
                lot.saveTypes[st] = *record;
            lot.saveTypeOrder.push_back(st);
            ++out.newSaveTypes;
        }
    std::sort(lot.objects.begin(), lot.objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    // Keep the module's order for the objects that were in it.
    std::map<uint32_t, size_t> order;
    for (size_t i = 0; i < saved.pairs.size(); ++i)
        order[saved.pairs[i].first & 0xFFFF] = i;
    std::stable_sort(lot.pairs.begin(), lot.pairs.end(), [&](const auto& a, const auto& b) {
        auto ia = order.find(a.first), ib = order.find(b.first);
        const size_t x = ia == order.end() ? SIZE_MAX : ia->second, y = ib == order.end() ? SIZE_MAX : ib->second;
        return x != y ? x < y : a.first < b.first;
    });
    for (const auto& s : saved.objects) {
        const Object* o = vm.object(s.id);
        out.removed += !o || saved.guidOf(s.id) != o->guid();
    }
    out.removed -= out.kept;
    // The post-object table has a u32 per object ID (0xFFFF for a new one).
    uint32_t maxId = 0;
    for (const auto& p : lot.pairs)
        maxId = std::max(maxId, p.first & 0xFFFF);
    if (lot.postTable.size() < maxId)
        lot.postTable.resize(maxId, 0xFFFF);
    // All the globals the SIMI's version holds (54 in the base game).
    for (size_t i = 0; i < lot.globals.size() && i < kGlobalCount; ++i)
        lot.globals[i] = vm.globals[i];
    // The simulator keeps a copy of the family's funds (+0x98, the globals'
    // 13/14 as SetFunds writes them; docs/save-mac.md §5).
    if (lot.simulator.read && lot.globals.size() > size_t(global::BudgetDiv10000))
        lot.simulator.f98 = uint32_t(int32_t(vm.globals[global::BudgetDiv10000]) * 10000 +
                                     vm.globals[global::BudgetMod10000]);
    // The lot and objects values (+0xA4/+0xA8/+0xAC, placement.hpp computeLotData).
    if (lot.simulator.read && vm.lotValues.known) {
        lot.simulator.fA4 = uint32_t(vm.lotValues.lot);
        lot.simulator.fA8 = uint32_t(vm.lotValues.lotObjects);
        lot.simulator.fAC = uint32_t(vm.lotValues.familyObjects);
    }
    return out;
}

std::vector<std::string> compareLotState(VM& a, VM& b, size_t limit) {
    std::vector<std::string> out;
    size_t more = 0, failed = 0;
    std::string failedWhy;
    // Person data the queue keeps (interactions.hpp: persondata).
    auto queueData = [](size_t i) { return i == 21 || i == 33 || i == 45 || i == 71 || i == 79 || i == 80 || i == 102 || i == 166; };
    auto diff = [&](const std::string& s) {
        if (out.size() < limit)
            out.push_back(s);
        else
            ++more;
    };
    auto join = [](const auto& v) {
        std::string s;
        for (const auto& x : v)
            s += (s.empty() ? "" : " ") + std::to_string(x);
        return s;
    };
    auto trimmed = [](std::vector<int16_t> v) {
        while (!v.empty() && v.back() == 0)
            v.pop_back();
        return v;
    };
    for (const auto& [id, x] : a.objects()) {
        if (b.globalsMadeAtLoad.count(id))
            continue;
        const Object* y = b.object(id);
        const std::string at = "object " + std::to_string(id) + ": ";
        if (!y) {
            diff(at + "missing from the second (" + (x.definition ? x.definition->fileName : std::string("?")) +
                 (x.position.inWorld() ? "" : ", out of the world") + ")");
            continue;
        }
        const bool mainFailed = [&] {
            auto t = a.threads.find(id);
            return t != a.threads.end() && t->second.status != Thread::Status::Running;
        }();
        if (x.guid() != y->guid())
            diff(at + "GUID " + hexString(x.guid()) + " vs " + hexString(y->guid()));
        if (x.position.x != y->position.x || x.position.y != y->position.y || x.position.level != y->position.level)
            diff(at + "position");
        if (x.attributes != y->attributes)
            diff(at + "attributes " + join(x.attributes) + " vs " + join(y->attributes));
        if (x.semiAttributes != y->semiAttributes)
            diff(at + "semi-attributes");
        for (size_t i = 0; i < x.data.size(); ++i)
            if (x.data[i] != y->data[i])
                diff(at + "object data " + std::to_string(i) + " " + std::to_string(x.data[i]) + " vs " +
                     std::to_string(y->data[i]));
        if (bool(x.person) != bool(y->person)) {
            diff(at + "person");
        } else if (x.person) {
            for (size_t i = 0; i < x.person->data.size(); ++i)
                if (x.person->data[i] != y->person->data[i] && !(mainFailed && queueData(i)))
                    diff(at + "person data " + std::to_string(i));
            if (x.person->motives != y->person->motives)
                diff(at + "motives " + join(x.person->motives) + " vs " + join(y->person->motives));
        }
        // The VM's own thread state (thread_state.hpp: routing frames, the
        // route walked).
        if (bool(x.route) != bool(y->route) || (x.route && x.route->waiting != y->route->waiting))
            diff(at + "route " + (x.route ? (x.route->waiting ? "waiting" : "walking") : "none") + " vs " +
                 (y->route ? (y->route->waiting ? "waiting" : "walking") : "none"));
        if (const auto sx = writeThreadState(a, x), sy = writeThreadState(b, *y); sx != sy) {
            size_t k = 0;
            if (sx && sy)
                while (k < sx->size() && k < sy->size() && (*sx)[k] == (*sy)[k])
                    ++k;
            if (std::getenv("OS2_DEBUG_ROUTE_DIFF")) // both routes, for looking into the difference
                for (const Object* q : {&x, y}) {
                    std::fprintf(stderr, "route of %d:", q->id);
                    if (q->route) {
                        for (const auto& g : q->route->goals)
                            std::fprintf(stderr, " goal(%.3f,%.3f,%.1f)", g[0], g[1], g[2]);
                        for (const auto& pt : q->route->points)
                            std::fprintf(stderr, " (%.3f,%.3f)", pt.x, pt.y);
                    }
                    if (q->route) {
                        const auto& l = q->route->loco;
                        std::fprintf(stderr, " loco k%d mv%d %s ser%u sw%.6f stw%.6f ov%.6f t%d/%d lead%.6f d%.6f sp%.6f f(%.6f,%.6f) h%.6f tu%.6f et%.6f cy%.6f cs%.6f seg%zu tk%zu arc%.6f layers%zu blocked%d pauses%d check%d last%llu", int(l.kind), l.moving, l.animation.c_str(), l.serial, l.strideWeight, l.stopWeight, l.overlap, l.tick, l.ticks, l.lead, l.distance, l.speed, l.fx, l.fy, l.heading, l.turn, l.endTurn, l.cycle, l.cycleStart, l.segment, l.taken, l.arc, l.layers.size(), q->route->blockedTicks, q->route->pauses, q->route->check, (unsigned long long)q->route->lastCheck);
                        for (const auto& y : l.layers)
                            std::fprintf(stderr, " [%s w%.6f r%.6f f%.6f]", y.animation.c_str(), y.weight, y.rate, y.fade);
                    }
                    std::fprintf(stderr, " pos (%.4f,%.4f)\n", q->position.x, q->position.y);
                }
            diff(at + "routing state (frames or route; " + std::to_string(sx ? sx->size() : 0) + " vs " +
                 std::to_string(sy ? sy->size() : 0) + " bytes, from byte " + std::to_string(k) + ")");
        }
        // The Animate Sim playing (resumed from the Sim's animation queue).
        if (x.animation.playing() != y->animation.playing() || x.animation.name != y->animation.name ||
            (x.animation.playing() && x.animation.requestId && (x.animation.ticksLeft != y->animation.ticksLeft ||
                                       x.animation.playedTicks != y->animation.playedTicks ||
                                       x.animation.pendingEvents != y->animation.pendingEvents)))
            diff(at + "animation " + (x.animation.playing() ? x.animation.name + " " + std::to_string(x.animation.ticksLeft) + " " + std::to_string(x.animation.playedTicks) + "/" + std::to_string(x.animation.pendingEvents.size()) : "none") +
                 " vs " + (y->animation.playing() ? y->animation.name + " " + std::to_string(y->animation.ticksLeft) + " " + std::to_string(y->animation.playedTicks) + "/" + std::to_string(y->animation.pendingEvents.size()) : "none"));
        {
            auto overlays = [](const Object& o) {
                std::string s;
                for (const auto& v : o.animation.overlays) {
                    // Where its event tree runs counts only with one.
                    s += " " + v.name + "@" + std::to_string(v.played) + "x" + std::to_string(v.loops) + "/" +
                         std::to_string(v.pending.size()) + (v.blendOutSent ? "b" : "");
                    if (v.eventTree)
                        s += ":" + std::to_string(v.me) + "," + std::to_string(v.codeOwner) + "," +
                             std::to_string(v.stackObject) + "," + std::to_string(v.eventTree);
                }
                for (const auto& v : o.animation.overlayEvents) {
                    s += " events " + v.name + "@" + std::to_string(v.played) + "/" + std::to_string(v.pending.size()) +
                         ":" + std::to_string(v.me) + "," + std::to_string(v.codeOwner) + "," +
                         std::to_string(v.stackObject) + "," + std::to_string(v.eventTree);
                }
                return s;
            };
            // Poses an object holds (no loops, no event tree) are lost when
            // its scene graph record has no animation queue to keep them in
            // (a bare cObject, as the game's lots have for the food dishes):
            // those are not compared.
            const bool posesOnly = !x.person && y->animation.overlays.empty() && x.animation.overlayEvents.empty() &&
                                   std::all_of(x.animation.overlays.begin(), x.animation.overlays.end(),
                                               [](const Overlay& v) { return v.loops <= 0 && !v.eventTree; });
            if (const auto ox = overlays(x), oy = overlays(*y); ox != oy && !posesOnly)
                diff(at + "overlays" + ox + " vs" + oy);
        }
        if (x.arrays.size() != y->arrays.size())
            diff(at + "array count");
        else
            for (size_t i = 0; i < x.arrays.size(); ++i)
                if (x.arrays[i].values != y->arrays[i].values)
                    diff(at + "array " + std::to_string(i));
        if (x.tempToken.token.guid != y->tempToken.token.guid ||
            x.tempToken.token.properties != y->tempToken.token.properties || x.tempToken.index != y->tempToken.index)
            diff(at + "temporary token");
        if (x.timers.size() != y->timers.size())
            diff(at + "timer count");
        else
            for (size_t i = 0; i < x.timers.size(); ++i)
                if (x.timers[i].id != y->timers[i].id || x.timers[i].tree != y->timers[i].tree ||
                    x.timers[i].ticks != y->timers[i].ticks || x.timers[i].params != y->timers[i].params)
                    diff(at + "timer " + std::to_string(i));
        if (mainFailed) {
        } else if (x.queue.size() != y->queue.size()) {
            diff(at + "queue of " + std::to_string(x.queue.size()) + " vs " + std::to_string(y->queue.size()));
        } else {
            for (size_t i = 0; i < x.queue.size(); ++i)
                if (x.queue[i].object != y->queue[i].object || x.queue[i].interaction != y->queue[i].interaction ||
                    x.queue[i].priority != y->queue[i].priority || x.queue[i].args != y->queue[i].args)
                    diff(at + "queue entry " + std::to_string(i));
            if (x.activeInteraction != y->activeInteraction)
                diff(at + "running interaction");
        }
        auto tx = a.threads.find(id);
        auto ty = b.threads.find(id);
        const bool hx = tx != a.threads.end() && tx->second.status == Thread::Status::Running;
        const bool hy = ty != b.threads.end() && ty->second.status == Thread::Status::Running;
        if (tx != a.threads.end() && !hx) {
            ++failed; // a failed Main is not saved (the game has none): its thread and queue are not compared
            failedWhy += " " + std::to_string(id) + ": " + (std::getenv("S2PKG_ERRORS") ? tx->second.error : tx->second.error.substr(0, tx->second.error.find('\n'))) + ";";
        } else if (hx != hy) {
            diff(at + "Main thread running in " + (hx ? "the first" : "the second") + " only" +
                 (hx ? (ty == b.threads.end() ? "" : " (" + ty->second.error.substr(0, ty->second.error.find('\n')) + ")")
                     : (tx == a.threads.end() ? "" : " (" + tx->second.error.substr(0, tx->second.error.find('\n')) + ")")));
        } else if (hx) {
            const Thread& p = tx->second;
            const Thread& q = ty->second;
            if (!std::equal(p.temps.begin(), p.temps.begin() + 8, q.temps.begin()))
                diff(at + "temps");
            if (p.stack.size() != q.stack.size()) {
                diff(at + "stack depth " + std::to_string(p.stack.size()) + " vs " + std::to_string(q.stack.size()));
            } else {
                for (size_t i = 0; i < p.stack.size(); ++i) {
                    const Frame& f = p.stack[i];
                    const Frame& g = q.stack[i];
                    std::string what;
                    // The resume pads parameters to 4 and locals to the tree's
                    // counts with zeros: only the values count.
                    auto values = [](const std::vector<int16_t>& x, const std::vector<int16_t>& y) {
                        for (size_t k = 0; k < std::max(x.size(), y.size()); ++k)
                            if ((k < x.size() ? x[k] : 0) != (k < y.size() ? y[k] : 0))
                                return false;
                        return true;
                    };
                    auto field = [&](bool differs, const char* name) {
                        if (differs)
                            what += std::string(what.empty() ? "" : ", ") + name;
                    };
                    field(f.instance != g.instance || f.pc != g.pc, "tree/node");
                    field(f.codeOwner != g.codeOwner, "code owner");
                    field(f.stackObject != g.stackObject, "stack object");
                    field(!values(f.params, g.params), "params");
                    field(!values(f.locals, g.locals), "locals");
                    field(f.interaction != g.interaction, "interaction");
                    if (!what.empty())
                        diff(at + "frame " + std::to_string(i) + " (tree " + hexString(f.instance) + " node " +
                             std::to_string(f.pc) + " vs tree " + hexString(g.instance) + " node " +
                             std::to_string(g.pc) + "): " + what);
                }
            }
        }
    }
    // The global sim objects the reload made (updateSimObjects, as
    // cEdithObjectModule::UpdateSimObjects): the curtain and the tutorial
    // controllers delete themselves once done, so the reload makes them
    // again, as the game's does, with their own birth date and Main.
    for (const auto& [id, o] : b.objects())
        if (!a.object(id) && !b.globalsMadeAtLoad.count(id))
            diff("object " + std::to_string(id) + (o.definition ? " (" + o.definition->fileName + ")" : std::string()) +
                 ": missing from the first");
    for (size_t i = 0; i < kGlobalCount; ++i)
        if (a.globals[i] != b.globals[i])
            diff("global " + std::to_string(i) + " " + std::to_string(a.globals[i]) + " vs " +
                 std::to_string(b.globals[i]));
    // The lot's relationships (one side at least a lot object; Sim-to-Sim
    // ones are the neighborhood's).
    auto lotRelationships = [&](VM& m) {
        std::map<std::pair<VM::RelationshipKey, VM::RelationshipKey>, std::vector<int16_t>> r;
        for (const auto& [k, v] : m.relationships)
            if (!(k.first.neighbor && k.second.neighbor) && !trimmed(v).empty())
                r[k] = trimmed(v);
        return r;
    };
    {
        const auto ra = lotRelationships(a), rb = lotRelationships(b);
        for (const auto& [k, v] : ra) {
            auto it = rb.find(k);
            if (it == rb.end() || it->second != v)
                diff("relationship " + std::to_string(k.first.id) + (k.first.neighbor ? "n" : "") + " -> " +
                     std::to_string(k.second.id) + (k.second.neighbor ? "n" : "") + ": " + join(v) + " vs " +
                     (it == rb.end() ? std::string("none") : join(it->second)));
        }
        for (const auto& [k, v] : rb)
            if (!ra.count(k))
                diff("relationship " + std::to_string(k.first.id) + (k.first.neighbor ? "n" : "") + " -> " +
                     std::to_string(k.second.id) + (k.second.neighbor ? "n" : "") + ": " + join(v) +
                     " only in the second");
    }
    if (a.clock.hour != b.clock.hour || a.clock.minute != b.clock.minute || a.clock.day != b.clock.day ||
        a.clock.month != b.clock.month || a.clock.year != b.clock.year)
        diff("clock");
    if (more)
        out.push_back("... and " + std::to_string(more) + " more");
    if (failed && !out.empty())
        out.push_back("(" + std::to_string(failed) + " Mains had failed before the save: their threads and queues were not compared:" + failedWhy + ")");
    return out;
}

} // namespace os2::vm
