#pragma once

// Placing, moving and binning lots in the neighborhood, as the Mac binary's
// lot tools do (docs/neighborhood-mac.md §11.4, §12):
//
// - The Lots & Houses bin (cTSLotCatalog, "LotCatalog/cx_<8 digits>.package"):
//   each entry is a lot file whose base lot info (cTSBaseLotInfo, resource
//   0x6C589723) gives the size, the road edges, the creation front edge and
//   the name. The player's bin is <user dir>/LotCatalog, filled from the
//   game's on first use.
// - Empty lots (cTSUserToolPlaceNewLot): the Lots & Houses panel's buttons
//   0xD001-0xD00A pick sizes 0-9 of the binary's tables (BV 0x2733010 /
//   0x2732fc0); the rectangle is that size plus a road strip on the side the
//   rotation names (UpdateLotRect, BV 0x2c6830: 0 x-min, 1 y-max, 2 x-max,
//   3 y-min).
// - Validity (cTSUserToolPlaceLotBase::IsOKToPlaceLotCommon, BV 0xa29e5c):
//   checkPlacement. A new lot's front is its first edge with a road; a lot
//   from the bin or moved must find the roads on its own road edges turned
//   by (front - creation front).
// - Placing or moving a lot removes the flora and props over it whose
//   catalog entry says removeonlotplop (FUN_00a284cc).
// - Writes go to the neighborhood folder (the player's copy), each file to a
//   temporary name renamed over the old one, as formats/hood_moves.hpp.
//
// Not done (inferred or skipped): TryModifyingTerrainToConformToLot and the
// smoothing under a new lot (the view flattens the terrain under every lot
// when it loads, formats/hood_lot_terrain.hpp), the lot name dialog (cTSWinProcPlaceLotDialog:
// new lots are residential and keep their base name), beach lots (flag
// 0x80), and moving an occupied lot to the bin (its family goes with it in
// the game). Plain C++.

#include "hood_terrain.hpp"
#include "neighborhood.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace os2 {
class ResourceIndex;
}

namespace os2::hood {

// A lot rectangle in neighborhood tiles: [x0, x1) x [y0, y1); its vertices
// run x0..x1, y0..y1.
struct TileRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int depth() const { return y1 - y0; }
};

// The lot's rectangle now (cTSLotInfo position and GetCurrentSize).
TileRect lotRect(const LotInfo& info);

// Empty lot sizes (cTSUserToolPlaceNewLot::Init's tables), without the road:
// button 0xD001 + i picks size i.
struct EmptyLotSize {
    int width = 0, depth = 0;
};
constexpr int kEmptyLotSizes = 10;
EmptyLotSize emptyLotSize(int index);
// The rectangle of an empty lot of `size` with its corner tile at (x, y),
// turned `rotation` (0-3): with the road strip.
TileRect emptyLotRect(EmptyLotSize size, int x, int y, int rotation);

// What a placement check found.
struct Placement {
    bool ok = false;
    std::string why;                    // the first rule that failed
    std::array<bool, 4> roads{};        // edges lined with road (this+0x68)
    int front = -1;                     // a new lot's front: its first road edge
    float roadHeight = 0;               // the front road's height (the lot's elevation)
};

// Neighborhood object catalog flags (catcanhobjects): may stay in a lot, is
// removed when a lot is placed.
struct OccupantFlags {
    bool allowedInLot = false, removeOnLotPlop = false;
};

// The neighborhood a placement is checked against.
struct PlacementWorld {
    const Neighborhood* hood = nullptr;
    const Terrain* terrain = nullptr;
    const Occupants* occupants = nullptr;
    std::map<uint32_t, OccupantFlags> objects; // by GUID
    float maxSlope = 0.5f;                     // DAT_0277a89c
};

// IsOKToPlaceLotCommon for `rect`, leaving out lot `self` (being moved; 0: none).
Placement checkPlacement(const PlacementWorld& world, const TileRect& rect, uint32_t self = 0);
// cTSUserToolMoveLot/PlaceLotFromInventory::IsOKToPlaceLot's road match:
// the base lot info's road edge i must be where the place has a road edge
// (i + front - creation front) mod 4.
bool roadsMatch(const LotInfo& base, int front, const std::array<bool, 4>& placeRoads);

// The neighborhood object catalog's flags (index: the game's resources).
std::map<uint32_t, OccupantFlags> loadOccupantFlags(ResourceIndex& index);

// One lot of the bin.
struct BinLot {
    std::filesystem::path file;
    uint32_t catalogId = 0; // cx_<id>
    LotInfo base;           // width/depth/roads/creation front/name
    int64_t price = 0;      // GetLotFileInfo of the lot file
};
// The player's bin folder for a neighborhood folder (<user>/Neighborhoods/<P>
// -> <user>/LotCatalog; empty if the folder is not in Neighborhoods), filled
// from `gameCatalog` the first time.
std::filesystem::path playerLotCatalog(const std::filesystem::path& hoodFolder,
                                       const std::filesystem::path& gameCatalog);
std::vector<BinLot> loadLotBin(const std::filesystem::path& catalogDir);

// cTSNeighborhood::AcquireNextAvailableLotID: the first ID from the next one
// after the largest whose lot file does not exist.
uint32_t nextLotId(const Neighborhood& hood);

struct LotResult {
    bool ok = false;
    std::string error;
    uint32_t lot = 0;
    std::vector<std::string> lines;
};

// cTSUserToolPlaceNewLot::PlaceLot: a new residential lot info (no lot file
// yet: its price is the world value). `forbidden` is the game's data.
LotResult placeEmptyLot(const std::filesystem::path& folder, const PlacementWorld& world, EmptyLotSize size, int x,
                        int y, int rotation, const std::filesystem::path& forbidden = {});
// PlaceLotFromInventory: the bin's lot file copied as the new lot's and its
// lot info made from the base, at (x, y) with `front`.
LotResult placeBinLot(const std::filesystem::path& folder, const PlacementWorld& world, const BinLot& bin, int x, int y,
                      int front, const std::filesystem::path& forbidden = {});
// cTSUserToolMoveLot: the lot at (x, y) with `front`.
LotResult moveLot(const std::filesystem::path& folder, const PlacementWorld& world, uint32_t lot, int x, int y,
                  int front, const std::filesystem::path& forbidden = {});
// cTSGameStateController::SaveNeighborhoodLotToCatalog + RemoveNeighborhoodLot:
// an unoccupied lot with a house goes to the bin (as cx_<next id>) and leaves
// the neighborhood.
LotResult moveLotToBin(const std::filesystem::path& folder, uint32_t lot, const std::filesystem::path& catalogDir,
                       const std::filesystem::path& forbidden = {});

// The (x, y, front) placements of a lot of base info `base` with its corner
// near (x, y) that pass the checks and the road match, the turn nearest the
// lot's creation first (a convenience: the game turns the lot with the keys).
std::optional<std::pair<int, Placement>> findFront(const PlacementWorld& world, const LotInfo& base, int x, int y,
                                                    uint32_t self = 0, int preferred = -1);

} // namespace os2::hood
