#pragma once

#include "lot_lightmap.hpp"
#include "model.hpp"

#include <filesystem>
#include <limits>
#include <optional>
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
// Walls are drawn as in the game's wall modes (docs/ui-live-mac.md 8c): up,
// cutaway (every inside wall of the level cut down, outside walls only when
// their outside face looks at the camera, seen along `viewDirection`, a
// horizontal vector from the camera into the scene; walls alone in the yard
// stay up) or down (all walls cut down). Only partition styles that allow
// cutaway are cut (normal and attic walls; not fences, foundations, pool
// sides or deck skirts).
//
// `level` is the level the camera shows (the lot's numbering, 0 = the
// ground): levels above it are not drawn, the walls of lower levels stay up
// and the wall mode applies to its own walls (kAllLevels: no level hidden,
// the mode applies to every level). The roof shows with walls up and `roof`
// on (the game's wall mode 3, walls up with the roof) and no level hidden (the levels above `level` have no walls nor floors).
struct LotViewOptions {
    enum class Walls { Up, Cutaway, Down };
    static constexpr int kAllLevels = std::numeric_limits<int>::max();
    Walls walls = Walls::Up;
    std::array<float, 2> viewDirection{1, 0};
    int level = kAllLevels;
    // The wall mode also cuts the walls of the levels from this one up to
    // `level` (a yard under a house on foundations is shown with its floor:
    // app/wall_view.hpp); kAllLevels: only `level`'s.
    int lowestCut = kAllLevels;
    bool roof = true;
};
// The highest level with walls (not attic walls: the gables go with the
// roof over the level below) or floors (lowest level if none).
int lotTopLevel(const Lot& lot);
//
// Walls, floors, wall tops and the terrain get light-map coordinates
// (MeshGroup::uvs2) into the atlas that `lightMap`, when given, describes
// (see lot_lightmap.hpp). The atlas does not depend on the view: walls cut
// down or not drawn keep their full-height cells, so that light maps made for
// one view serve all.
//
// `cache`, when given, keeps what does not change with the view between
// calls (the doors' and windows' holes, the TXMTs, the textures' mean
// colours): a rebuild for a new view then only makes the geometry. Call
// LotModelCache::geometryChanged after the lot itself changed.
struct LotModelCache {
    std::optional<std::vector<std::vector<std::array<float, 3>>>> holes; // per door or window: its points
    std::map<std::string, MaterialDefinition> materials;                // parsed TXMTs by name
    std::map<std::string, std::optional<std::array<float, 3>>> meanColors; // by material name
    // Out: the walls the last build cut down (x0, y0, x1, y1, level), for
    // the host to hide what hangs on them (windows, doors, pictures).
    std::vector<std::array<float, 5>> cut;
    void geometryChanged() { holes.reset(); }
};
ObjectModel buildLotModel(const Lot& lot, ResourceIndex& index, const PatternCatalog& patterns,
                          const LotViewOptions& view = {}, LotLightMapLayout* lightMap = nullptr,
                          LotModelCache* cache = nullptr);

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
