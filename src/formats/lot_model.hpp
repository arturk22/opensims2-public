#pragma once

#include "model.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace os2 {

struct Lot;
class ResourceIndex;

// Floor and wall pattern names -> material (TXMT name without "_txmt"):
// named patterns come from the build catalog scripts
// (Catalog/Scripts/floorPatterns.txt, wallPatterns.txt: floorPattern "sidewalk"
// -> material "floor_sidewalk"), catalog patterns are GUIDs looked up in the
// catalog's property sets (catpatterns.bundle.package). Anything else is
// taken as the material name itself.
class PatternCatalog {
public:
    // `resRoot` is TSData/Res; `index` covers it.
    PatternCatalog(ResourceIndex& index, const std::filesystem::path& resRoot);

    std::string floorMaterial(const std::string& pattern) const { return material(floors_, pattern); }
    std::string wallMaterial(const std::string& pattern) const { return material(walls_, pattern); }

    size_t catalogSize() const { return byGuid_.size(); }

private:
    std::map<std::string, std::string> floors_, walls_; // lowercase pattern name -> material
    std::map<uint32_t, std::string> byGuid_;

    std::string material(const std::map<std::string, std::string>& scripts, const std::string& pattern) const;
};

// Renderable terrain, floors and walls of a lot, in lot space (tiles, Z up,
// origin at the lot's corner). Materials are the game's TXMTs; the terrain
// gets StandardMaterials built here, with each paint blended over the base by
// its mask. Fences, roofs, stairs and objects are not included.
ObjectModel buildLotModel(const Lot& lot, ResourceIndex& index, const PatternCatalog& patterns);

} // namespace os2
