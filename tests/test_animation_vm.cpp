// Animation primitives (Animate Sim, Assign Locomotion Animations, ...), the
// locomotion names and a synthetic route walked with SimMotion between ticks.
#include "vm/animation.hpp"
#include "vm/sim_motion.hpp"
#include "vm/vm.hpp"

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

constexpr uint32_t kGroup = 0x7F000001, kSemi = 0x7F000002;

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

static Behavior tree(std::vector<Behavior::Instruction> code) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.instructions = std::move(code);
    return b;
}

static StringTable strings(std::initializer_list<const char*> values) {
    StringTable t;
    for (const char* v : values)
        t.entries.push_back({lang::EnglishUS, v, "", 0});
    return t;
}

// The global locomotion set and animation list, as in objects.package (cut down).
static void addGlobals(CodeLibrary& code, VM& vm) {
    code.add(kGlobalGroup, animlist::Locomotion,
             strings({"turnInPlace-alt(turnInPlace),stepShort-alt(stepShort)",
                      "stepInPlace-alt(stepInPlace),walkShort-alt0-normal(walkShort),walk-alt0-normal(walk),"
                      "walk-alt1-normal,walkPose",
                      "walk-alt0-normalBlend", "turnInPlace-alt@1.5(turnInPlace@1.5)",
                      "runShort-alt0-normal(runShort),run-alt0-normal(run)", ""}));
    code.add(kGlobalGroup, animlist::Adult, strings({"a-noExpression", ".", "a-blockedPath-long"}));
    code.add(kGlobalGroup, animlist::Child, strings({"c-noExpression", ".", "c-blockedPath-long"}));
    for (const char* name : {"a-male-walk-alt0-normal", "a-female-walk-alt0-normal", "c-walk-alt0-normal",
                             "a-male-run-alt0-normal", "a-male-swimSlow", "a-swimPose"})
        vm.animations.add(name, 0.7f, 2.0f);
    vm.animations.add("a-blockedPath-long", 0.5f); // 15 ticks
    vm.animations.add("c-blockedPath-long", 0.5f);
    vm.animations.add("t-blockedPath-long", 0.4f);
}

static Object& sim(VM& vm, float x, float y) {
    Object& o = vm.createObject(kGroup);
    o.semiGroup = kSemi;
    o.person.emplace();
    o.position = {x, y, 1};
    return o;
}

static void testLocomotionNames() {
    CodeLibrary code;
    VM vm(code);
    addGlobals(code, vm);
    Object& s = sim(vm, 1.5f, 1.5f);
    check(locomotionAnimation(vm, s, "walk") == "a-male-walk-alt0-normal", "global walk, adult male");
    check(locomotionAnimation(vm, s, "run") == "a-male-run-alt0-normal", "global run");
    s.person->data[65] = 1;
    check(locomotionAnimation(vm, s, "walk") == "a-female-walk-alt0-normal", "female walk");
    s.person->data[58] = lifestage::Child;
    check(locomotionAnimation(vm, s, "walk") == "c-walk-alt0-normal", "child walk (no gender)");
    check(agePrefix(s) == "c" && genderName(s) == "female", "age and gender");
    s.person->data[58] = lifestage::Teen;
    s.person->data[65] = 0;
    check(locomotionAnimation(vm, s, "walk") == "a-male-walk-alt0-normal", "teen falls back to the adult walk");
    check(locomotionAnimation(vm, s, "walkShort").empty(), "no resource, no name");
}

// Assign Locomotion Animations: push from the private and semi-global groups,
// pop, pop all; the errors of the game.
static void testAssignLocomotion() {
    CodeLibrary code;
    VM vm(code);
    addGlobals(code, vm);
    code.add(kSemi, animlist::Locomotion, strings({"swim-TurnInPlace", "swimSlowShort,swimSlow,swimPose", ""}));
    code.add(kGroup, animlist::Locomotion, strings({"", "walk-ghost(walk)", ""}));
    vm.animations.add("a-male-walk-ghost", 1.0f);
    Object& s = sim(vm, 1.5f, 1.5f);
    auto loco = [](uint8_t mode, uint8_t source, Target t) { return instr(0x72, t, E, {mode, 0, source}); };
    code.add(kGroup, 0x1000, tree({loco(2, 0, go(1)), loco(2, 2, T)}));
    Thread t = vm.start(s.id, 0x1000);
    vm.run(t);
    check(t.status == Thread::Status::Finished && s.animation.locomotion.size() == 2, "two pushes");
    check(s.animation.locomotion.back().group == kSemi && s.animation.locomotion.front().group == kGroup,
          "private (0) and semi-global (2) sets");
    check(locomotionAnimation(vm, s, "walk") == "a-male-swimSlow", "a set without roles: short, normal, pose");
    check(locomotionAnimation(vm, s, "walkPose") == "a-swimPose", "the pose of a set without roles");
    code.add(kGroup, 0x1001, tree({loco(1, 0, T)}));
    t = vm.start(s.id, 0x1001);
    vm.run(t);
    check(locomotionAnimation(vm, s, "walk") == "a-male-walk-ghost", "pop: back to the private set");
    code.add(kGroup, 0x1002, tree({loco(0, 0, go(1)), loco(1, 0, T)}));
    t = vm.start(s.id, 0x1002);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Invalid pop: locomotion stack is empty") == 0,
          "pop all, then a pop of an empty stack is the game's error");
    check(locomotionAnimation(vm, s, "walk") == "a-male-walk-alt0-normal", "empty stack: the global set");
    code.add(kGroup, 0x1003, tree({loco(2, 0, go(0))}));
    t = vm.start(s.id, 0x1003);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("too deep") != std::string::npos &&
              s.animation.locomotion.size() == kMaxLocomotionDepth,
          "pushes past the depth fail");
}

// Animate Sim: plays for the animation's length (in VM ticks) and then
// returns true; the index can come from a parameter; teens get their version.
static void testAnimateSim() {
    CodeLibrary code;
    VM vm(code);
    addGlobals(code, vm);
    Object& s = sim(vm, 1.5f, 1.5f);
    // Animation 2 of the global list (0x80), then animation in param 0 (flag 0x04).
    code.add(kGroup, 0x1000,
             tree({instr(0x6A, go(1), E, {2, 0, 0, 0x20, 0, 0, 0x80}),
                   instr(0x6A, T, E, {0, 0, 0x04, 0x20, 0, 0, 0x80})}));
    const int16_t args[] = {2};
    Thread t = vm.start(s.id, 0x1000, args);
    vm.run(t);
    check(t.status == Thread::Status::Running && s.animation.name == "a-blockedPath-long" && s.animation.serial == 1,
          "Animate Sim starts and waits");
    int turns = 1;
    while (t.status == Thread::Status::Running && s.animation.serial == 1 && turns < 100)
        vm.run(t), ++turns;
    check(turns == 16, "0.5 s = 15 ticks after the turn it starts on");
    check(s.animation.serial == 2 && s.animation.playing(), "the second one starts on the same turn");
    while (t.status == Thread::Status::Running && turns < 100)
        vm.run(t), ++turns;
    check(t.status == Thread::Status::Finished && t.result && !s.animation.playing(), "true when it ends");

    s.person->data[58] = lifestage::Teen;
    code.add(kGroup, 0x1001, tree({instr(0x6A, T, E, {2, 0, 0, 0x20, 0, 0, 0x80})}));
    t = vm.start(s.id, 0x1001);
    vm.run(t);
    check(s.animation.name == "t-blockedPath-long", "teens play the t- version when there is one");
    s.person->data[58] = lifestage::Child;
    code.add(kGroup, 0x1004, tree({instr(0x6A, T, E, {2, 0, 0, 0x20, 0, 0, 0x80})}));
    t = vm.start(s.id, 0x1004);
    vm.run(t);
    check(s.animation.name == "c-blockedPath-long", "children use the child list");

    code.add(kGroup, 0x1002, tree({instr(0x6A, T, E, {1, 0, 0, 0x20, 0, 0, 0x80})}));
    t = vm.start(s.id, 0x1002);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Animation name is empty string.") == 0,
          "an empty entry is the game's error");

    // Overlays do not block; Animate Stop clears them.
    Object& thing = vm.createObject(kGroup);
    code.add(kGroup, animlist::Object, strings({"o-thing-open", ""}));
    code.add(kGroup, 0x1003,
             tree({instr(0x69, go(1), E, {0, 0, 0, 0x20, 0, 0, 0x0A, 0, 0}),
                   instr(0x6C, T, E, {0, 0, 0, 0x0A, 0, 0, 0x86})}));
    t = vm.start(s.id, 0x1003, {}, thing.id);
    vm.trace = [&](const Thread&, const Frame& f, const Behavior::Instruction&) {
        if (f.pc == 1)
            check(thing.animation.overlays.size() == 1 && thing.animation.overlays[0] == "o-thing-open",
                  "Animate Object adds an overlay");
    };
    vm.run(t);
    vm.trace = nullptr;
    check(t.status == Thread::Status::Finished && thing.animation.overlays.empty(), "Animate Stop clears it");
}

// A Sim walks a synthetic route (scripted Go To Relative Position, then an
// Animate Sim); SimMotion follows it between ticks.
static void testScriptedWalk() {
    CodeLibrary code;
    VM vm(code);
    addGlobals(code, vm);
    vm.world = World(12, 12);
    Object& s = sim(vm, 1.5f, 1.5f);
    Object& target = vm.createObject(kGroup);
    target.position = {8.5f, 6.5f, 1};
    target.data[objdata::Flags] = 0; // blocks its tile
    startScriptedWalk(vm, s.id, target.id, 2);
    SimMotion motion;
    motion.sample(vm, s);
    bool sawWalk = false, sawAnim = false, monotonic = true, smooth = true;
    float lastDist = std::hypot(8.5f - 1.5f, 6.5f - 1.5f);
    float heading = motion.heading();
    int ticks = 0;
    for (; ticks < 1000; ++ticks) {
        vm.tick();
        motion.sample(vm, s);
        if (motion.current().walking) {
            sawWalk = sawWalk || motion.current().walk == "a-male-walk-alt0-normal";
            const float d = std::hypot(8.5f - s.position.x, 6.5f - s.position.y);
            monotonic = monotonic && d <= lastDist + 1e-4f;
            lastDist = d;
            // Steps between ticks are the walking speed; halfway is halfway.
            const float step = std::hypot(motion.current().x - motion.previous().x,
                                          motion.current().y - motion.previous().y);
            smooth = smooth && step <= walkTilesPerTick(vm, s) + 1e-4f &&
                     std::fabs(motion.x(0.5f) - (motion.previous().x + motion.current().x) / 2) < 1e-5f;
            // Four frames per tick, turning at most 7 rad/s.
            for (int f = 0; f < 4; ++f) {
                const float h = motion.turn(1.0f / 120);
                smooth = smooth && std::fabs(std::remainder(h - heading, float(2 * M_PI))) <= 7.0f / 120 + 1e-4f;
                heading = h;
            }
        }
        if (!motion.current().animation.empty())
            sawAnim = sawAnim || motion.current().animation == "a-blockedPath-long";
        if (vm.threads.at(s.id).status != Thread::Status::Running)
            break;
    }
    const Thread& t = vm.threads.at(s.id);
    check(sawWalk && monotonic && smooth, "walks towards the target, smoothly, with the walk animation");
    check(sawAnim && t.status == Thread::Status::Finished && t.result, "then plays the animation and ends");
    const float d = std::hypot(8.5f - s.position.x, 6.5f - s.position.y);
    check(d >= 0.5f && d <= 2.6f && !s.route, "ends next to the target");
    check(s.data[objdata::Direction] == directionOf(8.5f - s.position.x, 6.5f - s.position.y), "facing it");
    // 6-7 tiles at the walk animation's 2 tiles/s is 3+ seconds, then 15 ticks of animation.
    check(std::fabs(walkTilesPerTick(vm, s) - 2.0f / 30) < 1e-6f, "the walk animation's speed");
    check(ticks > 90 && ticks < 300, "takes the time the walk takes");
    // The heading settles on the direction the Sim faces.
    for (int f = 0; f < 120; ++f)
        motion.turn(1.0f / 60);
    check(std::fabs(std::remainder(motion.heading() - headingOfDirection(s.data[objdata::Direction]),
                                   float(2 * M_PI))) < 1e-4f,
          "standing: the heading is the direction's");
    check(std::fabs(headingOfDirection(2)) < 1e-6f && std::fabs(headingOfMotion(0, 1)) < 1e-6f &&
              std::fabs(headingOfDirection(0) - headingOfMotion(-1, 0)) < 1e-5f,
          "headings: direction 2 and +Y are 0, direction 0 is -X");
}

// Get Distance To and Get Direction To, from me or from an object in a temp.
static void testMeasure() {
    CodeLibrary code;
    VM vm(code);
    Object& s = sim(vm, 1.5f, 1.5f);
    Object& far = vm.createObject(kGroup);
    far.position = {4.5f, 5.5f, 2};
    Object& east = vm.createObject(kGroup);
    east.position = {3.5f, 1.5f, 1};
    code.add(kGroup, 0x1000,
             tree({instr(0x0B, go(1), E, {0, 0, 0}),                    // temp 0 := distance me -> stack object
                   instr(0x0B, go(2), E, {1, 0, 0, 0, 0, 0, 0x02}),     // temp 1 := in hundredths
                   instr(0x0C, go(3), E, {2, 0, 0x08, 0, 0}),           // temp 2 := direction me -> stack object
                   instr(0x0C, T, E, {3, 0, 0x08, 0, 1, 0x08, 4, 0})})); // temp 3 := from the object in temp 4
    Thread t = vm.start(s.id, 0x1000, {}, far.id);
    t.temps[4] = east.id;
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == 5 + 20 && t.temps[1] == 500 + 20,
          "distance: whole tiles (or hundredths) plus 20 a level");
    check(t.temps[2] == directionOf(3, 4) && t.temps[3] == directionOf(1, 4), "direction to the stack object");
}

int main() {
    testMeasure();
    testLocomotionNames();
    testAssignLocomotion();
    testAnimateSim();
    testScriptedWalk();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("animation vm: all tests passed\n");
    return failures ? 1 : 0;
}
