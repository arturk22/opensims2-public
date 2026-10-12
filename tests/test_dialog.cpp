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
    check(t.temps[4] == 1, "dialog: cancel writes 1 to the temp (byte 7 >> 4)");
    check(vm.dialogs.empty(), "dialog: closed");

    // Headless: answered at once with the first button -> true.
    vm.autoDialogAnswer = 0;
    Thread t2 = vm.start(me.id, 0x1000);
    vm.run(t2);
    vm.run(t2);
    check(t2.status == Thread::Status::Finished && t2.result, "dialog: auto answer button 1 -> true");
    check(t2.temps[4] == 0, "dialog: yes leaves the temp");
    vm.autoDialogAnswer = 1;
    Thread t4 = vm.start(me.id, 0x1000);
    t4.temps[4] = 7;
    vm.run(t4);
    vm.run(t4);
    check(t4.status == Thread::Status::Finished && !t4.result && t4.temps[4] == 0, "dialog: no writes 0 to the temp");

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

// The classes of the special dialogs and what their OK hands back.
static void testSpecialDialogs() {
    check(dialogClass(0).cls == DialogClass::Object && dialogClass(3).cls == DialogClass::Object,
          "class: plain types are cTSObjectDialog");
    check(dialogClass(5).script == 0x8C159243 && dialogClass(15).cls == DialogClass::PhoneServices,
          "class: services phone book");
    check(dialogClass(6).script == 0x8C159244 && dialogClass(7).script == 0x8C159242, "class: invite and party");
    check(dialogClass(17).cls == DialogClass::Clothing && dialogClass(23).script == 0x2D91050A,
          "class: wardrobe and names");
    CodeLibrary code;
    VM vm{code};
    code.add(kObjGroup, 0x12D, strings({"Call", "Who?", "OK", "Cancel"}));
    auto dlg = [&](uint16_t id, uint8_t type, uint8_t b7 = 0) {
        code.add(kObjGroup, id, tree({instr(0x24, {0, 0, 0, 3, 4, type, 1, b7, 0x01, 0, 0, 0, 0, 2, 0})}));
    };
    Object& me = vm.createObject(kObjGroup);
    Object& phone = vm.createObject(kObjGroup);
    Object& service = vm.createObject(kObjGroup);
    for (int16_t n : {11, 12, 13}) {
        vm.neighbors[n].guid = 0x1000u + uint32_t(n);
        vm.neighbors[n].firstName = "N" + std::to_string(n);
        vm.neighbors[n].personData[persondata::NeighborId] = n;
    }
    // Services (5): the objects of the stack object's array 0; OK -> [object].
    phone.array(0).values = {service.id};
    dlg(0x1100, 5);
    Thread s = vm.start(me.id, 0x1100, {}, phone.id);
    vm.run(s);
    check(vm.dialogs.count(me.id) && vm.dialogs[me.id].entries.size() == 1 && vm.dialogs[me.id].selection.size() == 1,
          "services: one row, chosen");
    vm.dialogs[me.id].answer = 0;
    vm.run(s);
    check(s.result && phone.array(0).values == std::vector<int16_t>{service.id}, "services: OK -> array 0 = object");
    // Invite (6): neighbors; the second one chosen -> [12].
    phone.array(0).values = {11, 12};
    dlg(0x1101, 6);
    Thread i = vm.start(me.id, 0x1101, {}, phone.id);
    vm.run(i);
    check(vm.dialogs[me.id].entries.size() == 2 && vm.dialogs[me.id].entries[1].name == "N12", "invite: rows by name");
    vm.dialogs[me.id].selection = {12};
    vm.dialogs[me.id].answer = 0;
    vm.run(i);
    check(i.result && phone.array(0).values == std::vector<int16_t>{12}, "invite: OK -> array 0 = neighbor");
    // Party (7): at most temp 5 guests; OK -> the guests.
    phone.array(0).values = {11, 12, 13};
    dlg(0x1102, 7);
    Thread p = vm.start(me.id, 0x1102, {}, phone.id);
    p.temps[5] = 2;
    vm.run(p);
    check(vm.dialogs[me.id].maxSelection == 2 && vm.dialogs[me.id].selection.empty(), "party: no guests at first");
    vm.dialogs[me.id].selection = {13, 11};
    vm.dialogs[me.id].answer = 0;
    vm.run(p);
    check(p.result && phone.array(0).values == (std::vector<int16_t>{13, 11}), "party: OK -> the guests");
    // Cancel leaves the array.
    Thread p2 = vm.start(me.id, 0x1102, {}, phone.id);
    vm.run(p2);
    vm.dialogs[me.id].answer = 1;
    vm.run(p2);
    check(!p2.result && phone.array(0).values.size() == 2, "party: cancel -> false, array kept");
    // A phone book on a stack object without arrays: error 0x6C.
    Object& bare = vm.createObject(kObjGroup);
    Thread e = vm.start(me.id, 0x1100, {}, bare.id);
    vm.run(e);
    check(e.status != Thread::Status::Running && !vm.dialogs.count(me.id), "services: no object array -> error");

    // Names (23): the neighbors in temps 0/1 (twins), each named by its box.
    vm.neighbors[12].personData[65] = 1; // a girl
    dlg(0x1103, 23);
    Thread n = vm.start(me.id, 0x1103);
    n.temps[0] = 11, n.temps[1] = 12;
    vm.run(n);
    Dialog& nd = vm.dialogs[me.id];
    check(nd.namePanel == 0xCDC2CAA6 && nd.names.size() == 2 && nd.names[0].text == "N11" && nd.names[1].female,
          "names: twins panel, default names, girl icon");
    nd.names[0].text = "  Ana ";
    nd.names[1].text = "Bia";
    nd.answer = 0;
    vm.run(n);
    check(n.result && vm.neighbors[11].firstName == "Ana" && vm.neighbors[12].firstName == "Bia",
          "names: OK renames each newborn (trimmed)");
    // One Sim older than a baby: the rename panel.
    vm.neighbors[13].personData[persondata::PersonAge] = 3;
    Thread r = vm.start(me.id, 0x1103);
    r.temps[0] = 13;
    vm.run(r);
    check(vm.dialogs[me.id].namePanel == 0xCDC2CAA4, "names: rename panel");
    vm.dialogs[me.id].answer = 1;
    vm.run(r);
    check(!r.result && vm.neighbors[13].firstName == "N13", "names: cancel keeps the name");

    // Text entry (3): OK hands the text to the simulator.
    dlg(0x1104, 3);
    Thread te = vm.start(me.id, 0x1104);
    vm.run(te);
    vm.dialogs[me.id].text = "Rex";
    vm.dialogs[me.id].answer = 0;
    vm.run(te);
    check(te.result && vm.enteredText == "Rex", "text entry: OK keeps the typed text");

    // Aspiration (24): the neighbor in the stack object; OK sets person data 46.
    dlg(0x1108, 24);
    Thread as = vm.start(me.id, 0x1108, {}, 12);
    vm.run(as);
    check(vm.dialogs[me.id].aspirationNeighbor == 12 && vm.dialogs[me.id].text == "Bia" && vm.dialogs[me.id].female,
          "aspiration: the neighbor, its name and gender");
    vm.dialogs[me.id].aspiration = 0x10;
    vm.dialogs[me.id].answer = 0;
    vm.run(as);
    check(as.result && vm.neighbors[12].personData[46] == 0x10, "aspiration: OK sets person data 46");
    // Wardrobe (17): temp 0 = 4 (formal) -> outfit category 0x020, the Sim running it.
    dlg(0x1109, 17);
    Thread wd = vm.start(me.id, 0x1109);
    wd.temps[0] = 4;
    vm.run(wd);
    check(vm.dialogs[me.id].outfitCategory == 0x020 && vm.dialogs[me.id].outfitSim == me.id, "wardrobe: formal wear");
    vm.dialogs.erase(me.id);

    // Racks (11): array 0 objects; OK writes -1 over those not picked.
    {
        Object& mag1 = vm.createObject(kObjGroup);
        Object& mag2 = vm.createObject(kObjGroup);
        phone.array(0).values = {mag1.id, mag2.id};
        dlg(0x110B, 11);
        Thread rk = vm.start(me.id, 0x110B, {}, phone.id);
        rk.temps[0] = 1;
        vm.run(rk);
        Dialog& r = vm.dialogs[me.id];
        check(r.cls.cls == DialogClass::SelectItems && r.entries.size() == 2 && r.multiSelect, "rack: items, several picks");
        r.selection = {mag2.id};
        r.answer = 0;
        vm.run(rk);
        check(rk.result && phone.array(0).values == (std::vector<int16_t>{-1, mag2.id}), "rack: OK -> -1 for the others");
    }
    // Food (14): locals 0-3 = units, cost per 50, delivery, most; OK -> temp 0 = units bought.
    {
        auto assign = [](uint8_t local, uint8_t value) { return instr(0x02, {local, 0, value, 0, 0, 5, 0x19, 0x07}); };
        auto b = tree({assign(0, 40), assign(1, 20), assign(2, 50), assign(3, 200),
                       instr(0x24, {0, 0, 0, 0, 0, 14, 0, 0, 0x01, 0, 0x14, 0, 0, 0, 0})},
                      4);
        for (int k = 0; k < 4; ++k)
            b.instructions[size_t(k)].onTrue = {Kind::Instruction, uint16_t(k + 1)};
        code.add(kObjGroup, 0x110A, std::move(b));
        Thread fd = vm.start(me.id, 0x110A);
        vm.run(fd);
        Dialog& f = vm.dialogs[me.id];
        check(f.cls.cls == DialogClass::Fill && f.fillUnits == 40 && f.fillCost == 20 && f.fillDelivery == 50 &&
                  f.fillMax == 200 && f.fillValue == 40,
              "food: the locals");
        f.fillValue = 90;
        f.answer = 0;
        vm.run(fd);
        check(fd.result && fd.temps[0] == 50, "food: OK -> temp 0 = units bought");
    }

    // "Return" (byte 7 bit 0): queued, the tree goes on at once.
    dlg(0x1105, 0, 0x01);
    Thread ret = vm.start(me.id, 0x1105);
    vm.run(ret);
    check(ret.status == Thread::Status::Finished && ret.result && vm.dialogs.size() == 1 &&
              vm.dialogs.begin()->first < 0 && vm.dialogs.begin()->second.detached,
          "return: true at once, the window stays");
    vm.dialogs.begin()->second.answer = 0;
    autoTimeOutDialogs(vm, 0);
    check(vm.dialogs.empty(), "return: gone once answered");
    // Icons: automatic = the stack object; neighbor = the ID in the stack object.
    dlg(0x1106, 0, 0x00);
    Thread ic = vm.start(me.id, 0x1106, {}, phone.id);
    vm.run(ic);
    check(vm.dialogs[me.id].iconObject == phone.id, "icon: automatic -> the stack object");
    vm.dialogs.erase(me.id);
    dlg(0x1107, 0, 0x04);
    Thread in = vm.start(me.id, 0x1107, {}, 12);
    vm.run(in);
    check(vm.dialogs[me.id].iconNeighbor == 12 && !vm.dialogs[me.id].iconObject, "icon: neighbor -> its head");
    vm.dialogs.erase(me.id);
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
    testSpecialDialogs();
    testLightAndOutfit();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("dialog tests passed\n");
    return failures ? 1 : 0;
}
