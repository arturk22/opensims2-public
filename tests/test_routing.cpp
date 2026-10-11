// World occupancy, pathfinding and the routing primitives on synthetic lots.
#include "vm/vm.hpp"
#include "vm/world.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

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

// Set to Next 8 (same tile, round and round), 9 (adjacent to the object in a
// local, by direction) and 34 (within one tile).
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
    // Same tile, from b: a, then b again.
    code.add(kGroup, 0x1000,
             tree({expr(StackId, 0, Assign, Lit, b.id, go(1), E), stn(8, 0, go(2), E),
                   expr(Temp, 0, Assign, StackId, 0, go(3), E), stn(8, 0, go(4), E),
                   expr(Temp, 1, Assign, StackId, 0, T, E)}));
    Thread t = vm.start(a.id, 0x1000);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == a.id && t.temps[1] == b.id,
          "Set to Next 8 goes round the tile's objects, not the contained one");
    // Adjacent to a (local 0): west (direction 0) comes before east (direction 4); then none.
    code.add(kGroup, 0x1001,
             tree({expr(Local, 0, Assign, Lit, a.id, go(1), E), expr(StackId, 0, Assign, Lit, 0, go(2), E),
                   stn(9, 0, go(3), E), expr(Temp, 0, Assign, StackId, 0, go(4), E), stn(9, 0, go(5), E),
                   expr(Temp, 1, Assign, StackId, 0, go(6), E), stn(9, 0, E, T)},
                  0, 1));
    t = vm.start(a.id, 0x1001);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == west.id && t.temps[1] == east.id,
          "Set to Next 9 by direction");
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

int main() {
    testPathfinder();
    testRoutingGoals();
    testDirections();
    testWorldWalls();
    testSetToNextTiles();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all routing tests passed\n");
    return failures ? 1 : 0;
}
