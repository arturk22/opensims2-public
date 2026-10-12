#pragma once

// "Go Here": the player clicks the ground (no object under the mouse) and
// the selected Sim's pie menu opens on a destination object put there, as
// the Mac binary's cTSUserToolStandardAction::MouseEvent and GoHere do
// (implemented in routing.cpp):
// - the destination's GUID by the place (NewDestinationObjectID, the
//   object folder's Get*GoHere): a pool tile takes "Destination - For Swim
//   Here" (0xECFEDB37), the rest "Destination - For Go Here & Shoo" (0x7C4;
//   the pond and beach ones are expansion objects);
// - the point moved 0.4 tile away from other objects
//   (TSObjectMoveAwayFromOtherObjects) and 0.45 tile (+0.001) away from
//   the walls of its tile (MovePointAwayFromWalls: the four sides; the
//   diagonal walls are not done here);
// - the object goes there if it can (TSObjectCanSetPosition; else it is
//   deleted and nothing opens), its pie menu opens for the selected Sim
//   (PickObject) and the Sim manages it (cTSPerson::AddManagedDestinationObject):
//   it is flushed once no Sim has it in its queue
//   (cTSRoutingObjectAggregate::FlushManagedDestinationObjects).
// The destination's interactions ("Vir Aqui", "Correr Aqui", "Vir
// Saltitando Aqui", ...: its TTAB, with their own guards) route the Sim to
// it.

#include "vm.hpp"

#include <optional>

namespace os2::vm {

constexpr uint32_t kGoHereGuid = 0x000007C4;
constexpr uint32_t kSwimHereGuid = 0xECFEDB37;

// Where a destination clicked at (x, y) on `level` goes (the moves above),
// or nothing when it cannot stand there (out of the lot, no floor, inside
// an object).
std::optional<Position> goHerePoint(VM& vm, ObjectId sim, float x, float y, int level);

// Makes the destination at (x, y) on `level` for `sim` (created, its Init
// run, placed) and returns it; 0 when it cannot go there. It is held (not
// flushed) until releaseDestination, so that it outlives the pie menu
// opened on it.
ObjectId createGoHereDestination(VM& vm, ObjectId sim, float x, float y, int level);

// The pie menu on a destination closed: it is flushed now unless a Sim has
// queued one of its interactions (then once none has it any more).
void releaseDestination(VM& vm, ObjectId destination);

// Removes the engine's destinations (move out of the way, go here) that no
// Sim has in its queue and that are not held.
void flushDestinations(VM& vm);

} // namespace os2::vm
