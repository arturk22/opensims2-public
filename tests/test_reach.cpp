// Run Functional Tree (0x14) and Reach/Put (0x74) on synthetic objects
// (src/vm/reach.cpp).

#include "vm/vm.hpp"

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

constexpr uint32_t kGroup = 0x7F000040;

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

enum Owner : uint8_t { SAttr = 0x01, Lit = 0x07, Temp = 0x08, Param = 0x09, StackId = 0x0A };
enum Op : uint8_t { Eq = 2, Assign = 5 };

static Behavior::Instruction expr(uint8_t lo, int16_t ld, uint8_t op, uint8_t ro, int16_t rd, Target t, Target f) {
    return instr(0x02, t, f,
                 {uint8_t(ld), uint8_t(uint16_t(ld) >> 8), uint8_t(rd), uint8_t(uint16_t(rd) >> 8), 0, op, lo, ro});
}

static Behavior tree(std::vector<Behavior::Instruction> code, uint8_t args = 0) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.argCount = args;
    b.instructions = std::move(code);
    return b;
}

static ObjectDefinition definition(uint32_t guid, uint16_t slotsId, bool person = false) {
    ObjectDefinition def;
    def.fields.resize(60);
    def.fields[(0x5C - 0x44) / 2] = uint16_t(guid);
    def.fields[(0x5E - 0x44) / 2] = uint16_t(guid >> 16);
    def.fields[(0x68 - 0x44) / 2] = slotsId;
    if (person)
        def.fields[(0x52 - 0x44) / 2] = kObjectTypePerson;
    return def;
}

// OBJD 0x80 a Sim (two hands: containment slots 0 and 1), 0x81 a chair whose
// "sit" (function 8 = OBJf entry 26) has guard 0x1010 and action 0x1011,
// 0x82 a newspaper, 0x83 a table with one containment slot at table height.
struct Fixture {
    CodeLibrary code;
    VM vm{code};
    ObjectId sim = 0, chair = 0, paper = 0, table = 0;

    Fixture() {
        registerPrimitives(vm);
        Slots hands;
        hands.slots.resize(2);
        code.add(kGroup, 0x80, hands);
        Slots surface;
        surface.slots.resize(1);
        surface.slots[0].height = 3;
        code.add(kGroup, 0x83, surface);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0x80, true));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0));
        code.add(kGroup, 0x82, definition(0xAAAA0003, 0));
        code.add(kGroup, 0x83, definition(0xAAAA0004, 0x83));
        ObjectFunctions f;
        f.functions.resize(27);
        f.functions[26] = {0x1010, 0x1011};
        code.add(kGroup, 0x81, std::move(f));
        // Guard: true unless the stack object's attribute 1 is 1.
        code.add(kGroup, 0x1010, tree({expr(SAttr, 1, Eq, Lit, 1, F, T)}));
        // Action: stack object attribute 0 := 5 + parameter 0, temp 2 := stack object ID.
        code.add(kGroup, 0x1011, tree({expr(SAttr, 0, Assign, Lit, 5, go(1), E), expr(Temp, 2, Assign, StackId, 0, go(2), E),
                                       instr(0x02, T, E, {0, 0, 0, 0, 0, 3, SAttr, Param})},
                                      1));
        StringTable list;
        list.entries.push_back({lang::EnglishUS, "a2o-paper-grasp, a2o-paper-carry(carry)", "", 0});
        code.add(kGroup, 0x81, list);
        sim = vm.instantiate(kGroup, 0x80).id;
        chair = vm.instantiate(kGroup, 0x81).id;
        paper = vm.instantiate(kGroup, 0x82).id;
        table = vm.instantiate(kGroup, 0x83).id;
        for (ObjectId id : {chair, paper, table})
            vm.object(id)->attributes.resize(4);
        vm.object(sim)->position = {2.5f, 2.5f, 1};
        vm.object(paper)->position = {3.5f, 2.5f, 1};
        vm.object(table)->position = {2.5f, 3.5f, 1};
        // One second each (30 ticks), the object changing hands at half.
        for (const char* a : {"a2o-floor-get", "a2o-table-get", "a2o-table-put", "a2o-floor-put"}) {
            vm.animations.add(a, 1.0f);
            vm.animations.addEvent(a, "event=0", 500);
        }
    }

    // Runs tree `id` in the Sim's thread with this stack object until it ends.
    Thread run(uint16_t id, ObjectId stackObject, std::array<int16_t, 4> args = {}, int maxTicks = 200) {
        Thread t = vm.start(sim, id, args, stackObject, stackObject);
        for (int i = 0; i < maxTicks && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        return t;
    }
};

static Behavior::Instruction functional(uint16_t function, uint8_t flags, Target t, Target f) {
    return instr(0x14, t, f, {uint8_t(function), uint8_t(function >> 8), flags});
}

static void testRunFunctionalTree() {
    Fixture fx;
    fx.code.add(kGroup, 0x1001, tree({functional(8, 0, T, F)}));
    Thread t = fx.run(0x1001, fx.chair);
    check(t.status == Thread::Status::Finished && t.result, "sit: the chair's OBJf entry 26 runs, true");
    check(fx.vm.object(fx.chair)->attributes[0] == 5 && t.temps[2] == fx.chair,
          "the action runs in my thread with the chair as stack object and code owner");

    fx.vm.object(fx.chair)->attributes[0] = 0;
    fx.code.add(kGroup, 0x1002, tree({functional(8, 0x04, T, F)}));
    t = fx.run(0x1002, fx.chair);
    check(t.result && fx.vm.object(fx.chair)->attributes[0] == 0, "flag 0x04 runs the check tree only");

    fx.vm.object(fx.chair)->attributes[1] = 1;
    t = fx.run(0x1001, fx.chair);
    check(t.status == Thread::Status::Finished && !t.result && fx.vm.object(fx.chair)->attributes[0] == 0,
          "a guard that fails: false, no action");
    fx.vm.object(fx.chair)->attributes[1] = 0;

    fx.code.add(kGroup, 0x1003, tree({functional(3, 0, T, F)}));
    t = fx.run(0x1003, fx.chair);
    check(t.status == Thread::Status::Finished && !t.result, "no action for the function (dispose): false");
    t = fx.run(0x1001, fx.paper);
    check(t.status == Thread::Status::Finished && !t.result, "an object without an OBJf: false");

    fx.code.add(kGroup, 0x1004, tree({functional(8, 0x02, T, F)}, 1));
    t = fx.run(0x1004, fx.chair, {4});
    check(t.result && fx.vm.object(fx.chair)->attributes[0] == 9, "flag 0x02 passes my parameters on");
}

static Behavior::Instruction reach(uint8_t owner, int16_t data, uint8_t mode, uint8_t flags, uint16_t grasp, Target t,
                                   Target f) {
    return instr(0x74, t, f,
                 {0, 0, 0, owner, uint8_t(data), uint8_t(uint16_t(data) >> 8), 0, 0, 0, flags, mode, uint8_t(grasp),
                  uint8_t(grasp >> 8)});
}

static void testReachPut() {
    Fixture fx;
    // Pick up the stack object (grasp entry 0).
    fx.code.add(kGroup, 0x1001, tree({reach(StackId, 0, 0, 0x04, 0, T, F)}));
    Thread t = fx.vm.start(fx.sim, 0x1001, {}, fx.paper, fx.paper);
    Object& sim = *fx.vm.object(fx.sim);
    int ticks = 0, handAt = -1;
    for (; ticks < 100 && t.status == Thread::Status::Running; ++ticks) {
        fx.vm.run(t);
        if (handAt < 0 && fx.vm.containedIn(fx.sim, 0) == fx.paper)
            handAt = ticks;
    }
    check(t.status == Thread::Status::Finished && t.result, "pick up: true when the reach ends");
    check(ticks == 31 && handAt == 15, "the object changes hands at event 0, the animation ends after its length");
    check(sim.animation.name.empty() && sim.animation.serial == 1, "the Sim played the floor reach once");
    Object& paper = *fx.vm.object(fx.paper);
    check(paper.data[objdata::ContainerId] == fx.sim && paper.data[objdata::SlotNumber] == 0,
          "the object is in the Sim's hand (slot 0)");
    check((sim.animation.overlays == std::vector<std::string>{"a2o-paper-grasp", "a2o-paper-carry"}),
          "grasp and carry overlays");
    sim.position = {4.5f, 4.5f, 1};
    fx.vm.followContainers();
    check(paper.position.x == 4.5f && paper.position.y == 4.5f, "what the Sim holds goes with it");

    // Drop onto the table in parameter 0, slot in temp 0.
    fx.code.add(kGroup, 0x1002, tree({expr(Temp, 0, Assign, Lit, 0, go(1), E), reach(Param, 0, 1, 0x01, 0, T, F)}, 1));
    t = fx.run(0x1002, fx.table, {fx.table});
    check(t.result && fx.vm.containedIn(fx.table, 0) == fx.paper && !fx.vm.containedIn(fx.sim, 0),
          "drop onto an object: into its containment slot");
    check(sim.animation.overlays.empty() && sim.carryOverlays.empty(), "the carry overlays come off");
    check(sim.animation.serial == 2, "a reach animation played");
    t = fx.run(0x1002, fx.table, {fx.table});
    check(t.status == Thread::Status::Finished && !t.result, "dropping with empty hands: false");

    // Pick it up from the table, then drop it on the floor.
    t = fx.run(0x1001, fx.paper);
    check(t.result && fx.vm.containedIn(fx.sim, 0) == fx.paper, "pick up from a slot");
    fx.code.add(kGroup, 0x1003, tree({reach(Lit, 0, 2, 0, 0, T, F)}));
    sim.data[objdata::Direction] = 4;
    t = fx.run(0x1003, 0);
    check(t.result && paper.data[objdata::ContainerId] == 0 && paper.position.x == 4.5f && paper.position.y == 4.5f,
          "drop onto the floor: on the Sim's tile without a world");
    fx.code.add(kGroup, 0x1004, tree({reach(Lit, 0, 0, 0, 0, T, F)}));
    t = fx.run(0x1004, 0);
    check(t.status == Thread::Status::Failed, "picking up nothing: the game's error");
}

int main() {
    testRunFunctionalTree();
    testReachPut();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("reach: all tests passed\n");
    return failures ? 1 : 0;
}
