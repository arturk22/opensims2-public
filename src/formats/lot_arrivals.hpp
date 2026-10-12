#pragma once

// A family that has just moved in (formats/hood_moves.hpp) has no Sims in
// the lot file yet: the game puts them on the lot when it loads it for the
// family (cTSFamilyManager::MoveIn raises the family's "new lot" flag and the
// lot load brings the family's members in). Here: every member of the family
// living on the lot (FAMI) whose Sim is not among the lot's saved objects
// gets a fresh object state (a PERS of the module's version, a new object ID,
// a save type for its character's GUID), standing by the lot's mailbox (else
// at the middle of the lot's front), so the VM creates it like any saved Sim
// and the next save writes it into the lot file — only for a lot drawn
// without the VM: with the VM the game's own "reset lot" broadcast makes
// them ("Sub - Generate Family", vm/lot.hpp runResetLot). A family moving in pays the lot's price and its
// first day starts at 8:00, the global FCNS 4 "new family start hour" (cTSLoadLotController::LoadLot steps 0x1B and
// 0x1F). Plain C++.

#include "lot.hpp"
#include "lot_objects.hpp"

#include <filesystem>
#include <vector>

namespace os2 {

class ResourceIndex;

struct LotArrivals {
    // One scene-graph-like entry per arriving Sim, and per saved Sim that has
    // no cPerson in the lot (one that arrived and was saved by us), for
    // drawing: definition = its character file's stem, position in tiles, Z
    // the floor's.
    std::vector<Lot::Object> sims;
    std::vector<std::filesystem::path> characters; // their character files
    // The character files of the family's members who arrive (not on the
    // lot yet), whether or not this placed them.
    std::vector<std::filesystem::path> arriving;
    // The move-in charge (cTSLoadLotController::LoadLot step 0x1B: the lot's
    // price, GetLotFileInfo, unless the lot is of type 2), due when the
    // family's Sims arrive: the caller takes it from `family`'s funds.
    uint32_t family = 0;
    int64_t charge = 0;
    // The family's first day was set up (SetupFamilyFirstDay): the clock at
    // the new family start hour (8:00, FCNS) and the lot running.
    bool firstDay = false;
    // The lot's family had been evicted (lot info flag 8): what went
    // (formats/lot_evict.hpp).
    size_t evictedSims = 0, evictedObjects = 0;
};

// `lotFile` is <neighborhood>/Lots/<P>_Lot<N>.package; `world` the lot read
// from it.
// `index` (the game's resources) is for the eviction cleanup and the Sims'
// cPerson records; without it they are skipped.
// `placeSims` false: the arriving Sims are not added (a VM run makes them
// with the game's "reset lot" broadcast, vm/lot.hpp runResetLot); the rest
// (charge, first day, eviction) is the same.
LotArrivals addArrivingSims(LotObjects& saved, const Lot& world, const std::filesystem::path& lotFile,
                            ResourceIndex* index = nullptr, bool placeSims = true);

} // namespace os2
