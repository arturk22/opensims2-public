// Removing objects as the Mac binary's object module does: Remove Object
// Instance puts the object on a kill queue (cEdithObjectModule::
// AddToKillQueue) and it dies after the simulator tick (FlushKillQueue ->
// KillObject -> KillOutOfWorldObject), so the trees that are running its
// code finish their turn first.

#pragma once

#include "vm.hpp"

namespace os2::vm {

// AddToKillQueue (Mac BV 0x52e9dc): marks the object dying with the parts
// of its multi-tile object and, recursively, what is in its slots except
// Sims (ProcessObjectForKillQueue: OBJD type 2 is skipped), and queues it.
// False (nothing done) for a missing object, one already dying or one
// inside a dying container.
bool queueKill(VM& vm, ObjectId id);

// True while a removed object waits for the end of the tick.
bool dying(const VM& vm, ObjectId id);

// FlushKillQueue, at the end of VM::tick: each queued object that is still
// dying dies with its parts and its contents (KillOutOfWorldObject: Sims in
// its slots are dropped, other objects in them die too). Before it goes,
// every thread is cleaned of it (cEdithObjectModule::CleanupPeople ->
// cTSObject::Cleanup(object)): waiting interactions with it leave the
// queues, and a thread running its code unwinds: when an interaction's
// action tree is below those frames the interaction ends there (its Idle
// for Input runs again); otherwise the thread restarts its Main, as FreeSO
// resets a Sim whose interaction object died (the exact unwinding of
// cTSObject::Cleanup is inferred). Relationships of the object go too.
void flushKillQueue(VM& vm);

} // namespace os2::vm
