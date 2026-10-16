// The motive engine and autonomy on synthetic objects (src/vm/motives.hpp,
// src/vm/autonomy.hpp).

#include "vm/autonomy.hpp"
#include "vm/animation.hpp"
#include "vm/environment.hpp"
#include "vm/interactions.hpp"
#include "vm/motives.hpp"
#include "formats/lot.hpp"

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
    // A second change of the same motive is added, not swapped in: both run.
    Fixture fz;
    fz.code.add(kSimGroup, 0x1100, tree({instr(0x1D, go(1), E, {Lit, Lit, 7, 0, 60, 0, 50, 0}),
                                         instr(0x1D, T, E, {Lit, Lit, 7, 0, 30, 0, 100, 0})}));
    Thread z = fz.vm.start(fz.sim, 0x1100);
    fz.vm.run(z);
    for (int i = 0; i < 900; ++i)
        fz.vm.tick();
    check(fz.p().motiveChanges.size() == 2 && fz.p().motives[7] == 45, "two changes of one motive add up");

    // Decay per day: -1440 a day is -1 a game minute, applied as -2 every
    // 60 ticks (cTSMotives vtable 0x10: data / 24 / 30).
    // The step comes when the simulator's tick (passed before it counts up)
    // is 60 past the person's last (0): from a counter of 0, the 61st tick.
    Fixture fy;
    fy.p().data[93] = -1440;
    for (int i = 0; i < 60; ++i)
        fy.vm.tick();
    check(fy.p().motives[7] == 0, "no decay before the 60-tick update");
    fy.vm.tick();
    check(fy.p().motives[7] == -2 && exactMotive(*fy.vm.object(fy.sim), 7) == -2.0f, "a 720th of the day's decay");
    for (int i = 0; i < 540; ++i)
        fy.vm.tick();
    check(fy.p().motives[7] == -20, "the daily decay of person data 93 lowers hunger");
    // The update clamps what the trees wrote, and the old set takes it.
    fy.p().motives[14] = 150;
    check(exactMotive(*fy.vm.object(fy.sim), 14) == 150.0f, "a motive a tree writes is taken as it is");
    for (int i = 0; i < 60; ++i)
        fy.vm.tick();
    check(fy.p().motives[14] == 100 && fy.p().oldMotives[14] == 100.0f && fy.p().oldMotives[7] == -22.0f,
          "the 60-tick update clamps to 100 and copies the old set");
    // Set Motive Change with flag 0x02 zeroes it; flag 0x01 clears every change.
    fy.code.add(kSimGroup, 0x1100, tree({instr(0x1D, go(1), E, {Lit, Lit, 7, 2, 0, 0, 0, 0}),
                                         instr(0x1D, T, E, {Lit, Lit, 0, 1, 0, 0, 0, 0})}));
    Thread u = fy.vm.start(fy.sim, 0x1100);
    fy.vm.run(u);
    check(fy.p().data[93] == 0 && fy.p().motiveChanges.empty(), "flags 0x02 (zero the decay) and 0x01 (clear all)");
    fy.p().data[persondata::MotivesStatic] = 2;
    fy.p().motives[7] = 0;
    fy.vm.tick();
    check(fy.p().data[persondata::TickCounter] == 662 && fy.p().motives[7] == 0, "static motives (person data 29 >= 2) do not move");
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
    for (int i = 0; i < 60; ++i)
        fx.vm.tick();
    check(fx.p().motives[3] == 0, "the mood waits for the 60-tick update");
    fx.vm.tick();
    check(fx.p().motives[3] == -17 && fx.p().motives[4] == -17, "the 60-tick update writes Mood and Mood+Mem");
    fx.p().data[persondata::MoodBooster] = 200;
    check(std::abs(computeMood(fx.vm, *fx.vm.object(fx.sim)) - (200.0f - 50.0f / 3)) < 1e-3f,
          "person data 163 is added to the mood");
    for (int i = 0; i < 60; ++i)
        fx.vm.tick();
    check(fx.p().motives[3] == 183 && fx.p().motives[4] == 183 && fx.p().oldMotives[3] == -50.0f / 3,
          "the base game's mood is not clamped (the old set has the last one, clamped)");
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

// The environment motive (vm/environment.hpp, docs/motives-mac.md §8): the
// environment room, the room's three scores less the crap, the light's term,
// the outdoor cells' tent, the walls and floors of a lot, and the step's
// tick from the simulator's counter.
static void testEnvironment() {
    check(environmentRoomId(-1, 3, 3) == -1 && environmentRoomId(3, 12.5f, 7.2f) == 0x10003 &&
              environmentRoomId(0, 12.5f, 7.2f) == 2 + 256 && environmentRoomId(0, -4, 4000) == 0xFF00,
          "environment room: -1, room + 0x10000, the outdoor cell of 5 tiles (0-255)");
    check(environmentLightTerm(0.9f) == 0 && environmentLightTerm(2) == 0 && environmentLightTerm(0.55f) == -20 &&
              environmentLightTerm(0.1f) == -20 && std::abs(environmentLightTerm(0.725f) + 10) < 1e-4f,
          "the light's term: 0 from 0.9 up, -20 from 0.55 down, linear between");

    constexpr uint32_t kRugGroup = 0x7F000050, kBinGroup = 0x7F000060;
    Fixture fx;
    StringTable curve;
    curve.format = 0xFFFD;
    curve.entries.push_back({lang::EnglishUS, "(0;0) (1000;1000000)  -- Max total object niceness vs Room Size", "", 0});
    fx.code.add(kGlobalGroup, 0x2BC, curve);
    auto priced = [](uint16_t price) {
        ObjectDefinition d = definition(4, 0, 0x0E000000u + price);
        d.fields[(0x64 - 0x44) / 2] = price; // field 0x12 "price"
        return d;
    };
    fx.code.add(kRugGroup, 0x80, priced(400));
    fx.code.add(kBinGroup, 0x80, priced(200));
    Object& rug = fx.vm.instantiate(kRugGroup, 0x80);
    rug.position = {6.5f, 6.5f, 1};
    rug.data[7] = 100; // Niceness Score: x 1
    Object& bin = fx.vm.instantiate(kBinGroup, 0x80);
    bin.position = {6.5f, 7.5f, 1};
    bin.data[7] = 50, bin.data[86] = 3; // x 0.5, crap 3
    for (ObjectId id : {fx.sim, rug.id, bin.id, fx.food, fx.loo})
        fx.vm.object(id)->data[objdata::Room] = 2;
    fx.vm.object(fx.food)->data[objdata::Room] = 1; // another room: not counted
    fx.vm.object(fx.loo)->data[objdata::Room] = 1;
    // Room 2: walls 28 over 2 sides (mean 14: 25), floors 46 over 4 tiles
    // (mean 11.5: 12.5), objects 400 + 100 against 1000 x sqrt(4) (12.5),
    // crap 0.2 + 3 + 1: 45.8.
    VM::EnvironmentArchitecture a;
    a.wallNice = 28, a.wallCrap = 0.2f, a.wallCount = 2, a.floorNice = 46, a.floorCrap = 1, a.floorArea = 4;
    fx.vm.environmentArchitecture[0x10002] = a;
    fx.vm.environmentKnown = true;
    const EnvironmentRooms rooms = environmentRooms(fx.vm);
    const EnvironmentRoom& r = rooms.at(0x10002);
    check(r.objectNice == 500 && r.objectCrap == 3 && r.objectCount == 2 && r.wallScore == 25 && r.floorScore == 12.5f &&
              std::abs(r.objectScore - 12.5f) < 1e-4f, // the curve's 1 / 1000 is not exact in float
          "a room's walls, floors and objects (price x niceness / 100, crap; 1000 x sqrt(area))");
    check(std::abs(environmentScore(fx.vm, *fx.vm.object(fx.sim)) - 45.8f) < 1e-4f,
          "a Sim's environment: its room's scores less the crap");
    fx.vm.floorIllumination = [](int, float, float) -> std::optional<float> { return 0.725f; };
    check(std::abs(environmentScore(fx.vm, *fx.vm.object(fx.sim)) - 35.8f) < 1e-4f, "less the light's term");
    fx.vm.floorIllumination = nullptr;
    // Outdoors: the cells around, by a tent two cells wide, not normalized.
    // At (7.5, 7.5) the cell (1, 1) weighs 1 and (2, 1) 0.5.
    VM::EnvironmentArchitecture lawn;
    lawn.floorNice = 25 * 23, lawn.floorArea = 25; // 25
    fx.vm.environmentArchitecture.clear();
    fx.vm.environmentArchitecture[1 + 256] = lawn;
    fx.vm.environmentArchitecture[2 + 256] = lawn;
    Object& sim = *fx.vm.object(fx.sim);
    sim.data[objdata::Room] = 0;
    sim.position = {7.5f, 7.5f, 1};
    check(environmentScore(fx.vm, sim) == 37.5f, "outdoors: the cells around by the tent, the weights not normalized");
    sim.position = {-1, -1, 1};
    check(environmentScore(fx.vm, sim) == 0, "no room (out of the world): 0");
    sim.position = {7.5f, 7.5f, 1};

    // The 60-tick step sets it; from a SIMI counter of 60 or more the first
    // step is the first tick.
    sim.person->motives[motive::Environment] = -70;
    fx.vm.simulatorTicks = 1234;
    fx.vm.tick();
    check(sim.person->motives[motive::Environment] == 38 && exactMotive(sim, motive::Environment) == 37.5f,
          "the 60-tick step sets the environment, at once after loading a saved counter");
    check(sim.person->memoriesTick == 1234 && fx.vm.simulatorTicks == 1235, "the step's tick is the simulator's");

    // A lot's floors: every tile of a room (a tile in each room it has a
    // quarter of), the outdoors by cells of every level; without catalog
    // products the area still counts.
    Lot lot;
    lot.width = 2, lot.height = 1, lot.minLevel = 0, lot.levelCount = 2;
    lot.rooms = {{1, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    setLotEnvironment(fx.vm, lot);
    check(fx.vm.environmentArchitecture.at(0x10001).floorArea == 2 && fx.vm.environmentArchitecture.at(0).floorArea == 3,
          "floors: a room's tiles, the outdoors' of every level in its cell");
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

// A visitor (person data 32 "Person Type" != 0) gets only the entries with
// "allow visitors" (TTAB flag 0x0001; cEdithObjectTestSim::TestInteraction).
static void testVisitorEntries() {
    Fixture fx;
    InteractionTable table;
    table.version = 0x4E;
    InteractionTable::Interaction open = adEntry(0x1001, 0, 0, 7, 0, 40, 1), closed = open;
    open.flags = 0x0C01;   // visitors allowed (bit 0x40 clear: adults, inverted before version 0x52)
    closed.flags = 0x0C00; // residents only
    check(availableToAge(*fx.vm.object(fx.sim), table, open) && availableToAge(*fx.vm.object(fx.sim), table, closed),
          "a resident (person type 0) gets both entries");
    fx.p().data[kPersondataPersonType] = 1;
    check(availableToAge(*fx.vm.object(fx.sim), table, open), "a visitor gets an entry with flag 0x0001");
    check(!availableToAge(*fx.vm.object(fx.sim), table, closed), "a visitor does not get an entry without flag 0x0001");
    fx.p().data[kPersondataPersonType] = 2;
    check(!availableToAge(*fx.vm.object(fx.sim), table, closed), "an NPC (person type 2) neither");
}

// Check trees get parameter 0 = 1 from autonomy and 0 from the pie menu
// (the cEdithObjectTestSim's "autonomous" byte +0x10: FBA_FindStaticInteractions
// and FBA_RunCheckTree set it to 1, TestInteraction passes it as parameter 0;
// docs/primitives-mac.md §4f, round n32). The Fire's "Interaction -
// Extinguish - TEST" refuses with parameter 0 = 1: the household never puts
// a fire out on its own, only when the player says so.
static void testAutonomousParameter() {
    constexpr uint32_t kFireGroup = 0x7F000050;
    Fixture fx;
    fx.p().motives[7] = -60;
    fx.p().motives[9] = 100;
    fx.code.add(kFireGroup, 0x80, definition(4, 1, 0x0F00D003));
    InteractionTable table;
    table.version = 0x4E;
    table.interactions = {adEntry(0x1001, 0x1002, 0, 7, 90, 0, 0)}; // a bigger hunger ad than the food's
    fx.code.add(kFireGroup, 1, std::move(table));
    fx.code.add(kFireGroup, 0x1001, tree({expr(SAttr, 0, Assign, Lit, 5, T, E)}));
    // The guard: parameter 0 == 1 -> false, else true (Extinguish - TEST's node 2).
    fx.code.add(kFireGroup, 0x1002, tree({expr(0x09, 0, 2, Lit, 1, F, T)}, 1));
    Object& o = fx.vm.instantiate(kFireGroup, 0x80);
    o.position = {6.5f, 5.5f, 1};
    const ObjectId fire = o.id;
    QueuedInteraction q;
    q.object = fire;
    q.guard = 0x1002;
    check(checkInteraction(fx.vm, fx.sim, q, nullptr, false) == CheckResult::True,
          "the pie menu's check tree gets parameter 0 = 0");
    check(checkInteraction(fx.vm, fx.sim, q, nullptr, true) == CheckResult::False,
          "an autonomous check gets parameter 0 = 1");
    AutonomyDecision seen;
    autonomyObserver() = [&](const AutonomyDecision& d) { seen = d; };
    fx.code.add(kSimGroup, 0x1100, tree({instr(0x03, T, F, {0, 0, 0, 0})}));
    Thread t = fx.vm.start(fx.sim, 0x1100);
    fx.vm.run(t);
    autonomyObserver() = nullptr;
    bool fireConsidered = false;
    for (const auto& c : seen.considered)
        fireConsidered |= c.object == fire && c.checked && !c.passed;
    check(t.result && fireConsidered && !seen.best.empty() && seen.best[0].object == fx.food &&
              fx.p().data[persondata::FindBestActionObject] == fx.food,
          "Find Best Interaction runs the check tree with parameter 0 = 1: the entry that refuses autonomy loses");
}

int main() {
    testVisitorEntries();
    testAutonomousParameter();
    testCurves();
    testMotiveChangeAndDecay();
    testMood();
    testFindBestInteraction();
    testCandidateOrder();
    testDynamicInteractions();
    testFind5Worst();
    testEnvironment();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("autonomy: all tests passed\n");
    return failures ? 1 : 0;
}
