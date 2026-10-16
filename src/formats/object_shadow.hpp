#pragma once

// An object's ground and wall shadow planes, the "GUOB" subsets of its model
// that nTSSG::cShadow turns on and off (docs/lighting-mac.md 12): the ground
// plane always, "wallshadow" while the walls are not down, and the four side
// planes where walls stand along the sides of the object's bounds, the walls
// up. Plain C++: the app asks it whenever the game's shadow would (the object
// moved, a wall beside it changed, the wall mode or level changed).

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string_view>
#include <tuple>

namespace os2 {

struct Lot;
struct ObjectModel;
struct Xform;

// nTSSG::kGUOBSubsetNames (Mac BV 0x277a8c0, base 1.0 0x15d6210), in order.
enum GuobIndex : int { kGuobGround = 0, kGuobWall, kGuobNorth, kGuobSouth, kGuobEast, kGuobWest, kGuobCount };
constexpr uint8_t kAllGuobs = (1u << kGuobCount) - 1;
// The GUOB a subset name is (case-insensitive), -1 if none.
int guobIndex(std::string_view subset);
const char* guobSubsetName(int index);

// What the rules read of a model, in model space.
struct GuobModelInfo {
    // Bit i: the model has GUOB subset i bound to a material other than
    // "GUOBOff" (FindGUOBMaterialFromSubset: a GUOBOff saved with the lot is
    // no material, and that subset is never turned on).
    uint8_t present = 0;
    // The whole model's box as GrowGlobalBBox(box, -1, object) grows it:
    // every shape's bound (ObjectModel::bounds: the GMDC's bounding mesh, the
    // body without the shadow planes; without one, every vertex of the shape,
    // the shadow planes too). hasBounds false: nothing to grow.
    bool hasBounds = false;
    std::array<float, 3> lo{}, hi{};
};
GuobModelInfo guobModelInfo(const ObjectModel& model);

// The axis-aligned world box of a model-space box placed at `at`.
void guobWorldBox(const Xform& at, const std::array<float, 3>& lo, const std::array<float, 3>& hi,
                  std::array<float, 3>& worldLo, std::array<float, 3>& worldHi);

// What UpdateGUOBs asks of the world.
struct GuobWorld {
    // cITSMainToolbar's wall mode: 0 down, 1 cutaway, 2 up, 3 up with the roof.
    int wallMode = 1;
    // A wall or fence on the tile edge from (x0, y0) to (x1, y1) (one tile
    // long, either way) on `level` (the lot's numbering): the partition
    // manager's WallExists, the graph of walls and fences.
    std::function<bool(int x0, int y0, int x1, int y1, int level)> wallExists;
};

// The tile edges a lot's partitions stand on (Lot::walls: walls, fences,
// pool and foundation walls, deck skirts; the game's graph of walls and
// fences), for GuobWorld::wallExists. Axis-aligned partitions only: a
// diagonal wall lies on no tile edge.
class LotWallEdges {
public:
    explicit LotWallEdges(const Lot& lot);
    bool operator()(int x0, int y0, int x1, int y1, int level) const;
    // The two tiles beside each edge (cShadowManager::UpdateShadowsAdjacentToWall):
    // (x, y - 1) and (x, y) for an edge along x from (x, y), (x - 1, y) and
    // (x, y) for one along y; as level, x, y.
    std::set<std::tuple<int, int, int>> tilesBeside() const;
    // The tiles beside the edges that are in one and not the other.
    static std::set<std::tuple<int, int, int>> tilesBesideChanges(const LotWallEdges& a, const LotWallEdges& b);
    // The tiles beside the edges on `level` (UpdateLevelShadows).
    std::set<std::tuple<int, int, int>> tilesBeside(int level) const;
    bool operator==(const LotWallEdges& o) const { return edges_ == o.edges_; }

private:
    // level, x, y of the edge's low end, and whether it runs along x.
    std::set<std::tuple<int, int, int, bool>> edges_;
};

// The tile rectangle round a world box (base 1.0 FUN_00a8e9ac): x0 = floor
// of the low x, x1 = ceil of the high x, y likewise.
struct GuobTileRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
GuobTileRect guobTileRect(const std::array<float, 3>& worldLo, const std::array<float, 3>& worldHi);

// The side planes the walls allow (bits kGuobNorth..kGuobWest), base 1.0
// FUN_00a8e9ac: none unless the object's level is shown and the wall mode
// is 2; else each side of the tile rectangle with a wall on every tile edge
// of it, named by the object's direction (object data 1).
uint8_t guobWallSides(const GuobTileRect& rect, int direction, int level, bool levelShown, const GuobWorld& world);

// cShadow::UpdateGUOBs (base 1.0 0xa8e138, BV 0x678488) for an object whose
// shadow style is the GUOB one: which of `present` are on. `visible` is the
// shadow's visibility (cShadow +0xc): none when it is hidden.
uint8_t guobsOn(uint8_t present, bool visible, uint8_t wallSides, const GuobWorld& world);

} // namespace os2
