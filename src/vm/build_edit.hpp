#pragma once

// Build mode's wall, floor and wall-covering tools over a lot in the VM
// (docs/buy-build-mac.md §7): the changes to the lot's geometry
// (formats/lot_edit.hpp) with the game's rules and prices, kept in step with
// the VM (the world's walls and doors for routing, the rooms and the room of
// each object, the family's funds), undoable as the game's
// cTSUserToolUndoableWall / FloorPattern / Wallpaper are. Levels are the
// VM's (1 = the lot's ground), coordinates the lot's grid (tiles).

#include "formats/build_catalog.hpp"
#include "formats/lot.hpp"
#include "vm.hpp"

#include <cstdint>
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
    // already are skipped; one that breaks a rule stops the whole run
    // (nothing is built), as does a price over the family's funds.
    Result drawWalls(int level, int x0, int y0, int x1, int y1);
    // Room tool: the walls around the rectangle between two grid points.
    Result drawRoom(int level, int x0, int y0, int x1, int y1);
    // Ctrl-drag: the walls along the run go for int(70 x 0.8) = 56 a
    // section (the full price comes back only by undo).
    Result deleteWalls(int level, int x0, int y0, int x1, int y1);

    // Floor tool over the tiles of a rectangle (corners inclusive), or the
    // given quarters of one tile (`quarters` bits: west, north, east, south).
    // A quarter costs a quarter of the pattern's price; covering another
    // pattern pays the new one in full; a null pattern erases (a quarter of
    // floor(price x 0.8) back).
    Result paintFloor(int level, int x0, int y0, int x1, int y1, const BuildPattern* pattern, int quarters = 0xF);

    // Wallpaper tool: the faces of the walls along a run, on its right
    // (side +1, seen from (x0, y0) to (x1, y1)), left (-1) or both (0), at
    // the pattern's price per side.
    // A null pattern takes the coverings off (Ctrl).
    Result paintWalls(int level, int x0, int y0, int x1, int y1, int side, const BuildPattern* pattern);
    // Shift: every wall face of the room of a tile (the game floods the
    // ring of faces that had the clicked face's pattern; here the whole
    // room: simplified).
    Result paintRoom(int level, int x, int y, const BuildPattern* pattern);

    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // Leaving Build mode: the history goes (cTSUserToolUndoManager reset).
    void resetHistory();

    // Bumped by every change, for whoever draws the lot.
    uint32_t revision() const { return revision_; }
    // The world again from the lot and the VM's doors (after the object
    // tool placed or took a door or window).
    void syncWorld();

    const Lot& lot() const { return lot_; }

    // Why a wall section cannot go there ("" if it can).
    // `code` gets the game's error code (UserMayPlacePartition: 0x40 out of
    // the lot, 0x45 not editable, 0x48 no support, 0x49 slope, 0x4B Sim,
    // 0x0C object).
    std::string wallBlocked(int level, int ax, int ay, int bx, int by, int* code = nullptr) const;

private:
    struct Geometry {
        std::vector<Lot::Wall> walls;
        std::vector<Lot::WallGraph> wallGraphs;
        std::vector<std::array<uint16_t, 4>> floors, rooms;
        std::map<uint32_t, std::vector<std::array<uint16_t, 4>>> roomLayers;
        std::map<uint16_t, std::string> floorPatterns, wallPatterns;
        std::map<uint32_t, Lot::LayerEntry> staleWallLayer;
    };
    struct Op {
        Geometry before, after;
        int cost = 0;
    };
    Geometry capture() const;
    void restore(const Geometry& g);
    // After a change: rooms, then the VM.
    void changed();
    void commit(Geometry before, int cost);
    Result cover(std::vector<std::pair<uint32_t, int>> faces, const BuildPattern* pattern, const std::string& what);
    int priceOf(const std::string& lotName, bool floor) const;

    VM& vm_;
    Lot& lot_;
    const BuildCatalog* catalog_ = nullptr;
    std::vector<Op> undo_, redo_;
    std::map<std::string, int> prices_; // patterns laid this session, by lot name
    uint32_t revision_ = 0;
};

// Splits a run between two grid points into tile-long sections (along an
// axis or a 45° diagonal); empty for other slopes.
std::vector<std::array<int, 4>> wallSections(int x0, int y0, int x1, int y1);

} // namespace os2::vm
