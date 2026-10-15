// Dialog (0x24), Change Light (0x6F) and Change Outfit (0x79): src/vm/dialog.*, appearance.cpp.

#include "vm/vm.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/hood_save.hpp"
#include "formats/character_write.hpp"
#include "formats/dbpf.hpp"
#include "formats/neighborhood.hpp"

#include <filesystem>
#include <unistd.h>

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
    phone.arrays.resize(1); // an OBJD with one object array
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
    n.temps[0] = 11, n.temps[1] = 12, n.temps[2] = 12; // the birth tree leaves the second baby in temp 2 too
    vm.run(n);
    Dialog& nd = vm.dialogs[me.id];
    check(nd.namePanel == 0xCDC2CAA6 && nd.names.size() == 2 && nd.names[0].text == "N11" && nd.names[1].female,
          "names: twins panel, default names, girl icon, one box per baby");
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

    // Clothing rack (16): the Sim in the stack object, temp 0 = everyday
    // (separates), temp 1 = price per item; OK puts the cart in the
    // family's bin (once) and takes the total off its funds.
    {
        Object& sim = vm.createObject(kObjGroup);
        sim.person.emplace();
        sim.person->data[persondata::FamilyNumber] = 5;
        vm.families[5].funds = 1000;
        const Tgi owned{0xEBCF3E27, 0x2C17B74A, 0x111, 0}, other{0xEBCF3E27, 0x2C17B74A, 0x222, 0};
        vm.families[5].wardrobe = {owned};
        dlg(0x110C, 16);
        Thread cp = vm.start(me.id, 0x110C, {}, sim.id);
        cp.temps[0] = 0;
        cp.temps[1] = 150;
        vm.run(cp);
        Dialog& d = vm.dialogs[me.id];
        check(d.cls.cls == DialogClass::ClothingPurchase && d.outfitSim == sim.id && d.outfitCategory == 0x001 &&
                  d.outfitSeparates && d.pricePerItem == 150 && d.fillFunds == 1000,
              "clothing rack: the Sim, everyday separates, price, funds");
        d.cart = {owned, other};
        d.answer = 0;
        vm.run(cp);
        check(cp.result && vm.families[5].funds == 700 && vm.families[5].wardrobe.size() == 2,
              "clothing rack: OK pays 2 x 150, the new item in the bin once");
        // Formal (4): whole outfits only; cancel pays nothing.
        Thread c2 = vm.start(me.id, 0x110C, {}, sim.id);
        c2.temps[0] = 4;
        vm.run(c2);
        check(vm.dialogs[me.id].outfitCategory == 0x020 && !vm.dialogs[me.id].outfitSeparates, "clothing rack: formal wear");
        vm.dialogs[me.id].cart = {other};
        vm.dialogs[me.id].answer = 1;
        vm.run(c2);
        check(!c2.result && vm.families[5].funds == 700, "clothing rack: cancel pays nothing");
        // Try-on (20): 6 is everyday without separates; vanity (21): the Sim.
        dlg(0x110D, 20);
        Thread to = vm.start(me.id, 0x110D, {}, sim.id);
        to.temps[0] = 6;
        vm.run(to);
        check(vm.dialogs[me.id].cls.cls == DialogClass::ClothingTryOn && vm.dialogs[me.id].outfitCategory == 0x001 &&
                  !vm.dialogs[me.id].outfitSeparates,
              "try-on: 6 -> everyday, whole outfits");
        vm.dialogs.erase(me.id);
        dlg(0x110E, 21);
        Thread va = vm.start(me.id, 0x110E, {}, sim.id);
        vm.run(va);
        check(vm.dialogs[me.id].cls.cls == DialogClass::Vanity && vm.dialogs[me.id].outfitSim == sim.id &&
                  !vm.dialogs[me.id].vanitySurgery,
              "vanity: the Sim in the stack object");
        vm.dialogs.erase(me.id);
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

// The family clothing bin: read from the neighborhood's BINX/3IDR, what the
// clothing rack adds is written back as new BINX/3IDR of the family's bin.
static void testClothingBin() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("os2-clothing-bin-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const Tgi a{0xEBCF3E27, 0x2C17B74A, 0xAAA, 0}, b{0xEBCF3E27, 0x2C17B74A, 0xBBB, 0};
    {
        hood::FamilyRecord f;
        f.version = 0x4E;
        f.funds = 500;
        DbpfWriter w;
        w.add(Tgi{hoodtype::FAMI, 0xFFFFFFFF, 1, 0}, f.write());
        w.add(Tgi{hoodtype::Wardrobe, 0xFFFFFFFF, 1, 0},
              cas::writePropertySetXml({{"type", {cas::PropertyValue::String, "clothing"}}}));
        w.add(Tgi{hoodtype::BINX, 0xFFFFFFFF, 0x40, 0},
              cas::writePropertySetXml({{"binidx", {cas::PropertyValue::Uint32, "1"}},
                                        {"objectidx", {cas::PropertyValue::Uint32, "2"}}}));
        w.add(Tgi{hoodtype::ReferenceList, 0xFFFFFFFF, 0x40, 0},
              cas::writeReferenceList({Tgi{}, Tgi{hoodtype::Wardrobe, 0xFFFFFFFF, 1, 0}, a}));
        w.save(dir / "N009_Neighborhood.package");
    }
    CodeLibrary code;
    VM vm{code};
    const auto h = hood::Neighborhood::load(dir / "N009_Neighborhood.package", false);
    loadNeighborhood(vm, h);
    check(vm.families[1].wardrobe == std::vector<Tgi>{a}, "clothing bin: read from BINX/3IDR");
    vm.families[1].wardrobe.push_back(b);
    const HoodSaveResult r = saveNeighborhood(vm, h, dir / "N009_Neighborhood.package", dir / "out.package");
    const auto h2 = hood::Neighborhood::load(dir / "out.package", false);
    check(r.clothing == 1 && h2.families.size() == 1 && h2.families[0].wardrobe == (std::vector<Tgi>{a, b}),
          "clothing bin: the bought item written back");
    fs::remove_all(dir);
}

// A neighbor made in play from an NPC template (no parents) is written to
// the neighborhood like any other (cTSNeighborManager::WriteNeighbors): its
// SDSC, its SDNA and its GUID among the service family's members.
static void testNpcNeighborSaved() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("os2-npc-save-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        hood::FamilyRecord f;
        f.version = 0x4E;
        f.funds = 20000;
        DbpfWriter w;
        w.add(Tgi{hoodtype::FAMI, 0xFFFFFFFF, 0x7FFF, 0}, f.write());
        w.save(dir / "N009_Neighborhood.package");
    }
    CodeLibrary code;
    VM vm{code};
    const auto h = hood::Neighborhood::load(dir / "N009_Neighborhood.package", false);
    loadNeighborhood(vm, h);
    Neighbor& n = vm.neighbors[50];
    n.guid = 0x0FD12345;
    n.firstName = "Allyn";
    n.lastName = "Smith";
    n.personData[persondata::NeighborId] = 50;
    n.personData[persondata::FamilyNumber] = 0x7FFF;
    n.personData[persondata::PersonAge] = 0x13;
    n.dna = {{"1", "00000001-0000-0000-0000-000000000000"}};
    const HoodSaveResult r = saveNeighborhood(vm, h, dir / "N009_Neighborhood.package", dir / "out.package");
    const auto h2 = hood::Neighborhood::load(dir / "out.package", false);
    const hood::Neighbor* saved = h2.neighbor(50);
    const hood::Family* service = h2.family(0x7FFF);
    check(r.npcs == 1 && r.born == 0 && saved && saved->description.guid == 0x0FD12345 &&
              saved->description.personData[persondata::PersonAge] == 0x13,
          "NPC made in play: its SDSC is written");
    check(saved && saved->dna == n.dna, "NPC made in play: its SDNA is written");
    check(service && service->record.members.size() == 1 && service->record.members[0] == 0x0FD12345,
          "NPC made in play: a member of the service family (FAMI 0x7FFF)");
    fs::remove_all(dir);
}

// Family membership changed in play (Generic Sims Call 4/6) goes to the
// FAMIs: the neighbor leaves its old family's members and joins the new
// one's, a family made in play gets a FAMI, a removed family loses its own.
static void testFamilyMembershipSaved() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("os2-members-save-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const uint32_t g1 = 0x0FD10001, g2 = 0x0FD10002, g3 = 0x0FD10003, g4 = 0x0FD10004;
    {
        DbpfWriter w;
        auto family = [&](uint32_t id, std::vector<uint32_t> members) {
            hood::FamilyRecord f;
            f.version = 0x4E;
            f.funds = 1000 * int32_t(id);
            f.members = std::move(members);
            w.add(Tgi{hoodtype::FAMI, 0xFFFFFFFF, id, 0}, f.write());
        };
        family(1, {g1, g2});
        family(2, {g3});
        family(3, {g4});
        auto sim = [&](uint16_t id, uint32_t guid, int16_t fam) {
            hood::SimDescription d;
            d.unknown0 = 0x70;
            d.version = 0x20;
            d.unknown2 = 0x20;
            d.unknown3 = 3;
            d.trailer = 1;
            d.neighborId = id;
            d.guid = guid;
            d.personData[persondata::FamilyNumber] = fam;
            w.add(Tgi{hoodtype::SDSC, 0xFFFFFFFF, id, 0}, d.write());
        };
        sim(10, g1, 1);
        sim(11, g2, 1);
        sim(12, g3, 2);
        sim(13, g4, 3);
        w.save(dir / "N009_Neighborhood.package");
    }
    CodeLibrary code;
    VM vm{code};
    const auto h = hood::Neighborhood::load(dir / "N009_Neighborhood.package", false);
    loadNeighborhood(vm, h);
    vm.neighbors[11].personData[persondata::FamilyNumber] = 2; // joins family 2
    vm.neighbors[12].personData[persondata::FamilyNumber] = 5; // a new family
    vm.families[5];
    vm.neighbors[13].personData[persondata::FamilyNumber] = 1; // family 3 left empty and removed
    vm.families.erase(3);
    vm.removedFamilies.insert(3);
    saveNeighborhood(vm, h, dir / "N009_Neighborhood.package", dir / "out.package");
    const auto h2 = hood::Neighborhood::load(dir / "out.package", false);
    const hood::Family *f1 = h2.family(1), *f2 = h2.family(2), *f3 = h2.family(3), *f5 = h2.family(5);
    check(f1 && f1->record.members == (std::vector<uint32_t>{g1, g4}) && f1->record.funds == 1000,
          "family members saved: one left, one joined");
    check(f2 && f2->record.members == std::vector<uint32_t>{g2}, "family members saved: the old member left, the new joined");
    check(f5 && f5->record.members == std::vector<uint32_t>{g3} && f5->record.version == 0x4E,
          "family members saved: a family made in play gets its FAMI");
    check(!f3, "family members saved: a removed family loses its FAMI");
    fs::remove_all(dir);
}

// A family's clothing bin entries (familyClothingEntries, as a family made
// in CAS gets its members' clothes): a BINX and 3IDR per item once each,
// instances past the package's, and the bin when the family has none.
static void testFamilyBinEntries() {
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / ("os2_bin_" + std::to_string(getpid()) + ".package");
    DbpfWriter w;
    w.add(Tgi{hoodtype::BINX, 0xFFFFFFFF, 5, 0}, {1, 2, 3});
    w.add(Tgi{hoodtype::Wardrobe, 0xFFFFFFFF, 1, 0}, {1});
    w.save(file);
    {
        DbpfPackage pkg(file);
        const Tgi a{0xEBCF3E27, 0x1, 10, 0}, b{0xEBCF3E27, 0x1, 11, 0};
        const auto e = familyClothingEntries(pkg, 9, {a, a, b});
        check(e.size() == 5, "family bin: two items (BINX + 3IDR) and the bin");
        check(e.size() == 5 && e[0].first.type == hoodtype::BINX && e[0].first.instance == 6 &&
                  e[2].first.instance == 7 && e[4].first.type == hoodtype::Wardrobe && e[4].first.instance == 9,
              "family bin: instances past the package's, the bin of the family");
        const auto f = familyClothingEntries(pkg, 1, {a});
        check(f.size() == 2, "family bin: a family with a bin gets no second one");
    }
    fs::remove(file);
}

// cTSObject::ParseUIString (base 1.0 FUN_00b36ed0): each token a pass in the
// game's order, found without case and as a prefix, its arguments locals
// (a local past the tree's stays as it is), $Money a x b, $Cash a + 10000 x
// b, $DayFlags the day names joined by ", ", the neighbor tokens of the
// neighbor in the stack object, $FamilyAssets of the family in the local.
static void testTokens() {
    CodeLibrary code;
    VM vm{code};
    code.add(kObjGroup, 0x12D,
             strings({"$Mes; $NeighborLocal:1 $Neighbor $FullNameLocal:1 $LastName; $$Money:2 $$Cash:2; "
                      "$DayFlags:4; $DayLocal:0; $$FamilyAssets:5; $NeighborLocal:9 stays; $TimeLocal:3 stays; "
                      "$neighborlocal:1 $FAMILY",
                      "OK"}));
    auto assign = [](uint8_t local, uint8_t value) { return instr(0x02, {local, 0, value, 0, 0, 5, 0x19, 0x07}); };
    // Locals: 0 = 3 (a day), 1 = 7 (a neighbor), 2/3 = 12 and 100, 4 = 0x45
    // (Monday, Wednesday, Sunday), 5 = 9 (a family).
    auto b = tree({assign(0, 3), assign(1, 7), assign(2, 12), assign(3, 100), assign(4, 0x45), assign(5, 9),
                   instr(0x24, {0, 0, 0, 2, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0, 1, 0})},
                  6);
    for (int k = 0; k < 6; ++k)
        b.instructions[size_t(k)].onTrue = {Kind::Instruction, uint8_t(k + 1)};
    code.add(kObjGroup, 0x1000, std::move(b));
    Neighbor n;
    n.firstName = "Bella", n.lastName = "Goth";
    n.personData[persondata::FamilyNumber] = 9;
    vm.neighbors[7] = n;
    vm.families[9].funds = 20500;
    vm.families[9].name = "Goth";
    Object& me = vm.createObject(kObjGroup);
    Thread t = vm.start(me.id, 0x1000, {}, 7);
    vm.run(t);
    auto it = vm.dialogs.find(me.id);
    check(it != vm.dialogs.end(), "tokens: a dialog");
    if (it == vm.dialogs.end())
        return;
    const std::string& m = it->second.message;
    auto has = [&](const char* s, const char* what) {
        if (m.find(s) == std::string::npos) {
            std::fprintf(stderr, "  message: %s\n", m.c_str());
            check(false, what);
        }
    };
    has("s; Bella Bella Bella Goth Goth;", "tokens: $Me as a prefix, the neighbor tokens");
    has("$1200 $1000012;", "tokens: $Money a x b, $Cash a + 10000 x b");
    // Without Live.package's STR# 0x99 here: the Portuguese names it has.
    has("Segunda-feira, Quarta-feira, Domingo;", "tokens: $DayFlags");
    has("; Quarta-feira;", "tokens: $DayLocal (local + 1 of STR# 0x99)");
    has("$20500;", "tokens: $FamilyAssets of the family in the local");
    // $NeighborLocal:9 stays, and the later $Neighbor pass takes its first
    // letters, as the game's prefix search does.
    has("BellaLocal:9 stays; $TimeLocal:3 stays;", "tokens: arguments that do not hold stay");
    has("Bella Goth", "tokens: without case");
    check(it->second.iconNeighbor == 7, "tokens: $Neighbor gives the icon");
}

// cTSObjectDialog::SetParams (BV 0x1d2b20): with byte 8 0x0E (the career's
// chance cards) the message is the string of temp 0 as it is (no + 1: only
// the text notifications add one, in TryDialog), button 1 temp 1's, button
// 2 temp 2's; button 3 the operand's.
static void testChanceCardTemps() {
    CodeLibrary code;
    VM vm{code};
    code.add(kObjGroup, 0x12D, strings({"one", "two", "three", "four", "five", "six"}));
    // Temps 0..2 := 2, 3, 4 (temp scope 8, literal 7); a yes/no/cancel dialog
    // reading message and buttons 1/2 from temps, button 3 string 6.
    auto temp = [](uint8_t t, uint8_t value) { return instr(0x02, {t, 0, value, 0, 0, 5, 0x08, 0x07}); };
    auto b = tree({temp(0, 2), temp(1, 3), temp(2, 4), instr(0x24, {6, 0, 0, 0, 0, 2, 0, 0x00, 0x0E, 0, 0, 0, 0, 0, 0})});
    for (int k = 0; k < 3; ++k)
        b.instructions[size_t(k)].onTrue = {Kind::Instruction, uint8_t(k + 1)};
    code.add(kObjGroup, 0x1000, std::move(b));
    Object& me = vm.createObject(kObjGroup);
    Thread t = vm.start(me.id, 0x1000);
    vm.run(t);
    auto it = vm.dialogs.find(me.id);
    check(it != vm.dialogs.end(), "chance card: a dialog");
    if (it == vm.dialogs.end())
        return;
    const Dialog& d = it->second;
    check(d.message == "two", "chance card: the message is temp 0's string, no + 1");
    check(d.buttons.size() == 3 && d.buttons[0] == "three" && d.buttons[1] == "four" && d.buttons[2] == "six",
          "chance card: buttons 1 and 2 from temps 1 and 2, button 3 from the operand");
}

int main() {
    lang::setPreferred(lang::EnglishUS);
    testFamilyBinEntries();
    testClothingBin();
    testNpcNeighborSaved();
    testFamilyMembershipSaved();
    testYesNoCancel();
    testTokens();
    testChanceCardTemps();
    testNotification();
    testSpecialDialogs();
    testLightAndOutfit();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("dialog tests passed\n");
    return failures ? 1 : 0;
}
