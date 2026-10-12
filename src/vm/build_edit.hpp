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
    // already are skipped; the first one that breaks a rule stops the run
    // there (PlaceWallLine: the sections before it are built), and a price
    // over the family's funds builds nothing. On the ground the terrain
    // under each section is levelled first to the height of the run's
    // start (TryAdjustFloorElevationForWall, free: FloorElevationUnitChangeCost
    // is 0), unless a floor or another wall holds a point it would move.
    Result drawWalls(int level, int x0, int y0, int x1, int y1);
    // Room tool: the walls around the rectangle between two grid points.
    Result drawRoom(int level, int x0, int y0, int x1, int y1);
    // Ctrl-drag: the walls along the run go for int(70 x 0.8) = 56 a
    // section (the full price comes back only by undo). The objects
    // attached to a deleted section are sold with it (EraseWallAttachedObjects:
    // doors and windows in it, objects that need a wall on that side);
    // one of them in use stops the run there (0x1B), as does a Sim beside
    // it (0x4B).
    Result deleteWalls(int level, int x0, int y0, int x1, int y1);

    // Fence tool: a fence of the catalog's `fence` along a run, its `cost` a
    // section, with a post at every grid point of the run that has none
    // (UserMayPlaceFence: no slope steeper than 60°, 0x31). Ctrl deletes
    // fences (int(cost x 0.8) back a section; posts left with no fence go).
    Result drawFences(int level, int x0, int y0, int x1, int y1, const BuildFence& fence);
    Result deleteFences(int level, int x0, int y0, int x1, int y1);

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
    // A ground tile under a foundation.
    bool foundationTile(int x, int y) const;

    // Pool tool (deckfoundations.txt "pool 3"): a rectangle of ground tiles
    // (corners inclusive) dug into a pool: the lot gets its underground
    // level if it has none, the terrain round it levelled to the first
    // corner (0x81), the pool's floor ("floor_poolRoundedLipBlue") one
    // story down, pool walls (style 29) round the edge there and a POOL
    // entry (the water 0.25 under the ground). §75 a tile. Only on exposed
    // terrain of the ground (0x8A), not into another pool (0x86), water
    // (0x1E) or objects (0x0C). Sims do not walk over it.
    Result drawPool(int x0, int y0, int x1, int y1);
    // Ctrl: the pool under a tile goes (int(75 x 0.8) back a tile); not
    // with a Sim in it (0x2D) or a ladder or diving board on it (0x2E).
    Result deletePool(int x, int y);
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
    // A floor tile could go there above the ground (IsFloorable).
    bool floorable(int level, int x, int y) const;

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
    // Leaving Build mode: the history goes (cTSUserToolUndoManager reset),
    // and what was sold with the walls dies (`forget` is told each part
    // first).
    void resetHistory(const std::function<void(ObjectId)>& forget = {});

    // Bumped by every change, for whoever draws the lot.
    uint32_t revision() const { return revision_; }
    // The world again from the lot and the VM's doors (after the object
    // tool placed or took a door or window).
    void syncWorld();

    const Lot& lot() const { return lot_; }

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
        std::vector<std::array<uint8_t, 4>> floorRotations;
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
    };
    Geometry capture() const;
    void restore(const Geometry& g);
    // After a change: rooms, then the VM.
    void changed();
    void commit(Geometry before, int cost, std::vector<Sold> sold = {}, std::vector<Sold> created = {});
    // The objects attached to a wall section (lead IDs), and whether one
    // of them is in use.
    std::vector<ObjectId> attachedObjects(int level, int ax, int ay, int bx, int by, bool* inUse) const;
    // Levels the ground's grid points to `height` (the lot's level 0);
    // false (nothing changed) if one of them is held by a floor or by a
    // wall that is not among `free`.
    bool levelPoints(const std::vector<std::array<int, 2>>& points, float height,
                     const std::vector<std::array<int, 4>>& free = {});
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
    std::optional<Stroke> stroke_;
    // The flight a modular stair object belongs to: its objects (steps first,
    // top to bottom).
    std::vector<ObjectId> flightOf(ObjectId part) const;
    struct StairsPlan;
    int planStairs(StairsPlan& plan, TilePos top, int direction, std::optional<std::array<float, 2>> end,
                   bool wholeTiles) const;
    // A ground grid point no floor, wall or foundation holds.
    bool terrainFree(int x, int y) const;
    uint32_t revision_ = 0;
};

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

// Splits a run between two grid points into tile-long sections (along an
// axis or a 45° diagonal); empty for other slopes.
std::vector<std::array<int, 4>> wallSections(int x0, int y0, int x1, int y1);

} // namespace os2::vm
