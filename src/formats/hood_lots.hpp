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
// - A lot from the bin or moved conforms the terrain to its own ground
//   (TryModifyingTerrainToConformToLot); a new empty lot smooths the
//   terrain under it (cTSNHoodTerrain::Smoothen) (docs/neighborhood-mac.md §13).
//
// - A new empty lot flattens the roads along its edges first
//   (FlattenAbuttingRoadsIfPossible, docs/neighborhood-mac.md §15.2).
//
// Not done: the beach shaping of a new beach lot (FUN_00a2afec; the base
// game has no beach empty lots). Plain C++.

#include "hood_terrain.hpp"
#include "neighborhood.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
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
    std::array<float, 4> edgeHeight{};  // each road edge's height (this+0x6c)
    // The road height at edge `front` (0 when that edge has none, as
    // FUN_00a265cc).
    float heightAt(int front) const { return roads[size_t(front & 3)] ? edgeHeight[size_t(front & 3)] : 0.0f; }
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
    float maxSlope = 0.5f;                     // DAT_0277a89c: tan(MaxLotSlope)
    float talusAngle = 2.0f;                   // DAT_0277955c (degrees)
    int smoothingPasses = 8;                   // DAT_02779558
};

// The lot tools' tunables from Neighborhood.ini (TerrainSettings): the
// slope limit is tan(MaxLotSlope x 3.14 / 180) as FUN_00a286f8 has it.
void applyTunables(PlacementWorld& world, const TerrainSettings& settings);

// cTSUserToolPlaceLotBase::TryModifyingTerrainToConformToLot (BV 0xa2a918):
// the neighborhood vertices a lot of base info `base` at (x, y) with `front`
// sets to its own vertex heights (turned by front - creation front) plus
// the road height of edge `front`. A vertex off the road strip whose height
// would change by more than 0.01 may not belong to another lot (one that
// overlaps the 3x3 vertex rectangle, other than `self`), and the roads
// around it carry the change to the vertices they keep level
// (GetSameHeightVertices); two heights for one vertex fail it. Fails when
// the base heights are not one per vertex.
struct TerrainEdit {
    bool ok = false;
    std::string why;
    std::map<std::pair<int, int>, float> heights; // (x, y) -> height
};
TerrainEdit conformTerrainToLot(const PlacementWorld& world, const LotInfo& base, uint32_t self, int x, int y,
                                int front, const Placement& placement);
// cTSNHoodTerrain::Smoothen (BV 0x1689538, the interface's vtable 0x88):
// `passes` passes over the rectangle's vertices; between a vertex and each
// of its 8 neighbors in the rectangle whose drop passes tan(talus) x 10
// (x 1.414 diagonally), each side unless locked moves 0.2 of the excess
// towards the other. Locked (FUN_004d359c): a vertex with an occupant (any
// type) or another lot in its 3x3 vertex rectangle. Then FUN_00a28af0 rounds
// every vertex of the rectangle to a multiple of 0.1875. Returns the
// vertices that changed.
size_t smoothTerrainUnderLot(Terrain& terrain, const PlacementWorld& world, const TileRect& rect, uint32_t self = 0);

// cTSUserToolPlaceLotBase::FlattenAbuttingRoadsIfPossible (BV 0xa2a598),
// for a new empty lot of rectangle `rect` with `front` (its first road edge)
// and that edge's road height: the roads in each of the four tile strips
// just outside the lot are levelled, all their rectangles' vertices, to one
// height (rounded to 0.1875): `height` when the strip touches the front road
// strip, else the height of the vertices another lot shares with them, else
// the first road's (x0, y0) vertex. A strip whose roads touch lot vertices of
// different heights (by more than 0.01) is left alone.
struct RoadFlatten {
    std::map<std::pair<int, int>, float> heights;          // (x, y) -> height
    std::set<size_t> roads;                                // the roads levelled (index into `roads`)
    std::map<size_t, std::array<float, 4>> roadHeights;    // their corners laid on the new terrain
    std::vector<std::string> lines;
};
RoadFlatten flattenAbuttingRoads(const PlacementWorld& world, const TileRect& rect, int front, float height);

// IsOKToPlaceLotCommon for `rect`, leaving out lot `self` (being moved; 0: none).
// `beach`: the base lot info of a beach lot (flag 0x80): water is allowed
// and IsOKToPlaceBeachLot is checked.
Placement checkPlacement(const PlacementWorld& world, const TileRect& rect, uint32_t self = 0,
                         const LotInfo* beach = nullptr);
// A bin lot or a moved lot (base info `base`) at (x, y) with `front`: the
// placement, the road match and the terrain conforming as a dry run.
Placement checkLot(const PlacementWorld& world, const LotInfo& base, uint32_t self, int x, int y, int front);
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
    std::string familyName; // the family that moved to the bin with the lot (empty: none)
    uint32_t familyId = 0;  // its family number (its FAMI in the lot file)
};
// cTSLotCatalog's remove (cTSWinProcLotCatalog::onBtnClickedID 0xC): the lot
// file leaves the bin.
bool deleteBinLot(const BinLot& lot, const std::filesystem::path& forbidden = {});
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

// cTSUserToolPlaceNewLot: an empty lot of `size` at (x, y) turned `rotation`
// (its road strip on edge `rotation`): checkPlacement, and the strip must
// face a road; the front is the strip's edge. Inferred from the lot
// templates (each has its 10-tile road strip, the car portals and the
// mailbox on the template's front): a lot whose strip faces no road has no
// template that fits it (playtest 33: N001 lot 83, 4x3 with the strip on
// x low and the road on y low, would need a 30x40 template).
Placement checkEmptyLot(const PlacementWorld& world, EmptyLotSize size, int x, int y, int rotation);

// cTSUserToolPlaceNewLot::PlaceLot: a new lot info of `size` (checkEmptyLot),
// then its lot file made from the game's lot template (makeLotFile; the
// game makes it when the lot first loads, cTSLoadLotController::LoadLot
// steps 0xB-0x11, with the same content). `templatesDir` is the game's
// TSData/Res/LotTemplates (empty: no lot file yet, the price is then the
// world value); `forbidden` is the game's data. `name` and `zone` come from
// the place-lot dialog (cTSWinProcPlaceLotDialog: 0 residential, 1 community).
LotResult placeEmptyLot(const std::filesystem::path& folder, const PlacementWorld& world, EmptyLotSize size, int x,
                        int y, int rotation, const std::filesystem::path& forbidden = {}, const std::string& name = {},
                        uint8_t zone = 0, const std::filesystem::path& templatesDir = {});

// The game's lot template for a lot `across` x `along` neighborhood tiles
// (across the road, with the road strip; along the road): TSData/Res/
// LotTemplates/LotTemplate<across x 10>x<along x 10>.package, or
// CommunityLotTemplate... for zone 1 (LoadLot's "%s%dx%d"). The templates
// hold the road strip on their edge 0 (x low, locked), the car and
// pedestrian portals, a mailbox and a trash can (a phone booth on a
// community lot), flat ground of "lottexture-test-01" (every shipped lot's
// and the neighborhood's lot skirt) and the lot value of their size.
// Empty if the game has none of that size.
std::filesystem::path lotTemplate(const std::filesystem::path& templatesDir, uint8_t zone, int across, int along);

// LoadLot's new lot (steps 0xB-0x11, "open the template by size and type,
// turned as the lot is"): the lot file of lot `lot`, which has none, written
// from its template, so the lot loads as an empty, unoccupied lot. The file
// keeps the template's frame, road strip on edge 0: when the lot's front
// is another edge, its lot info becomes that of a lot made facing edge 0
// and turned to its front (creation front edge 0, base size across x
// along, road edges in that frame), which the neighborhood draws and moves
// the same (GetCurrentSize, RotateLotImposter). Not the game's own record
// (it turns the template's content and keeps the creation front = front),
// the same lot. A lot whose road strip faces no road (lot 83 of playtest
// 33, placed before checkEmptyLot) gets the template of its size unturned.
// Nothing is done when the lot already has its file.
LotResult makeLotFile(const std::filesystem::path& folder, uint32_t lot, const std::filesystem::path& templatesDir,
                      const std::filesystem::path& forbidden = {});
// PlaceLotFromInventory: the bin's lot file copied as the new lot's and its
// lot info made from the base, at (x, y) with `front`.
LotResult placeBinLot(const std::filesystem::path& folder, const PlacementWorld& world, const BinLot& bin, int x, int y,
                      int front, const std::filesystem::path& forbidden = {});
// cTSUserToolMoveLot: the lot at (x, y) with `front`.
LotResult moveLot(const std::filesystem::path& folder, const PlacementWorld& world, uint32_t lot, int x, int y,
                  int front, const std::filesystem::path& forbidden = {});
// cTSGameStateController::SaveNeighborhoodLotToCatalog + RemoveNeighborhoodLot:
// a lot with a house goes to the bin (as cx_<next id>) and leaves the
// neighborhood; a family living there goes with it (its FAMI, name and
// wardrobe in the lot file, its photo; its relationships with other Sims
// dropped) and comes back when the lot is placed again (placeBinLot).
LotResult moveLotToBin(const std::filesystem::path& folder, uint32_t lot, const std::filesystem::path& catalogDir,
                       const std::filesystem::path& forbidden = {});

// cTSUserToolPlaceLotBase::ChangeRotationDueToMouseDrag (FUN_00a27080, used
// by RotateLot BV 0x222b2c and the other two lot tools while the button is
// held): the front edge the drag from the press point (fromX, fromY) to the
// mouse (toX, toY), neighborhood world units, points to; nothing while
// |dx| + |dy| < 16. front = floor(4.5 - 2 (atan2(dy, dx) + pi) / pi) mod 4,
// so +x -> 2, +y -> 1, -x -> 0, -y -> 3 (the edges' own numbers).
std::optional<int> frontFromDrag(float fromX, float fromY, float toX, float toY);

// The (x, y, front) placements of a lot of base info `base` with its corner
// near (x, y) that pass the checks and the road match, the turn nearest the
// lot's creation first (a convenience: the game turns the lot with the keys).
std::optional<std::pair<int, Placement>> findFront(const PlacementWorld& world, const LotInfo& base, int x, int y,
                                                    uint32_t self = 0, int preferred = -1);

// Decorations: the neighborhood's own flora, stones, landmarks and effects
// the puck's "Decorações - F3" panel lists (docs/neighborhood-mac.md 14, 15.4).
// Each is a product of the object catalog (catcanhobjects, type 0x6D619378);
// the panel's five sort buttons are the product's `sort`. The catalog loader
// (FUN_00b36510) turns the sort into cTSProductNeighborhoodObject::ObjectType
// (flora 0, stone 1, landmark 2, effects 3, misc 4) and
// cTSUserToolPlopNeighborhoodObject::InstantiateObjectForCatalogProduct
// makes type 0 a flora occupant (the manager's CreateFloraOccupant) and the
// others props (CreatePropOccupant).
struct DecorProduct {
    uint32_t guid = 0;
    std::string name, model, sort;      // `model` is the CRES name without "_cres"
    bool inLot = false, onRoad = false; // allowedinlot, allowedonroad
    bool removeOnLotPlop = false;       // removeonlotplop
    int surface = 0;                    // placementsurface: 0 land, 1 water, 2 either
    uint32_t thumbGroup = 0, thumbInstance = 0; // a UI image (effects only)
    bool flora() const { return sort == "flora"; }
};
// The products shown in the catalog (showincatalog 1, not deprecated), by sort then GUID.
std::vector<DecorProduct> loadDecorCatalog(ResourceIndex& index);
// cTSUserToolPlopNeighborhoodObject::IsOKToPlaceObjectAtCurrentLocation (BV
// 0x415d34): 0 when `product` may stand with this footprint, else the error,
// the entry of the neighborhood's STR# 0x89 its tooltip shows (1-based):
// 1 a lot under the footprint's tiles when the product may not be in a lot
// or is removed when a lot is placed; 3 a tile not all land (surface 0) or
// 4 not all water (surface 1); 2 a road whose bounds touch the footprint's
// when it may not be on a road. 5: off the terrain (not the game's; the tool
// keeps the object on it).
enum DecorError { DecorOk = 0, DecorInLot = 1, DecorOnRoad = 2, DecorNeedsLand = 3, DecorNeedsWater = 4, DecorOffTerrain = 5 };
int decorationError(const PlacementWorld& world, const DecorProduct& product, const OccupantPosition& at);
// "" or the error in words.
std::string decorationBlocked(const PlacementWorld& world, const DecorProduct& product, const OccupantPosition& at);
struct DecorResult {
    bool ok = false;
    std::string error;
    int code = 0; // the DecorError when placing failed on a rule
    std::vector<std::string> lines;
};
// Adds / removes one occupant in the neighborhood package of `folder` (the
// user's copy; `forbidden` is the game's data). `index` is into the flora
// (flora = true) or props.
DecorResult addDecoration(const std::filesystem::path& folder, const PlacementWorld& world, const DecorProduct& product,
                          const OccupantPosition& at, float rotation, const std::filesystem::path& forbidden);
DecorResult removeDecoration(const std::filesystem::path& folder, const PlacementWorld& world, bool flora, size_t index,
                             const std::filesystem::path& forbidden);

} // namespace os2::hood
