// The kill queue (kill.hpp).

#include "kill.hpp"

#include "interactions.hpp"

#include <algorithm>

namespace os2::vm {

namespace {

constexpr uint16_t kObjdTypePerson = 2;
constexpr size_t kObjdType = 0x52; // OBJD "type" (editor label 9)

bool isPerson(const Object& o) {
    return o.person || (o.definition && o.definition->field(kObjdType) == kObjdTypePerson);
}

// The object, the parts of its multi-tile object and, recursively, what is
// in their slots except Sims.
void collect(VM& vm, ObjectId id, std::vector<ObjectId>& out) {
    const Object* o = vm.object(id);
    if (!o || std::find(out.begin(), out.end(), id) != out.end())
        return;
    const ObjectId group = multiTileGroup(*o);
    std::vector<ObjectId> parts;
    for (const auto& [pid, p] : vm.eachObject())
        if (pid == id || multiTileGroup(p) == group)
            parts.push_back(pid);
    for (ObjectId pid : parts)
        if (std::find(out.begin(), out.end(), pid) == out.end())
            out.push_back(pid);
    for (ObjectId pid : parts)
        for (const auto& [cid, c] : vm.eachObject())
            if (cid != pid && c.data[objdata::ContainerId] == pid && !isPerson(c))
                collect(vm, cid, out);
}

// cTSObject::Cleanup(object) on one thread: unwind the frames running the
// object's code.
// True if the thread ran some of it (the tree sim's cleanup, vtable 0xe4,
// found frames: cTSObject::Cleanup then skips the person's OnCleanup).
bool unwind(VM& vm, ObjectId owner, Thread& t, const std::vector<ObjectId>& gone) {
    auto isGone = [&](ObjectId id) { return std::find(gone.begin(), gone.end(), id) != gone.end(); };
    size_t first = t.stack.size();
    for (size_t i = 0; i < t.stack.size(); ++i)
        if (isGone(t.stack[i].codeOwner)) {
            first = i;
            break;
        }
    if (first == t.stack.size())
        return false;
    // The innermost interaction frame at or below the first frame of the object.
    std::optional<size_t> action;
    for (size_t i = 0; i <= first; ++i)
        if (t.stack[i].interaction && !t.stack[i].foundAction)
            action = i;
    const bool sub = std::any_of(t.stack.begin() + std::ptrdiff_t(action ? *action : first), t.stack.end(),
                                 [](const Frame& f) { return f.subInteraction; });
    if (sub)
        if (Object* me = vm.object(owner)) {
            me->subAction.reset();
            syncQueueData(*me);
        }
    // The frames go one by one, each with StackJustPopped (the person's
    // object records, records.cpp).
    if (action && *action > 0) {
        popFrames(vm, t, *action);
        endInteraction(vm, t);
        return true;
    }
    popFrames(vm, t, 0);
    const Object* o = vm.object(owner);
    const uint16_t main = o ? vm.functionTree(*o, 1) : 0;
    if (!main) {
        t.status = Thread::Status::Killed;
        return true;
    }
    const size_t done = t.instructions;
    if (Object* me = vm.object(owner)) { // the queue's running entry is gone with its tree
        if (me->activeInteraction >= 0 && size_t(me->activeInteraction) < me->queue.size())
            me->queue.erase(me->queue.begin() + me->activeInteraction);
        me->activeInteraction = -1;
        becomeEmptyAction(vm, *me);
        syncQueueData(*me);
        setInteractionData(*me);
    }
    t = vm.start(owner, main);
    t.instructions = done;
    return true;
}

// cTSPerson::OnCleanup(object) (BV 0xadcbcc), for a person whose thread ran
// none of the object's code: the object its current interaction is on
// (person data 80) or its sub-queue object (157) gone resets the person as
// Cleanup(nobody) does; so does the target of the route it walks
// (cTSRoutingObjectAggregate::OnCleanup BV 0x5727c8 flags the route,
// DoObjectDeletedWhileWalking 0x56d880 resets the walker,
// "Router::DoObjectDeletedWhileWalking"); so does an object in one of its
// object records (HasObjectReference, records.cpp).
bool personLosesTarget(VM& vm, const Object& p, const std::vector<ObjectId>& gone) {
    auto isGone = [&](ObjectId id) { return id > 0 && std::find(gone.begin(), gone.end(), id) != gone.end(); };
    if (!p.person)
        return false;
    for (ObjectId g : gone)
        if (hasObjectReference(p, g))
            return true;
    if (isGone(p.person->data[persondata::CurrentInteractionObject]) || isGone(p.person->data[persondata::SubQueueObject]))
        return true;
    return p.route && isGone(p.route->target);
}

} // namespace

bool dying(const VM& vm, ObjectId id) {
    return vm.dyingObjects.contains(id);
}

bool queueKill(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o || dying(vm, id))
        return false;
    for (ObjectId in = o->data[objdata::ContainerId]; in > 0;) {
        if (dying(vm, in))
            return false;
        const Object* c = vm.object(in);
        if (!c || c->data[objdata::ContainerId] == in)
            break;
        in = c->data[objdata::ContainerId];
    }
    std::vector<ObjectId> all;
    collect(vm, id, all);
    vm.dyingObjects.insert(all.begin(), all.end());
    vm.killQueue.push_back(id);
    return true;
}

void flushKillQueue(VM& vm) {
    while (!vm.killQueue.empty()) {
        const ObjectId id = vm.killQueue.front();
        vm.killQueue.erase(vm.killQueue.begin());
        if (!dying(vm, id) || !vm.object(id))
            continue;
        std::vector<ObjectId> gone;
        collect(vm, id, gone);
        for (ObjectId g : gone) {
            cancelInteractionsWith(vm, g);
            vm.dyingObjects.erase(g);
        }
        // Sims in its slots are dropped where they are.
        for (const auto& [sid, s] : vm.eachObject())
            if (s.person && std::find(gone.begin(), gone.end(), s.data[objdata::ContainerId]) != gone.end()) {
                vm.setContainer(*vm.object(sid), 0, -1);
            }
        for (auto& [tid, t] : vm.threads)
            if (std::find(gone.begin(), gone.end(), tid) == gone.end() && t.status == Thread::Status::Running &&
                !unwind(vm, tid, t, gone))
                if (const Object* p = vm.object(tid); p && personLosesTarget(vm, *p, gone))
                    vm.resetObject(tid, t, false); // the object is in the kill queue (flag 0x1000): no cascade
        // Its own table goes with it, and every object left drops the entry
        // keyed by its ID (KillOutOfWorldObject, BV 0x532d08: RemoveEntry on
        // each object's table; the key is raw, so an ownership entry whose
        // neighbor ID equals the ID goes too). Neighbors' tables stay.
        for (auto it = vm.relationships.begin(); it != vm.relationships.end();) {
            const auto& [a, b] = it->first;
            const bool drop = (!a.neighbor && std::find(gone.begin(), gone.end(), a.id) != gone.end()) ||
                              (!b.neighbor && std::find(gone.begin(), gone.end(), b.id) != gone.end());
            it = drop ? vm.relationships.erase(it) : std::next(it);
        }
        for (ObjectId g : gone)
            vm.removeObject(g);
    }
    vm.dyingObjects.clear();
}

} // namespace os2::vm
