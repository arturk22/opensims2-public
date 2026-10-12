// World occupancy, pathfinding and the routing primitives on synthetic lots.
#include "vm/animation.hpp"
#include "vm/go_here.hpp"
#include "vm/interactions.hpp"
#include "vm/locomotion.hpp"
#include "vm/path_planner.hpp"
#include "vm/vm.hpp"
#include "vm/world.hpp"
#include "formats/lot.hpp"

#include <array>
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

// A model footprint tile (the CRES "footprint" mask) as the planner's
// rectangles: the piano bench's tile (1,1) of pianoUprightSaloon, rows 0-4
// of 16 with columns 0-6 set ("7f00"), halves to rows 0-2 (any of four
// subcells) and columns 0-3: three rectangles x 0.51..0.99, y 0.51..0.865.
static void testFootprintMask() {
    std::array<uint8_t, 32> mask{};
    for (int row = 0; row < 5; ++row)
        mask[size_t(2 * row)] = 0x7F;
    const auto rows = halveFootprintMask(mask);
    check(rows[0] == 0x0F && rows[1] == 0x0F && rows[2] == 0x0F && rows[3] == 0, "the bench's mask halves to 3 rows of 4 cells");
    const auto rects = footprintMaskRects(rows, 1, 1);
    check(rects.size() == 3, "one rectangle per row's run");
    if (rects.size() == 3) {
        check(std::fabs(rects[0][0] - 0.51f) < 1e-4f && std::fabs(rects[0][2] - 0.99f) < 1e-4f, "x from the tile's centre, shrunk 0.01");
        check(std::fabs(rects[0][1] - 0.51f) < 1e-4f && std::fabs(rects[2][3] - 0.865f) < 1e-4f, "y over rows 0-2 of 8");
    }
    // A whole row: one rectangle across the tile.
    std::array<uint8_t, 32> full{};
    full[0] = full[1] = 0xFF;
    const auto one = footprintMaskRects(halveFootprintMask(full), 0, 0);
    check(one.size() == 1 && std::fabs(one[0][0] + 0.49f) < 1e-4f && std::fabs(one[0][2] - 0.49f) < 1e-4f,
          "a full row spans the tile less 0.01 a side");
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
    // DoBlockedGoalStillAvailable: it cannot be asked (one goal), so the
    // walker steps aside itself once (TryGetSelfOutOfWay), is blocked again
    // and gives up with "blocked by person" (the binary waits only after a
    // request the other took).
    if (std::getenv("DEBUG_BLOCK"))
        std::fprintf(stderr, "corridor: status %d result %d rr %d waited %d ticks %d x %.2f\n", int(t.status), t.result,
                     walker.person ? walker.person->data[62] : -1, waited, ticks, walker.position.x);
    check(t.status == Thread::Status::Finished && !t.result && walker.person && walker.person->data[62] == 14 &&
              ticks < 300 && walker.position.x < 6,
          "blocked by a Sim in a corridor: steps aside once, then fails with 14");
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
    if (std::getenv("DEBUG_BLOCK"))
        std::fprintf(stderr, "pass: a %d/%d at %.2f,%.2f (pd62 %d) b %d/%d at %.2f,%.2f (pd62 %d) closest %.2f\n",
                     int(vm.threads.at(a.id).status), vm.threads.at(a.id).result, a.position.x, a.position.y,
                     a.person->data[62], int(vm.threads.at(b.id).status), vm.threads.at(b.id).result, b.position.x,
                     b.position.y, b.person->data[62], closest);
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

// BeginFollow's shape of a walk (docs/routing-mac.md §6.1): legs cut every
// 0.2 tile, and a Sim facing away from its way turns in place first (the
// TurnInPlace animation of the 45-degree steps, mirrored counter-clockwise);
// up to 40 degrees off it walks on at once.
static void testTurnInPlace() {
    for (const int facing : {0, 4, 2}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(12, 12);
        for (const char* n : {"a-TurnInPlace-45", "a-TurnInPlace-90", "a-TurnInPlace-135", "a-TurnInPlace-180"})
            vm.animations.add(n, 1.0f);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        lamp.position = {10.5f, 5.5f, 1};
        Object& sim = vm.instantiate(kGroup, 0x81);
        sim.position = {2.5f, 5.5f, 1};
        sim.person->data[58] = 27; // an adult
        sim.data[objdata::Direction] = int16_t(facing);
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        vm.run(t);
        const bool turning = sim.route && sim.route->climb.turnSteps != 0;
        const int steps = turning ? sim.route->climb.turnSteps : 0;
        const std::string anim = turning ? sim.route->climb.animation : "";
        const float x0 = sim.position.x;
        // Each walking leg on the floor at most 0.2 tile (+ the remainder).
        float longest = 0;
        if (sim.route) {
            Point2 at{sim.position.x, sim.position.y};
            for (const auto& p : sim.route->points)
                if (p.ticks == 0) {
                    longest = std::max(longest, std::hypot(p.x - at[0], p.y - at[1]));
                    at = {p.x, p.y};
                }
        }
        for (int i = 0; i < 25 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        const float x25 = sim.position.x;
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        if (std::getenv("DEBUG_TURN"))
            std::fprintf(stderr, "facing %d: steps %d %s x0 %.2f x25 %.2f longest %.3f result %d\n", facing, steps,
                         anim.c_str(), x0, x25, longest, t.result);
        check(t.status == Thread::Status::Finished && t.result, "turn in place: the walk ends");
        check(longest < 0.41f, "the legs are cut every 0.2 tile");
        if (facing == 4) { // direction 4 faces +X, the way: no turn, walking at once
            check(!turning && x25 > 4, "facing the way: no turn in place");
        } else {
            const int want = facing == 2 ? -2 : -4; // +Y to +X: 90 degrees clockwise; -X to +X: 180
            check(steps == want && anim == "a-TurnInPlace-" + std::to_string(45 * -want) && x0 == 2.5f && x25 == 2.5f,
                  "facing away: turns in place (TurnInPlace of its steps) before walking");
        }
    }
}

// The locomotion's clips (docs/routing-mac.md §6.1): a standing Sim starts
// with the turnAndGo of its first leg's 45-degree steps (mirrored the other
// way round), its frame covering the clip's distance in the clip's length
// while it stands where the clip's root is; then the gait's walk cycle at its
// speed, and the turnAndStop at the end over what is left, which leaves it
// facing the final way.
static StringTable strings(std::initializer_list<const char*> values) {
    StringTable t;
    for (const char* v : values)
        t.entries.push_back({lang::EnglishUS, v, "", 0});
    return t;
}

// `extra`: also the walkShort cycle (half the walk's speed and stride) and
// the long "step" clips (0.6 over 1.2 s).
static void addLocoSet(CodeLibrary& code, VM& vm, bool extra = false) {
    code.add(kGlobalGroup, 0x84,
             strings({extra ? "turnInPlace-alt(turnInPlace),turnAndGoShort-alt(turnAndGoShort),turnAndGo-alt(turnAndGo),"
                              "turnAndStopShort-alt(turnAndStopShort),turnAndStop-alt(turnAndStop),stepShort-alt(stepShort),"
                              "step-alt(step)"
                            : "turnInPlace-alt(turnInPlace),turnAndGoShort-alt(turnAndGoShort),turnAndGo-alt(turnAndGo),"
                              "turnAndStopShort-alt(turnAndStopShort),turnAndStop-alt(turnAndStop),stepShort-alt(stepShort)",
                      extra ? "stepInPlace-alt(stepInPlace),walkShort-alt0-normal(walkShort),walk-alt0-normal(walk)"
                            : "stepInPlace-alt(stepInPlace),walk-alt0-normal(walk)",
                      "",
                      "turnAndGo-alt@2(turnAndGo@2),turnAndStop-alt@2(turnAndStop@2)", "run-alt0-normal(run)", ""}));
    // Clips at 0-180 degrees: a start 0.75 s over 0.85 tile whose root lags
    // 0.1 behind its frame and turns from where the Sim faced to the way;
    // a stop 0.5 s over 0.34 turning to the final facing; a step 0.6 s.
    for (int k = 0; k <= 4; ++k) {
        const float turn = -float(k) * float(M_PI / 4);
        const std::string deg = std::to_string(45 * k);
        auto clip = [&](const std::string& name, int ms, float distance, float yaw0, float yaw1, float lag) {
            LocoClip c;
            c.ticks = ms, c.distance = distance, c.velocity = distance / (float(ms) / 1000), c.turn = turn;
            c.cycle = 0.5f, c.type = 6;
            const int n = int(std::ceil(float(ms) / 1000 * 30));
            for (int i = 0; i <= n; ++i) {
                const float u = float(i) / float(n);
                c.root.push_back({0, lag * (1 - u), yaw0 + (yaw1 - yaw0) * u});
            }
            vm.animations.add(name, float(ms) / 1000, c.velocity);
            vm.animations.addLoco(name, c);
        };
        clip("a-turnAndGo-alt-" + deg, 750, 0.85f, -turn, 0, -0.1f);
        clip("a-turnAndGoShort-alt-" + deg, 750, 0.35f, -turn, 0, 0);
        clip("a-turnAndStop-alt-" + deg, 500, 0.34f, 0, turn, 0);
        clip("a-turnAndStopShort-alt-" + deg, 500, 0.16f, 0, turn, 0);
        clip("a-stepShort-" + deg, 600, 0.3f, -turn, -turn, 0);
        if (extra)
            clip("a-step-" + deg, 1200, 0.6f, -turn, -turn, 0);
    }
    if (extra) {
        LocoClip still;
        still.ticks = 600, still.type = 10;
        still.root.assign(19, {0, 0, 0});
        vm.animations.add("a-stepInPlace", 0.6f, 0);
        vm.animations.addLoco("a-stepInPlace", still);
        LocoClip shorter;
        shorter.ticks = 690, shorter.distance = 0.69f, shorter.velocity = 1.0f, shorter.type = 11;
        shorter.root.assign(22, {0, 0, 0});
        vm.animations.add("a-male-walkShort-alt0-normal", 0.69f, 1.0f);
        vm.animations.addLoco("a-male-walkShort-alt0-normal", shorter);
    }
    LocoClip walk;
    walk.ticks = 690, walk.distance = 1.38f, walk.velocity = 2.0f, walk.type = 11;
    walk.root.assign(22, {0, 0, 0});
    vm.animations.add("a-male-walk-alt0-normal", 0.69f, 2.0f);
    vm.animations.addLoco("a-male-walk-alt0-normal", walk);
    vm.animations.add("a-male-run-alt0-normal", 0.5f, 8.0f);
}

static void testLocoPhases() {
    using Loco = Object::Route::Loco;
    // Facing +X (direction 4, the way), +Y (2: 90 degrees clockwise to the
    // way) and -Y (6: 90 degrees counter-clockwise).
    for (const int facing : {4, 2, 6}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(14, 12);
        addLocoSet(code, vm);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        lamp.position = {11.5f, 5.5f, 1};
        Object& sim = vm.instantiate(kGroup, 0x81);
        sim.position = {2.5f, 5.5f, 1};
        sim.person->data[58] = 27;
        sim.data[objdata::Direction] = int16_t(facing);
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        std::vector<std::string> clips;
        std::string firstClip;
        bool firstMirror = false;
        int startTicks = 0, stopTicks = 0;
        float lagMid = 0, walkStep = 0, startStep = 0;
        float prevFx = sim.position.x;
        int lastDir = -1;
        for (int i = 0; i < 1000 && t.status == Thread::Status::Running; ++i) {
            vm.run(t);
            if (!sim.route)
                continue;
            const Loco& L = sim.route->loco;
            if (L.animation.empty() || L.kind == Loco::None)
                continue;
            if (clips.empty() || clips.back() != L.animation)
                clips.push_back(L.animation);
            if (L.kind == Loco::Start) {
                if (firstClip.empty())
                    firstClip = L.animation, firstMirror = L.mirror;
                ++startTicks;
                if (L.tick == L.ticks / 2) {
                    lagMid = std::hypot(sim.position.x - L.fx, sim.position.y - L.fy);
                    startStep = L.fx - prevFx;
                }
            } else if (L.kind == Loco::Walk && L.fx - prevFx > 0.01f) {
                walkStep = L.fx - prevFx;
            } else if (L.kind == Loco::Stop) {
                ++stopTicks;
            }
            prevFx = L.fx;
            lastDir = sim.data[objdata::Direction];
        }
        if (std::getenv("DEBUG_LOCO")) {
            std::fprintf(stderr, "facing %d: start %s%s (%d ticks, lag %.3f, step %.4f) walk step %.4f stop %d ticks, at %.2f,%.2f dir %d result %d clips:",
                         facing, firstClip.c_str(), firstMirror ? " mirrored" : "", startTicks, lagMid, startStep, walkStep,
                         stopTicks, sim.position.x, sim.position.y, sim.data[objdata::Direction], t.result);
            for (const auto& c : clips)
                std::fprintf(stderr, " %s", c.c_str());
            std::fprintf(stderr, "\n");
        }
        const std::string want = facing == 4 ? "a-turnAndGo-alt-0" : "a-turnAndGo-alt-90";
        check(t.status == Thread::Status::Finished && t.result, "loco: the walk ends there");
        check(firstClip == want && firstMirror == (facing == 6), "loco: the start turns by the first leg's steps, mirrored counter-clockwise");
        // (Started 100 ms back, FollowOneStep's head start of the first phase.)
        check(startTicks >= 19 && startTicks <= 21, "loco: the start lasts its clip (0.75 s) less the 100 ms head start");
        check(std::fabs(lagMid - 0.05f) < 0.02f, "loco: in the start the Sim stands at the clip's root, behind its frame");
        check(std::fabs(startStep - 0.85f / 23) < 0.01f || facing != 4, "loco: the start's frame covers its distance in its length");
        check(std::fabs(walkStep - 2.0f / 30) < 0.002f, "loco: the walk cycle at the gait's speed");
        check(clips.size() >= 3 && clips[clips.size() - 2] == "a-male-walk-alt0-normal" &&
                  clips.back().starts_with("a-turnAndStop-alt-") && stopTicks >= 14 && stopTicks <= 16,
              "loco: start, walk, stop");
        check(std::fabs(sim.position.x - 10.5f) < 0.05f && lastDir == 4, "loco: stopped in front of the lamp, facing it");
    }
}

// The start and stop clips take the 45-degree clip nearest the angle (no
// blend of two angles: that is CreateStepCycle's, for sidesteps), the walk
// follows the route's B-spline (its heading changes little from tick to
// tick), and the stop leaves the Sim at the final facing.
static void testLocoBlend() {
    using Loco = Object::Route::Loco;
    CodeLibrary code;
    VM vm(code);
    vm.world = World(14, 12);
    addLocoSet(code, vm);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {11.5f, 8.5f, 1};
    Object& sim = vm.instantiate(kGroup, 0x81);
    sim.position = {2.5f, 5.5f, 1};
    sim.person->data[58] = 27;
    sim.data[objdata::Direction] = 4;
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    bool oneAngle = true, sawStart = false, sawStop = false;
    float maxTurn = 0, lastHeading = 0;
    bool walking = false;
    for (int i = 0; i < 1000 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (!sim.route)
            continue;
        const Loco& L = sim.route->loco;
        if (L.kind == Loco::Start || L.kind == Loco::Stop) {
            (L.kind == Loco::Start ? sawStart : sawStop) = true;
            for (const auto& l : L.layers)
                oneAngle = oneAngle && l.animation.substr(l.animation.rfind('-')) ==
                                           L.layers.front().animation.substr(L.layers.front().animation.rfind('-'));
        }
        if (L.kind == Loco::Walk) {
            if (walking)
                maxTurn = std::max(maxTurn, std::fabs(std::remainder(L.heading - lastHeading, float(2 * M_PI))));
            walking = true;
            lastHeading = L.heading;
        } else {
            walking = false;
        }
    }
    if (std::getenv("DEBUG_LOCO"))
        std::fprintf(stderr, "blend: one angle %d max turn %.3f at %.2f,%.2f dir %d\n", oneAngle, maxTurn, sim.position.x,
                     sim.position.y, sim.data[objdata::Direction]);
    check(t.status == Thread::Status::Finished && t.result, "loco blend: the walk ends there");
    check(sawStart && sawStop && oneAngle, "loco: a start or a stop takes the 45-degree clip nearest the angle, no angle blend");
    check(maxTurn < 0.5f, "loco blend: the walk's frame follows a curve (no turn of 30 degrees in a tick)");
}

// The frame's curve: a uniform cubic B-spline with the ends tripled
// (cPathStrategyCubic::UpdatePathData): it starts and ends at the ends, cuts
// a corner, and each segment is the matrix's blend of four control points.
static void testLocoCurve() {
    const LocoCurve c(0, 0, {{1, 0}, {1, 1}});
    const auto a = c.at(0, 0), z = c.at(c.segments() - 1, 1), s1 = c.at(1, 0);
    float nearest = 9;
    for (size_t j = 0; j < c.segments(); ++j)
        for (int k = 0; k <= 20; ++k) {
            const auto p = c.at(j, float(k) / 20);
            nearest = std::min(nearest, std::hypot(p[0] - 1, p[1]));
        }
    check(c.segments() == 4 && std::hypot(a[0], a[1]) < 1e-6f && std::hypot(z[0] - 1, z[1] - 1) < 1e-6f,
          "loco curve: starts and ends at the route's ends (tripled)");
    check(std::hypot(s1[0] - 1.0f / 6, s1[1]) < 1e-6f, "loco curve: a segment is 1/6 (P0 + 4 P1 + P2) at its start");
    check(nearest > 0.1f && c.total() > std::sqrt(2.0f) && c.total() < 2, "loco curve: it cuts the corner");
    check(c.passedAfter(0) == 0 && c.passedAfter(1) == 1 && c.passedAfter(2) == 1 && c.passedAfter(3) == 2,
          "loco curve: the route's points are passed as the segments ending near them are done");
}

// walk and walkShort blend by stride (InitChannelNetwork): the walk between
// the start and the stop is a whole number of half cycles, the start and the
// stop blended with their short clips by the same weight.
// The "*Blend" poses' blend (InitChannelNetwork's +0xec): 0.2, at most
// 0.95 of the start's and the stop's distances, in seconds at the speed.
static void testLocoPoseBlend() {
    check(std::fabs(locoPoseBlend(0, 0, 2) - 0.1f) < 1e-5f, "pose blend: 0.2 at 2 per second");
    check(std::fabs(locoPoseBlend(0.1f, 0, 1) - 0.095f) < 1e-5f, "pose blend: 0.95 of a short start");
    check(std::fabs(locoPoseBlend(1, 0.15f, 1) - 0.1425f) < 1e-5f, "pose blend: 0.95 of a short stop");
}

// fitStride as InitChannelNetwork fits a way (walk 1.393 / walkShort 0.683
// a cycle, turnAndGo 0.853 / Short 0.347, turnAndStop 0.337 / 0.16).
static void testFitStride() {
    std::function<std::pair<float, float>(bool)> stop = [](bool) { return std::pair<float, float>{0.337f, 0.16f}; };
    // 5 tiles: 3.81 left, 6 half cycles, w = (5.369 - 5) / (5.369 - 2.556).
    StrideFit f = fitStride(5, 1.393f, 0.683f, 0.853f, 0.347f, &stop, false, 0, 0);
    check(std::fabs(f.cycles - 3) < 1e-4f && std::fabs(f.weight - 0.369f / 2.813f) < 2e-3f &&
              std::fabs(f.stopWeight - f.weight) < 1e-5f,
          "fit stride: whole half cycles, one short weight for start, cycles and stop");
    // 1.2 tiles: one half cycle would want w = 0.66 > 0.3: half a cycle less,
    // the last quarter and the stop at v = 1, w from the rest.
    f = fitStride(1.2f, 1.393f, 0.683f, 0.853f, 0.347f, &stop, false, 0, 0);
    check(std::fabs(f.cycles) < 1e-4f && std::fabs(f.stopWeight - 1) < 1e-5f && std::fabs(f.weight - 0.4857f) < 2e-3f,
          "fit stride: over 0.3, half a cycle less and the stop short");
    const float startD = f.weight * 0.347f + (1 - f.weight) * 0.853f;
    const float stride = f.weight * 0.683f + (1 - f.weight) * 1.393f;
    check(std::fabs(startD + 0.25f * stride + 0.25f * 0.683f + 0.16f - 1.2f) < 2e-3f, "fit stride: the rest covers the way");
    // Walking on (no start): what is left of the half cycle counts first.
    f = fitStride(3, 1.393f, 0.683f, 0, 0, &stop, false, 0.3f, 0);
    check(std::fabs(f.cycles - (0.3f + 0.5f * 4)) < 1e-4f, "fit stride: f0 + half cycles");
}

static void testLocoStride() {
    using Loco = Object::Route::Loco;
    CodeLibrary code;
    VM vm(code);
    vm.world = World(14, 12);
    addLocoSet(code, vm, true);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {6.5f, 5.5f, 1};
    Object& sim = vm.instantiate(kGroup, 0x81);
    sim.position = {2.5f, 5.5f, 1};
    sim.person->data[58] = 27;
    sim.data[objdata::Direction] = 4;
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    float startShort = -1, walkShort = -1, weight = -1;
    int walkTicks = 0;
    float walked = 0;
    for (int i = 0; i < 1000 && t.status == Thread::Status::Running; ++i) {
        const float fx = sim.route ? sim.route->loco.fx : sim.position.x;
        vm.run(t);
        if (!sim.route)
            continue;
        const Loco& L = sim.route->loco;
        weight = L.strideWeight;
        for (const auto& l : L.layers) {
            if (L.kind == Loco::Start && l.animation.find("turnAndGoShort") != std::string::npos)
                startShort = l.weight;
            if (L.kind == Loco::Walk && l.animation.find("walkShort") != std::string::npos)
                walkShort = l.weight;
        }
        if (L.kind == Loco::Walk) {
            ++walkTicks;
            walked += L.fx - fx;
        }
    }
    // 3 tiles: start 0.85 + stop 0.34 long, 0.35 + 0.16 short; strides 1.38
    // and 0.69: 1.81 left, 3 half cycles, w = 0.26 / 1.715.
    const float want = 0.26f / 1.715f;
    const float halfCycles = walked / (0.5f * ((1 - weight) * 1.38f + weight * 0.69f));
    if (std::getenv("DEBUG_LOCO"))
        std::fprintf(stderr, "stride: weight %.3f start short %.3f walk short %.3f walk %d ticks %.3f tiles = %.2f half cycles at %.2f\n",
                     weight, startShort, walkShort, walkTicks, walked, halfCycles, sim.position.x);
    check(t.status == Thread::Status::Finished && t.result, "loco stride: the walk ends there");
    check(std::fabs(weight - want) < 0.02f && std::fabs(startShort - weight) < 1e-3f && std::fabs(walkShort - weight) < 1e-3f,
          "loco stride: walk and walkShort, turnAndGo and turnAndGoShort blend by the same stride weight");
    check(std::fabs(halfCycles - 3) < 0.25f, "loco stride: the walk covers a whole number of half cycles");
}

// A straight way to the facing the Sim already has is a sidestep
// (CreateStepCycle): the "step" clips blended by distance, played 1.4 times
// as fast, the body keeping its facing.
static void testSidestep() {
    using Loco = Object::Route::Loco;
    CodeLibrary code;
    VM vm(code);
    vm.world = World(14, 12);
    addLocoSet(code, vm, true);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {3.5f, 3.5f, 1};
    Object& sim = vm.instantiate(kGroup, 0x81);
    sim.position = {2.5f, 4.0f, 1};
    sim.person->data[58] = 27;
    sim.data[objdata::Direction] = 4; // facing +X, the lamp's west side's facing
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    bool sidestep = false, kept = true, walked = false;
    int ticks = 0;
    std::string clips;
    for (int i = 0; i < 400 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (!sim.route)
            continue;
        const Loco& L = sim.route->loco;
        if (L.sidestep && L.kind == Loco::Start) {
            sidestep = true;
            ++ticks;
            kept = kept && sim.data[objdata::Direction] == 4;
            if (clips.empty())
                for (const auto& l : L.layers)
                    clips += l.animation + "*" + std::to_string(l.weight).substr(0, 4) + " ";
        }
        walked = walked || L.kind == Loco::Walk;
    }
    if (std::getenv("DEBUG_LOCO"))
        std::fprintf(stderr, "sidestep: %d (%d ticks, %s) kept %d walked %d at %.2f,%.2f dir %d result %d\n", sidestep, ticks,
                     clips.c_str(), kept, walked, sim.position.x, sim.position.y, sim.data[objdata::Direction], t.result);
    check(t.status == Thread::Status::Finished && t.result && std::fabs(sim.position.y - 3.5f) < 0.05f,
          "sidestep: the walk ends there");
    // 0.5 between stepShort (0.3) and step (0.6): 1/3 short, 2/3 long, at
    // (0.6/1.2 * 2/3 + 0.3/0.6 * 1/3) * 1.4 = 0.7 m/s: 0.71 s.
    check(sidestep && kept && !walked && ticks >= 17 && ticks <= 23 && clips.find("a-step-90") != std::string::npos,
          "sidestep: steps sideways without turning (step/stepShort by distance, 1.4 times as fast)");
}

// A walker that comes on a Sim standing in its way slows to a stop with a
// stop clip (StopLocomotion) before the blocked handling starts; an object
// put in its way (the per-step test against objects) fails the route with
// "blocked by object" (15).
static void testLocoHalt() {
    using Loco = Object::Route::Loco;
    for (const bool object : {false, true}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(16, 5);
        for (int x = 0; x < 16; ++x) {
            vm.world->addWall(1, x, 1, x + 1, 1);
            vm.world->addWall(1, x, 2, x + 1, 2);
        }
        addLocoSet(code, vm);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        lamp.position = {15.5f, 1.5f, 1};
        Object& walker = vm.instantiate(kGroup, 0x81);
        walker.position = {1.5f, 1.5f, 1};
        walker.person->data[58] = 27;
        walker.data[objdata::Direction] = 4;
        if (!object) {
            Object& other = vm.instantiate(kGroup, 0x81);
            other.position = {8.5f, 1.5f, 1};
            other.person->data[58] = 27;
        }
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
        Thread t = vm.start(walker.id, 0x1000, {}, lamp.id);
        int haltTicks = 0, halts = 0, firstHalt = 0;
        bool wasHalting = false;
        float closest = 99;
        for (int i = 0; i < 600 && t.status == Thread::Status::Running; ++i) {
            if (object && i == 40) { // a box dropped two tiles ahead
                Object& box = vm.instantiate(kGroup, 0x80);
                box.position = {std::floor(walker.position.x) + 2.5f, 1.5f, 1};
            }
            vm.run(t);
            if (!walker.route)
                continue;
            const bool halting = walker.route->loco.halting && walker.route->loco.kind == Loco::Stop;
            if (halting) {
                ++haltTicks;
                if (!wasHalting)
                    ++halts;
                if (halts == 1)
                    ++firstHalt;
            }
            wasHalting = halting;
            if (!object && vm.object(3))
                closest = std::min(closest, std::hypot(walker.position.x - vm.object(3)->position.x,
                                                       walker.position.y - vm.object(3)->position.y));
        }
        if (std::getenv("DEBUG_LOCO"))
            std::fprintf(stderr, "halt (%s): %d halts, first %d ticks (%d in all), closest %.3f, result %d route result %d at %.2f\n",
                         object ? "object" : "Sim", halts, firstHalt, haltTicks, closest, t.result, walker.person->data[62],
                         walker.position.x);
        check(halts >= 1 && firstHalt >= 14 && firstHalt <= 16, "loco halt: a blocked walker plays a stop clip first (0.5 s)");
        if (object)
            check(t.status == Thread::Status::Finished && !t.result && walker.person->data[62] == 15,
                  "loco halt: an object put in the way fails the route (blocked by object)");
        else
            check(closest >= 0.4f, "loco halt: the stop keeps it out of the other Sim's way");
    }
}

// A runner (steps longer than the predicted blocking looks ahead) does not
// step into a Sim standing in its way: the locomotor's placement test refuses
// the step and the Sim becomes the blocker.
static void testRunnerStep() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(16, 5);
    for (int x = 0; x < 16; ++x) {
        vm.world->addWall(1, x, 1, x + 1, 1);
        vm.world->addWall(1, x, 2, x + 1, 2);
    }
    vm.walkTilesPerTick = 0.45f; // longer steps than the prediction looks ahead
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    lamp.position = {15.5f, 1.5f, 1};
    Object& runner = vm.instantiate(kGroup, 0x81);
    runner.position = {1.5f, 1.5f, 1};
    runner.person->data[58] = 27;
    Object& other = vm.instantiate(kGroup, 0x81);
    other.position = {6.5f, 1.5f, 1};
    other.person->data[58] = 27;
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(runner.id, 0x1000, {}, lamp.id);
    float closest = 99;
    bool blocked = false;
    for (int i = 0; i < 400 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        closest = std::min(closest, std::hypot(runner.position.x - other.position.x, runner.position.y - other.position.y));
        blocked = blocked || (runner.route && runner.route->waiting);
    }
    if (std::getenv("DEBUG_LOCO"))
        std::fprintf(stderr, "runner: closest %.3f blocked %d result %d at %.2f\n", closest, blocked, t.result, runner.position.x);
    (void)blocked;
    check(t.status == Thread::Status::Finished && !t.result && runner.person->data[62] == 14 && closest >= 0.283f,
          "a runner is blocked (blocked by person) before it steps into a Sim in its way");
}

// Two Sims walking at each other down a corridor with a bay block each
// other: the binary's DoBlockedGoalStillAvailable sees the blocker blocked
// by the walker and the walker steps aside itself (TryGetSelfOutOfWay), so
// both get through at once instead of asking and waiting.
static void testMutualBlock() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(14, 5);
    for (int x = 0; x < 14; ++x) {
        vm.world->addWall(1, x, 1, x + 1, 1);
        if (x < 6 || x > 8)
            vm.world->addWall(1, x, 2, x + 1, 2);
    }
    vm.world->addWall(1, 6, 2, 6, 4); // a bay three tiles wide, two deep
    vm.world->addWall(1, 9, 2, 9, 4);
    vm.world->addWall(1, 6, 4, 9, 4);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& east = vm.instantiate(kGroup, 0x80);
    east.position = {13.5f, 1.5f, 1};
    Object& west = vm.instantiate(kGroup, 0x80);
    west.position = {0.5f, 1.5f, 1};
    Object& a = vm.instantiate(kGroup, 0x81);
    a.position = {2.5f, 1.5f, 1};
    Object& b = vm.instantiate(kGroup, 0x81);
    b.position = {11.5f, 1.5f, 1};
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    code.add(kGroup, 0x1001, tree({instr(0x1B, T, F, {0, 0, 4, 0xFE, 0, 0, 0})}));
    // The pause (DoPause): "CT - Route Blocked Animation", here a 30-tick wait.
    Behavior pauseTree = tree({expr(0x09, 2, Assign, Lit, 30, go(1), E), instr(0x00, T, E, {2, 0})}, 4);
    pauseTree.fileName = "CT - Route Blocked Animation";
    code.add(kGlobalGroup, 0x1BB, pauseTree);
    vm.threads.insert_or_assign(a.id, vm.start(a.id, 0x1000, {}, east.id));
    vm.threads.insert_or_assign(b.id, vm.start(b.id, 0x1001, {}, west.id));
    int ticks = 0;
    for (; ticks < 1500; ++ticks) {
        vm.tick();
        if (vm.threads.at(a.id).status != Thread::Status::Running && vm.threads.at(b.id).status != Thread::Status::Running)
            break;
    }
    if (std::getenv("DEBUG_BLOCK"))
        std::fprintf(stderr, "mutual: %d ticks, a %d/%d at %.2f,%.2f b %d/%d at %.2f,%.2f\n", ticks,
                     int(vm.threads.at(a.id).status), vm.threads.at(a.id).result, a.position.x, a.position.y,
                     int(vm.threads.at(b.id).status), vm.threads.at(b.id).result, b.position.x, b.position.y);
    check(vm.threads.at(a.id).result && vm.threads.at(b.id).result && a.position.x > 11 && b.position.x < 3 && ticks < 335,
          "two Sims blocking each other in a corridor: one steps aside, both get through");
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
    // Near the lamp's corner: pushed along the shorter way out only
    // (TSObjectMoveAwayFromOtherObjects: y by -0.5001, x left alone).
    const auto q2 = goHerePoint(vm, sim.id, 8.8f, 4.1f, 1);
    check(q2 && std::fabs(q2->x - 8.8f) < 1e-4f && std::fabs(q2->y - 3.5999f) < 1e-3f,
          "go here: out of a footprint along the shorter axis");
    // Next to the Sim: 0.4 off its 0.2-wide footprint.
    const auto q3 = goHerePoint(vm, sim.id, 6.7f, 1.5f, 1);
    check(q3 && std::fabs(q3->x - 7.0001f) < 1e-3f, "go here: 0.4 off the Sim's footprint");
    // The diagonal walls as the binary moves off them: along the wall.
    {
        World dw(8, 8);
        dw.addWall(1, 0, 4, 4, 0); // slope -1: fx + fy = 1 in tile (1, 2)
        dw.addWall(1, 4, 4, 8, 8); // slope +1: fx = fy in tile (5, 5)
        check(dw.diagonalSides({1, 2, 1}) == 0x10 && dw.diagonalSides({5, 5, 1}) == 0x20 &&
                  dw.diagonalSides({1, 1, 1}) == 0,
              "go here: diagonal wall sides");
        const Point2 a = movePointAwayFromWalls(dw, {1.5f, 2.6f}, 1, 0.45f);
        check(std::fabs(a[0] - 1.7689f) < 1e-3f && std::fabs(a[1] - 2.3311f) < 1e-3f,
              "go here: diagonal 0x10 as the binary has it");
        const Point2 b = movePointAwayFromWalls(dw, {5.5f, 5.6f}, 1, 0.45f);
        check(std::fabs((b[0] - 5.5f) - (b[1] - 5.6f)) < 1e-4f && b[0] > 5.7f, "go here: diagonal 0x20 along the wall");
    }
    // A rug (type 0x13): its menu merges a destination at the floor under
    // the mouse; without interactions the destination's alone; another
    // object only its own.
    {
        ObjectDefinition rugDef = definition(0xAAAA0003, 0);
        rugDef.fields[(0x52 - 0x44) / 2] = 0x13;
        code.add(kGroup, 0x82, rugDef);
        Object& rug = vm.instantiate(kGroup, 0x82);
        rug.position = {10.5f, 2.5f, 1};
        const ObjectId rugId = rug.id;
        const Position floor{10.2f, 2.7f, 1};
        check(isGoHereRug(vm, rugId) && !isGoHereRug(vm, lamp.id), "rug: type 0x13");
        auto m = objectClick(vm, sim.id, rugId, floor, true);
        check(m.object == rugId && m.destination && vm.object(m.destination)->guid() == kGoHereGuid,
              "rug: its menu with a go-here destination");
        releaseDestination(vm, m.destination);
        check(!vm.object(m.destination), "rug: the destination goes with the menu");
        m = objectClick(vm, sim.id, rugId, floor, false);
        check(m.object && m.object != rugId && !m.destination, "rug without interactions: the destination's menu");
        releaseDestination(vm, m.object);
        m = objectClick(vm, sim.id, rugId, std::nullopt, true);
        check(m.object == rugId && !m.destination, "rug off the floor: its menu alone");
        vm.object(rugId)->data[objdata::Category] = 0xA7;
        check(!isGoHereRug(vm, rugId), "rug: not with category 0xA7");
        m = objectClick(vm, sim.id, lamp.id, floor, true);
        check(m.object == lamp.id && !m.destination, "another object: its menu");
        vm.removeObject(rugId);
    }
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
    testFootprintMask();
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
    testMutualBlock();
    testTurnInPlace();
    testLocoPhases();
    testRunnerStep();
    testLocoBlend();
    testLocoCurve();
    testLocoStride();
    testFitStride();
    testLocoPoseBlend();
    testSidestep();
    testLocoHalt();
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
