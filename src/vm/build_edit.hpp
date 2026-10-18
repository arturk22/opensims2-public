#pragma once

// Build mode's wall, floor and wall-covering tools over a lot in the VM
// (docs/buy-build-mac.md §7): the changes to the lot's geometry
// (formats/lot_edit.hpp) with the game's rules and prices, kept in step with
// the VM (the world's walls and doors for routing, the rooms and the room of
// each object, the family's funds), undoable as the game's
// cTSUserToolUndoableWall / FloorPattern / Wallpaper are. Levels are the
// VM's (1 = the lot's ground), coordinates the lot's grid (tiles).

#include "formats/build_catalog.hpp"
#include "formats/lot_edit.hpp"
#include "formats/lot.hpp"
#include "placement.hpp"
#include "vm.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace os2::vm {

// The point of the lot's ground under a ray (the mouse): the first crossing
// with the terrain of `level` (the VM's, 1 = the ground), or, at the ground,
// with the bottom of a dug pool (the terrain is cut out over pool tiles and
// the bottom, 3 under it, shows through the water: the plain terrain hit
// landed about 2 tiles off what is seen). Marched and bisected: the old fixed
// point iteration on the terrain height oscillated across a dip.
// `poolWater`: over a pool, the water's surface instead (the pool tool works
// on the ground's grid: the bottom put its cursor about a tile off the mouse
// over the water, live QA pass 11).
std::optional<std::array<float, 2>> pickLotGround(const Lot& lot, int level, std::array<float, 3> eye,
                                                  std::array<float, 3> dir, bool poolWater = false);
// The height pickLotGround meets at (x, y): what to draw a cursor on.
float lotPickHeight(const Lot& lot, int level, float x, float y, bool poolWater = false);

class BuildEditor {
public:
    // `catalog` prices the patterns that floors give back when erased.
    BuildEditor(VM& vm, Lot& lot, const BuildCatalog* catalog = nullptr);

    struct Result {
        bool ok = false;
        int cost = 0;    // paid (negative: refunded)
        int changed = 0; // wall sections, quarter tiles or wall sides changed
        int error = 0;   // the game's placement error code when refused (STR# 0x89)
        std::string message;
    };

    // Wall tool: a wall along a straight run of grid points, along the
    // grid or diagonal (cTSUserToolWall drags in 45° steps), one section
    // per tile, §70 each (kWallSectionPrice). Sections that are there
    // already are skipped; any other refusal, or a price over the family's
    // funds, builds nothing (the tool's release undoes an invalid drag,
    // docs/buy-build-mac.md §7.24). The ground under each section is
    // levelled first to the height of the run's start vertex (the walls'
    // leveller, levelForWall; free: FloorElevationUnitChangeCost is 0).
    Result drawWalls(int level, int x0, int y0, int x1, int y1);
    // Room tool: the walls around the rectangle between two grid points,
    // then every grid point of the rectangle levelled to the start's height
    // (FUN_009a7dd8); a point that cannot go refuses the room.
    Result drawRoom(int level, int x0, int y0, int x1, int y1);
    // Ctrl-drag: the walls along the run go for int(70 x 0.8) = 56 a
    // section (the full price comes back only by undo). The objects
    // attached to a deleted section are sold with it (EraseWallAttachedObjects:
    // doors and windows in it, objects that need a wall on that side);
    // one of them in use (0x1B), or a Sim beside a section (0x4B), deletes
    // nothing.
    Result deleteWalls(int level, int x0, int y0, int x1, int y1);

    // Fence tool: a fence of the catalog's `fence` along a run, its `cost` a
    // section, with a post at every grid point of the run that has none
    // (UserMayPlaceFence: no slope steeper than 60°, 0x31); a refusal builds
    // nothing. Ctrl deletes fences (int(cost x 0.8) back a section; posts
    // left with no fence go).
    Result drawFences(int level, int x0, int y0, int x1, int y1, const BuildFence& fence);
    // Fence arches (fencearches.txt): on the fence of an edge (0x79 "Must be
    // placed on a fence."; where the game raises it is not found), then
    // fenceArchBlocked (0x45, 0x4F, 0x0C: another arch at an end), its catalog price; Ctrl takes it off
    // (int(price x 0.8) back). Deleting the fence takes its arch with it.
    Result placeFenceArch(int level, int ax, int ay, int bx, int by, const BuildFenceArch& arch);

    // The fireplace tool (cTSUserToolFireplace::TryPlace, BV 0x27a38c;
    // docs/buy-build-mac.md §7.26). After the mantle fits (the object tool's
    // TryPlace) the chimney: on the tile behind the mantle (a stove, type 4
    // of its "CT - Get Link Data": its own tile), from the top story T --
    // the highest from the mantle's up where the mantle's tile HasSupport --
    // down. Over T a stack of caps a unit apart, floor(max(2, the roof over
    // the chimney (RoofHeightAt) - T's floor there + 2)) down to 0, the
    // lowest the cap with the footprint; on a one-story fireplace the cap
    // with the footprint on the level over T (made when the lot has none,
    // up to level 4), or a cap at T at the story's height, and the story
    // must be a full one (StandardLevelHeight). Under them a hearth piece on
    // each story down to the ground (a stove: none), stopping under the
    // mantle's story where the tile is in a load-bearing room or under a
    // roof, the ground's on a foundation or a raised floor the foundation
    // piece. Each piece is linked to the one below and above it, the
    // mantle's story's piece to the mantle.
    struct ChimneyPiece {
        enum Kind { Hearth, Top, TopWithFootprint, Foundation };
        Kind kind = Hearth;
        Position at;         // its lead's place (VM level) and its elevation over the floor
        int below = -1, above = -1; // the pieces it links to: indices, kMantleLink the mantle, -1 none
    };
    static constexpr int kMantleLink = -2;
    struct ChimneyPlan {
        int error = 0; // 0 fits, -1 refused (TryPlace gives no text: the footprint goes red)
        std::string why;
        bool newLevel = false; // the cap needs a level over the lot's top (SetLevelRange)
        std::vector<ChimneyPiece> pieces; // in TryPlace's order: the caps from the top, then the hearths down
        int mantleBelow = -1, mantleAbove = -1;
    };
    ChimneyPlan chimneyPlan(ObjectId mantle, const Position& at, int direction) const;
    // The fireplace placed (TryPlace, then SubmitUndoableForPlace: LinkObjects
    // with every part, the pieces' undoables after the mantle's): the mantle
    // at `tile` facing `direction`, each piece made from the product's OBJD,
    // tested where it goes (TryPlaceObject: the position test at its
    // elevation; FUN_0027664c: on the ground not over a pool, and nothing
    // on its tile without od 42 bit 0x01) and put there, then the links, the
    // price of all (the objects' prices), the Mains. A piece that does not
    // fit takes everything back (RevertChanges). Returns the objects placed
    // (the mantle first) in `placed`.
    // A multi-tile mantle with a hearth or a cap piece behind one of its
    // parts goes in front of that chimney instead (IsChimneyBehindHeldMantle,
    // IsValidChimneyBehindHeldMantle, OnRelease): under the hearth piece of
    // its story (error 6 when a mantle is under it already, 5 when the
    // pieces behind do not fit), or, before the cap at the story's height
    // (a story built over a chimney's top), with the chimney's top made
    // again behind it (CreateUndoableListBehindObjectHeld).
    Result placeFireplace(const BuildFireplace& product, ObjectId mantle, TilePos tile, int direction,
                          std::vector<ObjectId>* placed = nullptr);
    // The sledgehammer on a fireplace (cTSUserToolSledgeHammer::
    // DeleteFireplaces -> EraseLinkedFireplace, BV 0x306454): the pieces up
    // the chain from `part`'s, then down it, then its own, each sold.
    Result deleteFireplace(ObjectId part);
    Result deleteFenceArch(int level, int ax, int ay, int bx, int by, const BuildFenceArch* arch = nullptr);
    Result deleteFences(int level, int x0, int y0, int x1, int y1);
    // UserMayPlaceFenceArch (BV 0x9a92fc) on the unit edge (lot level `li`):
    // 0x45 neither side tile in bounds, 0x4F an object on an end's fence
    // post, 0x0C an arch at an end or an object meeting a post; 0 if it may go.
    int fenceArchBlocked(int li, int ax, int ay, int bx, int by) const;

    // Foundation tool (cTSUserToolDeckFoundation, deckfoundations.txt
    // "foundation 0"): a rectangle of ground tiles (corners inclusive)
    // raised a quarter story: the terrain under it is levelled to its first
    // corner's height (0x81 when a point cannot move), the level above gets
    // its floor there 0.75 higher (and the stories over it with it), with
    // the "floor_plywoodFoundation" floor, and foundation walls (style 23,
    // "foundationbrick") run round its edge. §4 a tile (kFoundationTilePrice).
    // Only on the ground (0x1A), not over water (0x1E), walls, floors or
    // objects that do not allow it (0x0C; od 42 bit 0x200 does).
    // `product` is deckfoundations.txt's: 0 the foundation, 1 the deck
    // with a lattice skirt (wall style 16, "deckRedwood"; §3 a tile), 2 the
    // deck on columns (style 4, not drawn; its supportObject 0xAA75A097 at
    // the rectangle's corners, supportLayout -corners; §2 a tile). Decks
    // have the "deckDarkRedwood" floor. Edges keep the style they had;
    // new ones take the tool's.
    Result drawFoundation(int x0, int y0, int x1, int y1, int product = 0);
    // Ctrl: the foundation tiles of the rectangle go (int(4 x 0.8) back a
    // tile); the walls follow the edge of what is left.
    Result deleteFoundation(int x0, int y0, int x1, int y1);
    // A ground tile under a foundation or a deck: its markers (3ARY 0x14) on
    // the ground when the lot keeps them, else the story above 0.25-1.25
    // over the terrain at its four grid points (a lot without the array).
    bool foundationTile(int x, int y) const;
    // Every quarter of the tiles (x0, y0)-(x1, y1) on lot level `level` gets
    // the marker `style` (SetDeckFoundationMarkers, docs/buy-build-mac.md
    // §7.25).
    void setMarkers(int level, int x0, int y0, int x1, int y1, int32_t style);
    // A lot read without the markers gets them from its heights and pools.
    void ensureMarkers();
    // The deck tool's cost of setting `style` on the tiles (ax, ay)-(bx, by)
    // of lot level `level` (SetDeckFoundationStyle: a quarter of the new
    // product's price less 0.8 of the old one's for each quarter that
    // changes, floored).
    int markerCost(int level, int ax, int ay, int bx, int by, int32_t style) const;
    // UserDeckFoundation::CalculateTopElevation from the drag's start grid
    // point (sx, sy): its ground + 0.75 (a pool: + 0), or the raised floor
    // there when a deck or foundation is round it.
    float deckTopHeight(int sx, int sy, bool pool) const;
    // MayPlaceQuadrilateral's grid points: 0x81 when one of the rectangle's
    // (tiles (ax, ay)-(bx, by)) on lot level `li` not at `top` fails the
    // vertex test; 0 if they all may go.
    int deckLevelError(int li, int ax, int ay, int bx, int by, float top) const;
    // MoveFloorElevations / TryMoveFloorElevation / ChangeFloorVertex (BV
    // 0x9af1c4, 0x9aef18, 0x9acee0) over the grid points one past the
    // rectangle (docs/buy-build-mac.md §7.25): to `top` (`place`) or back a
    // story over the level below, each that passes the vertex test.
    void moveDeckFloor(int li, int ax, int ay, int bx, int by, float top, bool pool, bool place);

    // Pool tool (deckfoundations.txt "pool 3"): a rectangle of ground tiles
    // (corners inclusive) dug into a pool: the lot gets its underground
    // level if it has none, the terrain round it levelled to the first
    // corner (0x0D), the pool's floor ("floor_poolRoundedLipBlue") one
    // story down, pool walls (style 29) round the edge there and a POOL
    // entry (the water 0.25 under the ground). §75 a tile. Only on exposed
    // terrain of the ground (0x5B), not into another pool (0x4F), water
    // (0x20) or objects (0x0C). Sims do not walk over it.
    Result drawPool(int x0, int y0, int x1, int y1);
    // The refusal code drawPool would give for the drag (0: it would dig),
    // with the reason and the number of new pool tiles; no money check. Used
    // by the cursor's preview (red where the drag would be refused).
    int poolDigError(int x0, int y0, int x1, int y1, std::string* why = nullptr, int* tiles = nullptr) const;
    // Ctrl: the pool under a tile goes (int(75 x 0.8) back a tile); not
    // with a Sim in it (0x2D) or a ladder or diving board on it (0x2E).
    Result deletePool(int x, int y);
    // Ctrl-drag over a pool: erase the tiles of the rectangle (x0, y0)-(x1,
    // y1) (cWorldPool::IsQuadLegalForPoolErase / QuadListOnPoolErase).
    Result erasePool(int x0, int y0, int x1, int y1);
    bool poolTile(int x, int y) const;

    // Roof tool (cTSUserToolRoof, roofs.txt "roof 0-5" = Lot::RoofType 1-6:
    // shed gable, shed hip, short gable, long gable, hip, mansard): a roof
    // over the rectangle between two grid points, its base on `level` (the
    // VM level above the walls it covers; the lot keeps it as level - 1 +
    // 1), its slope the way of the drag for shed roofs, the lot's roof
    // pattern (the game draws every roof with one). Free. Gable ends get
    // attic walls (style 3, floorToRoof) where no other roof of the level
    // covers them (inferred: MakeShedWalls is not read; the rule gives 788
    // of the 905 attic walls of the 62 saved lots, and 74 they lack). Ctrl deletes the roof under a point with its attic
    // walls (attic walls go with deleteTool roof).
    Result drawRoof(int level, int x0, int y0, int x1, int y1, uint32_t type);
    // Why a roof cannot go there (cTSUserToolRoof::IsRoofValid, BV
    // 0x1d9fd4): 0x10 too big (|dx| x |dy| >= 1840), 0x7E a corner's tiles
    // off the lot or locked, 0x7F it would come down through an object on
    // its level, 0x1C below the ground or started over a pool; 0 fits.
    int roofError(const Lot::Roof& roof) const;
    Result deleteRoof(int level, float x, float y);
    // Dormers (roofs.txt "roof 6-11", cTSUserToolRoof MakeDormer, BV
    // 0x1d5d20): a box of normal walls `width` (1-3: (product - 6) % 3 + 1)
    // by width + 2 tiles on the VM level `level` (the attic the click is on)
    // with its corner at grid point (x, y), turned by `direction` (0, 2, 4,
    // 6: the box runs width x deep for 0 and 4, deep x width for 2 and 6;
    // 2 starts `deep` tiles to -x, 4 `deep` to -y), and over it a long gable
    // (products 6-8) or hip roof (9-11) based on the level above, with the
    // attic walls of the gable ends. Each new wall section costs §70 (the
    // walls are pre/post operations of the roof's undoable, whose cost
    // TotalCost adds; 8, 12 and 16 sections give the catalog's 560, 840 and
    // 1120); the roof is free. Wall vertices on walls refuse (0x08); no level
    // above refuses (0x06).
    Result placeDormer(int product, int level, int x, int y, int direction);
    // Auto-roof (cTSUserToolAutoRoof::CreateAutoRoof -> CreateAutoRoofs per
    // level, docs/buy-build-mac.md §7.12): on each level the tiles of the
    // rooms below (the light rooms) whose tile above is no room, has no floor
    // and no roof yet, cut into rectangles row by row (ComputeRoofRects), each
    // a roof of a type chosen by the tool and the rectangle's shape (tool 1:
    // hip; 2: long gable; 3: hip if not deeper than wide, else long gable;
    // 4: short gable if shallower than wide, else long gable); tool 5 (the
    // tiled slab) lays the roof floor pattern ("CCC26098") there instead.
    // Unless `preserve`, the roofs the tool made before go first. Free.
    Result autoRoof(int tool, bool preserve);
    // Every roof goes (C300 "Remover Telhado"; with their attic walls).
    Result removeRoofs();
    // Roof patterns (cTSUserToolUndoableRoofPattern::SetRoofPatternProduct):
    // every roof of the lot takes the pattern (catalog GUID). Free.
    Result setRoofPattern(uint32_t guid);

    // Terrain tools (cTSUserToolTerrainSculpt / TerrainPaint): a stroke
    // is one undoable change; while the mouse is down the brush is applied
    // every 100 ms (ApplyTerrainSculptBrush) around a point, its strength
    // per grid point from the brush's profile (a 32x32 grey image,
    // terrainSculptBrushes.txt profileTextureID) stretched over the brush
    // width (2, 6 or 10 grid points for the small, medium and large brush:
    // SetBrushWidthChoice). Raise: height + profile x 0.0002 rounded up to
    // the terrain step, lower: height - profile x 0.00002 rounded down
    // (Raise/LowerBrushFn; the step, 0.00625, is inferred from the heights
    // the lots hold), flatten: to the height where the stroke began,
    // smooth: towards the mean of the four neighbours (inferred). Each
    // application is paid at once: round(total |height change| x §1)
    // (UpdateFundsForHeightChange; catalog price 1). Only exposed terrain
    // of the ground (0x8B); points under floors and walls stay.
    // Paint: the paint's mask (4 points a tile) gains the profile, the
    // others lose it (DiminishAllPaints); free.
    // Water (cTSUserToolWater, the pond tool; catalog text "build" 133, §1
    // a unit): the brush (spWaterBrushProfile) digs the terrain, height -
    // profile x 0.00001 rounded down to the step (WaterBrushFn), and after
    // each application the water levels of the dug grid points and the
    // water connected to them follow (AdjustWaterElevations ->
    // cWorldTerrain::UpdateWaterElevations): each connected group of them is
    // a pond whose water is 0.25 below the lowest ground round it; a point
    // more than 0.25 over that water stays dry (water 1 under it), a dry one
    // beside a pond gets the pond's water less 0.1, others 1 under the ground.
    enum class TerrainOp { Raise, Lower, Flatten, Smooth, Paint, Erase, Water };
    void beginTerrainStroke(TerrainOp op, float x, float y);
    Result applyTerrainBrush(float x, float y, int size, const std::vector<uint8_t>& profile,
                             const std::string& paint = "");
    Result endTerrainStroke();
    // A whole stroke at one point: `ticks` applications (headless runs).
    Result terrainStroke(TerrainOp op, float x, float y, int ticks, int size, const std::vector<uint8_t>& profile,
                         const std::string& paint = "");

    // Floor tool over the tiles of a rectangle (corners inclusive), or the
    // given quarters of one tile (`quarters` bits: west, north, east, south).
    // A quarter costs a quarter of the pattern's price; covering another
    // pattern (or the same with another rotation) pays the new one in full;
    // a null pattern erases (a quarter of floor(price x 0.8) back).
    // `rotation` is the pattern's (1, 2, 4, 8 = 0-270 degrees). Painting on
    // the ground levels the terrain under the rectangle to the height of
    // its first tile (LevelRect, free); above it a tile must be floorable
    // (IsFloorable: supported from below or near a supported tile, 0x52).
    Result paintFloor(int level, int x0, int y0, int x1, int y1, const BuildPattern* pattern, int quarters = 0xF,
                      int rotation = 1);
    // The quarters a click on point (px, py) of a tile floors
    // (FUN_003000cc): the whole tile, or the half on the point's side when
    // a diagonal wall splits it.
    int clickQuarters(int level, float px, float py) const;
    // A floor tile could go there above the ground (nTSWorld::IsFloorable,
    // BV 0x16a990; docs/buy-build-mac.md §7.21): flat, and held by
    // HasSupport, by a neighbour SupportedUnderneath on its facing quarter,
    // or, next to a floor of the level, by a tile SupportedUnderneath within
    // two (|dx| + |dy| < 4) or a load-bearing wall below within one.
    bool floorable(int level, int x, int y) const;
    // nTSWorld::SupportedUnderneath (BV 0x16a5b8): the ground (level 1 and
    // below) always; above it, a floor on the level below in one of the
    // tile's `quarters` (bits 1 west, 2 north, 4 east, 8 south), or an
    // object of OBJD type 5 ("support", cTSObject::IsSupport) on the tile
    // below (HasObjectSupport, BV 0x16a328; `ignore` is the object being
    // tested, which does not hold itself).
    bool supportedUnderneath(int level, int x, int y, int quarters, ObjectId ignore = 0) const;
    // FUN_001654b4: a load-bearing wall (PartitionStyleBearsLoad) on the
    // level below on one of the tile's diagonals or on the edge of one of
    // its `quarters`.
    bool wallSupported(int level, int x, int y, int quarters) const;
    // cWorldRoof::DoesTileHasRoofSupport (BV 0xc245d4): a mansard roof of
    // the level below whose rectangle, less StandardLevelHeight (3) tiles
    // on every side, holds the tile (its flat top).
    bool roofSupported(int level, int x, int y) const;
    // nTSWorld::HasSupport (BV 0x16a640): SupportedUnderneath, a
    // load-bearing wall below, or the roof under it (WorldDB::Roof(),
    // vtable 0xa4 = DoesTileHasRoofSupport).
    bool hasSupport(int level, int x, int y, int quarters, ObjectId ignore = 0) const {
        return supportedUnderneath(level, x, y, quarters, ignore) || wallSupported(level, x, y, quarters) ||
               roofSupported(level, x, y);
    }

    // FUN_008a7164 -> nTSWorld::CeilingTooHighForObject (BV 0x16a6f8), the
    // one place object placement asks HasSupport (TestPlacementCommon,
    // docs/buy-build-mac.md §7.24): a staircase part (OBJD type 10) whose
    // tile's story above is on the lot, HasSupport, and has a grid point more
    // than StandardLevelHeight (3) + 0.001 over the same point of the tile's
    // own story: 0x30 (unless placement flags 2, od 82, have 0x20). 0 fits.
    int stairCeilingError(const Placement& p) const;

    // Wallpaper tool: the faces of the walls along a run, on its right
    // (side +1, seen from (x0, y0) to (x1, y1)), left (-1) or both (0), at
    // the pattern's price per side.
    // A null pattern takes the coverings off (Ctrl).
    Result paintWalls(int level, int x0, int y0, int x1, int y1, int side, const BuildPattern* pattern);
    // Every wall face of the room of a tile (headless `--wallpaper-room`).
    Result paintRoom(int level, int x, int y, const BuildPattern* pattern);
    // Shift as the game does it (UserSetWallPatternFlood): from the face of
    // the wall section (ax, ay)-(bx, by) on the run's right (side +1) or
    // left (-1), the connected faces of that room that have the clicked
    // face's pattern.
    Result paintFlood(int level, int ax, int ay, int bx, int by, int side, const BuildPattern* pattern);

    // Modular stairs tool (cTSUserToolModularStairs, UserPlaceModularStairs,
    // docs/buy-build-mac.md §7.12): a flight of `product` going down from
    // the floor tile `top` (VM level) the way of `direction` (0, 2, 4, 6:
    // the steps face it). `end` is the point the drag reached (the last step
    // goes there: n = ceil((distance from the top tile's centre - 0.5) x 4)
    // steps of a quarter tile, rounded up to a multiple of 4 with
    // `wholeTiles`); without it the end is found by walking down until the
    // last step meets the ground (UserPlaceModularStairsFindEnd). Each step
    // drops 3/16 (a story in 16 steps). On the terrain the ground over a step
    // is lowered under it and the tile past the last step levelled to it.
    // Objects: a landing on the top tile, the top step, the steps, the bottom
    // step, a landing half a tile past the last step turned 180°, and a
    // placeholder over every tile of steps on each level the flight crosses;
    // the steps are linked (object data 80 the next, 81 the previous: -1 at
    // the ends) as LinkObjects does. The price is the product's per step.
    // Refusals give the game's codes 0x6C-0x78 (0x16 without the money).
    Result placeStairs(const BuildStairs& product, TilePos top, int direction,
                       std::optional<std::array<float, 2>> end = std::nullopt, bool wholeTiles = false);
    // The steps placeStairs would lay (0 when refused) and their cost, for the
    // cursor's preview (CalculatePreviewCost).
    int stairsSteps(TilePos top, int direction, std::optional<std::array<float, 2>> end, bool wholeTiles,
                    int* error = nullptr) const;
    // The sledgehammer on a modular staircase (DeleteModularStairs ->
    // RemoveFlight): every object of the flight that `part` belongs to is
    // sold (each part's current value back) as one undoable change.
    Result deleteStairs(ObjectId part);
    // The Railing Tool (B620, cTSUserToolStairsRailing::ChangeRailings and
    // ChangeOneRailing): on a staircase part, every type 11/12 object of its
    // flight (or the object alone outside a flight) gets a railing on `side`
    // (railingSide: true = left of the steps' facing) for the tool's price
    // (VM::railingPrice) each, or with `remove` (Ctrl) loses it for the
    // price back. Models (railingObjectModel): 1 none, 2 / 3 one side, 4 both;
    // adding to 1 gives 2 (side false) or 3 (true), adding the other side
    // to 2 / 3 gives 4; removing from 4 leaves the other side, from the
    // side's own model leaves 1; an object that already is as asked is
    // skipped. Undoable (the model swap runs entry 47 again).
    Result changeRailing(ObjectId part, bool side, bool remove);

    // A staircase object just put down (its parts' IDs): the floor of the
    // level above goes from over its steps, free, as one undoable change (the
    // stairwell; inferred from the modular stairs' placeholders that reserve
    // the space above the steps, docs/buy-build-mac.md §7.12).
    Result openStairwell(ObjectId staircase);

    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    size_t undoDepth() const { return undo_.size(); }
    bool canRedo() const { return !redo_.empty(); }
    // The history goes (FlushCommandQueues: the game's on entering Live
    // mode), and what was sold with the walls dies (`forget` is told each
    // part first).
    void resetHistory(const std::function<void(ObjectId)>& forget = {});
    // ReleaseRedoList (a new change here, or in Buy mode): the redo list
    // goes, and the objects its changes had put down die (the columns of an
    // undone deck). `onForget` is told each part first (also when a new
    // change of the editor's own releases the list).
    void dropRedo();
    std::function<void(ObjectId)> onForget;

    // Bumped by every change, for whoever draws the lot.
    uint32_t revision() const { return revision_; }
    // The world again from the lot and the VM's doors (after the object
    // tool placed or took a door or window).
    void syncWorld();

    const Lot& lot() const { return lot_; }

    // Locked tiles (cWorldDB::IsLocked, cIWorldDB slot 0xc4: the lot's 3ARY
    // 0x03, the road strip the lot template locks; lotTileLocked, with the
    // inferred road strip only for a lot without the array).
    bool tileLocked(int li, int x, int y) const { return lotTileLocked(lot_, editable_, li, x, y); }
    // A tile on the lot and not locked (`li` the lot level).
    bool editableTile(int x, int y, int li = 0) const {
        return x >= 0 && y >= 0 && x < int(lot_.width) && y < int(lot_.height) && !tileLocked(li, x, y);
    }
    // FUN_009e8780 (the vertex test's mask bit 0, BV): a grid point on the
    // world's edge, or with one of its four tiles off the lot or locked, may
    // not move (0x62).
    bool editablePoint(int x, int y, int li = 0) const {
        return editableTile(x - 1, y - 1, li) && editableTile(x, y - 1, li) && editableTile(x - 1, y, li) &&
               editableTile(x, y, li);
    }
    // The vertex test (FUN_009e9550, docs/buy-build-mac.md §7.24): may grid
    // point (x, y) of lot level `li` go to `height`? 0 if so, else the game's
    // code (kSilentRefusal for the tests that refuse without one). `mask`
    // picks the tests (vertextest bits); a point already at the height only
    // takes the edge test (bit 0). Done: the edge or a locked tile round it
    // (0x62), the height limits (0x65/0x66), a floor quarter on a tile round
    // it on its level (0x5A), the point a story up or down that would have
    // to follow within MinLevelHeight (0.75) and cannot (0x69/0x6A), a wall
    // on an edge from it that needs a flat bottom (0x5B, this level) or top
    // (0x5C, the level below), a pool round it (silent) and a fence that
    // would get steeper than 60° (0x31), and the objects
    // round it (bit 7, vertexObjectsError: 0x61, 0x60, 0x68).
    static constexpr int kSilentRefusal = -1;
    int vertexMoveError(int li, int x, int y, float height, uint32_t mask) const;
    // The vertex test's bit 7 (FUN_009e8e0c, docs/buy-build-mac.md §7.25)
    // on the four tiles round grid point (x, y) of lot level `li` going to
    // `height`: a person or an object without od 42 bit 0x100 pins the
    // ground (0x61 "Can't move occupied terrain"); with the point left less
    // than a story over the one below, an object on the tiles a level down
    // that does not conform to the ceiling (0x60; here any but a support,
    // inferred); an od 42 bit 0x80 object the floor would rise over (0x68).
    int vertexObjectsError(int li, int x, int y, float height) const;
    // nTSWorld::HypotheticalFloorElevation: lot level `li`'s floor at (px,
    // py) with grid point (vx, vy) at `height`.
    float hypotheticalFloor(int li, int vx, int vy, float height, float px, float py) const;

    // UserMayPlacePartition's common test (BV 0x9a7384) and its partition
    // test FUN_009a6b20, for an edge on lot level `li`: 0x40 off the lot,
    // 0x41 a partition of a blocked graph on the edge, 0x4D a wall next to an
    // arch, 0x50 an arch next to a wall, 0x45 locked, 0x49 (arches) not flat;
    // 0 if none.
    enum class PartitionKind { Wall, Fence, Arch };
    int partitionCommonError(int li, int ax, int ay, int bx, int by, PartitionKind kind) const;

    // Why a wall section cannot go there ("" if it can).
    // `code` gets the game's error code (UserMayPlacePartition: 0x40 out of
    // the lot, 0x45 not editable, 0x48 no support, 0x49 slope, 0x4B Sim,
    // 0x0C object). `fence` tests a fence's rules (60° slope at most,
    // 0x31) instead of a wall's flat bottom; `levelled` leaves the slope
    // out (the wall tool levels the ground first).
    std::string wallBlocked(int level, int ax, int ay, int bx, int by, int* code = nullptr, bool fence = false,
                            bool levelled = false) const;

private:
    struct Geometry {
        std::vector<float> elevations;
        std::vector<std::array<uint8_t, 4>> floorVisibility;
        std::vector<uint16_t> floorRotations;
        std::vector<Lot::Object> objects;
        std::vector<Lot::FencePost> fencePosts;
        std::vector<Lot::TerrainPaint> paints;
        std::vector<Lot::Roof> roofs;
        std::vector<Lot::SwimmingPool> pools;
        std::vector<float> water;
        int minLevel = 0, levelCount = 0;
        std::vector<Lot::Wall> walls;
        std::vector<Lot::WallGraph> wallGraphs;
        std::vector<std::array<uint16_t, 4>> floors, rooms;
        std::map<uint32_t, std::vector<std::array<uint16_t, 4>>> roomLayers;
        std::map<uint16_t, std::string> floorPatterns, wallPatterns;
        std::map<uint32_t, Lot::LayerEntry> staleWallLayer;
        std::vector<std::array<int32_t, 4>> deckFoundationStyles;
        int deckFoundationBase = 0;
    };
    // An object sold with a wall: where it was, for undo.
    struct Sold {
        ObjectId id = 0;
        Placement where;
    };
    struct Op {
        Geometry before, after;
        int cost = 0;
        std::vector<Sold> sold;
        std::vector<Sold> created; // objects the change put down (deck columns)
        std::vector<std::array<int, 3>> models; // railing swaps: object, model before, after
        // Fireplace links changed (a mantle put in front of a chimney):
        // object, below and above before, below and above after.
        std::vector<std::array<int, 5>> links;
    };
    Geometry capture() const;
    void restore(const Geometry& g);
    // After a change: rooms, then the VM.
    void changed();
    void commit(Geometry before, int cost, std::vector<Sold> sold = {}, std::vector<Sold> created = {});
    // The objects attached to a wall section (lead IDs), and whether one
    // of them is in use.
    std::vector<ObjectId> attachedObjects(int level, int ax, int ay, int bx, int by, bool* inUse) const;
    Result eraseRun(int level, int x0, int y0, int x1, int y1, bool fences);
    // The attic walls the roofs of a lot level need (gable ends over walls).
    std::vector<std::array<int, 4>> atticWalls(int lotLevel) const;
    void syncAtticWalls(int lotLevel, const std::vector<std::array<int, 4>>& before);
    // Foundation walls along the edge between foundation and other tiles.
    // The raised edges round the tiles a foundation/deck change touched
    // (`laid`: they were laid with product `newStyle`, else taken away).
    void foundationWalls(int newStyle, const std::set<std::pair<int, int>>* changedTiles, bool laid);
    void poolWalls();
    Result cover(std::vector<std::pair<uint32_t, int>> faces, const BuildPattern* pattern, const std::string& what);
    int priceOf(const std::string& lotName, bool floor) const;

    VM& vm_;
    Lot& lot_;
    const BuildCatalog* catalog_ = nullptr;
    std::vector<Op> undo_, redo_;
    std::set<uint32_t> autoRoofs_;  // roof IDs the auto-roof tool made
    std::vector<RoomMerge> merges_; // rooms the walls removed since the last changed() joined
    std::map<std::string, int> prices_; // patterns laid this session, by lot name
    std::vector<Stair> loadedStairs_;   // the world's stairs from the lot's cObjects
    struct Stroke {
        TerrainOp op = TerrainOp::Raise;
        Geometry before;
        float height = 0;   // flatten's target
        float moved = 0;    // total |height change|
        int paid = 0;
        int changed = 0;
        std::set<std::pair<int, int>> dug; // the water tool's grid points
    };
    // UpdateWaterElevations over the grid points the water tool dug.
    void adjustWater(const std::set<std::pair<int, int>>& dug);
    // cWorldTerrain::UpdateWaterElevations (BV 0x8b185c) over `points`: the
    // water tool's (`floor` false: they are ponds) or one ground point a
    // floor change moved (`floor`: FUN_008b02d4, `wasPond` its pond flag from
    // before the move), the ponds connected to them, each pond group's water
    // 0.25 under its lowest rim, the points that are no pond the water of a
    // pond beside them less 0.1 or 1 under the ground.
    void updateWaterElevations(const std::vector<std::pair<int, int>>& points, bool floor, bool wasPond);
    // A ground grid point moved with the stories over it, its water after it
    // (ChangeFloorAndWaterElevationCoupled -> UpdateWaterElevation).
    void moveGroundWithWater(int x, int y, float height);
    std::optional<Stroke> stroke_;
    // The flight a modular stair object belongs to: its objects (steps first,
    // top to bottom).
    std::vector<ObjectId> flightOf(ObjectId part) const;
    struct StairsPlan;
    int planStairs(StairsPlan& plan, TilePos top, int direction, std::optional<std::array<float, 2>> end,
                   bool wholeTiles) const;
    // A ground grid point no floor, wall or foundation holds.
    bool terrainFree(int x, int y) const;
    // Moves a grid point of lot level `li` to `height` and the stories
    // above it by the same amount (ChangeFloorAndWaterElevationCoupled;
    // inferred from the name, as the terrain brush does).
    void moveVertexCoupled(int li, int x, int y, float height);
    // The walls' leveller over the grid points of lot level `li`
    // (TryAdjustFloorElevationForWall / FUN_009a79d0 / FUN_009a5a68, docs/
    // buy-build-mac.md §7.24): each point not yet at `height` that is under
    // water refuses (0x47), else it must pass the vertex test with the walls'
    // flags (vertextest::kWalls) and moves with the stories above; a wall
    // style that requiresFlatTop also takes the point a story up to `height`
    // + 3 when the lot has that level. Nothing moves when one refuses; `code`
    // gets the refusal (0: one the game refuses without a code).
    bool levelForWall(int li, const std::vector<std::array<int, 2>>& points, float height, bool flatTop,
                      int* code);
    uint32_t revision_ = 0;
    std::array<int, 4> editable_{0, 0, 0, 0}; // inferredRoadStripArea, for a lot without 3ARY 0x03
};

// cITSProductDeckFoundation::tDeckFoundationType (the deckfoundations.txt
// parser's SetType, docs/buy-build-mac.md §7.25): deck 1, foundation 2, pool
// 3; the BV's stage is 4. deckFoundationType gives a base-game marker's
// (product 0 foundation, 1 and 2 decks, 3 pool); 0 for none.
constexpr int kDeckTypeDeck = 1, kDeckTypeFoundation = 2, kDeckTypePool = 3, kDeckTypeStage = 4;
int deckFoundationType(int32_t style);
// A base-game product's price a tile (deckfoundations.txt 0-3; 0 for none).
int deckProductPrice(int32_t style);

// The modular stairs among the VM's objects: each "Stair Modular - ... -
// Portal - A" top step followed down its links (object data 80) to the
// "... - End" (flights between two adjacent levels only).
std::vector<Stair> modularStairs(const VM& vm, const Lot& lot);

// The stairs of the staircase objects in the world (their "Lo ... Pad"
// and "Hi ... Pad" parts are the landings).
std::vector<Stair> objectStairs(const VM& vm, const Lot& lot);

// The auto-roof's rectangles over a grid of marked tiles (W x H, row by
// row): cAutoRoofData::ComputeRoofRects (runs grow the active rectangles
// they hold, overlaps start new ones, so rectangles can overlap) and BV
// 0x9a0a08 (rectangles more elongated than `aspectLimit` to one whose
// tiles all stay covered by others go). [x0, y0, x1, y1), tiles.
std::vector<std::array<int, 4>> autoRoofRects(const std::vector<uint8_t>& grid, int W, int H, float aspectLimit = 2.0f);

// The terrain's height limits (FUN_009e7700, the first test of
// FUN_009e9550): cWorldDB's Min/MaxTerrainElevation = -/+150 x
// FloorElevationUnit (0.1875, cWorldDB +0x44). The game's code for a grid
// point going to `height`: 0x65 "Can't lower terrain further" under the
// minimum, 0x66 "Can't raise terrain further" over the maximum, else 0.
constexpr float kFloorElevationUnit = 0.1875f;
// cWorldDB::StandardLevelHeight (WorldDB vtable 0x15c): a story.
constexpr float kStandardLevelHeight = 3.0f;
constexpr float kMinTerrainElevation = -150 * kFloorElevationUnit;
constexpr float kMaxTerrainElevation = 150 * kFloorElevationUnit;
int terrainLimitError(float height);

// The vertex test's flags (FUN_009e9550 tests a std::bitset bit by bit;
// docs/buy-build-mac.md §7.24) and the bitsets the game's static
// initializers build: the walls' leveller and the modular stairs use 0x2BB7,
// the terrain brush 0x2BF7 (its extra bit 6 only changes a local copy).
namespace vertextest {
constexpr uint32_t Edge = 1u << 0, Limits = 1u << 1, Floor = 1u << 2, Floorable = 1u << 3, Above = 1u << 4,
                   Below = 1u << 5, Objects = 1u << 7, Partitions = 1u << 8, Pool = 1u << 9,
                   Submersible = 1u << 10, FenceSlope = 1u << 11,
                   TileMarker = 1u << 13; // WorldDB slot 0xcc (not the deck markers), never set in the base game
constexpr uint32_t kWalls = 0x2BB7, kBrush = 0x2BF7;
// cWorldDB::MinLevelHeight (the global 0x02b14460, set to 0.75 by a static
// initializer): how close a story may come to the one below.
constexpr float kMinLevelHeight = 0.75f;
} // namespace vertextest

// Splits a run between two grid points into tile-long sections (along an
// axis or a 45° diagonal); empty for other slopes.
std::vector<std::array<int, 4>> wallSections(int x0, int y0, int x1, int y1);

} // namespace os2::vm
