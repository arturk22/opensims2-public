// Dialog (0x24), Change Light (0x6F) and Change Outfit (0x79): src/vm/dialog.*, appearance.cpp.

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

constexpr uint32_t kObjGroup = 0x7F000001;

using Target = Behavior::Target;
using Kind = Behavior::TargetKind;
static const Target T{Kind::True, 0}, F{Kind::False, 0};

static Behavior::Instruction instr(uint16_t opcode, std::initializer_list<uint8_t> operands, uint8_t nodeVersion = 1) {
    Behavior::Instruction in;
    in.opcode = opcode;
    in.onTrue = T;
    in.onFalse = F;
    in.nodeVersion = nodeVersion;
    size_t i = 0;
    for (auto b : operands)
        in.operands[i++] = b;
    return in;
}

static Behavior tree(std::vector<Behavior::Instruction> code, uint8_t locals = 0) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.localCount = locals;
    b.instructions = std::move(code);
    return b;
}

static StringTable strings(std::initializer_list<const char*> values) {
    StringTable t;
    t.format = 0xFFFD;
    for (const char* v : values)
        t.entries.push_back({lang::EnglishUS, v, "", 0});
    return t;
}

static void testYesNoCancel() {
    CodeLibrary code;
    VM vm{code};
    code.add(kObjGroup, 0x12D,
             strings({"Title", "$Me works $TimeLocal:0 to $TimeLocal:1 for $$Local:2, $Unknown:3 stays", "Take", "Again",
                      "Stop"}));
    // Local 0..2 := 9, 17, 45; then a three-choice dialog (title 1, message
    // 2, buttons 3/4/5, choice into temp 4, private strings).
    auto assign = [](uint8_t local, uint8_t value) {
        return instr(0x02, {local, 0, value, 0, 0, 5, 0x19, 0x07});
    };
    auto b = tree({assign(0, 9), assign(1, 17), assign(2, 45),
                   instr(0x24, {5, 0, 0, 3, 4, 2, 1, 0x40, 0x01, 0, 0, 0, 0, 2, 0})},
                  3);
    b.instructions[0].onTrue = {Kind::Instruction, 1};
    b.instructions[1].onTrue = {Kind::Instruction, 2};
    b.instructions[2].onTrue = {Kind::Instruction, 3};
    code.add(kObjGroup, 0x1000, std::move(b));
    Object& me = vm.createObject(kObjGroup);
    Thread t = vm.start(me.id, 0x1000);
    vm.run(t);
    check(t.status == Thread::Status::Running, "dialog: waits for an answer");
    auto it = vm.dialogs.find(me.id);
    check(it != vm.dialogs.end(), "dialog: recorded");
    if (it == vm.dialogs.end())
        return;
    const Dialog& d = it->second;
    check(d.title == "Title", "dialog: title");
    check(d.message.find("works 9:00 to 17:00 for $45, $Unknown:3 stays") != std::string::npos, "dialog: tokens");
    check(d.buttons.size() == 3 && d.buttons[0] == "Take" && d.buttons[2] == "Stop", "dialog: buttons");
    vm.run(t);
    check(t.status == Thread::Status::Running, "dialog: still waiting");
    vm.dialogs[me.id].answer = 2;
    vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result, "dialog: button 3 -> false");
    check(t.temps[4] == 2, "dialog: the choice goes to temp (byte 7 >> 4)");
    check(vm.dialogs.empty(), "dialog: closed");

    // Headless: answered at once with the first button -> true.
    vm.autoDialogAnswer = 0;
    Thread t2 = vm.start(me.id, 0x1000);
    vm.run(t2);
    vm.run(t2);
    check(t2.status == Thread::Status::Finished && t2.result, "dialog: auto answer button 1 -> true");

    // A yes/no dialog without a title string (byte 6 = 0, as the phone's
    // "is ... available to talk?"): no title, never the type's editor label.
    vm.autoDialogAnswer = -1;
    code.add(kObjGroup, 0x1001, tree({instr(0x24, {0, 0, 0, 3, 4, 1, 0, 0, 0x01, 0, 0, 0, 0, 2, 0})}));
    Thread t3 = vm.start(me.id, 0x1001);
    vm.run(t3);
    auto y = vm.dialogs.find(me.id);
    check(y != vm.dialogs.end() && y->second.title.empty() && y->second.buttons.size() == 2,
          "dialog: yes/no without a title string has none");
}

static void testNotification() {
    CodeLibrary code;
    VM vm{code};
    code.add(kObjGroup, 0x12D, strings({"Hello $Object"}));
    // Text notification: does not wait.
    code.add(kObjGroup, 0x1000, tree({instr(0x24, {0, 0, 0, 0, 0, 8, 0, 0x81, 0x01, 0, 0x14, 0, 1, 1, 0})}));
    Object& me = vm.createObject(kObjGroup);
    Thread t = vm.start(me.id, 0x1000, {}, me.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.result, "notification: returns at once");
    check(vm.notifications.size() == 1 && vm.dialogs.empty(), "notification: shown, not waiting");
    // Without a stack object (it left the lot) the tree-owning object gives
    // the text, as TryDialog does; the Main no longer dies (N002_Lot3 Circe).
    Thread u = vm.start(me.id, 0x1000, {}, 0);
    vm.run(u);
    check(u.status == Thread::Status::Finished && u.result && vm.notifications.size() == 2,
          "notification: shown without a stack object");
}

static void testLightAndOutfit() {
    CodeLibrary code;
    VM vm{code};
    // Change Light on me (owner 0x0A stack object id... use "my id": literal
    // object id via owner 7): intensity 0, duration 10, all lights.
    Object& lamp = vm.createObject(kObjGroup);
    const auto id = uint8_t(lamp.id);
    code.add(kObjGroup, 0x1000, tree({instr(0x6F, {0, 0, 0x07, id, 0, 10, 0, 0, 0xFF}, 0)}));
    check(lightLevel(vm, lamp) == -1, "light: unknown until changed");
    Thread t = vm.start(lamp.id, 0x1000);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.result, "light: true");
    check(lightLevel(vm, lamp) == 100, "light: fades from full when unknown");
    vm.ticks += 5;
    check(lightLevel(vm, lamp) == 50, "light: half way");
    vm.ticks += 5;
    check(lightLevel(vm, lamp) == 0, "light: off");
    // Over 100 becomes 50.
    code.add(kObjGroup, 0x1001, tree({instr(0x6F, {0, 0, 0x07, id, 0, 0, 0, 200, 0xFF}, 0)}));
    Thread t2 = vm.start(lamp.id, 0x1001);
    vm.run(t2);
    check(lightLevel(vm, lamp) == 50, "light: > 100 -> 50");

    // Change Outfit: Sim = literal id (owner byte 9, data u16 10), category byte 8.
    Object& sim = vm.createObject(kObjGroup);
    sim.person.emplace();
    const auto sid = uint8_t(sim.id);
    code.add(kObjGroup, 0x1002, tree({instr(0x79, {0x02, 0, 0, 0, 0x78, 0x56, 0x34, 0x12, 5, 0x07, sid, 0}, 0)}));
    Thread t3 = vm.start(sim.id, 0x1002);
    vm.run(t3);
    check(t3.status == Thread::Status::Finished && t3.result, "outfit: true");
    check(sim.person->data[8] == 5, "outfit: person data 8 = category");
    check(uint16_t(sim.person->data[119]) == 0x5678 && uint16_t(sim.person->data[120]) == 0x1234,
          "outfit: source GUID in person data 119/120");
}

int main() {
    lang::setPreferred(lang::EnglishUS);
    testYesNoCancel();
    testNotification();
    testLightAndOutfit();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("dialog tests passed\n");
    return failures ? 1 : 0;
}
