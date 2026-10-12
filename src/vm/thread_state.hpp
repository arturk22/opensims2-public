#pragma once

// The part of a resumed object the game's records cannot hold: the VM's own
// frame state of a routing instruction (the route's trees, the failure on
// its way to the post-route tree, ...) and the route it walks (its way, the
// locomotion phase), so that a lot saved with a Sim walking goes on exactly
// as the one that saved it; and an engine destination's (Move Out Of Way's
// routing slot, which the binary attaches as a synthetic slot; whether the
// Sims manage it, cTSPerson::AddManagedDestinationObject, so that it goes
// once no queue has it).
//
// The game keeps no way either: its route (cTSRoute in the routing record
// 0xBC66BAEC) holds its goals and plans again when the lot loads. The VM
// writes this in a record of its own (type kThreadStateType, group
// 0xFFFFFFFF, instance = the object ID) next to the game's, which stay as
// the game writes them; the game ignores the record. A lot without it (a
// game's save, or a state that no longer fits the stack it was written for)
// goes on as the game does: resumeRoutes (vm.hpp) plans the way again.

#include "vm.hpp"

#include "formats/lot_objects.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace os2::vm {

constexpr uint32_t kThreadStateType = kVmThreadStateType; // "OS2V"

// The record for the object (its Main thread, its destination part), or
// nothing when there is nothing the stack and the game's records do not
// already say (no frame with routing state, no route, not a destination).
std::optional<std::vector<uint8_t>> writeThreadState(VM& vm, const Object& o);

// Applies a record to the object: the destination part always, the thread
// part to its resumed Main thread when it was written for this stack (the
// same trees and nodes); false when that does not fit (or there is none).
bool applyThreadState(VM& vm, Object& o, std::span<const uint8_t> bytes);

// The lot's part (instance 0 of the record type): the path planner's
// noise, so that the next plans come out as the saving VM's would, and the
// VM's tick count (the routes' last checks are ticks).
std::vector<uint8_t> writeLotState(VM& vm);
void applyLotState(VM& vm, std::span<const uint8_t> bytes);

} // namespace os2::vm
