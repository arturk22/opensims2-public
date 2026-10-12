// Linked interactions (interactions.hpp: newInteractionId, linkInteractions,
// unlinkInteraction), as the Mac binary's cTSInteraction::UnLinkInteractions
// works (docs/primitives-mac.md, "Sociais esperando o outro Sim").

#include "interactions.hpp"

#include <algorithm>

namespace os2::vm {

namespace {

constexpr size_t kPersondataInteractionsLinkedDeleted = 110;

// Every interaction of every Sim: the queue (the running one too), the
// sub-queue and the running sub-action.
template <typename F> void forEachInteraction(VM& vm, F&& f) {
    std::vector<ObjectId> ids;
    for (const auto& [id, o] : vm.eachObject())
        if (o.person)
            ids.push_back(id);
    for (ObjectId id : ids) {
        Object* sim = vm.object(id);
        if (!sim)
            continue;
        for (auto& q : sim->queue)
            if (f(*sim, q))
                return;
        for (auto& q : sim->subQueue)
            if (f(*sim, q))
                return;
        if (sim->subAction && f(*sim, *sim->subAction))
            return;
    }
}

} // namespace

uint16_t newInteractionId(VM& vm, Object&) {
    // cTSInteraction::AssignUniqueGUID (BV 0x4c5b20) asks the simulator
    // (BV 0x4989e0) for the next ID: its global 36 "Current Interaction
    // GUID", a u16 that skips 0, so it is saved with the globals and the
    // IDs go on where they were after a load.
    auto& counter = vm.globals[global::InteractionGuid];
    if (counter == 0)
        counter = 1;
    const uint16_t id = uint16_t(counter);
    counter = int16_t(uint16_t(id + 1));
    return id;
}

void linkInteractions(VM& vm, QueuedInteraction& made, uint16_t other) {
    // TryPushAction's flag 0x40 (BV 0xe33f..): the linked interaction is
    // looked up by its ID (cTSInteraction::FindInteraction); when found, the
    // new one takes its link and it links to the new one
    // (new.SetLinkedGUID(linked.GetLinkedGUID()),
    // linked.SetLinkedGUID(new.AssignUniqueGUID())): a chain from the linked
    // one through every interaction linked to it since, newest first. A
    // three-Sim social ("Social - Baby - Talk To": A, then B and the baby's
    // holder C, both linked to A) is A -> C -> B, so A ending reaches both;
    // before, B and A linked to each other and the holder was never told
    // (N001_Lot36's Brandi held the baby for 11 hours).
    forEachInteraction(vm, [&](Object&, QueuedInteraction& q) {
        if (q.uid != other)
            return false;
        made.link = q.link;
        q.link = made.uid;
        return true;
    });
}

void unlinkInteraction(VM& vm, ObjectId owner, const QueuedInteraction& ended) {
    // cTSInteraction::UnLinkInteractions: follow the chain of links; another
    // Sim's interaction with the linked ID that is running (or its running
    // sub-action) gets person data 110 "Interactions Linked Deleted" := 1,
    // one that waits in a queue is taken out of it.
    uint16_t link = ended.link;
    for (int guard = 0; link && link != ended.uid && guard < 16; ++guard) {
        uint16_t nextLink = 0;
        bool found = false;
        ObjectId removeFrom = 0;
        uint16_t removeUid = 0;
        forEachInteraction(vm, [&](Object& sim, QueuedInteraction& q) {
            if (sim.id == owner || q.uid != link)
                return false;
            found = true;
            nextLink = q.link;
            q.link = 0;
            const bool running = (sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < sim.queue.size() &&
                                  &sim.queue[size_t(sim.activeInteraction)] == &q) ||
                                 (sim.subAction && &*sim.subAction == &q);
            if (running)
                sim.person->data[kPersondataInteractionsLinkedDeleted] = 1;
            else
                removeFrom = sim.id, removeUid = q.uid;
            return true;
        });
        if (!found)
            break;
        if (removeFrom) {
            Object* sim = vm.object(removeFrom);
            std::erase_if(sim->subQueue, [&](const QueuedInteraction& x) { return x.uid == removeUid; });
            auto& q = sim->queue;
            for (size_t i = q.size(); i-- > 0;)
                if (q[i].uid == removeUid && int(i) > sim->activeInteraction)
                    q.erase(q.begin() + std::ptrdiff_t(i));
            syncQueueData(*sim);
        }
        link = nextLink;
    }
}

} // namespace os2::vm
