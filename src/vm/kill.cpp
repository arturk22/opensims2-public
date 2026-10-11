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
    for (const auto& [pid, p] : vm.objects())
        if (pid == id || multiTileGroup(p) == group)
            parts.push_back(pid);
    for (ObjectId pid : parts)
        if (std::find(out.begin(), out.end(), pid) == out.end())
            out.push_back(pid);
    for (ObjectId pid : parts)
        for (const auto& [cid, c] : vm.objects())
            if (cid != pid && c.data[objdata::ContainerId] == pid && !isPerson(c))
                collect(vm, cid, out);
}

// cTSObject::Cleanup(object) on one thread: unwind the frames running the
// object's code.
void unwind(VM& vm, ObjectId owner, Thread& t, const std::vector<ObjectId>& gone) {
    auto isGone = [&](ObjectId id) { return std::find(gone.begin(), gone.end(), id) != gone.end(); };
    size_t first = t.stack.size();
    for (size_t i = 0; i < t.stack.size(); ++i)
        if (isGone(t.stack[i].codeOwner)) {
            first = i;
            break;
        }
    if (first == t.stack.size())
        return;
    // The innermost interaction frame at or below the first frame of the object.
    std::optional<size_t> action;
    for (size_t i = 0; i <= first; ++i)
        if (t.stack[i].interaction && !t.stack[i].foundAction)
            action = i;
    if (action && *action > 0) {
        t.stack.resize(*action);
        endInteraction(vm, t);
        return;
    }
    const Object* o = vm.object(owner);
    const uint16_t main = o ? vm.functionTree(*o, 1) : 0;
    if (!main) {
        t.status = Thread::Status::Killed;
        return;
    }
    const size_t done = t.instructions;
    if (Object* me = vm.object(owner)) { // the queue's running entry is gone with its tree
        if (me->activeInteraction >= 0 && size_t(me->activeInteraction) < me->queue.size())
            me->queue.erase(me->queue.begin() + me->activeInteraction);
        me->activeInteraction = -1;
        syncQueueData(*me);
    }
    t = vm.start(owner, main);
    t.instructions = done;
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
        for (const auto& [sid, s] : vm.objects())
            if (s.person && std::find(gone.begin(), gone.end(), s.data[objdata::ContainerId]) != gone.end()) {
                Object& sim = *vm.object(sid);
                sim.data[objdata::ContainerId] = 0;
                sim.data[objdata::SlotNumber] = -1;
            }
        for (auto& [tid, t] : vm.threads)
            if (std::find(gone.begin(), gone.end(), tid) == gone.end() && t.status == Thread::Status::Running)
                unwind(vm, tid, t, gone);
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
