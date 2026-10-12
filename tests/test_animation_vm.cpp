// Animation primitives (Animate Sim, Assign Locomotion Animations, ...), the
// locomotion names and a synthetic route walked with SimMotion between ticks.
#include "vm/animation.hpp"
#include "vm/locomotion.hpp"
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
    check(t.status == Thread::Status::Finished && t.result,
          "a \".\" entry is a name with no animation: true at once");
    code.add(kGlobalGroup, animlist::Adult, strings({"a-noExpression", "", "a-blockedPath-long"}));
    code.add(kGroup, 0x100F, tree({instr(0x6A, T, E, {1, 0, 0, 0x20, 0, 0, 0x80})}));
    s.person->data[58] = 0;
    t = vm.start(s.id, 0x100F);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Animation name is empty string.") == 0,
          "an empty entry is the game's error 0x43");
    code.add(kGlobalGroup, animlist::Adult, strings({"a-noExpression", ".", "a-blockedPath-long"}));

    // Tree events: the event tree gets each "event=N" when the animation
    // reaches it, with N in parameter 0 (here: attribute 0 += parameter 0,
    // as Expression 0x02 with owners attribute 0x00 / parameter 0x09).
    s.person->data[58] = 0;
    s.attributes.assign(1, 0);
    vm.animations.addTreeEvent("a-blockedPath-long", 250, 7);
    vm.animations.addTreeEvent("a-blockedPath-long", 499, 5);
    Behavior ev = tree({instr(0x02, T, E, {0, 0, 0, 0, 0, 3, 0x00, 0x09})});
    ev.argCount = 5;
    code.add(kGroup, 0x1005, ev);
    code.add(kGroup, 0x1006, tree({instr(0x6A, T, E, {2, 0, 0, 0x20, 0x05, 0x10, 0x80})}));
    t = vm.start(s.id, 0x1006);
    vm.run(t);
    check(s.attributes[0] == 0, "no event before its time");
    for (int i = 0; i < 9; ++i)
        vm.run(t);
    check(s.attributes[0] == 7, "the first event at 250 ms");
    while (t.status == Thread::Status::Running && turns < 200)
        vm.run(t), ++turns;
    check(t.status == Thread::Status::Finished && s.attributes[0] == 12, "every event has run when it ends");

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

    // An object's list (0x81) is the running tree's object's, then its
    // semi-global's where the entry is empty; not the stack object's
    // (try_animate_sim: the selector of the tree's behavior). A "~" label
    // row is indexed like any other.
    constexpr uint32_t kDrink = 0x7F000010, kStool = 0x7F000011;
    Object& drink = vm.createObject(kDrink);
    drink.semiGroup = kSemi;
    Object& stool = vm.createObject(kStool);
    s.person->data[58] = 0;
    code.add(kDrink, animlist::Adult, strings({"a2o-drink-sip", ""}));
    code.add(kSemi, animlist::Adult, strings({"a2o-semi-zero", "a2o-semi-one"}));
    code.add(kStool, animlist::Adult, strings({"~Get Out", "a2o-stool-one"}));
    vm.animations.add("a2o-drink-sip", 1.0f);
    vm.animations.add("a2o-semi-one", 1.0f);
    code.add(kDrink, 0x1010, tree({instr(0x6A, T, E, {0, 0, 0, 0x20, 0, 0, 0x81})}));
    code.add(kDrink, 0x1011, tree({instr(0x6A, T, E, {1, 0, 0, 0x20, 0, 0, 0x81})}));
    t = vm.start(s.id, 0x1010, {}, stool.id, drink.id);
    vm.run(t);
    check(s.animation.name == "a2o-drink-sip", "the running tree's object's list, not the stack object's");
    t = vm.start(s.id, 0x1011, {}, stool.id, drink.id);
    vm.run(t);
    check(s.animation.name == "a2o-semi-one", "an empty entry falls back to its semi-global's");
    code.add(kStool, 0x1012, tree({instr(0x6A, T, E, {0, 0, 0, 0x20, 0, 0, 0x81})}));
    t = vm.start(s.id, 0x1012, {}, stool.id, stool.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && vm.animations.missing.count("~Get Out") == 1,
          "a label row is an animation that does not exist: true at once");
}

// Animate Overlay: an overlay plays once (its kind from the entry's role),
// its event tree gets 9997 as it starts blending out and 9999 at its end,
// two of a name at most; Animate Stop mode 5 stops the idles only, and
// their event trees get -1.
static void testOverlayLifetime() {
    CodeLibrary code;
    VM vm(code);
    addGlobals(code, vm);
    code.add(kGlobalGroup, animlist::Adult,
             strings({"a-noExpression", ".", "a-blockedPath-long", "a-idle-test(idle)", "a-gesture-test(gesture)"}));
    vm.animations.add("a-idle-test", 1.0f);
    vm.animations.add("a-gesture-test", 1.0f);
    Object& s = sim(vm, 1.5f, 1.5f);
    s.attributes.assign(2, 0);
    // The event tree: attribute 0 := parameter 0, attribute 1 += parameter 0.
    Behavior ev = tree({instr(0x02, Target{Kind::Instruction, 1}, E, {0, 0, 0, 0, 0, 5, 0x00, 0x09}),
                        instr(0x02, T, E, {1, 0, 0, 0, 0, 3, 0x00, 0x09})});
    ev.argCount = 5;
    code.add(kGroup, 0x1005, ev);
    // Animate Overlay entry 3 (or 4) of the global list on the stack object, event tree 0x1005.
    code.add(kGroup, 0x1007, tree({instr(0x6B, T, E, {3, 0, 0, 0x20, 0x05, 0x10, 0x0A, 0, 0, 0x80})}));
    code.add(kGroup, 0x1008, tree({instr(0x6B, T, E, {4, 0, 0, 0x20, 0x05, 0x10, 0x0A, 0, 0, 0x80})}));
    Thread t = vm.start(s.id, 0x1007, {}, s.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && s.animation.overlays.size() == 1, "Animate Overlay adds an overlay");
    check(s.animation.overlays[0].oeType == oetype::Idle && s.animation.overlays[0].loops == 1,
          "an idle, played once");
    const int ticks = int(std::ceil(vm.clock.ticksPerSecond)); // the animation's length
    bool blendOut = false;
    for (int i = 0; i < ticks - 1; ++i) {
        tickOverlayEvents(vm);
        blendOut = blendOut || s.attributes[0] == animevent::BlendOut;
    }
    check(blendOut && s.animation.overlays.size() == 1, "9997 before its end");
    for (int i = 0; i < 3; ++i)
        tickOverlayEvents(vm);
    check(s.animation.overlays.empty() && s.attributes[0] == animevent::End, "it ends after its length, with 9999");
    check(s.attributes[1] == int16_t(animevent::BlendOut + animevent::ActionEnd + animevent::End),
          "9997, then 9998 and 9999 at its end (cCustomEventHandler2: key types 7, 6, 3)");
    for (int i = 0; i < 3; ++i) {
        t = vm.start(s.id, 0x1007, {}, s.id);
        vm.run(t);
    }
    check(s.animation.overlays.size() == 2, "two of a name at most");
    t = vm.start(s.id, 0x1008, {}, s.id);
    vm.run(t);
    code.add(kGroup, 0x1009, tree({instr(0x6C, T, E, {0, 0, 0, 0x0A, 0, 0, 0, 5})}));
    t = vm.start(s.id, 0x1009, {}, s.id);
    vm.run(t);
    check(s.animation.overlays.size() == 1 && s.animation.overlays[0].oeType == oetype::Gesture,
          "Animate Stop mode 5 stops the idles");
    check(s.attributes[0] == animevent::Stopped, "their event trees get -1");

    // An overlay from the Sim-to-Sim list takes the ages' prefix, as Animate
    // Sim's ("soc-playRedHands-playee-loopOw" is "a2a-soc-..." in the data).
    code.add(kSemi, animlist::Social, strings({"soc-test-loopOw"})); // the person semi-global's, as 0x7F7BAB85's
    vm.animations.add("a2a-soc-test-loopOw", 1.0f);
    code.add(kGroup, 0x100A, tree({instr(0x6B, T, E, {0, 0, 0, 0x20, 0, 0, 0x0A, 0, 0, 0x83})}));
    t = vm.start(s.id, 0x100A, {}, s.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !s.animation.overlays.empty() &&
              s.animation.overlays.back().name == "a2a-soc-test-loopOw",
          "Animate Overlay from the social list plays a2a-soc-test-loopOw");
}

// The channel levels the binary gives (animlevel): Animate Overlay's
// priority byte 11 -> 2p + 1 (< 0: 1), Animate Sim's byte 12 -> 2p; Animate
// Stop's priority byte 8 stops levels 2p - 2 and 2p - 1 only, and its blend
// flags set how the overlays go (0x02: the blend time, 0x20: 0.3 of it,
// none: a cut).
static void testOverlayLevels() {
    CodeLibrary code;
    VM vm(code);
    addGlobals(code, vm);
    code.add(kGlobalGroup, animlist::Adult, strings({"a-noExpression", ".", "a-blockedPath-long", "a-idle-test(idle)"}));
    vm.animations.add("a-idle-test", 1.0f);
    vm.animations.add("a-blockedPath-long", 0.5f);
    Object& s = sim(vm, 1.5f, 1.5f);
    // Animate Overlay entry 3 as the base game writes it (node version 0:
    // the priority in the u16 at 12, UpgradeVersion moves it to byte 11):
    // priority 2 (level 5), then -1 (level 1).
    code.add(kGroup, 0x1007, tree({instr(0x6B, T, E, {3, 0, 0, 0x20, 0, 0, 0x0A, 0, 0, 0x80, 0xFF, 0xFF, 2, 0})}));
    code.add(kGroup, 0x1008, tree({instr(0x6B, T, E, {3, 0, 0, 0x20, 0, 0, 0x0A, 0, 0, 0x80, 0xFF, 2, 0xFF, 0xFF})}));
    for (uint16_t t : {0x1007, 0x1008}) {
        Thread th = vm.start(s.id, t, {}, s.id);
        vm.run(th);
    }
    check(s.animation.overlays.size() == 2 && s.animation.overlays[0].level == 5 && s.animation.overlays[1].level == 1,
          "Animate Overlay's level: 2 x priority + 1, -1 -> 1 (version 0: the priority at 12)");
    check(animlevel::ofOverlay(4) == 0 && animlevel::ofSim(1) == 2 && animlevel::ofSim(7) == 6 && animlevel::ofSim(-1) == 0,
          "levels of the other priorities");
    // Animate Stop mode 1 (the overlays), priority 3 (levels 4-5), blended (0x02).
    code.add(kGroup, 0x1009, tree({instr(0x6C, T, E, {0, 0, 0x02, 0x0A, 0, 0, 0, 1, 3})}));
    Thread th = vm.start(s.id, 0x1009, {}, s.id);
    vm.run(th);
    check(s.animation.overlays.size() == 1 && s.animation.overlays[0].level == 1, "a priority stops its levels only");
    check(!s.animation.stoppedFades.empty() && std::fabs(s.animation.stoppedFades.back().second - kBlendTicks / 1000) < 1e-4f &&
              std::fabs(kBlendTicks - 233.333f) < 1e-2f,
          "blended Out: the engine's blend time, 233.3 ms");
    // Short blended (0x20), any priority.
    code.add(kGroup, 0x100A, tree({instr(0x6C, T, E, {0, 0, 0x20, 0x0A, 0, 0, 0, 1, 0})}));
    th = vm.start(s.id, 0x100A, {}, s.id);
    vm.run(th);
    check(s.animation.overlays.empty() && std::fabs(s.animation.stoppedFades.back().second - 0.07f) < 1e-4f,
          "short blended Out: 0.3 of it");
    // Animate Sim: the level and the request weight (byte 13 / 100; -1 -> 1).
    code.add(kGroup, 0x100B, tree({instr(0x6A, T, E, {2, 0, 0, 0x20, 0, 0, 0x80, 0, 0, 0x0A, 0, 0, 3, 50})}));
    th = vm.start(s.id, 0x100B, {}, s.id);
    vm.run(th);
    check(s.animation.playing() && s.animation.level == 6 && std::fabs(s.animation.weight - 0.5f) < 1e-4f,
          "Animate Sim's level 2 x priority and its request weight");
    // The IK object and the request heading (try_animate_overlay/_sim): the
    // overlay's byte 15 flag 0x08 takes temp 6 as its IK object, 0x02 temp 4
    // as -45 degree steps; Animate Sim's byte 8 flag 0x04 temp 4, its IK
    // object the owner byte 9 / u16 10 (temp 6).
    Object& s2 = sim(vm, 3.5f, 1.5f);
    code.add(kGroup, 0x100C, tree({instr(0x6B, T, E, {3, 0, 0, 0x20, 0, 0, 0x0A, 0, 0, 0x80, 0xFF, 0xFF, 2, 0, 0, 0x0A})}));
    code.add(kGroup, 0x100D, tree({instr(0x6A, T, E, {2, 0, 0, 0x20, 0, 0, 0x80, 0, 0x04, 0x08, 6, 0, 1, 0})}));
    for (uint16_t t : {0x100C, 0x100D}) {
        th = vm.start(s2.id, t, {}, s2.id);
        th.temps[4] = 2;
        th.temps[6] = s.id;
        vm.run(th);
    }
    check(s2.animation.overlays.size() == 1 && s2.animation.overlays[0].ikObject == s.id &&
              std::fabs(s2.animation.overlays[0].headingOffset + 1.5707963f) < 1e-4f,
          "Animate Overlay's IK object in temp 6 and heading from temp 4");
    check(s2.animation.eventObject == s.id && std::fabs(s2.animation.headingOffset + 1.5707963f) < 1e-4f,
          "Animate Sim's IK object and heading from temp 4");
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
    if (std::getenv("DEBUG_WALK"))
        std::fprintf(stderr, "scripted walk: saw %d monotonic %d smooth %d\n", sawWalk, monotonic, smooth);
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

// A vehicle's drive loop (Animate Object with an event tree that starts the
// loop again on 9999): the same animation replaces itself each time (no
// "two of a name" limit), and each loop's end moves the object forward
// (its facing) by the animation's root travel.
static void testVehicleDrive() {
    CodeLibrary code;
    VM vm(code);
    addGlobals(code, vm);
    Object& car = vm.createObject(kGroup);
    car.position = {5.5f, 5.5f, 1};
    car.data[objdata::Direction] = 4; // +X
    car.attributes.assign(1, 0);
    code.add(kGroup, animlist::Object, strings({"o-car-drive"}));
    vm.animations.add("o-car-drive", 0.3f);
    vm.animations.addRootTravel("o-car-drive", 2.0f);
    // 0x1001: Animate Object entry 0 on me, event tree 0x1002; 0x1002: on
    // 9999 attribute 0 += 1 and play the loop again.
    code.add(kGroup, 0x1001, tree({instr(0x69, T, E, {0, 0, 0, 0x20, 0x02, 0x10, 0x0A, 0, 0})}));
    Behavior::Instruction again = instr(0x1001, T, E);
    again.operands.fill(0xFF);
    Behavior ev = tree({instr(0x02, go(1), E, {0, 0, 1, 0, 0, 3, 0x00, 0x07}), again});
    ev.argCount = 5;
    code.add(kGroup, 0x1002, ev);
    Thread t = vm.start(car.id, 0x1001, {}, car.id);
    vm.run(t);
    for (int i = 0; i < 60; ++i)
        tickOverlayEvents(vm);
    check(car.attributes[0] >= 4, "the drive loops again and again");
    check(car.position.x > 5.5f + 2.0f * 3 && std::fabs(car.position.y - 5.5f) < 1e-4f,
          "each loop moves the car forward by the root travel");
}

// The gait (BeginFollow): person data 112/113 weigh walk-alt0/alt1, 114/115
// another cycle times 1 - 117/100, 116 adds 117/100 to another; the speed
// is the weighed cycles' speeds; object data 17 = 1 runs. And the start,
// stop and turn clips by role and 45-degree steps, mirrored the other way.
static void testGait() {
    CodeLibrary code;
    VM vm(code);
    code.add(kGlobalGroup, animlist::Locomotion,
             strings({"turnInPlace-alt(turnInPlace),turnAndGo-alt(turnAndGo),turnAndStopR-alt(turnAndStopR),"
                      "turnAndStop-alt(turnAndStop),step-alt(step)",
                      "stepInPlace-alt(stepInPlace),walkShort-alt0-normal(walkShort),walk-alt0-normal(walk),"
                      "walkShort-alt1-normal,walk-alt1-normal,walkPose,walkShort-alt8-sneak,walk-alt8-sneak",
                      "walk-alt0-normalBlend", "turnAndGo-alt@2.75(turnAndGo@2.75)",
                      "runShort-alt0-normal(runShort),run-alt0-normal(run)", ""}));
    vm.animations.add("a-male-walk-alt0-normal", 0.69f, 2.0f);
    vm.animations.add("a-male-walk-alt1-normal", 0.82f, 2.2f);
    vm.animations.add("a-male-walk-alt8-sneak", 1.0f, 1.0f);
    vm.animations.add("a-male-walkShort-alt0-normal", 0.69f, 1.0f);
    vm.animations.add("a-male-run-alt0-normal", 0.5f, 8.0f);
    for (const char* n : {"a-turnAndGo-alt-90", "a-turnAndGo-alt-0", "a-turnInPlace-45", "a-step-135", "a-turnAndStop-alt-45"})
        vm.animations.add(n, 1.0f);
    Object& s = sim(vm, 1.5f, 1.5f);
    Gait g = gaitOf(vm, s);
    check(g.animation == "a-male-walk-alt0-normal" && std::fabs(g.speed - 2.0f) < 1e-4f && !g.run,
          "no weights: the alt0 walk at its speed");
    s.person->data[112] = 25, s.person->data[113] = 75;
    g = gaitOf(vm, s);
    check(g.animation == "a-male-walk-alt1-normal" && std::fabs(g.speed - 2.15f) < 1e-4f,
          "LocoWeight 0/1: the heavier cycle plays, the speeds weighed");
    s.person->data[116] = 8, s.person->data[117] = 80; // "Sneak Here"
    g = gaitOf(vm, s);
    check(g.animation == "a-male-walk-alt8-sneak" && std::fabs(g.speed - (2.0f * 0.05f + 2.2f * 0.15f + 0.8f)) < 1e-4f,
          "LocoMoodIndex 8 with weight 80: the sneak, the others times 0.2");
    s.person->data[114] = 1, s.person->data[115] = 100;
    check(std::fabs(gaitOf(vm, s).speed - g.speed) < 1e-5f, "LocoPersonalityIndex below 2 does nothing");
    s.data[17] = 1;
    g = gaitOf(vm, s);
    check(g.run && g.animation == "a-male-run-alt0-normal" && std::fabs(g.speed - 8.0f) < 1e-4f,
          "walk style 1: the run");
    check(std::fabs(walkTilesPerTick(vm, s) - 8.0f / 30) < 1e-5f, "walkTilesPerTick follows the gait");
    auto pick = locoClip(vm, s, "turnAndGo", 2);
    check(pick && pick->animation == "a-turnAndGo-alt-90" && pick->mirror && std::fabs(pick->speed - 2.75f) < 1e-4f,
          "running: the run set's start, 2.75 times as fast, mirrored counter-clockwise");
    s.data[17] = 0;
    pick = locoClip(vm, s, "turnAndGo", -2);
    check(pick && pick->animation == "a-turnAndGo-alt-90" && !pick->mirror && pick->speed == 1.0f,
          "a clockwise start plays the clip as made");
    pick = locoClip(vm, s, "turnInPlace", 1);
    check(pick && pick->animation == "a-turnInPlace-45" && pick->mirror, "turn in place: \"turnInPlace-45\" without -alt");
    pick = locoClip(vm, s, "step", -3);
    check(pick && pick->animation == "a-step-135" && !pick->mirror, "step: \"step-135\"");
    pick = locoClip(vm, s, "turnAndStopR", -1);
    check(pick && pick->animation == "a-turnAndStop-alt-45" && !pick->mirror,
          "the other foot's stop without a clip at that angle: the plain one (the set takes either foot)");
    pick = locoClip(vm, s, "turnAndGoR", 0);
    check(pick && pick->animation == "a-turnAndGo-alt-0", "a start of the other foot falls back to the plain one");
}

int main() {
    testVehicleDrive();
    testMeasure();
    testLocomotionNames();
    testAssignLocomotion();
    testAnimateSim();
    testOverlayLifetime();
    testOverlayLevels();
    testScriptedWalk();
    testGait();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("animation vm: all tests passed\n");
    return failures ? 1 : 0;
}
