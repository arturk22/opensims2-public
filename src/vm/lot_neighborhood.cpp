// The neighborhood of a saved lot in the VM: lot_neighborhood.hpp.

#include "lot_neighborhood.hpp"
#include "formats/log_once.hpp"

#include "formats/neighborhood.hpp"
#include "hood_bridge.hpp"

#include <exception>

namespace os2::vm {

bool loadLotNeighborhood(VM& vm, const std::filesystem::path& lotPackage) {
    const auto hoodPackage = neighborhoodPackageOf(lotPackage);
    if (hoodPackage.empty())
        return false;
    try {
        const hood::Neighborhood hood = hood::Neighborhood::load(hoodPackage, true);
        loadNeighborhood(vm, hood);
        enterLot(vm, lotIdOf(lotPackage), hood.info ? int16_t(hood.info->id) : int16_t(0));
    } catch (const std::exception& err) {
        os2::logOnce("lot's neighborhood", hoodPackage.filename().string(), err);
        return false;
    }
    return true;
}

} // namespace os2::vm
