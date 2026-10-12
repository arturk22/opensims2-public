// World occupancy, pathfinding and the routing primitives on synthetic lots.
#include "vm/go_here.hpp"
#include "vm/interactions.hpp"
#include "vm/path_planner.hpp"
#include "vm/vm.hpp"
#include "vm/world.hpp"
#include "formats/lot.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr uint32_t kGroup = 0x7F000001;

using Target = Behavior::Target;
using Kind = Behavior::TargetKind;
static const Target T{Kind::True, 0}, F{Kind::False, 0}, E{Kind::Error, 0};
static Target go(uint16_t i) { return {Kind::Instruction, i}; }

static Behavior::Instruction instr(uint16_t opcode, Target onTrue, Target onFalse,
                                   std::initializer_list<uint8_t> operands = {}) {
    Behavior::Instruction in;
    in.opcode = opcode;
    in.onTrue = onTrue;
    in.onFalse = onFalse;
    size_t i = 0;
    for (auto b : operands)
        in.operands[i++] = b;
    return in;
}

static Behavior::Instruction expr(uint8_t lo, int16_t ld, uint8_t op, uint8_t ro, int16_t rd, Target t, Target f) {
    return instr(0x02, t, f,
                 {uint8_t(ld), uint8_t(uint16_t(ld) >> 8), uint8_t(rd), uint8_t(uint16_t(rd) >> 8), 0, op, lo, ro});
}

static Behavior tree(std::vector<Behavior::Instruction> code, uint8_t args = 0, uint8_t locals = 0) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.argCount = args;
    b.localCount = locals;
    b.instructions = std::move(code);
    return b;
}

enum : uint8_t { Temp = 0x08, Lit = 0x07, StackId = 0x0A, Local = 0x19, Data = 0x03 };
enum : uint8_t { Assign = 5, Eq = 2 };

// An object placed at the centre of a tile.
static Object& place(VM& vm, int x, int y, int level = 1) {
    Object& o = vm.createObject(kGroup);
    o.position = {float(x) + 0.5f, float(y) + 0.5f, level};
    return o;
}

static void testDirections() {
    check(directionStep(0) == std::array<int, 2>{-1, 0} && directionStep(2) == std::array<int, 2>{0, 1} &&
              directionStep(4) == std::array<int, 2>{1, 0} && directionStep(6) == std::array<int, 2>{0, -1},
          "direction steps");
    bool roundTrip = true;
    for (int d = 0; d < 8; ++d) {
        const auto [dx, dy] = directionStep(d);
        roundTrip &= directionOf(float(dx), float(dy)) == d;
    }
    check(roundTrip && directionOf(0, 0) == -1 && directionStep(-2) == directionStep(6), "directions round trip");
}

static void testWorldWalls() {
    World w(6, 6);
    w.addWall(1, 2, 0, 2, 4); // a wall along x = 2, from y = 0 to 4: separates column 1 from column 2
    check(w.wallBetween({1, 1, 1}, 1, 0) && w.wallBetween({2, 3, 1}, -1, 0), "wall between columns");
    check(!w.wallBetween({1, 4, 1}, 1, 0) && !w.wallBetween({1, 1, 1}, 0, 1), "wall ends");
    BlockedGrid open(36, 0);
    check(!canStep(w, open, {1, 1, 1}, 1, 0) && canStep(w, open, {1, 4, 1}, 1, 0), "steps across walls");
    check(!canStep(w, open, {1, 3, 1}, 1, 1) && canStep(w, open, {1, 4, 1}, 1, 1), "no cutting the wall's end");
    BlockedGrid blocked = open;
    blocked[w.index(4, 5)] = 1;
    check(!canStep(w, blocked, {3, 4, 1}, 1, 1) && !canStep(w, blocked, {4, 4, 1}, 0, 1), "objects block, also corners");
    check(!w.lineOfSight({0, 1, 1}, {4, 2, 1}) && w.lineOfSight({0, 5, 1}, {5, 4, 1}), "line of sight");
    w.openDoor({1, 1, 1}, 0); // facing -X: its back is the wall at x = 2
    check(!w.wallBetween({1, 1, 1}, 1, 0) && w.wallBetween({1, 2, 1}, 1, 0), "a door opens its wall edge");
    w.setWalkable({3, 3, 1}, false);
    check(!canStep(w, open, {3, 2, 1}, 0, 1) && !w.contains({6, 0, 1}) && !w.contains({0, 0, 2}), "floors and bounds");
}

// Diagonal walls cut tiles in half: walkers go around the tiles they cross
// and do not slip through the line diagonally; a door in one opens it.
static void testDiagonalWalls() {
    World w(8, 8);
    w.addWall(1, 1, 1, 5, 5); // "/" through tiles (1, 1) .. (4, 4)
    w.addWall(1, 7, 1, 6, 2); // "\\" through tile (6, 1)
    check(w.diagonalWall({2, 2, 1}) && !w.walkable({2, 2, 1}) && !w.walkable({6, 1, 1}) && w.walkable({2, 3, 1}),
          "the tiles a diagonal wall crosses");
    BlockedGrid open(64, 0);
    check(!canStep(w, open, {3, 2, 1}, -1, 1), "no slipping between the halves");
    check(!w.lineOfSight({3, 1, 1}, {1, 3, 1}) && w.lineOfSight({3, 1, 1}, {5, 3, 1}), "a diagonal wall hides");
    Path p;
    check(findPath(w, open, {3, 1, 1}, {{1, 3, 0}}, p) && p.cost > 5, "around the diagonal wall");
    w.openDoor({3, 3, 1}, 1);
    check(w.walkable({3, 3, 1}) && findPath(w, open, {4, 2, 1}, {{2, 4, 0}}, p) && p.tiles.size() <= 4,
          "through a door in it");
}

// Set to Next 8 (same tile, after the target), 9 (adjacent to the object in
// a local: +X, -X, +Y, -Y) and 34 (within one tile).
static void testSetToNextTiles() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(8, 8);
    Object& a = place(vm, 3, 3);    // 1
    Object& b = place(vm, 3, 3);    // 2
    Object& east = place(vm, 4, 3); // 3: direction 4 (+X)
    Object& west = place(vm, 2, 3); // 4: direction 0 (-X)
    Object& diag = place(vm, 4, 4); // 5
    Object& held = place(vm, 3, 3); // 6: in a slot of b
    held.data[objdata::ContainerId] = b.id;
    place(vm, 6, 6); // 7: far away
    (void)east, (void)west, (void)diag;
    auto stn = [](uint8_t type, uint8_t local, Target t, Target f) {
        return instr(0x1F, t, f, {0, 0, 0, 0, uint8_t(0x80 | type), StackId, local, 0, 0});
    };
    // Same tile, from a: b, then a again (it wraps around, as FreeSO's
    // ObjectOnSameTile: the game's trees loop until they are back at the start).
    code.add(kGroup, 0x1000,
             tree({expr(StackId, 0, Assign, Lit, a.id, go(1), E), stn(8, 0, go(2), E),
                   expr(Temp, 0, Assign, StackId, 0, go(3), E), stn(8, 0, go(4), E),
                   expr(Temp, 1, Assign, StackId, 0, T, E)}));
    Thread t = vm.start(a.id, 0x1000);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == b.id && t.temps[1] == a.id,
          "Set to Next 8 goes through the tile's objects after the target, not the contained one, and wraps");
    // Adjacent to a (local 0): +X (east) comes before -X (west); then none.
    code.add(kGroup, 0x1001,
             tree({expr(Local, 0, Assign, Lit, a.id, go(1), E), expr(StackId, 0, Assign, Lit, 0, go(2), E),
                   stn(9, 0, go(3), E), expr(Temp, 0, Assign, StackId, 0, go(4), E), stn(9, 0, go(5), E),
                   expr(Temp, 1, Assign, StackId, 0, go(6), E), stn(9, 0, E, T)},
                  0, 1));
    t = vm.start(a.id, 0x1001);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == east.id && t.temps[1] == west.id,
          "Set to Next 9: +X, -X, +Y, -Y");
    // Within one tile of a: b, east, west, diag (by ID), not a itself nor the far one.
    code.add(kGroup, 0x1002,
             tree({expr(Local, 0, Assign, Lit, a.id, go(1), E), expr(StackId, 0, Assign, Lit, 0, go(2), E),
                   stn(34, 0, go(3), T), expr(Temp, 0, 3 /* add */, Lit, 1, go(2), E)},
                  0, 1));
    t = vm.start(a.id, 0x1002);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == 4, "Set to Next 34 within one tile");

    check(VM::blocksRouting(a) && !VM::blocksRouting(held), "objects in slots do not block");
    a.data[objdata::Flags] = 0x10;
    check(!VM::blocksRouting(a) && !vm.tileFree({3, 3, 1}) && vm.tileFree({3, 3, 1}, std::vector<ObjectId>{b.id}),
          "person intersection lets Sims through; ignored objects");
}

// A* on synthetic grids: around walls, through a door, past objects, to the
// cheapest of several goals, and no path.
static void testPathfinder() {
    World w(10, 10);
    for (int y = 0; y < 9; ++y)
        w.addWall(1, 5, y, 5, y + 1); // a wall at x = 5 with a gap at the top row
    BlockedGrid open(100, 0);
    Path p;
    check(findPath(w, open, {1, 1, 1}, {{8, 1, 0}}, p) && p.tiles.back() == TilePos{8, 1, 1}, "path around a wall");
    bool crosses = false;
    for (size_t i = 0; i + 1 < p.tiles.size(); ++i)
        crosses |= p.tiles[i].x == 4 && p.tiles[i + 1].x == 5 && p.tiles[i].y < 9;
    check(!crosses && p.cost > 14, "the path goes through the gap");
    for (size_t i = 0; i < p.tiles.size(); ++i) {
        const TilePos from = i ? p.tiles[i - 1] : TilePos{1, 1, 1};
        crosses |= !canStep(w, open, from, p.tiles[i].x - from.x, p.tiles[i].y - from.y);
    }
    check(!crosses, "every step of the path is allowed");
    w.openDoor({4, 1, 1}, 0); // a door in the wall, facing -X
    check(findPath(w, open, {1, 1, 1}, {{8, 1, 0}}, p) && p.tiles.size() == 7 && std::fabs(p.cost - 7) < 1e-4f,
          "straight through the door");
    check(findPath(w, open, {1, 1, 1}, {{3, 3, 0}}, p) && p.tiles.size() == 2 && std::fabs(p.cost - 2 * M_SQRT2) < 1e-4f,
          "diagonal steps");
    check(findPath(w, open, {1, 1, 1}, {{8, 1, 5}, {1, 8, 0}}, p) && p.goal == 1, "the cheapest goal wins");
    check(findPath(w, open, {1, 1, 1}, {{1, 1, 0}}, p) && p.tiles.empty(), "already there");
    BlockedGrid wall(100, 0);
    for (int x = 0; x < 10; ++x)
        wall[w.index(x, 5)] = 1;
    check(!findPath(w, wall, {1, 1, 1}, {{1, 8, 0}}, p), "no path past a row of objects");
    check(!findPath(w, wall, {1, 1, 1}, {{3, 5, 0}}, p), "a blocked goal");
    wall[w.index(1, 1)] = 1;
    check(findPath(w, wall, {1, 1, 1}, {{2, 2, 0}}, p), "leaving a blocked start tile");
}

// Routing slot goals: a ring of directions around the object, rotated with
// it; a point slot on the object's tile.
static void testRoutingGoals() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 10);
    Object& o = place(vm, 5, 5);
    o.data[objdata::Direction] = 2; // faces +Y
    Slots::Slot front;
    front.type = 3;
    front.flags = 0x01; // direction 0 relative to the object: its front
    front.minDistance = front.maxDistance = front.optimalDistance = 1;
    front.facing = -2;
    auto g = vm.routingGoals(front, o);
    check(g.goals.size() == 1 && g.goals[0].x == 5 && g.goals[0].y == 6 && g.facings[0] == 6,
          "in front of an object facing +Y, facing it");
    front.flags = 0x55;
    g = vm.routingGoals(front, o);
    check(g.goals.size() == 4, "four sides");
    vm.world->addWall(1, 5, 5, 6, 5); // a wall just behind it (y = 5)
    g = vm.routingGoals(front, o);
    check(g.goals.size() == 3, "not through a wall");
    Slots::Slot point;
    point.type = 3;
    point.facing = -2;
    g = vm.routingGoals(point, o);
    check(g.goals.size() == 1 && g.onTarget && g.facings[0] == 2, "a bone slot is on the object, facing its way");
}

static ObjectDefinition definition(uint32_t guid, uint16_t slotsId = 0, bool person = false) {
    ObjectDefinition def;
    def.fields.resize(60);
    def.fields[(0x5C - 0x44) / 2] = uint16_t(guid);
    def.fields[(0x5E - 0x44) / 2] = uint16_t(guid >> 16);
    def.fields[(0x68 - 0x44) / 2] = slotsId;
    if (person)
        def.fields[(0x52 - 0x44) / 2] = kObjectTypePerson;
    return def;
}

// A Sim walks to an object over ticks at walking speed, around a wall, and
// ends facing it; a goal it cannot reach is false; out of the world is the
// game's error.
static void testWalking() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(12, 12);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0x80));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Slots table;
    table.slots.resize(3);
    table.slots[0].type = 0; // a container slot
    table.slots[1].type = 3; // routing slot 0: in front, 1 tile
    table.slots[1].flags = 0x01;
    table.slots[1].minDistance = table.slots[1].maxDistance = table.slots[1].optimalDistance = 1;
    table.slots[1].facing = -2;
    table.slots[1].resolutionTiles = 1; // three points a row (laterals -1, 0, 1)
    table.slots[1].gradient = 0.2f;      // the middle one is the best
    table.slots[2].type = 3; // routing slot 1: on the object (a bone, in the game)
    table.slots[2].facing = -2;
    code.add(kGroup, 0x80, table);
    Object& chair = vm.instantiate(kGroup, 0x80);
    chair.position = {8.5f, 2.5f, 1};
    chair.data[objdata::Direction] = 4; // faces +X: its front is (9, 2)
    Object& sim = vm.instantiate(kGroup, 0x81);
    sim.position = {1.5f, 2.5f, 1};
    vm.world->addWall(1, 5, 0, 5, 6); // in the way up to y = 6
    // Go To Routing Slot, literal slot 0.
    code.add(kGroup, 0x1000, tree({instr(0x2D, T, F, {0, 0, 1, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, chair.id);
    vm.run(t);
    check(t.status == Thread::Status::Running && sim.route && sim.position.x > 1.5f, "walking yields");
    int ticks = 1;
    while (t.status == Thread::Status::Running && ticks < 2000)
        vm.run(t), ++ticks;
    // Past the wall's end (x = 5, y = 6) with 1/8 tile to spare, and back
    // down: a straight line each way, not tile centres.
    const float walked = std::hypot(5 - 1.5f, 6.125f - 2.5f) + std::hypot(9.5f - 5, 6.125f - 2.5f);
    check(t.status == Thread::Status::Finished && t.result && !sim.route, "the walk ends true");
    check(std::fabs(sim.position.x - 9.5f) < 1e-4f && std::fabs(sim.position.y - 2.5f) < 1e-4f &&
              sim.data[objdata::Direction] == 0,
          "in front of the chair, facing it");
    check(ticks > int(walked / vm.walkTilesPerTick) * 0.95 && ticks < int(walked / vm.walkTilesPerTick) * 1.15,
          "at walking speed");
    // Routing slot 1 is on the chair itself, which does not block it.
    code.add(kGroup, 0x1001, tree({instr(0x2D, T, F, {1, 0, 1, 0, 0})}));
    t = vm.start(sim.id, 0x1001, {}, chair.id);
    for (int i = 0; i < 100 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && std::fabs(sim.position.x - 8.5f) < 1e-4f &&
              sim.data[objdata::Direction] == 4,
          "a slot on the object");
    code.add(kGroup, 0x1002, tree({instr(0x2D, T, F, {7, 0, 1, 0, 0})}));
    t = vm.start(sim.id, 0x1002, {}, chair.id);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Invalid routing slot") == 0, "missing routing slot");

    // Go To Relative Position: behind the chair (location 4), any direction;
    // walled in, it cannot get there.
    sim.position = {1.5f, 9.5f, 1};
    code.add(kGroup, 0x1003, tree({instr(0x1B, T, F, {0, 0, 4, 0xFF, 0, 0, 2})}));
    t = vm.start(sim.id, 0x1003, {}, chair.id);
    for (int i = 0; i < 1000 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && std::fabs(sim.position.x - 7.5f) < 1e-4f &&
              std::fabs(sim.position.y - 2.5f) < 1e-4f,
          "Go To Relative Position behind");
    for (int x = 0; x < 12; ++x)
        vm.world->addWall(1, x, 8, x + 1, 8);
    sim.position = {1.5f, 9.5f, 1};
    t = vm.start(sim.id, 0x1003, {}, chair.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result, "no way there: false");
    sim.position = {-1, -1, 1};
    t = vm.start(sim.id, 0x1003, {}, chair.id);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("out of world") != std::string::npos,
          "routing out of the world");
}

// Snap Into puts an object in a containment slot; Find Location For places
// objects near another one, or out of the world.
static void testSnapAndPlace() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(8, 8);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0x80));
    Slots table;
    table.slots.resize(3);
    table.slots[1].type = 3; // containers: slots 0 and 2 of the table, numbered 0 and 1
    code.add(kGroup, 0x80, table);
    Object& counter = vm.instantiate(kGroup, 0x80);
    counter.position = {3.5f, 3.5f, 1};
    Object& plate = place(vm, 6, 6);
    Object& cup = place(vm, 6, 5);
    // Snap the stack object (plate) into me (counter), slot 1; then the cup into the same slot.
    code.add(kGroup, 0x1000, tree({instr(0x71, T, F, {StackId, 0, 0, Data, 11, 0, 1, 0, 0, 0})}));
    Thread t = vm.start(counter.id, 0x1000, {}, plate.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && vm.containedIn(counter.id, 1) == plate.id &&
              plate.position.x == 3.5f,
          "Snap Into a slot");
    t = vm.start(counter.id, 0x1000, {}, cup.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result, "the slot is taken");
    code.add(kGroup, 0x1001, tree({expr(Temp, 0, Assign, Lit, 2, go(1), E),
                                   instr(0x71, T, F, {StackId, 0, 0, Data, 11, 0, 0, 0, 0, 1})}));
    t = vm.start(counter.id, 0x1001, {}, cup.id);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Slot number out of range") == 0, "slot from temp 0");

    // Find Location For, normal: the cup next to the counter (whose tile is taken).
    code.add(kGroup, 0x1002, tree({instr(0x10, T, F, {0, 0, 0})}));
    cup.data[objdata::ContainerId] = 0;
    t = vm.start(counter.id, 0x1002, {}, cup.id);
    vm.run(t);
    const TilePos at = VM::tileOf(cup);
    check(t.status == Thread::Status::Finished && t.result && std::max(std::abs(at.x - 3), std::abs(at.y - 3)) == 1,
          "Find Location For next to the reference");
    // Out of world.
    code.add(kGroup, 0x1003, tree({instr(0x10, T, F, {1, 0, 0})}));
    t = vm.start(counter.id, 0x1003, {}, cup.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !cup.position.inWorld(), "Find Location For out of world");
    // Explicit objects (flag 0x08): the plate (local 0) near the cup's... the
    // counter (me), along its vector (mode 3): in front of it first.
    counter.data[objdata::Direction] = 2; // faces +Y
    code.add(kGroup, 0x1004, tree({expr(Local, 0, Assign, Lit, plate.id, go(1), E),
                                   instr(0x10, T, F, {3, 0, 8, 0, Local, 0, 0, Data, 11, 0})},
                                  0, 1));
    t = vm.start(counter.id, 0x1004, {}, 0);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && VM::tileOf(plate) == TilePos{3, 4, 1} &&
              plate.data[objdata::ContainerId] == 0,
          "Find Location For along the vector takes it out of its slot");
}

// Stairs: read off the steps' cObjects, then walked by the routing
// primitives from one level to the next, phase by phase.
static void testStairs() {
    Lot lot;
    lot.width = lot.height = 10;
    lot.minLevel = 0;
    lot.levelCount = 2; // no elevations: level l is 3 * l high
    auto piece = [&](const char* kind, float y, float z) {
        Lot::Object o;
        o.definition = std::string("Stair Modular - Test - ") + kind + "_objt";
        o.position = {4.5f, y, z};
        o.rotation = {0, 0, -1, 0}; // yaw -180: the steps face -Y (down)
        lot.objects.push_back(o);
    };
    piece("Portal - A", 8.75f, 2.8125f);
    for (int k = 1; k < 15; ++k)
        if (k != 7) // a step lost from the save: bridged
            piece("Bridge", 8.75f - 0.25f * float(k), 2.8125f - 0.1875f * float(k));
    piece("End", 5.0f, 0);
    piece("Portal - A", 8.75f, 2.8125f); // a stale copy
    const auto stairs = lotStairs(lot);
    check(stairs.size() == 1, "one stair from the steps");
    if (stairs.size() == 1) {
        const Stair& st = stairs[0];
        check(st.bottom == TilePos{4, 4, 1} && st.top == TilePos{4, 9, 2} && st.steps == 16 && st.direction == 2 &&
                  std::fabs(st.bottomZ) < 1e-4f && std::fabs(st.topZ - 3) < 1e-4f,
              "the stair's landings, steps and heights");
    }

    // A stair up +X from (2, 1) on level 1 to (5, 1) on level 2.
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 4, 1, 2);
    World& w = *vm.world;
    for (int x = 3; x <= 4; ++x)
        w.setWalkable({x, 1, 2}, false); // the stairwell
    Stair st;
    st.bottom = {2, 1, 1}, st.top = {5, 1, 2};
    st.bottomEdge = {3.0f, 1.5f};
    st.up = {1, 0};
    st.steps = 8;
    st.bottomZ = 0, st.topZ = 1.5f;
    st.direction = 4;
    w.addStair(st);
    check(!w.walkable({3, 1, 1}) && !w.walkable({4, 1, 1}) && w.walkable({2, 1, 1}), "no walking under the steps");
    std::vector<BlockedGrid> blocked(2, BlockedGrid(40, 0));
    Path p;
    std::vector<int> via;
    check(findPath(w, blocked, {0, 1, 1}, 2, {{8, 1, 0}}, p, via) && p.tiles.back() == TilePos{8, 1, 2},
          "a path up the stair");
    size_t climbs = 0;
    for (size_t i = 0; i < p.tiles.size(); ++i)
        climbs += via[i] == 0 && p.tiles[i] == st.top && i > 0 && p.tiles[i - 1] == st.bottom;
    check(climbs == 1, "the path takes the stair once, landing to landing");
    check(!findPath(w, blocked, {0, 1, 1}, 2, {{3, 1, 0}}, p, via), "no goal in the stairwell");

    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {8.5f, 1.5f, 2};
    Object& sim = vm.instantiate(kGroup, 0x81);
    sim.position = {0.5f, 1.5f, 1};
    // Go To Relative Position: in front of the lamp (direction 0, -X), facing it.
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x04})})); // 0x04: another level
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    std::set<std::string> phases;
    float maxZ = 0;
    int ticks = 0;
    for (; ticks < 3000 && t.status == Thread::Status::Running; ++ticks) {
        vm.run(t);
        if (sim.route && !sim.route->climb.animation.empty()) {
            phases.insert(sim.route->climb.animation);
            maxZ = std::max(maxZ, sim.route->climb.z);
        }
    }
    check(t.status == Thread::Status::Finished && t.result && sim.position.level == 2 &&
              sim.position.x > 6 && sim.position.x < 8 && sim.data[objdata::Direction] == 4,
          "walked upstairs to the lamp");
    check(phases.count("a-stairs-up-start-R") && phases.count("a-stairs-up-loop-L") &&
              phases.count("a-stairs-up-loop-R") && phases.count("a-stairs-up-stop-R") && std::fabs(maxZ - 1.5f) < 1e-4f,
          "the climb's phases");
    // And back down.
    lamp.position = {1.5f, 2.5f, 1};
    t = vm.start(sim.id, 0x1000, {}, lamp.id);
    phases.clear();
    for (int i = 0; i < 3000 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (sim.route && !sim.route->climb.animation.empty())
            phases.insert(sim.route->climb.animation);
    }
    check(t.status == Thread::Status::Finished && t.result && sim.position.level == 1 &&
              phases.count("a-stairs-down-start-R") && phases.count("a-stairs-down-stop-R"),
          "and back down");
}

// The continuous path planner (path_planner.hpp): inflated obstacles,
// diagonal walls cut tiles in half, doors are portals, smoothing, costs and
// the failure codes.
static float pathLength(Point2 start, const PlannedPath& p) {
    float len = 0;
    Point2 a = start;
    for (const auto& w : p.points) {
        len += std::hypot(w.x - a[0], w.y - a[1]);
        a = {w.x, w.y};
    }
    return len;
}

static void testPathPlanner() {
    World w(10, 10);
    PathPlanner planner(w);
    PlannedPath p;
    RouteFail why;
    // Open ground: one straight line.
    check(planner.plan({1.5f, 1.5f}, 1, 1, {{8.5f, 6.5f, 0}}, p, why) && p.points.size() == 1 &&
              std::fabs(p.points[0].x - 8.5f) < 1e-4f,
          "planner: a straight line in the open");
    // An object's tile, inflated by 1/8 tile: the walk keeps clear of it,
    // and is shorter than going round tile centres.
    planner.addObstacle(1, 4, 0, 5, 9, PathPlanner::kObject); // a row of objects up to y = 9
    check(!planner.free(1, {3.9f, 3.5f}) && planner.free(1, {3.85f, 3.5f}), "planner: obstacles inflated 1/8 tile");
    check(planner.plan({1.5f, 1.5f}, 1, 1, {{8.5f, 1.5f, 0}}, p, why), "planner: around a row of objects");
    bool clear = true;
    Point2 a{1.5f, 1.5f};
    for (const auto& q : p.points) {
        clear &= planner.clear(1, a, {q.x, q.y});
        a = {q.x, q.y};
    }
    const float len = pathLength({1.5f, 1.5f}, p);
    // Up to (3.875, 9.125), across and down: about 17.8 tiles.
    check(clear && len > 17.5f && len < 18.5f,
          "planner: the path hugs the inflated corner");
    // A gap narrower than a Sim (two objects 0.2 apart, inflated, close it).
    planner.clearObstacles();
    planner.addObstacle(1, 4, 0, 5, 4.9f, PathPlanner::kObject);
    planner.addObstacle(1, 4, 5.1f, 5, 10, PathPlanner::kObject);
    check(!planner.plan({1.5f, 5.0f}, 1, 1, {{8.5f, 5.0f, 0}}, p, why) && why == RouteFail::PathNotFound,
          "planner: no squeezing through a gap, path not found (2)");
    // Failure codes: no goals (6), a goal under an object (7).
    planner.clearObstacles();
    check(!planner.plan({1.5f, 1.5f}, 1, 1, {}, p, why) && why == RouteFail::NoValidGoals, "planner: no valid goals (6)");
    planner.addObstacle(1, 8, 8, 9, 9, PathPlanner::kObject);
    check(!planner.plan({1.5f, 1.5f}, 1, 1, {{8.5f, 8.5f, 0}}, p, why) && why == RouteFail::DestinationOccupied,
          "planner: destination occupied (7)");
    // A start inside an object (getting up from a chair) steps out first.
    check(planner.plan({8.5f, 8.5f}, 1, 1, {{1.5f, 1.5f, 0}}, p, why) && p.escaped, "planner: out of a footprint");
    // Another Sim is a cost, not a wall: the walk goes round it if that is cheaper.
    planner.clearObstacles();
    planner.addRouter(1, 5.0f, 5.0f, 0, false);
    check(planner.plan({1.0f, 5.0f}, 1, 1, {{9.0f, 5.0f, 0}}, p, why), "planner: past another Sim");
    float closest = 9;
    a = {1.0f, 5.0f};
    for (const auto& q : p.points) {
        for (int k = 0; k <= 20; ++k) {
            const float u = float(k) / 20, x = a[0] + (q.x - a[0]) * u, y = a[1] + (q.y - a[1]) * u;
            closest = std::min(closest, std::hypot(x - 5.0f, y - 5.0f));
        }
        a = {q.x, q.y};
    }
    check(closest > 0.3f, "planner: round another Sim, not through");

    // Diagonal walls cut tiles in half: both halves are places to stand.
    World d(8, 8);
    d.addWall(1, 0, 4, 4, 0); // from (0, 4) to (4, 0)
    PathPlanner dp(d);
    check(dp.free(1, {2.75f, 1.75f}) && dp.free(1, {2.25f, 1.25f}) && !dp.free(1, {2.5f, 1.5f}),
          "planner: the halves of a diagonal tile");
    check(dp.plan({6.5f, 6.5f}, 1, 1, {{2.75f, 1.75f, 0}}, p, why), "planner: to the outer half of a diagonal tile");
    check(!dp.plan({6.5f, 6.5f}, 1, 1, {{2.2f, 1.2f, 0}}, p, why), "planner: not through the diagonal wall");

    // A door is a portal: the walk goes through its two points, straight.
    World h(10, 6);
    for (int y = 0; y < 6; ++y)
        h.addWall(1, 5, y, 5, y + 1); // a wall at x = 5
    h.openDoor({4, 2, 1}, 0);          // facing -X, its back to the wall
    PathPlanner hp(h);
    check(hp.portals().size() == 1 && !hp.free(1, {5.0f, 2.5f}), "planner: the door's opening is closed, a portal opens it");
    check(hp.plan({1.5f, 4.5f}, 1, 1, {{8.5f, 4.5f, 0}}, p, why), "planner: through the door");
    bool through = false;
    for (size_t i = 0; i < p.points.size(); ++i)
        through |= p.points[i].portal == 0 && std::fabs(p.points[i].x - 5.5f) < 1e-4f && i > 0 &&
                   std::fabs(p.points[i - 1].x - 4.5f) < 1e-4f && std::fabs(p.points[i - 1].y - 2.5f) < 1e-4f;
    check(through, "planner: in front of the door, then behind it");

    // The rooms a walker reaches (GetAccessibleRooms): a walled-off room
    // is not reached until a door opens it.
    World r(10, 6);
    for (int y = 0; y < 6; ++y)
        r.addWall(1, 5, y, 5, y + 1);
    for (int x = 0; x < 10; ++x)
        for (int y = 0; y < 6; ++y)
            r.setRoom({x, y, 1}, int16_t(x < 5 ? 1 : 2));
    {
        PathPlanner rp(r);
        check(rp.accessibleRooms(1) == std::vector<int16_t>{1} && rp.accessibleRooms(2) == std::vector<int16_t>{2},
              "accessible rooms: a wall keeps the other room out");
    }
    r.openDoor({4, 2, 1}, 0);
    PathPlanner rp2(r);
    check(rp2.accessibleRooms(1) == (std::vector<int16_t>{1, 2}) && rp2.accessibleRooms(7).empty(),
          "accessible rooms: through a door's portal; an unknown room reaches nothing");

    // Floor costs: a costly band is walked round when that is cheaper.
    World c(12, 6);
    for (int y = 0; y < 5; ++y)
        c.setCost({6, y, 1}, 10);
    PathPlanner cp(c);
    check(cp.plan({1.5f, 1.5f}, 1, 1, {{10.5f, 1.5f, 0}}, p, why) && pathLength({1.5f, 1.5f}, p) > 9.5f,
          "planner: round a costly floor");
}

// A failed route writes the route result (person data 62) and the
// primitive returns false.
static void testRouteResult() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 10);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {8.5f, 8.5f, 1};
    Object& sim = vm.instantiate(kGroup, 0x81);
    sim.position = {1.5f, 1.5f, 1};
    check(sim.person.has_value(), "a person");
    for (int x = 0; x < 10; ++x)
        vm.world->addWall(1, x, 5, x + 1, 5); // walled off
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    for (int i = 0; i < 10 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result && sim.person && sim.person->data[62] == 2,
          "route result 2: path not found");
}

// A Sim standing in a one-tile corridor blocks another: the walker waits
// (asking it to move), plans again, and after five tries fails with
// "blocked by person" (14).
static void testBlockedBySim() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(12, 3);
    for (int x = 0; x < 12; ++x) {
        vm.world->addWall(1, x, 1, x + 1, 1);
        vm.world->addWall(1, x, 2, x + 1, 2);
    }
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {11.5f, 1.5f, 1};
    Object& walker = vm.instantiate(kGroup, 0x81);
    walker.position = {0.5f, 1.5f, 1};
    Object& other = vm.instantiate(kGroup, 0x81);
    other.position = {6.5f, 1.5f, 1};
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(walker.id, 0x1000, {}, lamp.id);
    int ticks = 0, waited = 0;
    for (; ticks < 3000 && t.status == Thread::Status::Running; ++ticks) {
        vm.run(t);
        waited += walker.route && walker.route->waiting && walker.route->blocker == other.id;
    }
    check(t.status == Thread::Status::Finished && !t.result && walker.person && walker.person->data[62] == 14 &&
              waited > 300 && walker.position.x < 6,
          "blocked by a Sim in a corridor: waits, then fails with 14");
    // The Sim steps aside (out of the corridor): the way is free.
    other.position = {-1, -1, 1};
    t = vm.start(walker.id, 0x1000, {}, lamp.id);
    for (int i = 0; i < 3000 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && walker.position.x > 9, "and walks on once it is gone");

    // Two Sims walking at each other in the open pass each other.
    vm.world = World(12, 12);
    Object& a = walker;
    Object& b = other;
    a.position = {1.5f, 6.5f, 1};
    b.position = {10.5f, 6.5f, 1};
    Object& lampB = vm.instantiate(kGroup, 0x80);
    lamp.position = {11.5f, 6.5f, 1};
    lampB.position = {0.5f, 6.5f, 1};
    code.add(kGroup, 0x1001, tree({instr(0x1B, T, F, {0, 0, 4, 0xFE, 0, 0, 0})})); // behind the lamp (its +X side)
    vm.threads.insert_or_assign(a.id, vm.start(a.id, 0x1000, {}, lamp.id));
    vm.threads.insert_or_assign(b.id, vm.start(b.id, 0x1001, {}, lampB.id));
    float closest = 99;
    for (int i = 0; i < 2000; ++i) {
        vm.tick();
        closest = std::min(closest, std::hypot(a.position.x - b.position.x, a.position.y - b.position.y));
        if (vm.threads.at(a.id).status != Thread::Status::Running && vm.threads.at(b.id).status != Thread::Status::Running)
            break;
    }
    check(vm.threads.at(a.id).status == Thread::Status::Finished && vm.threads.at(a.id).result &&
              vm.threads.at(b.id).status == Thread::Status::Finished && vm.threads.at(b.id).result && a.position.x > 9 &&
              b.position.x < 3 && closest > 0.25f,
          "two Sims walking at each other pass");
}

// Through a door: the walker stops at the door's portal (waiting for it to
// open: the route's wait point names the door object), then goes through.
static void testDoorWait() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 6);
    for (int y = 0; y < 6; ++y)
        vm.world->addWall(1, 5, y, 5, y + 1);
    vm.world->openDoor({4, 2, 1}, 0);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    ObjectDefinition doorDef = definition(0xAAAA0003, 0);
    doorDef.fields[(0x52 - 0x44) / 2] = kObjectTypeDoor;
    code.add(kGroup, 0x82, doorDef);
    Object& door = vm.instantiate(kGroup, 0x82);
    door.position = {4.5f, 2.5f, 1};
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {9.5f, 2.5f, 1};
    Object& sim = vm.instantiate(kGroup, 0x81);
    sim.position = {1.5f, 4.5f, 1};
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    int waitTicks = 0;
    bool named = false;
    for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (sim.route && sim.route->waiting) {
            ++waitTicks;
            named |= std::fabs(sim.position.x - 4.5f) < 1e-3f && !sim.route->points.empty() &&
                     sim.route->points.front().door == door.id;
        }
    }
    if (std::getenv("DEBUG_DOOR"))
        std::fprintf(stderr, "door test: status %d result %d x %.2f named %d wait %d type %d\n", int(t.status), t.result,
                     sim.position.x, named, waitTicks, door.definition ? door.definition->objectType() : -1);
    check(t.status == Thread::Status::Finished && t.result && sim.position.x > 7 && named && waitTicks >= 10,
          "a door: wait at its portal, then through");
}

// A door with a portal function (OBJf entry 15): planning runs its guard
// as the TEST tree (false locks the door: no way through), arriving runs
// its action on the walker's thread with the door as the stack object and
// the portal's parameters (6 = the routing slot on the walker's side), and
// the walk goes on through when it returns true.
static void testDoorPortalTree() {
    for (const bool locked : {false, true}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(10, 6);
        for (int y = 0; y < 6; ++y)
            vm.world->addWall(1, 5, y, 5, y + 1);
        vm.world->openDoor({4, 2, 1}, 0);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        ObjectDefinition doorDef = definition(0xAAAA0003, 0);
        doorDef.fields[(0x52 - 0x44) / 2] = kObjectTypeDoor;
        code.add(kGroup, 0x82, doorDef);
        ObjectFunctions f;
        f.functions.resize(16);
        f.functions[15] = {0x1010, 0x1011};
        code.add(kGroup, 0x82, f);
        constexpr uint8_t Global = 0x06, Param = 0x09;
        // TEST: open while global 12 is 0.
        code.add(kGroup, 0x1010, tree({expr(Global, 12, Eq, Lit, 0, T, F)}));
        // The portal: global 10 = parameter 6 + 100, global 11 = the stack object.
        code.add(kGroup, 0x1011, tree({expr(Global, 10, Assign, Param, 6, go(1), E),
                                       expr(Global, 10, 3, Lit, 100, go(2), E),
                                       expr(Global, 11, Assign, StackId, 0, T, E)}, 8));
        vm.globals[12] = locked ? 1 : 0;
        Object& door = vm.instantiate(kGroup, 0x82);
        door.position = {4.5f, 2.5f, 1};
        Object& lamp = vm.instantiate(kGroup, 0x80);
        lamp.position = {9.5f, 2.5f, 1};
        Object& sim = vm.instantiate(kGroup, 0x81);
        sim.position = {1.5f, 4.5f, 1};
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x02})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        if (locked)
            check(t.status == Thread::Status::Finished && !t.result && sim.position.x < 5 && vm.globals[11] == 0 &&
                      sim.person && sim.person->data[62] == int16_t(RouteFail::PathNotFound),
                  "a door whose portal TEST is false is locked");
        else
            check(t.status == Thread::Status::Finished && t.result && sim.position.x > 7 && vm.globals[11] == door.id &&
                      vm.globals[10] == 100,
                  "a door's portal tree runs on arrival, with the door as stack object and its side's slot");
    }
}

// The walker's pre-route tree (OBJf entry 0x25) runs once a way is found,
// on its thread with itself as the stack object and the target in
// parameter 0 (here it counts its runs); the route is planned again after it.
// False fails the route with "pre-route failed" (11).
static void testPreRouteTree() {
    for (const bool refuse : {false, true}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(10, 6);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        ObjectFunctions f;
        f.functions.resize(0x26);
        f.functions[0x25] = {0, 0x1010};
        code.add(kGroup, 0x81, f);
        constexpr uint8_t Global = 0x06, Param = 0x09;
        code.add(kGroup, 0x1010, tree({expr(Global, 10, 3, Lit, 1, go(1), E),          // global 10 += 1
                                       expr(Global, 11, Assign, Param, 0, go(2), E),   // global 11 = the target
                                       expr(Global, 12, Assign, StackId, 0, go(3), E), // global 12 = stack object
                                       expr(Data, 1, Assign, Lit, 0, refuse ? F : T, E)}, 6));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        lamp.position = {8.5f, 2.5f, 1};
        Object& sim = vm.instantiate(kGroup, 0x81);
        sim.position = {1.5f, 2.5f, 1};
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x02})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        if (refuse)
            check(t.status == Thread::Status::Finished && !t.result && sim.position.x < 2 && vm.globals[10] == 1 &&
                      sim.person && sim.person->data[62] == int16_t(RouteFail::PreRouteFailed),
                  "a pre-route tree's false fails the route with code 11");
        else
            check(t.status == Thread::Status::Finished && t.result && sim.position.x > 6 && vm.globals[10] == 1 &&
                      vm.globals[11] == lamp.id && vm.globals[12] == sim.id,
                  "the walker's pre-route tree runs once, with the target and itself as stack object");
    }
}

// A Sim standing in a corridor is asked to move (MoveOutOfWay): a
// destination object (GUID 0x7C4) is made on it, with a routing slot 0 in
// the alcove beside it, and its
// "Interaction - Move Out Of Way" (action 0x100B: Go To Routing Slot 0 of
// the destination) goes on the blocker's queue with the walker's priority
// less one; the blocker's Main runs it, it steps aside and the walker gets
// through. With the blocker's priority as high, it is not asked.
static void testMoveOutOfWay() {
    for (const bool lowPriority : {true, false}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(12, 4);
        for (int x = 0; x < 12; ++x) {
            vm.world->addWall(1, x, 1, x + 1, 1);
            if (x != 6)
                vm.world->addWall(1, x, 2, x + 1, 2);
        }
        vm.world->addWall(1, 6, 2, 6, 3);
        vm.world->addWall(1, 7, 2, 7, 3);
        vm.world->addWall(1, 6, 3, 7, 3);
        constexpr uint32_t kDestGroup = 0x7F000002;
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        ObjectDefinition simDef = definition(0xAAAA0002, 0, true);
        code.add(kGroup, 0x81, simDef);
        ObjectFunctions mainOnly;
        mainOnly.functions.resize(2);
        mainOnly.functions[1].action = 0x1002;
        code.add(kGroup, 0x81, mainOnly);
        // The Sims' Main: Idle for Input with "allow push", over and over.
        code.add(kGroup, 0x1002, tree({expr(0x09, 0, Assign, Lit, 1, go(1), E), instr(0x11, go(0), E, {0, 0, 1, 0})}, 1));
        // The destination: one routing slot on itself, the move-out-of-way interaction.
        ObjectDefinition destDef = definition(0x000007C4, 0x80);
        destDef.fields[(0x4E - 0x44) / 2] = 1; // its TTAB
        code.add(kDestGroup, 0x80, destDef);
        Slots slots;
        Slots::Slot here;
        here.type = 3;
        slots.slots = {here};
        code.add(kDestGroup, 0x80, slots);
        InteractionTable ttab;
        ttab.version = 0x4D;
        InteractionTable::Interaction e;
        e.action = 0x100B;
        ttab.interactions = {e};
        code.add(kDestGroup, 1, ttab);
        code.add(kDestGroup, 0x100B, tree({instr(0x2D, T, F, {0, 0, 1, 0, 0x01})}));
        // Its Init lets Sims through it (object flag 5, "allow person intersection", as the game's).
        ObjectFunctions destInit;
        destInit.functions.resize(1);
        destInit.functions[0].action = 0x1001;
        code.add(kDestGroup, 0x80, destInit);
        code.add(kDestGroup, 0x1001, tree({expr(Data, 8, 9, Lit, 5, T, E)}));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        lamp.position = {11.5f, 1.5f, 1};
        Object& walker = vm.instantiate(kGroup, 0x81);
        walker.position = {0.5f, 1.5f, 1};
        walker.person->data[33] = 50; // user driven
        Object& other = vm.instantiate(kGroup, 0x81);
        other.position = {6.5f, 1.5f, 1};
        other.data[objdata::Direction] = 0; // facing the walker
        other.person->data[33] = lowPriority ? 0 : 50;
        vm.startMain(other.id);
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0xFF, 0xFE, 0, 0, 0x02})})); // anywhere near: goals
        vm.threads.insert_or_assign(walker.id, vm.start(walker.id, 0x1000, {}, lamp.id));
        for (int i = 0; i < 3000 && vm.threads.at(walker.id).status == Thread::Status::Running; ++i)
            vm.tick();
        const Thread& t = vm.threads.at(walker.id);
        if (std::getenv("DEBUG_MOVE"))
            std::fprintf(stderr, "move: status %d result %d walker %.2f,%.2f other %.2f,%.2f queue %zu objects %zu main %d %s rr %d\n",
                         int(t.status), t.result, walker.position.x, walker.position.y, other.position.x, other.position.y,
                         other.queue.size(), vm.objects().size(), int(vm.threads.at(other.id).status),
                         vm.threads.at(other.id).error.c_str(), walker.person->data[62]);
        if (lowPriority)
            check(t.status == Thread::Status::Finished && t.result && walker.position.x > 9 && other.position.y > 2,
                  "a Sim in the way is asked to move: it steps into the alcove and the walker gets through");
        if (lowPriority)
            check(vm.objects().size() == 3, "the move-out-of-way destination is gone once its interaction is");
        else
            check(t.status == Thread::Status::Finished && !t.result && other.position.y < 2 && other.queue.empty(),
                  "a Sim of the same priority is not asked to move");
    }
}

// Go Here: a click on the ground makes the destination there, moved 0.45
// tile off the wall of its tile and out of what blocks; it is kept while
// the pie menu is open and while its interaction is queued, and goes once
// the Sim has walked there.
static void testGoHere() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(12, 8);
    vm.world->addWall(1, 3, 0, 3, 8); // between x = 2 and x = 3
    constexpr uint32_t kDestGroup = 0x7F000003;
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    ObjectDefinition simDef = definition(0xAAAA0002, 0, true);
    code.add(kGroup, 0x81, simDef);
    ObjectFunctions mainOnly;
    mainOnly.functions.resize(2);
    mainOnly.functions[1].action = 0x1002;
    code.add(kGroup, 0x81, mainOnly);
    code.add(kGroup, 0x1002, tree({expr(0x09, 0, Assign, Lit, 1, go(1), E), instr(0x11, go(0), E, {0, 0, 1, 0})}, 1));
    ObjectDefinition destDef = definition(kGoHereGuid, 0);
    destDef.fields[(0x4E - 0x44) / 2] = 1; // its TTAB
    code.add(kDestGroup, 0x80, destDef);
    InteractionTable ttab;
    ttab.version = 0x4D;
    InteractionTable::Interaction e;
    e.action = 0x1000;
    ttab.interactions = {e};
    code.add(kDestGroup, 1, ttab);
    // "Go Here": to the destination's spot (Go To Relative Position, on it).
    code.add(kDestGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0xFF, 0xFE, 0, 0, 0x02})}));
    ObjectFunctions destInit;
    destInit.functions.resize(1);
    destInit.functions[0].action = 0x1001;
    code.add(kDestGroup, 0x80, destInit);
    code.add(kDestGroup, 0x1001, tree({expr(Data, 8, 9, Lit, 5, T, E)}));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {8.5f, 4.5f, 1};
    Object& sim = vm.instantiate(kGroup, 0x81);
    sim.position = {6.5f, 1.5f, 1};
    sim.person->data[33] = 0;
    vm.startMain(sim.id);
    const size_t before = vm.objects().size();
    // Next to the wall: 0.45 off it.
    const auto p = goHerePoint(vm, sim.id, 3.1f, 2.5f, 1);
    check(p && std::fabs(p->x - 3.451f) < 1e-3f && std::fabs(p->y - 2.5f) < 1e-4f, "go here: 0.45 tile off the wall");
    // On the lamp: out of its tile.
    const auto q = goHerePoint(vm, sim.id, 8.5f, 4.5f, 1);
    check(q && (std::floor(q->x) != 8 || std::floor(q->y) != 4), "go here: out of what blocks");
    check(!goHerePoint(vm, sim.id, 13.0f, 2.0f, 1), "go here: not out of the lot");
    // Cancelled: the destination goes when the menu closes.
    ObjectId d = createGoHereDestination(vm, sim.id, 9.5f, 6.5f, 1);
    check(d && vm.objects().size() == before + 1, "go here: the destination is made");
    vm.tick();
    check(vm.object(d) != nullptr, "go here: kept while the pie menu is open");
    releaseDestination(vm, d);
    check(!vm.object(d), "go here: gone when the menu closes without a pick");
    // Picked: the Sim walks there and the destination goes after.
    d = createGoHereDestination(vm, sim.id, 9.5f, 6.5f, 1);
    auto in = makeInteraction(vm, d, 0);
    check(d && in.has_value(), "go here: its interaction");
    if (in) {
        in->priority = 50;
        enqueue(vm, sim, *in);
    }
    releaseDestination(vm, d);
    check(vm.object(d) != nullptr, "go here: kept while queued");
    for (int i = 0; i < 600 && vm.object(d); ++i)
        vm.tick();
    check(!vm.object(d) && std::hypot(sim.position.x - 9.5f, sim.position.y - 6.5f) < 1.5f,
          "go here: the Sim walks there, then the destination goes");
}

int main() {
    testStairs();
    testWalking();
    testSnapAndPlace();
    testPathfinder();
    testRoutingGoals();
    testDirections();
    testWorldWalls();
    testDiagonalWalls();
    testSetToNextTiles();
    testPathPlanner();
    testRouteResult();
    testBlockedBySim();
    testDoorWait();
    testDoorPortalTree();
    testPreRouteTree();
    testMoveOutOfWay();
    testGoHere();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all routing tests passed\n");
    return failures ? 1 : 0;
}
