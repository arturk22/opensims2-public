#pragma once

#include "vm.hpp"

#include "formats/lot_objects.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace os2 {
class DbpfPackage;
struct Lot;
} // namespace os2

namespace os2::vm {

// The records of a lot save outside the object module (docs/save-mac.md
// §5-§6), put in `lot.otherRecords` (LotObjects::save writes them over the
// source's):
// - the scene graph object (cObject 0xFA1C39F7, instance = object ID) of
//   each object that moved, turned or changed level during play: its two
//   copies of the position and rotation (position in tiles, Z up: the
//   floor's elevation plus the object's; rotation about Z, angle 90° - 45°
//   × direction), the rest kept; and of each object created during play, a
//   copy of a cObject of the same definition from another lot
//   (`templatePackages`), moved there;
//   A Sim's record also gets the Animate Sim it plays in its animation
//   queue (formats/scene_anim.hpp), as the game's cAnimatable saves it;
// - the routing state (0xBC66BAEC, instance = object ID) of each Sim
//   created during play: an idle one (no route, as the game writes for a
//   standing Sim);
// - the objects' relationship tables and the lot's SRELs (0xCC364C2A,
//   instance = object ID << 16 | key; docs/relationships-mac.md §1-§2):
//   each object's table (in its XOBJ) lists the keys of its VM entries, and
//   each changed or new entry is written as cTSRelationshipEntry does;
//   records of entries the tables lost are deleted (Sim-to-Sim ones belong
//   to the neighborhood). Values the VM cut to 16 bits keep their saved 32
//   bits while the cut agrees.
// The cTSDataTables records (0x3053CF74) the game deletes when an object
// has none: no lot of the game has one and the VM keeps none, so none is
// written.
// `before` is the lot as read, `lot` the one captureLot made from the VM.
struct RecordsResult {
    size_t sceneMoved = 0, sceneNew = 0, routing = 0, relationshipsChanged = 0, relationshipsNew = 0,
           relationshipsGone = 0;
    size_t people = 0; // of sceneNew: cPerson records made for Sims that had none
    size_t animations = 0; // records whose animation queue was rewritten (what the object plays)
    size_t threadStates = 0; // the VM's own thread states written (thread_state.hpp)
    std::vector<std::string> warnings;
};
RecordsResult captureLotRecords(VM& vm, const Lot& world, DbpfPackage& source, const LotObjects& before,
                                LotObjects& lot, const std::vector<std::filesystem::path>& templatePackages);

} // namespace os2::vm
