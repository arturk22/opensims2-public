#pragma once

// The neighborhood of a saved lot, for the hosts that run a lot (the app's
// LotSimulation, s2pkg autonomy and pie), as lot-run does: before loading
// the lot, the neighborhood package next to it goes into the VM
// (loadNeighborhood: neighbors, relationships, families, ...) and the lot
// is entered (enterLot: current house/family globals); after loading it,
// the lot's Sims get their neighbors' person data (bindLotSims), before
// their saved state is restored. Without it, the trees that read a
// neighbor (data owner 0x18, "Soc&Rel: Adjust STR From A->B w/ nIDs" in
// every social) fail with "Missing neighbor for data access".

#include "vm.hpp"

#include <filesystem>

namespace os2::vm {

// Loads the lot's neighborhood and enters the lot; false if the lot has no
// neighborhood package next to it (or it does not load).
bool loadLotNeighborhood(VM& vm, const std::filesystem::path& lotPackage);

} // namespace os2::vm
