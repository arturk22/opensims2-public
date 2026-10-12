// The motive engine and autonomy on synthetic objects (src/vm/motives.hpp,
// src/vm/autonomy.hpp).

#include "vm/autonomy.hpp"
#include "vm/animation.hpp"
#include "vm/interactions.hpp"
#include "vm/motives.hpp"

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

constexpr uint32_t kSimGroup = 0x7F000010;
constexpr uint32_t kFoodGroup = 0x7F000020;
constexpr uint32_t kLooGroup = 0x7F000030;
constexpr uint32_t kMenuGroup = 0x7F000040;

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

enum Owner : uint8_t { SAttr = 0x01, Lit = 0x07, Temp = 0x08, AdRange = 0x0C, AdMin = 0x1D };
enum Op : uint8_t { Assign = 5 };

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

static ObjectDefinition definition(uint16_t type, uint16_t treeTable, uint32_t guid) {
    ObjectDefinition d;
    d.fileName = "test object";
    d.fields.resize(120);
    auto at = [&](size_t offset) -> uint16_t& { return d.fields[(offset - 0x44) / 2]; };
    at(0x4E) = treeTable;
    at(0x52) = type;
    at(0x5C) = uint16_t(guid);
    at(0x5E) = uint16_t(guid >> 16);
    return d;
}

static StringTable curves(std::initializer_list<std::pair<size_t, const char*>> set) {
    StringTable t;
    t.format = 0xFFFD;
    for (size_t m = 0; m < kMotiveCount; ++m) {
        const char* text = "(-100;0) (100;0)";
        for (auto [k, s] : set)
            if (k == m)
                text = s;
        t.entries.push_back({lang::EnglishUS, text, "", 0});
    }
    return t;
}

static InteractionTable::Interaction adEntry(uint16_t action, uint16_t guard, uint32_t tta, size_t motiveIndex,
                                             int16_t min, int16_t range, uint16_t personality, uint32_t atten = 1) {
    InteractionTable::Interaction i;
    i.action = action;
    i.guard = guard;
    i.stringIndex = tta;
    i.autonomy = 50;
    i.attenuationCode = atten;
    i.motives.assign(7, std::vector<InteractionTable::Advertisement>(kMotiveCount));
    i.motives[0][motiveIndex] = {min, range, personality}; // the adult table (eSimAge 0)
    return i;
}

// A Sim (adult, autonomy level 50) and two objects: "food" (TTA 0, hunger
// ad 0 + 40 x nice; TTA 1 the same, but its check tree zeroes the range)
// and "loo" (TTA 0, bladder ad 30 flat). The adult interaction score curves
// make hunger and bladder linear from -100 to 0 (score 0 to 50), flat above.
struct Fixture {
    CodeLibrary code;
    VM vm{code};
    ObjectId sim = 0, food = 0, loo = 0;

    Fixture() {
        code.add(kGlobalGroup, 0x28B, curves({{7, "(-100;0) (0;50) (100;50)"}, {9, "(-100;0) (0;50) (100;50)"}}));
        code.add(kGlobalGroup, 0x227, curves({{7, "(-100;3) (0;1) (100;1)"}, {9, "(-100;1) (100;1)"}}));
        code.add(kSimGroup, 0x80, definition(kObjectTypePerson, 0, 0x5150001));
        code.add(kFoodGroup, 0x80, definition(4, 1, 0x0F00D001));
        InteractionTable foodTable;
        foodTable.version = 0x4E;
        foodTable.interactions = {adEntry(0x1001, 0, 0, 7, 0, 40, 1), adEntry(0x1001, 0x1002, 1, 7, 0, 40, 1)};
        code.add(kFoodGroup, 1, std::move(foodTable));
        // Action: stack object attribute 0 := 7. Guard of TTA 1: ad range[7] := 0.
        code.add(kFoodGroup, 0x1001, tree({expr(SAttr, 0, Assign, Lit, 7, T, E)}));
        code.add(kFoodGroup, 0x1002, tree({expr(AdRange, 7, Assign, Lit, 0, T, E)}, 1));
        code.add(kLooGroup, 0x80, definition(4, 1, 0x0B00D001));
        InteractionTable looTable;
        looTable.version = 0x4E;
        looTable.interactions = {adEntry(0x1001, 0, 0, 9, 30, 0, 0)};
        code.add(kLooGroup, 1, std::move(looTable));
        code.add(kLooGroup, 0x1001, tree({expr(SAttr, 0, Assign, Lit, 9, T, E)}));

        Object& s = vm.instantiate(kSimGroup, 0x80);
        sim = s.id;
        s.person->data[persondata::PersonAge] = lifestage::Adult;
        s.person->data[persondata::AutonomyLevel] = 50;
        s.person->data[2] = 500; // nice
        s.position = {5.5f, 5.5f, 1};
        Object& f = vm.instantiate(kFoodGroup, 0x80);
        food = f.id;
        f.position = {7.5f, 5.5f, 1};
        Object& l = vm.instantiate(kLooGroup, 0x80);
        loo = l.id;
        l.position = {5.5f, 9.5f, 1};
    }
    Person& p() { return *vm.object(sim)->person; }
};

static void testCurves() {
    const MotiveCurve c = MotiveCurve::parse("(-100;10) (-90;6.5) (0;1) (100;1)");
    check(c.points.size() == 4, "curve points parsed");
    check(std::abs(c.at(-95) - 8.25f) < 1e-4f && c.at(-200) == 10 && c.at(50) == 1, "curves interpolate linearly, flat beyond the ends");
    const MotiveCurve u = MotiveCurve::parse("(-100;-100) (50;50) (-25;40)");
    check(u.points.size() == 3 && u.points[1].first == -25, "points out of order are sorted");
}

static void testMotiveChangeAndDecay() {
    Fixture fx;
    // Set Motive Change: hunger (7) +60 an hour up to 50; bladder (9) -60 down to -10.
    fx.code.add(kSimGroup, 0x1100, tree({instr(0x1D, go(1), E, {Lit, Lit, 7, 0, 60, 0, 50, 0}),
                                         instr(0x1D, T, E, {Lit, Lit, 9, 0, uint8_t(-60), 0xFF, uint8_t(-10), 0xFF})}));
    Thread t = fx.vm.start(fx.sim, 0x1100);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && fx.p().motiveChanges.size() == 2, "Set Motive Change adds to the list");
    for (int i = 0; i < 900; ++i) // half an hour
        fx.vm.tick();
    check(fx.p().motives[7] == 30 && fx.p().motives[9] == -10, "changes run per tick (delta / 60 / 30), until the limit");
    for (int i = 0; i < 900; ++i)
        fx.vm.tick();
    check(fx.p().motives[7] == 50, "and stop at it");
    check(fx.p().data[persondata::TickCounter] == 1800, "person data 27 counts the ticks");

    // Decay per day: -1440 a day is -1 a game minute.
    Fixture fy;
    fy.p().data[93] = -1440;
    for (int i = 0; i < 600; ++i)
        fy.vm.tick();
    check(fy.p().motives[7] == -20, "the daily decay of person data 93 lowers hunger");
    // Set Motive Change with flag 0x02 zeroes it; flag 0x01 clears every change.
    fy.code.add(kSimGroup, 0x1100, tree({instr(0x1D, go(1), E, {Lit, Lit, 7, 2, 0, 0, 0, 0}),
                                         instr(0x1D, T, E, {Lit, Lit, 0, 1, 0, 0, 0, 0})}));
    Thread u = fy.vm.start(fy.sim, 0x1100);
    fy.vm.run(u);
    check(fy.p().data[93] == 0 && fy.p().motiveChanges.empty(), "flags 0x02 (zero the decay) and 0x01 (clear all)");
    fy.p().data[persondata::MotivesStatic] = 2;
    fy.p().motives[7] = 0;
    fy.vm.tick();
    check(fy.p().data[persondata::TickCounter] == 601 && fy.p().motives[7] == 0, "static motives (person data 29 >= 2) do not move");
}

static void testMood() {
    Fixture fx;
    fx.p().motives[7] = -50; // weight 2
    fx.p().motives[9] = 50;  // weight 1
    check(std::abs(computeMood(fx.vm, *fx.vm.object(fx.sim)) - (-50.0f * 2 + 50.0f) / 3) < 1e-3f,
          "mood: motives averaged by the happy curve weights");
    fx.p().data[persondata::MotivesNotInMood] = 1 << 9;
    check(std::abs(computeMood(fx.vm, *fx.vm.object(fx.sim)) + 50.0f) < 1e-3f, "person data 118 leaves motives out");
    fx.p().data[persondata::MotivesNotInMood] = 0;
    fx.vm.tick();
    check(fx.p().motives[3] == -17 && fx.p().motives[4] == -17, "the tick writes Mood and Mood+Mem");
}

static void testFindBestInteraction() {
    Fixture fx;
    fx.p().motives[7] = -60;
    fx.p().motives[9] = 20;
    check(adValue(*fx.vm.object(fx.sim), 0, 40, 1) == 20 && adValue(*fx.vm.object(fx.sim), 0, 40, 2) == 20 &&
              adValue(*fx.vm.object(fx.sim), 10, 40, 0) == 50 && adValue(*fx.vm.object(fx.sim), 10, 40, 5) == 10,
          "ad = min + range x personality / 1000 (even types inverted; type 0: min + range)");
    AutonomyDecision seen;
    autonomyObserver() = [&](const AutonomyDecision& d) { seen = d; };
    // Main: Find Best Interaction (all motives), then Gosub Found Action, then temp 0 := 1.
    fx.code.add(kSimGroup, 0x1100, tree({instr(0x03, go(1), F, {0, 0, 0, 0}), instr(0x1E, go(2), go(2), {0, 0}),
                                         expr(Temp, 0, Assign, Lit, 1, T, E)}));
    Thread t = fx.vm.start(fx.sim, 0x1100);
    fx.vm.run(t);
    autonomyObserver() = nullptr;
    // Food: hunger -60 + 20 / 1000: +0.01 on the curve, / 10. The loo's
    // bladder is on the flat part: 0. TTA 1 of the food: its check tree
    // zeroed the range.
    check(seen.found && seen.candidates == 3 && seen.checked == 2,
          "the check tree that sets ads runs before ranking, then the best ranked one is tested and wins");
    check(seen.best.size() == 1 && seen.best[0].object == fx.food && seen.best[0].interaction == 0 &&
              std::abs(seen.best[0].score - 0.001f) < 1e-6f,
          "score = interaction curve gain; a check tree that zeroes the ad drops the entry");
    check(t.status == Thread::Status::Finished && t.temps[0] == 1, "Gosub Found Action branches when the action returns");
    check(!fx.vm.object(fx.food)->attributes.empty() && fx.vm.object(fx.food)->attributes[0] == 7,
          "the action tree ran with the object as stack object");
    check(fx.p().data[persondata::FindBestActionObject] == fx.food && fx.vm.object(fx.sim)->queue.empty(),
          "person data 53 holds the object; the interaction left the queue when it ended");

    // The bigger gain wins.
    Fixture fy;
    fy.p().motives[7] = -60;
    fy.p().motives[9] = -40; // loo: +0.03 x 0.5 / 10, against +0.02 x 0.5 / 10 for the food
    fy.code.add(kSimGroup, 0x1100, tree({instr(0x03, T, F, {0, 0, 0, 0})}));
    autonomyObserver() = [&](const AutonomyDecision& d) { seen = d; };
    Thread u = fy.vm.start(fy.sim, 0x1100);
    fy.vm.run(u);
    autonomyObserver() = nullptr;
    check(u.result && seen.best.size() == 2 && seen.best[0].object == fy.loo, "unattenuated, the bigger gain wins");
    // Only hunger (mask 1 << 7).
    fy.code.add(kSimGroup, 0x1101, tree({instr(0x03, T, F, {0x80, 0, 1, 0})}));
    Thread w = fy.vm.start(fy.sim, 0x1101);
    autonomyObserver() = [&](const AutonomyDecision& d) { seen = d; };
    fy.vm.run(w);
    autonomyObserver() = nullptr;
    check(w.result && seen.best.size() == 1 && seen.best[0].object == fy.food, "a motive mask limits the motives");
    // Nothing worth doing at full motives.
    fy.p().motives[7] = 100;
    fy.p().motives[9] = 100;
    Thread x = fy.vm.start(fy.sim, 0x1100);
    fy.vm.run(x);
    check(x.status == Thread::Status::Finished && !x.result, "false when no entry scores above 0");
}

static void testFind5Worst() {
    Fixture fx;
    auto& m = fx.p().motives;
    m = {};
    m[5] = 50, m[6] = -10, m[7] = 20, m[8] = 30, m[9] = -50, m[13] = 90, m[14] = 0, m[15] = 10;
    fx.code.add(kSimGroup, 0x1100, tree({instr(0x21, T, E, {0, 0, 0, 0, 0, 0, 0, 0})}));
    Thread t = fx.vm.start(fx.sim, 0x1100);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == 9 && t.temps[1] == 6 && t.temps[2] == 14 &&
              t.temps[3] == 15 && t.temps[4] == 7,
          "Find 5 Worst Motives: the five lowest motive numbers in temps 0-4");
}


// A check tree that adds action strings (the fridge's food menu): each one
// is a dynamic interaction ranked with the ads of the moment it was made,
// with parameters {stack object, temp 0}; the entry itself does not count.
static void testDynamicInteractions() {
    Fixture fx;
    fx.p().motives[7] = -60;
    fx.p().motives[9] = 100;
    fx.code.add(kMenuGroup, 0x80, definition(4, 1, 0x0F00D002));
    InteractionTable menu;
    menu.version = 0x4E;
    menu.interactions = {adEntry(0x1001, 0x1002, 0, 7, 0, 1, 0)};
    fx.code.add(kMenuGroup, 1, std::move(menu));
    StringTable names;
    names.format = 0xFFFD;
    names.entries.push_back({lang::EnglishUS, "Small", "", 0});
    names.entries.push_back({lang::EnglishUS, "Big", "", 0});
    fx.code.add(kMenuGroup, 0x12E, std::move(names));
    fx.code.add(kMenuGroup, 0x1001, tree({expr(SAttr, 0, Assign, Lit, 3, T, E)}));
    // ad min[7] := 10, "Small"; temp 0 := 4; ad min[7] := 90, "Big"; false.
    fx.code.add(kMenuGroup, 0x1002,
                tree({expr(AdMin, 7, Assign, Lit, 10, go(1), E), instr(0x32, go(2), go(2), {0, 0, 0, 0, 1}),
                      expr(Temp, 0, Assign, Lit, 4, go(3), E), expr(AdMin, 7, Assign, Lit, 90, go(4), E),
                      instr(0x32, go(5), go(5), {0, 0, 0, 0, 2}), expr(Temp, 0, Assign, Lit, 0, F, E)},
                     1));
    Object& m = fx.vm.instantiate(kMenuGroup, 0x80);
    m.position = {5.5f, 6.5f, 1};
    const ObjectId menuId = m.id;
    check((checkTreeAdFlags(fx.vm, m, 0x1002) & (CheckTreeChangesAds | CheckTreeActionStrings)) ==
              (CheckTreeChangesAds | CheckTreeActionStrings),
          "the tree scan sees the ads and the action strings");
    AutonomyDecision seen;
    autonomyObserver() = [&](const AutonomyDecision& d) { seen = d; };
    fx.code.add(kSimGroup, 0x1100, tree({instr(0x03, T, F, {0, 0, 0, 0})}));
    Thread t = fx.vm.start(fx.sim, 0x1100);
    fx.vm.run(t);
    autonomyObserver() = nullptr;
    check(t.result && !seen.best.empty() && seen.best[0].object == menuId && seen.best[0].name == "Big",
          "the action string made with the bigger ad wins");
    check(fx.p().foundActionArgs[0] == menuId && fx.p().foundActionArgs[1] == 4 && fx.p().foundActionName == "Big",
          "its parameters are the stack object and temp 0");
}

// The binary's candidate order (FBA_FindStaticInteractions): motive by
// motive (lowest bit first), and an object's entries last first
// (AppendInteractionsForAuto walks them backwards). Two entries with the
// same ads tie, and the libc qsort keeps a short list's order (insertion
// sort under 7 items): the later entry wins, as the trash pile's "Limpar"
// (entry 1) over "Jogar Fora" (entry 0).
static void testCandidateOrder() {
    Fixture fx;
    InteractionTable foodTable;
    foodTable.version = 0x4E;
    foodTable.interactions = {adEntry(0x1001, 0, 0, 7, 0, 40, 1), adEntry(0x1001, 0x1002, 1, 7, 0, 40, 1),
                              adEntry(0x1001, 0, 2, 7, 0, 40, 1)};
    fx.code.add(kFoodGroup, 1, std::move(foodTable));
    fx.p().motives[7] = -60;
    fx.p().motives[9] = -40;
    fx.code.add(kSimGroup, 0x1100, tree({instr(0x03, T, F, {0, 0, 0, 0})}));
    AutonomyDecision seen;
    autonomyObserver() = [&](const AutonomyDecision& d) { seen = d; };
    Thread t = fx.vm.start(fx.sim, 0x1100);
    fx.vm.run(t);
    autonomyObserver() = nullptr;
    check(seen.considered.size() == 4 && seen.considered[0].object == fx.food && seen.considered[3].object == fx.loo,
          "candidates motive by motive: hunger (7) before bladder (9)");
    // considered[0..2]: the food's entries 2, 1, 0
    check(seen.best.size() >= 3 && seen.best[0].object == fx.loo && seen.best[1].object == fx.food &&
              seen.best[1].interaction == 2 && seen.best[2].interaction == 0,
          "an object's entries last first: of two with the same ads the later one ranks first");
}

int main() {
    testCurves();
    testMotiveChangeAndDecay();
    testMood();
    testFindBestInteraction();
    testCandidateOrder();
    testDynamicInteractions();
    testFind5Worst();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("autonomy: all tests passed\n");
    return failures ? 1 : 0;
}
