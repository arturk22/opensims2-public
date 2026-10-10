#pragma once

#include "model.hpp"

#include <filesystem>
#include <array>
#include <map>
#include <vector>
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

    // Roof patterns (catalog type 0xACA8EA06): texture names (TXTR without
    // "_txtr") of the roof's parts; the roof itself has no TXMT.
    struct Roof {
        std::string top, topBump, edges, trim, under;
    };
    const Roof* roof(uint32_t guid) const;

    // Fences (catalog type 0x2CB230B8): CRES names (without "_cres") of the
    // rail (one per tile), the diagonal rail and the post.
    struct Fence {
        std::string rail, diagonalRail, post;
    };
    const Fence* fence(uint32_t guid) const;

private:
    std::map<std::string, std::string> floors_, walls_; // lowercase pattern name -> material
    std::map<uint32_t, std::string> byGuid_;
    std::map<uint32_t, Roof> roofs_;
    std::map<uint32_t, Fence> fences_;

    std::string material(const std::map<std::string, std::string>& scripts, const std::string& pattern) const;
};

// Renderable terrain, floors, walls, roofs and water of a lot, in lot space
// (tiles, Z up, origin at the lot's corner). Materials are the game's TXMTs;
// the terrain, roofs and water get materials built here (the game draws them
// with its own shaders), with each terrain paint blended over the base by its
// mask. Fences (see lotFencePieces), stairs and objects are not included.
//
// Walls are drawn as in the game's wall modes: up, cutaway (walls with a room
// behind them as seen along `viewDirection`, a horizontal vector from the
// camera into the scene, are cut down) or down (all walls cut down); the roof
// shows only with walls up.
struct LotViewOptions {
    enum class Walls { Up, Cutaway, Down };
    Walls walls = Walls::Up;
    std::array<float, 2> viewDirection{1, 0};
};
ObjectModel buildLotModel(const Lot& lot, ResourceIndex& index, const PatternCatalog& patterns,
                          const LotViewOptions& view = {});

// One rail or post of a fence: the CRES (without "_cres") and its placement
// in lot space, a column-major 4x4 matrix (rails are sheared to follow the
// ground between their ends).
struct LotFencePiece {
    std::string model;
    std::array<float, 16> transform;
};
std::vector<LotFencePiece> lotFencePieces(const Lot& lot, const PatternCatalog& patterns,
                                          std::vector<std::string>* warnings = nullptr);

} // namespace os2
