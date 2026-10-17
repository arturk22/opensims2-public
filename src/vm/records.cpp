// The person's object records (cTSPerson +0x60c, cTSPerson::cObjectRecord)
// and the resets they carry along (interactions.hpp; docs/primitives-mac.md
// §4j "Os registros de objetos da pessoa").

#include "interactions.hpp"
#include "kill.hpp"

#include <algorithm>

namespace os2::vm {

namespace {

constexpr size_t kObjectDataUseCount = 62;

void addUse(VM& vm, ObjectId id, int16_t by) {
    if (Object* o = vm.object(id))
        o->data[kObjectDataUseCount] = int16_t(o->data[kObjectDataUseCount] + by);
}

// The current sub-action, else the current action (the interaction whose
// target the records' flag sets), or none (the binary's empty interaction).
QueuedInteraction* currentInteraction(Object& p) {
    if (p.subAction)
        return &*p.subAction;
    if (p.activeInteraction >= 0 && size_t(p.activeInteraction) < p.queue.size())
        return &p.queue[size_t(p.activeInteraction)];
    return nullptr;
}

} // namespace

// cTSPerson::OnGosubObjectTree(object, flag) (BV 0xad2318, base 1.0
// 0xb878f0), after cTSObject::GosubObjectTree pushed the object's tree (base
// 1.0: only when the push worked): a record with the object, the tree
// stack's depth with the new frame and the flag; with the flag the current
// sub-action (else action) takes the object as its target (SetTargetObject:
// the queue icon) and drops the icon by GUID; the object's data 62 "use
// count" goes up by 1. cTSObject::OnGosubObjectTree does nothing: only
// persons keep records.
void gosubObjectTree(VM& vm, Thread& t, ObjectId object, bool changeIcon) {
    Object* me = vm.object(t.me);
    if (!me || !me->person || !vm.object(object))
        return;
    if (changeIcon)
        if (QueuedInteraction* q = currentInteraction(*me))
            q->target = object;
    me->person->objectRecords.push_back({object, int16_t(t.stack.size()), changeIcon, t.serial});
    addUse(vm, object, 1);
}

// cTSPerson::StackJustPopped (BV 0xadef18, base 1.0 0xb83b04), after a
// frame of the person's tree stack is popped: with records, the last one
// leaves when the stack is now its depth - 1 (its frame went: data 62 of
// its object - 1, and every record with the flag, the leaving one too,
// gives its object as the current (sub-)action's target in turn) or is
// shallower than it (data 62 - 1). Only the thread that pushed it pops it
// (a one-tick tree runs on a thread of its own here). The action's and
// sub-action's ends that follow in the binary (+0x646, +0x644) are when
// their frames return (endInteraction, endSubInteraction).
void stackJustPopped(VM& vm, Thread& t) {
    Object* me = vm.object(t.me);
    if (!me || !me->person)
        return;
    auto& records = me->person->objectRecords;
    if (records.empty() || records.back().thread != t.serial)
        return;
    const int depth = int(t.stack.size());
    const auto last = records.back();
    if (depth == last.depth - 1) {
        addUse(vm, last.object, -1);
        for (const auto& r : records)
            if (r.flag)
                if (QueuedInteraction* q = currentInteraction(*me))
                    q->target = r.object;
        records.pop_back();
    } else if (depth < last.depth) {
        addUse(vm, last.object, -1);
        records.pop_back();
    }
}

void popFrames(VM& vm, Thread& t, size_t size) {
    while (t.stack.size() > size) {
        t.stack.pop_back();
        stackJustPopped(vm, t);
    }
}

// The records of a thread that is gone (a one-tick tree, a check tree, an
// OBJf entry run on a thread of its own) and left some (it failed, or ran
// out of turns): off, with their data 62.
void dropThreadRecords(VM& vm, const Thread& t) {
    Object* me = vm.object(t.me);
    if (!me || !me->person)
        return;
    auto& records = me->person->objectRecords;
    for (size_t i = records.size(); i-- > 0;)
        if (records[i].thread == t.serial) {
            addUse(vm, records[i].object, -1);
            records.erase(records.begin() + std::ptrdiff_t(i));
        }
}

// cTSPerson::HasObjectReference (BV 0xacce70): the object is in a record.
bool hasObjectReference(const Object& p, ObjectId id) {
    if (!p.person)
        return false;
    return std::any_of(p.person->objectRecords.begin(), p.person->objectRecords.end(),
                       [&](const Person::ObjectRecord& r) { return r.object == id; });
}

// cTSPerson::OnCleanup's records (BV 0xadcbcc), as a person is reset
// (Cleanup(nobody), or Cleanup(object) of an object in a record, 157 or 80):
// from the last record to the first, its frame goes (data 62 - 1) and, with
// `cascade` (the person, or that object, is not in the kill queue: flag
// 0x1000), every other person is told of the record's object
// (cEdithObjectModule::CleanupPeople, BV 0x524ea4) and the object is reset
// (cTSObject::Cleanup(nobody), BV 0xb09838). The thread itself is started
// over by the caller.
void cleanupRecords(VM& vm, ObjectId person, bool cascade) {
    for (;;) {
        Object* p = vm.object(person);
        if (!p || !p->person || p->person->objectRecords.empty())
            break;
        const auto rec = p->person->objectRecords.back();
        p->person->objectRecords.pop_back();
        addUse(vm, rec.object, -1);
        if (cascade) { // the person's own records too (the router's trees): its partners are reset
            cleanupPeople(vm, rec.object, true);
            cleanupObject(vm, rec.object);
        }
    }
    if (Object* p = vm.object(person); p && p->person) {
        p->person->recordsAtAction = 1;
        p->person->recordsAtSubAction = 0;
    }
}

// cEdithObjectModule::CleanupPeople(x): every person but x gets
// Cleanup(x), cTSPerson::OnCleanup(x): reset (as Cleanup(nobody), the
// cascade going on unless x is dying) when x is in one of its records or is
// its sub-queue object (157) or current interaction object (80); else the
// waiting entries of its queue and sub-queue on x (object or target) leave
// (DeleteTopAction: ActionSkipped) and its current interaction drops x as
// its target.
void cleanupPeople(VM& vm, ObjectId x, bool cascade) {
    std::vector<ObjectId> people;
    for (const Object* o : vm.objectTable())
        if (o && o->person && o->id != x)
            people.push_back(o->id);
    for (ObjectId id : people) {
        Object* p = vm.object(id);
        if (!p || !p->person || vm.cleaningUp.contains(id))
            continue;
        const bool full = hasObjectReference(*p, x) || p->person->data[persondata::SubQueueObject] == x ||
                          p->person->data[persondata::CurrentInteractionObject] == x;
        if (full) {
            if (auto t = vm.threads.find(id); t != vm.threads.end())
                vm.resetObject(id, t->second, cascade && !dying(vm, x));
            continue;
        }
        auto on = [&](const QueuedInteraction& q) { return q.object == x || q.target == x; };
        const size_t first = size_t(p->activeInteraction + 1);
        for (size_t i = p->queue.size(); i-- > first;)
            if (on(p->queue[i])) {
                const QueuedInteraction gone = p->queue[i];
                p->queue.erase(p->queue.begin() + std::ptrdiff_t(i));
                actionSkipped(vm, id, gone);
                p = vm.object(id);
                if (!p)
                    break;
            }
        if (!p)
            continue;
        for (size_t i = p->subQueue.size(); i-- > 0;)
            if (on(p->subQueue[i])) {
                const QueuedInteraction gone = p->subQueue[i];
                p->subQueue.erase(p->subQueue.begin() + std::ptrdiff_t(i));
                actionSkipped(vm, id, gone);
                p = vm.object(id);
                if (!p)
                    break;
            }
        if (!p)
            continue;
        if (QueuedInteraction* q = currentInteraction(*p); q && q->target == x)
            q->target = 0;
        syncQueueData(*p);
    }
}

// cTSObject::Cleanup(nobody) of `id` (BV 0xb09838): its reset
// (VM::resetObject; a person's goes on to its own records).
void cleanupObject(VM& vm, ObjectId id) {
    if (!vm.object(id) || vm.cleaningUp.contains(id))
        return;
    if (auto t = vm.threads.find(id); t != vm.threads.end())
        vm.resetObject(id, t->second, true);
}

} // namespace os2::vm
