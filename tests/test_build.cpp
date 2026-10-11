// Build mode on a synthetic lot: walls into the graphs, rooms from the
// walls, floors, wall coverings, prices, undo and the VM's world.
#include "formats/build_catalog.hpp"
#include "formats/lot.hpp"
#include "formats/lot_edit.hpp"
#include "vm/build_edit.hpp"
#include "vm/placement.hpp"
#include "vm/vm.hpp"

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

int main() {
    testSections();
    testRooms();
    testEditor();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::printf("build: all passed\n");
    return 0;
}
