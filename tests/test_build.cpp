// Build mode on a synthetic lot: walls into the graphs, rooms from the
// walls, floors, wall coverings, prices, undo and the VM's world.
#include "formats/build_catalog.hpp"
#include "formats/design_catalog.hpp"
#include "formats/lot.hpp"
#include "formats/lot_edit.hpp"
#include "vm/build_edit.hpp"
#include "vm/placement.hpp"
#include "vm/vm.hpp"

#include <cmath>
#include <cstdio>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// A flat 12x10 lot with one level (the ground), the outdoors one room (1),
// and the game's empty wall graphs.
static Lot flatLot() {
    Lot lot;
    lot.width = 12, lot.height = 10, lot.levelCount = 1;
    lot.elevations.assign(13 * 11, 0.0f);
    lot.floors.assign(12 * 10, {});
    lot.rooms.assign(12 * 10, {1, 1, 1, 1});
    lot.roomLayers[0x0A] = lot.rooms;
    lot.roomLayers[0x0B] = lot.rooms;
    for (uint32_t instance : {5u, 6u, 7u, 8u, 0x13u, 0x16u, 0x17u, 0x18u}) {
        Lot::WallGraph g;
        g.instance = instance;
        g.header = {0, 0, 0, 12, 10, 1, 0, 0};
        g.rooms = {0, 1};
        lot.wallGraphs.push_back(g);
    }
    lot.floorPatterns[1] = "sidewalk";
    return lot;
}

static uint16_t roomAt(const Lot& lot, int x, int y, int q = 0) { return lot.rooms[size_t(x) * lot.height + size_t(y)][size_t(q)]; }

static void testSections() {
    check(wallSections(0, 0, 3, 0).size() == 3, "straight run splits into tiles");
    check(wallSections(2, 2, 0, 0).size() == 2, "diagonal run splits into tiles");
    check(wallSections(0, 0, 2, 1).empty(), "other slopes make no walls");
}

static void testRooms() {
    Lot lot = flatLot();
    // A 3x3 room from (2,2) to (5,5).
    for (const auto& s : {std::array<int, 4>{2, 2, 5, 2}, {5, 2, 5, 5}, {5, 5, 2, 5}, {2, 5, 2, 2}})
        for (const auto& t : wallSections(s[0], s[1], s[2], s[3]))
            check(addWall(lot, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF}).has_value(), "wall added");
    check(!addWall(lot, 0, 2, 2, 3, 2, 1, {0xFFFF, 0xFFFF}), "no second wall on a wall");
    check(lot.walls.size() == 12, "12 wall sections");
    size_t inGraph7 = 0, inGraph17 = 0;
    for (const auto& g : lot.wallGraphs)
        (g.instance == 7 ? inGraph7 : g.instance == 0x17 ? inGraph17 : inGraph7) += g.instance == 7 || g.instance == 0x17 ? g.lines.size() : 0;
    check(inGraph7 == 12 && inGraph17 == 0, "walls go into the light graph, not the attic one");
    updateRooms(lot);
    const uint16_t inside = roomAt(lot, 3, 3), outside = roomAt(lot, 0, 0);
    check(outside == 1, "the outdoors keeps its room");
    check(inside != 0 && inside != outside, "the closed room gets a new ID");
    check(roomAt(lot, 4, 4, 2) == inside && roomAt(lot, 2, 2, 0) == inside, "all of the inside shares it");
    check(roomAt(lot, 1, 3, 2) == outside, "the quarter outside the wall is outdoors");
    // The graph lines know their rooms: wall (2,2)->(3,2) has the room on its left (+y).
    for (const auto& g : lot.wallGraphs)
        if (g.instance == 5)
            for (const auto& l : g.lines)
                if (l.id == lot.walls[0].id)
                    check(l.roomLeft == inside && l.roomRight == outside, "line rooms on their sides");
    // A diagonal splits it in two; the bigger part keeps the ID.
    check(addWall(lot, 0, 2, 2, 3, 3, 1, {0xFFFF, 0xFFFF}).has_value(), "diagonal wall");
    check(addWall(lot, 0, 3, 3, 4, 4, 1, {0xFFFF, 0xFFFF}).has_value(), "diagonal wall 2");
    check(addWall(lot, 0, 4, 4, 5, 5, 1, {0xFFFF, 0xFFFF}).has_value(), "diagonal wall 3");
    updateRooms(lot);
    check(roomAt(lot, 3, 3, QNorth) != roomAt(lot, 3, 3, QEast), "the diagonal separates the quarters");
    check(roomAt(lot, 3, 3, QNorth) == roomAt(lot, 2, 4, QEast), "west half is one room");
    // Taking the walls down joins everything again.
    for (const auto& t : wallSections(2, 2, 5, 5))
        check(removeWall(lot, 0, t[0], t[1], t[2], t[3]).has_value(), "diagonal removed");
    check(removeWall(lot, 0, 3, 2, 2, 2).has_value(), "wall removed either way round");
    updateRooms(lot);
    check(roomAt(lot, 3, 3) == outside, "an open room is outdoors again");
}

static void testEditor() {
    Lot lot = flatLot();
    // The street along y = 0.
    for (int x = 0; x < 12; ++x)
        lot.floors[size_t(x) * 10] = {1, 1, 1, 1};
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetDiv10000] = 0;
    vm.globals[global::BudgetMod10000] = 2000;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawWalls(1, 2, 0, 2, 3);
    check(!r.ok && funds(vm) == 2000, "no walls on the street");
    r = ed.drawRoom(1, 2, 2, 6, 5);
    check(r.ok && r.changed == 14 && r.cost == 14 * kWallSectionPrice, "room tool: 14 sections at §70");
    check(funds(vm) == 2000 - 980, "the walls are paid for");
    check(vm.world->wallBetween({2, 2, 1}, 0, -1), "the world has the wall for routing");
    check(vm.world->room({3, 3, 1}) != vm.world->room({0, 3, 1}), "the world has the new room");
    BuildPattern carpet;
    carpet.guid = 0x1234, carpet.floor = true, carpet.price = 8, carpet.name = "carpet";
    r = ed.paintFloor(1, 2, 2, 3, 2, &carpet);
    check(r.ok && r.changed == 8 && r.cost == 16, "two tiles of carpet at §8 a tile");
    check(lot.floorPatterns.count(2) && lot.floorPatterns[2] == "1234", "the pattern gets a reference by GUID");
    r = ed.paintFloor(1, 3, 2, 3, 2, nullptr);
    check(r.ok && r.cost == -6, "erasing a tile gives back floor(8 x 0.8) a tile");
    BuildPattern paint;
    paint.guid = 0xabc, paint.price = 3, paint.name = "paint";
    r = ed.paintRoom(1, 3, 3, &paint);
    check(r.ok && r.changed == 14 && r.cost == 42, "the room's 14 wall faces at §3");
    r = ed.paintWalls(1, 2, 2, 6, 2, -1, &paint);
    check(r.ok && r.changed == 0, "the inside faces are painted already");
    BuildPattern paper, tile;
    paper.guid = 0xdef, paper.price = 2, paper.name = "paper";
    tile.guid = 0x987, tile.price = 1, tile.name = "tile";
    r = ed.paintWalls(1, 2, 2, 6, 2, -1, &paper);
    check(r.ok && r.changed == 4, "the south wall's inside papered");
    r = ed.paintFlood(1, 6, 2, 6, 3, -1, &tile);
    check(r.ok && r.changed == 10 && r.cost == 10, "Shift covers only the connected faces with the clicked pattern");
    r = ed.paintFlood(1, 3, 2, 2, 2, 1, &paint);
    check(r.ok && r.changed == 4, "Shift on the papered faces takes those four");
    r = ed.paintWalls(1, 2, 2, 6, 2, 1, &paint);
    check(r.ok && r.changed == 4, "the outside faces of a run");
    r = ed.paintWalls(1, 2, 2, 6, 2, 1, nullptr);
    check(r.ok && r.changed == 4 && r.cost == -8, "taking paint off gives back int(3 x 0.8) a face");
    const int before = funds(vm);
    r = ed.deleteWalls(1, 2, 2, 6, 2);
    check(r.ok && r.changed == 4 && r.cost == -224, "walls come back at int(70 x 0.8) a section");
    check(!vm.world->wallBetween({2, 2, 1}, 0, -1), "the wall is gone from the world");
    check(ed.undo() && funds(vm) == before && vm.world->wallBetween({2, 2, 1}, 0, -1), "undo puts it back");
    check(ed.redo() && funds(vm) == before + 224, "redo takes it again");
    while (ed.undo()) {
    }
    check(funds(vm) == 2000 && lot.walls.empty(), "undoing everything restores the lot and the funds");
}

// Round 9: the ground levelled under walls and floors, fences and their
// posts, floor rotation, a click's quarters, the upper floors' support.
static void testRound9() {
    Lot lot = flatLot();
    // A slope along x: elevation = x / 4.
    for (int x = 0; x <= 12; ++x)
        for (int y = 0; y <= 10; ++y)
            lot.elevations[size_t(x) * 11 + size_t(y)] = float(x) * 0.25f;
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetDiv10000] = 0;
    vm.globals[global::BudgetMod10000] = 5000;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawWalls(1, 2, 3, 6, 3);
    check(r.ok && r.changed == 4, "a wall across a slope");
    check(lot.elevation(0, 6, 3) == 0.5f && lot.elevation(0, 4, 3) == 0.5f, "the ground under it is levelled to the start");
    check(lot.elevation(0, 6, 4) == 1.5f, "the ground beside it is not");
    check(ed.undo() && lot.elevation(0, 6, 3) == 1.5f, "undo puts the ground back");
    // Floors level the rectangle to the first tile's height.
    BuildPattern tile;
    tile.guid = 0x77, tile.floor = true, tile.price = 4, tile.name = "tile";
    r = ed.paintFloor(1, 8, 5, 9, 6, &tile, 0xF, 4);
    check(r.ok && r.changed == 16, "a floor on the slope");
    check(lot.elevation(0, 10, 7) == lot.elevation(0, 8, 5) && lot.elevation(0, 8, 5) == 2.125f,
          "the ground under the floor is level at the first tile's mean height");
    check(floorRotation(lot, 0, 8, 5, 0) == 4, "the floor keeps its rotation");
    r = ed.paintFloor(1, 8, 5, 8, 5, &tile, 0xF, 4);
    check(r.ok && r.changed == 0, "the same pattern and rotation is no change");
    r = ed.paintFloor(1, 8, 5, 8, 5, &tile, 0xF, 2);
    check(r.ok && r.changed == 4 && r.cost == 4, "another rotation pays the pattern again");
    // A diagonal wall: a click floors the half under the mouse.
    ed.drawWalls(1, 2, 6, 3, 7);
    check(ed.clickQuarters(1, 2.2f, 6.8f) == 0x3 && ed.clickQuarters(1, 2.8f, 6.2f) == 0xC, "a click's half of a diagonal");
    check(ed.clickQuarters(1, 5.5f, 5.5f) == 0xF, "a click floors a whole tile");
    // Fences: their cost a section, a post at each point, the refund.
    BuildFence picket;
    picket.guid = 0x12345678, picket.price = 10, picket.name = "picket";
    BuildCatalog none;
    const int before = funds(vm);
    r = ed.drawFences(1, 1, 8, 4, 8, picket);
    check(r.ok && r.changed == 3 && r.cost == 30, "three fence sections at their cost");
    check(lot.fencePosts.size() == 4, "a post at each grid point of the run");
    check(funds(vm) == before - 30, "the fence is paid for");
    r = ed.deleteFences(1, 3, 8, 4, 8);
    check(r.ok && r.changed == 1 && lot.fencePosts.size() == 3, "the post left with no fence goes");
    check(ed.undo() && lot.fencePosts.size() == 4 && funds(vm) == before - 30, "undo puts the fence back");
    // Upper floors: supported from below or near a supported tile.
    Lot two = flatLot();
    two.levelCount = 2;
    two.elevations.resize(13 * 11 * 2, 3.0f);
    two.floors.resize(12 * 10 * 2);
    two.rooms.resize(12 * 10 * 2);
    for (auto& [k, l] : two.roomLayers)
        l.resize(12 * 10 * 2);
    for (int x = 2; x < 5; ++x)
        for (int y = 2; y < 5; ++y)
            two.floors[size_t(x) * 10 + size_t(y)] = {1, 1, 1, 1};
    VM vm2(code);
    vm2.globals[global::BudgetMod10000] = 5000;
    vm2.world = World::fromLot(two);
    BuildEditor ed2(vm2, two);
    check(ed2.floorable(2, 3, 3), "a tile over a floor is floorable");
    check(!ed2.floorable(2, 9, 9), "a tile over nothing is not");
    r = ed2.paintFloor(2, 9, 9, 9, 9, &tile);
    check(!r.ok && r.error == 0x52, "flooring in the air is refused (0x52)");
    r = ed2.paintFloor(2, 2, 2, 5, 2, &tile);
    check(r.ok && ed2.floorable(2, 6, 2) && !ed2.floorable(2, 8, 8), "a floor reaches out next to a supported tile");
}

// Room IDs as cWallGraph makes them: outdoors 0, a new room the smallest
// free ID, opening a room onto the outdoors gives it back to room 0.
static void testRoomIds() {
    Lot lot = flatLot();
    for (auto* m : {&lot.rooms, &lot.roomLayers[0x0A], &lot.roomLayers[0x0B]})
        m->assign(12 * 10, {0, 0, 0, 0});
    auto room = [&](int x0, int y0, int x1, int y1) {
        for (const auto& s : std::array<std::array<int, 4>, 4>{{{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}})
            for (const auto& t : wallSections(s[0], s[1], s[2], s[3]))
                addWall(lot, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF});
        updateRooms(lot);
    };
    room(1, 1, 3, 3);
    room(5, 1, 7, 3);
    check(roomAt(lot, 2, 2) == 1 && roomAt(lot, 6, 2) == 2 && roomAt(lot, 0, 0) == 0, "rooms 1 and 2, outdoors 0");
    removeWall(lot, 0, 1, 1, 2, 1);
    updateRooms(lot);
    check(roomAt(lot, 2, 2) == 0 && roomAt(lot, 6, 2) == 2, "an opened room goes back to the outdoors");
    room(1, 5, 3, 7);
    check(roomAt(lot, 2, 6) == 1, "a new room takes the smallest free ID");
    // A fence splits the placement rooms (0x09) only.
    for (const auto& t : wallSections(8, 0, 8, 10))
        addWall(lot, 0, t[0], t[1], t[2], t[3], 0x12345678, {0xFFFF, 0xFFFF});
    updateRooms(lot);
    const auto& light = lot.roomLayers[0x0B];
    check(roomAt(lot, 10, 5) == 0 && light[size_t(10) * 10 + 5][0] == 0, "fences cut no room off the lot's edge");

    // Removing the wall between two rooms keeps the right-hand room's ID
    // (SetRoomsPreDetach), whatever their sizes.
    Lot two = flatLot();
    for (auto* m : {&two.rooms, &two.roomLayers[0x0A], &two.roomLayers[0x0B]})
        m->assign(12 * 10, {0, 0, 0, 0});
    auto box = [&](int x0, int y0, int x1, int y1) {
        for (const auto& s : std::array<std::array<int, 4>, 4>{{{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}})
            for (const auto& t : wallSections(s[0], s[1], s[2], s[3]))
                addWall(two, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF});
        updateRooms(two);
    };
    box(1, 1, 3, 4); // room 1, the larger
    box(3, 1, 5, 3); // room 2, east of it: the wall (3,1)-(3,3) has room 2 on its right
    check(roomAt(two, 2, 2) == 1 && roomAt(two, 4, 2) == 2, "rooms 1 and 2 side by side");
    std::vector<RoomMerge> merges;
    removeWall(two, 0, 3, 1, 3, 2, &merges);
    removeWall(two, 0, 3, 2, 3, 3, &merges);
    updateRooms(two, merges);
    check(roomAt(two, 2, 2) == 2 && roomAt(two, 4, 2) == 2 && roomAt(two, 2, 3) == 2,
          "the joined room keeps the right-hand side's ID");
    merges.clear();
    removeWall(two, 0, 1, 1, 2, 1, &merges);
    updateRooms(two, merges);
    check(roomAt(two, 2, 2) == 0 && roomAt(two, 4, 2) == 0, "joined with the outdoors it is room 0");
}

static void testFoundation() {
    Lot lot = flatLot();
    lot.levelCount = 3;
    lot.elevations.resize(13 * 11 * 3);
    for (size_t i = 0; i < 13 * 11 * 3; ++i)
        lot.elevations[i] = 3.0f * float(i / (13 * 11));
    lot.floors.resize(12 * 10 * 3);
    lot.rooms.resize(12 * 10 * 3);
    for (auto& [k, l] : lot.roomLayers)
        l.resize(12 * 10 * 3);
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawFoundation(2, 2, 4, 3);
    check(r.ok && r.changed == 6 && r.cost == 6 * kFoundationTilePrice, "six foundation tiles at §4");
    check(lot.elevation(1, 3, 3) == kFoundationHeight && lot.elevation(2, 3, 3) == 3.0f + kFoundationHeight,
          "the stories above sit a foundation higher");
    check(lot.elevation(1, 8, 8) == 3.0f, "elsewhere they do not");
    check(ed.foundationTile(3, 2) && !ed.foundationTile(5, 2), "the tiles are foundation tiles");
    size_t walls = 0;
    for (const auto& w : lot.walls)
        walls += w.type == Lot::Foundation;
    check(walls == 10, "foundation walls round its edge");
    check(lot.floor(1, 3, 3)[0] != 0, "the foundation has its floor");
    r = ed.deleteFoundation(4, 2, 4, 3);
    check(r.ok && r.changed == 2 && r.cost == -2 * 3, "two tiles back at int(4 x 0.8)");
    walls = 0;
    for (const auto& w : lot.walls)
        walls += w.type == Lot::Foundation;
    check(walls == 8 && lot.elevation(1, 5, 2) == 3.0f && lot.elevation(1, 4, 2) == kFoundationHeight,
          "the walls and heights follow what is left");
    check(ed.undo() && ed.undo() && lot.walls.empty() && lot.elevation(1, 3, 3) == 3.0f, "undo takes it all back");
}

static void testTerrain() {
    Lot lot = flatLot();
    lot.floors[size_t(1) * 10] = {1, 1, 1, 1}; // the sidewalk on tile (1, 0)
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    std::vector<uint8_t> profile(32 * 32, 255);
    auto r = ed.terrainStroke(BuildEditor::TerrainOp::Raise, 6, 5, 10, 1, profile);
    check(r.ok && r.changed > 0 && lot.elevation(0, 6, 5) > 0.4f, "raising the ground");
    check(lot.elevation(0, 0, 0) == 0.0f, "only under the brush");
    const float h = lot.elevation(0, 6, 5);
    check(std::fabs(h / 0.00625f - std::round(h / 0.00625f)) < 1e-3f, "heights stay on the terrain step");
    check(r.cost == funds(vm) * 0 + r.cost && r.cost > 0 && funds(vm) == 5000 - r.cost, "the change is paid for");
    check(ed.undo() && lot.elevation(0, 6, 5) == 0.0f && funds(vm) == 5000, "undo flattens it again");
    r = ed.terrainStroke(BuildEditor::TerrainOp::Raise, 1.5f, 0.5f, 3, 1, profile);
    check(!r.ok && r.error == 0x8B, "a floored tile is not exposed terrain");
    r = ed.terrainStroke(BuildEditor::TerrainOp::Paint, 6, 5, 4, 1, profile, "terrain-grass-b-paint");
    check(r.ok && lot.paints.size() == 1 && lot.maskWidth == 49, "painting adds a paint and its mask");
}

static void testRoof() {
    Lot lot = flatLot();
    lot.levelCount = 2;
    lot.elevations.resize(13 * 11 * 2, 3.0f);
    lot.floors.resize(12 * 10 * 2);
    lot.rooms.assign(12 * 10 * 2, {0, 0, 0, 0}); // outdoors (auto-roof reads the rooms)
    for (auto& [k, l] : lot.roomLayers)
        l = lot.rooms;
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    ed.drawRoom(1, 2, 2, 8, 5);
    auto r = ed.drawRoof(1, 2, 2, 8, 5, Lot::LongGable);
    check(r.ok && lot.roofs.size() == 1 && lot.roofs[0].level == 1 && r.cost == 0, "a free roof over the walls");
    size_t attic = 0;
    for (const auto& w : lot.walls)
        attic += w.type == Lot::Attic && w.a.level == 1;
    check(attic == 6, "attic walls on the two gable ends (the short sides)");
    r = ed.drawRoof(1, 2, 2, 8, 5, Lot::Hip);
    r = ed.deleteRoof(1, 5, 3);
    check(r.ok && lot.roofs.size() == 1, "deleting the roof on top");
    r = ed.deleteRoof(1, 5, 3);
    attic = 0;
    for (const auto& w : lot.walls)
        attic += w.type == Lot::Attic;
    check(r.ok && lot.roofs.empty() && attic == 0, "the attic walls go with the roof");

    // Auto-roof: the room (6 x 3 tiles, wider than deep) gets one rectangle.
    r = ed.autoRoof(3, false);
    check(r.ok && lot.roofs.size() == 1 && lot.roofs[0].type == Lot::Hip && lot.roofs[0].x0 == 2 &&
              lot.roofs[0].x1 == 8 && lot.roofs[0].y0 == 2 && lot.roofs[0].y1 == 5,
          "tool 3 puts a hip roof over a room no deeper than wide");
    r = ed.autoRoof(4, false);
    check(r.ok && lot.roofs.size() == 1 && lot.roofs[0].type == Lot::ShortGable,
          "the tool's earlier roofs go; tool 4 makes a short gable over a wide room");
    r = ed.autoRoof(1, true);
    check(r.ok && lot.roofs.size() == 1, "preserving: the roofed room gets nothing more");
    r = ed.removeRoofs();
    check(r.ok && lot.roofs.empty(), "C300 removes the roofs");
}

static void testPool() {
    Lot lot = flatLot();
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawPool(3, 3, 5, 4);
    check(r.ok && r.cost == 6 * kPoolTilePrice && lot.pools.size() == 1, "a pool of six tiles at §75");
    check(lot.minLevel == -1 && lot.levelCount == 2 && lot.elevation(-1, 4, 4) == -3.0f, "the lot gets its underground level");
    check(lot.floor(-1, 4, 3)[0] != 0 && lot.floor(0, 4, 3)[0] == 0, "the pool's floor is a level down");
    size_t walls = 0;
    for (const auto& w : lot.walls)
        walls += w.type == Lot::Pool && w.a.level == -1;
    check(walls == 10, "pool walls round it");
    check(vm.world->pool({4, 3, 1}) && !vm.world->walkable({4, 3, 1}), "nobody walks over the water");
    r = ed.drawPool(5, 4, 6, 6);
    check(!r.ok && r.error == 0x86, "no pool into a pool");
    r = ed.deletePool(4, 4);
    check(r.ok && lot.pools.empty() && r.cost == -6 * 60, "deleting it gives back int(75 x 0.8) a tile");
    check(ed.undo() && ed.undo() && lot.minLevel == 0 && lot.levelCount == 1 && funds(vm) == 5000,
          "undo takes the level away again");
}

// Modular stairs (§7.12): how many steps a flight gets, and the refusals.
// (The objects need the game's OBJDs: lot-save --stairs checks those.)
static void testStairs() {
    Lot lot = flatLot();
    lot.levelCount = 3;
    lot.elevations.resize(13 * 11 * 3);
    for (size_t i = 0; i < 13 * 11 * 3; ++i)
        lot.elevations[i] = 3.0f * float(i / (13 * 11));
    lot.floors.resize(12 * 10 * 3);
    lot.rooms.resize(12 * 10 * 3);
    for (auto& [k, l] : lot.roomLayers)
        l.resize(12 * 10 * 3);
    lot.floorPatterns[2] = "ccc26098";
    for (int x = 2; x <= 4; ++x)
        for (int y = 2; y <= 3; ++y)
            for (int q = 0; q < 4; ++q)
                setFloor(lot, 1, x, y, q, 2);
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    int error = 0;
    check(ed.stairsSteps({4, 2, 2}, 4, std::nullopt, false, &error) == 16 && error == 0,
          "a story down to the ground in 16 steps, the end found by itself");
    check(ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{9.0f, 2.5f}, false) == 16, "dragged to where the last step goes");
    check(ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{6.2f, 2.5f}, true) == 8, "whole tiles round up to 4 a tile");
    // A short flight: the tile past its last step is brought to it (the
    // binary sets that tile's corners to the last step's height).
    check(ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{6.2f, 2.5f}, false, &error) == 5 && error == 0,
          "a short drag makes a short flight");
    ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{4.6f, 2.5f}, false, &error);
    check(error == 0x78, "no steps on the top tile itself (0x78)");
    ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{7.5f, 5.5f}, false, &error);
    check(error == 0x71, "the end off the line of the flight (0x71)");
    ed.stairsSteps({7, 7, 2}, 4, std::nullopt, false, &error);
    check(error == 0x6C, "no floor on the top tile (0x6C)");
    // A hill under the flight: the ground over the steps is lowered.
    for (int x = 6; x <= 7; ++x)
        for (int y = 0; y <= 10; ++y)
            lot.elevations[size_t(x) * 11 + size_t(y)] = 0.5f;
    check(ed.stairsSteps({4, 2, 2}, 4, std::nullopt, false, &error) > 0 && error == 0, "a flight down a slope");
    ed.drawWalls(1, 7, 1, 7, 4);
    ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{9.0f, 2.5f}, false, &error);
    check(error == 0x76 || error == 0x6F, "a wall across the flight stops it");
}

// The Design tool's choices (MMAT property sets): the model's first two
// subsets with choices, clean materials only, the default first.
static void testDesignCatalog() {
    DesignCatalog c;
    auto mmat = [&](const char* subset, const char* name, int def, int state) {
        c.add({{"type", "modelMaterial"}, {"modelName", "sofaTest_cres"}, {"subsetName", subset}, {"name", name},
               {"defaultMaterial", std::to_string(def)}, {"materialStateFlags", std::to_string(state)}, {"family", name}});
    };
    mmat("wood", "sofatest_wood_dark", 0, 0);
    mmat("cushion", "sofatest_cushion_red", 1, 0);
    mmat("cushion", "sofatest_cushion_blue", 0, 0);
    mmat("cushion", "sofatest_cushion_blue_dirty", 0, 2);
    mmat("legs", "sofatest_legs_steel", 1, 0);
    const auto subsets = c.subsets("SofaTest", {"groundshadow", "Cushion", "wood", "legs"});
    check(subsets.size() == 2 && subsets[0] == "cushion" && subsets[1] == "wood", "the first two subsets with choices, in the model's order");
    const auto opts = c.options("sofatest_cres", "cushion");
    check(opts.size() == 2 && opts[0]->material == "sofatest_cushion_red" && opts[1]->material == "sofatest_cushion_blue",
          "clean choices, the default first");
    check(c.subsets("other", {}).empty(), "a model without choices is not designable");
}

// Low fences (maywalkover) bound placement but Sims walk over them.
static void testWalkOver() {
    Lot lot = flatLot();
    addWall(lot, 0, 2, 2, 3, 2, 0x1234567, {0xFFFF, 0xFFFF}); // a walk-over fence
    addWall(lot, 0, 5, 2, 6, 2, 0x7654321, {0xFFFF, 0xFFFF}); // a tall one
    setWalkOverFences({0x1234567});
    const World w = World::fromLot(lot);
    check(!w.wallBetween({2, 2, 1}, 0, -1) && w.wallBetween({2, 2, 1}, 0, -1, true),
          "a walk-over fence does not block routing but blocks placement");
    check(w.wallBetween({5, 2, 1}, 0, -1), "another fence blocks routing");
    setWalkOverFences({});
}

int main() {
    testStairs();
    testDesignCatalog();
    testWalkOver();
    testPool();
    testRoof();
    testTerrain();
    testFoundation();
    testRoomIds();
    testSections();
    testRooms();
    testEditor();
    testRound9();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::printf("build: all passed\n");
    return 0;
}
