#pragma once

// Writing back to the neighborhood package what a lot save changes there
// (docs/save-mac.md §6 "Vizinhança"), from the VM that ran the lot:
// - FAMI: the funds of every family (those of the lot's family are the
//   simulator's, globals 13/14, as cTSFamily::Funds gives them);
// - SDSC: the persistent person data (cTSNeighbor::sGetPersistentDataFields,
//   the fields of SDSC version <= 0x20) of each Sim on the lot, from its
//   person data, and of the other neighbors the fields the VM changed; and
//   the list of neighbors each has a relationship with;
// - SREL: the Sim-to-Sim relationships the VM changed or added (values the
//   VM cut to 16 bits keep their saved 32 bits while the cut agrees);
// - NGBH: the inventories (global, lot, family, neighbor) when the VM's
//   differ from the package's;
// - SWAF: the wants, fears and achievement history of the Sims whose want
//   state changed (wants.hpp);
// - Sims born in play (Make New Character): SDSC, SDNA, FAMI member, family
//   tree.
// Everything else of the package is copied as it is.

#include "vm.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace os2::hood {
struct Neighborhood;
}

namespace os2::vm {

struct HoodSaveResult {
    size_t families = 0, descriptions = 0, relationshipsChanged = 0, relationshipsNew = 0;
    bool inventories = false;
    size_t wants = 0; // SWAF records written (wants.hpp)
    size_t born = 0;  // Sims born in play written (SDSC, SDNA, FAMI member, family tree)
    size_t clothing = 0; // family clothing bin entries added (BINX + 3IDR: the clothing rack's purchases)
    std::vector<std::string> warnings;
};

// The SDSC person data the game keeps for a neighbor (base game).
const std::vector<size_t>& persistentPersonData();

// `hood` is the neighborhood as loaded into the VM from `source` (the
// package); `out` must differ from `source`.
HoodSaveResult saveNeighborhood(VM& vm, const hood::Neighborhood& hood, const std::filesystem::path& source,
                                const std::filesystem::path& out);

// Differences in what the neighborhood keeps between two VMs: the
// families' funds, the Sim-to-Sim relationships, the inventories and the
// neighbors' persistent person data (a Sim's own while it is on the lot).
std::vector<std::string> compareNeighborhoodState(VM& a, VM& b, size_t limit = 50);

} // namespace os2::vm
