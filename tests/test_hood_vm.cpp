// The neighborhood in the VM (vm/hood_bridge): loading a hand-built
// hood::Neighborhood and the primitives and data owners that read it.
#include "formats/neighborhood.hpp"
#include "vm/hood_bridge.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/interactions.hpp"
#include "vm/placement.hpp"

#include <cstdlib>
#include <filesystem>

#include <cstdio>
#include <initializer_list>
#include <set>
#include <tuple>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

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
    in.nodeVersion = opcode == 0x1A ? 1 : 0; // Relationship: the current operand layout
    size_t i = 0;
    for (auto b : operands)
        in.operands[i++] = b;
    return in;
}

static Behavior::Instruction expr(uint8_t lo, int16_t ld, uint8_t op, uint8_t ro, int16_t rd, Target t, Target f) {
    return instr(0x02, t, f,
                 {uint8_t(ld), uint8_t(uint16_t(ld) >> 8), uint8_t(rd), uint8_t(uint16_t(rd) >> 8), 0, op, lo, ro});
}

enum Owner : uint8_t { Global = 0x06, Lit = 0x07, Temp = 0x08, StackId = 0x0A, PData = 0x12, Neigh = 0x18,
                       HoodData = 0x22 };
enum Op : uint8_t { Eq = 2, Assign = 5 };

static Behavior tree(std::vector<Behavior::Instruction> code) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.instructions = std::move(code);
    return b;
}

constexpr uint32_t kSimGroup = 0x7F000010;
constexpr uint32_t kMortimerGuid = 0x4DAE6810, kBellaGuid = 0x0DAE6972, kTownieGuid = 0x2DAE885B;

// Two families: the Goths (1: Mortimer 1, Bella 15, baby Cassandra 16) on lot
// 11, and family 5 (Nina 14) on lot 18; lot 47 is a community lot.
static hood::Neighborhood makeHood() {
    hood::Neighborhood h;
    h.prefix = "N001";
    h.info = hood::NeighborhoodInfo{.version = 3, .prefix = "N001", .id = 1};
    auto neighbor = [&](uint16_t id, uint32_t guid, uint32_t family, int16_t age) {
        hood::Neighbor n;
        n.id = id;
        n.description.neighborId = id;
        n.description.guid = guid;
        n.description.personData[hood::sdsc::Age] = age;
        n.description.personData[hood::sdsc::FamilyNumber] = int16_t(family);
        n.description.personData[hood::sdsc::NeighborId] = 0x70; // not the neighbor ID in the game's SDSCs
        n.description.personData[hood::sdsc::Nice] = int16_t(id * 10);
        n.family = family;
        h.neighbors.push_back(n);
    };
    neighbor(1, kMortimerGuid, 1, hood::lifestage::Adult);
    neighbor(14, kTownieGuid, 5, hood::lifestage::Adult);
    neighbor(15, kBellaGuid, 1, hood::lifestage::Adult);
    neighbor(16, 0x8DAE69D9, 1, hood::lifestage::Baby);
    hood::Character c;
    c.guid = kMortimerGuid;
    c.outfits = {{0x00010008, {}}, {0x00800004, {}}, {0x00800010, {}}, {0x00200008, {}}};
    h.characters.push_back(c);
    h.neighbors[0].character = 0;

    hood::Family goths;
    goths.id = 1;
    goths.record.lot = 11;
    goths.record.funds = 511632;
    goths.record.flags = 0x9; // telephone, computer
    goths.record.friends = 3;
    goths.members = {1, 15, 16};
    hood::Family five;
    five.id = 5;
    five.record.lot = 18;
    five.record.funds = 1040;
    five.record.flags = 0x1;
    five.members = {14};
    h.families = {goths, five};

    for (auto [id, type, x, y] : {std::tuple{11u, uint8_t(0), 67u, 56u}, std::tuple{18u, uint8_t(0), 70u, 60u},
                                  std::tuple{47u, uint8_t(1), 78u, 70u}}) {
        hood::Lot l;
        l.id = id;
        l.info.type = type;
        l.info.x = x, l.info.y = y;
        h.lots.push_back(l);
    }

    auto rel = [&](uint16_t from, uint16_t to, std::vector<int32_t> values) {
        hood::Relationship r;
        r.from = from, r.to = to, r.values = std::move(values);
        h.relationships.push_back(r);
    };
    rel(1, 15, {90, 0xC033, 89, 9}); // known, family, friend, best friend, crush, love; spouse
    rel(1, 14, {60, 0x8010, 40, 0}); // known, friend
    rel(15, 1, {80, 0xC03A, 70, 9});
    rel(1, 16, {50, 0xC000, 50, 1}); // known, family; child

    h.familyTree = {{16, 1, hood::TieType::Parent}, {16, 15, hood::TieType::OtherParent},
                    {1, 16, hood::TieType::Child},  {15, 16, hood::TieType::Child},
                    {1, 15, hood::TieType::Spouse}, {15, 1, hood::TieType::Spouse}};

    h.inventories.global.singular = {{0x1234, 0, {1, 2}}};
    hood::Inventory famInv;
    famInv.id = 1;
    famInv.counted = {{0x0D26B746, 1, {12, 3}}}; // 12 games of category 3
    h.inventories.families = {famInv};
    hood::Inventory memories;
    memories.id = 1;
    memories.singular = {{0x5555, 3, {7, 0x6810, 0x4DAE}}};
    h.inventories.neighbors = {memories};
    return h;
}

struct Fixture {
    CodeLibrary code;
    VM vm{code};
    hood::Neighborhood hood = makeHood();
    Fixture() {
        loadNeighborhood(vm, hood);
        enterLot(vm, 11, 1);
    }
    // A Sim object standing for neighbor `id` (its OBJD GUID set through a definition).
    Object& sim(uint32_t guid) {
        ObjectDefinition d;
        d.fields.resize(60);
        d.fields[(0x5C - 0x44) / 2] = uint16_t(guid);
        d.fields[(0x5E - 0x44) / 2] = uint16_t(guid >> 16);
        d.fields[(0x52 - 0x44) / 2] = kObjectTypePerson;
        static uint16_t next = 0x80;
        const uint16_t inst = next++;
        code.add(kSimGroup, inst, d);
        return vm.instantiatePart(kSimGroup, inst);
    }
    Thread run(ObjectId me, uint16_t id, Behavior b, ObjectId stackObject = 0) {
        code.add(kSimGroup, id, std::move(b));
        Thread t = vm.start(me, id, {}, stackObject);
        vm.run(t);
        if (t.status == Thread::Status::Failed)
            std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
        return t;
    }
};

static bool ok(const Thread& t, bool result = true) {
    return t.status == Thread::Status::Finished && t.result == result;
}

static void testLoad() {
    Fixture fx;
    VM& vm = fx.vm;
    check(vm.neighbors.size() == 4, "every SDSC is a neighbor");
    const Neighbor* m = vm.neighbor(1);
    check(m && m->guid == kMortimerGuid && m->house == 11, "neighbor GUID and house (its family's lot)");
    check(m && m->personData[persondata::NeighborId] == 1 && m->personData[persondata::FamilyNumber] == 1 &&
              m->personData[hood::sdsc::Nice] == 10,
          "neighbor person data: SDSC, 31 = neighbor ID, 61 = family");
    check(m && m->outfits.size() == 4, "neighbor outfits from the character file");
    auto r = vm.relationships.find({{true, 1}, {true, 15}});
    check(r != vm.relationships.end() && r->second.size() == 4 && r->second[0] == 90 && uint16_t(r->second[1]) == 0xC033,
          "relationship values (flags keep their 16 bits)");
    check(vm.familyTree[16].parents == std::set<int16_t>{1, 15} && vm.familyTree[1].children.count(16) &&
              vm.familyTree[1].spouses.count(15),
          "family tree");
    const auto& g = vm.inventory(InventoryKind::Global, 0);
    check(g.singular.size() == 1 && g.singular[0].guid == 0x1234 && g.singular[0].properties.size() == 2,
          "global inventory");
    const auto& f = vm.inventory(InventoryKind::Family, 1);
    check(f.counted.size() == 1 && countOf(f.counted[0]) == 12 && categoryOf(f.counted[0]) == 3 && f.counted[0].flags == 1,
          "counted token: count and category");
    const auto& n = vm.inventory(InventoryKind::Neighbor, 1);
    check(n.singular.size() == 1 && n.singular[0].flags == 3 && n.singular[0].properties[2] == 0x4DAE,
          "neighbor inventory (a memory)");
    check(vm.families.size() == 2 && vm.families[1].funds == 511632 && vm.lots[47].zoning == 1, "families and lots");

    check(vm.globals[global::CurrentHouse] == 11 && vm.globals[global::CurrentFamily] == 1 &&
              vm.globals[global::CurrentNeighborhoodId] == 1 && vm.globals[global::LotZoning] == 0,
          "entering the lot sets its globals");
    check(vm.globals[global::BudgetDiv10000] == 51 && vm.globals[global::BudgetMod10000] == 1632 &&
              familyFunds(vm, 1) == 511632,
          "the current family's funds are the budget globals");

    Object& mortimer = fx.sim(kMortimerGuid);
    fx.sim(0x11111111); // a Sim that is no neighbor
    check(bindLotSims(vm) == 1, "bindLotSims matches Sims by GUID");
    check(mortimer.person->data[persondata::NeighborId] == 1 && mortimer.person->data[hood::sdsc::Nice] == 10,
          "the lot's Sim gets its neighbor's person data");
    check(vm.neighborSim(1) == &mortimer, "the neighbor's Sim is found by neighbor ID");
}

static void testPrimitives() {
    Fixture fx;
    VM& vm = fx.vm;
    Object& me = fx.sim(kMortimerGuid);
    bindLotSims(vm);

    // Alter Budget: test paying 10,000, pay 1,632, earn 10,000.
    auto t = fx.run(me.id, 0x1001,
                    tree({instr(0x19, go(1), E, {3, Lit, 0x10, 0x27, 0x01, 0, 0, 0}),
                          instr(0x19, go(2), E, {0, 0, 0x60, 0x06, 0x00, 0, 4, 0}),     // literal 1,632, subtract
                          instr(0x19, T, E, {3, Lit, 0x10, 0x27, 0x02, 0, 2, 0})}));     // 10,000, add
    check(ok(t) && familyFunds(vm, 1) == 511632 - 1632 + 10000 && vm.families[1].funds == familyFunds(vm, 1),
          "Alter Budget: test, subtract and add");
    vm.globals[global::BudgetDiv10000] = 0, vm.globals[global::BudgetMod10000] = 5;
    t = fx.run(me.id, 0x1002, tree({instr(0x19, T, F, {0, 0, 0x06, 0x00, 0x00, 0, 0, 0})}));
    check(ok(t, false) && familyFunds(vm, 1) == 5, "Alter Budget: false when the family cannot pay");

    // Generic Sims Calls: 27 community lots, 35 zoning, 36 family in lot, 34 distance.
    t = fx.run(me.id, 0x1003,
               tree({instr(0x01, go(1), E, {27}), expr(Temp, 5, Assign, Temp, 0, go(2), E),
                     expr(Temp, 0, Assign, Lit, 47, go(3), E), instr(0x01, go(4), E, {35}),
                     expr(Temp, 6, Assign, Temp, 1, go(5), E), expr(Temp, 0, Assign, Lit, 18, go(6), E),
                     instr(0x01, go(7), E, {36}), expr(Temp, 7, Assign, Temp, 1, go(8), E),
                     expr(Temp, 0, Assign, Lit, 11, go(9), E), expr(Temp, 1, Assign, Lit, 18, go(10), E),
                     instr(0x01, T, E, {34})}));
    check(ok(t) && t.temps[5] == 1 && t.temps[6] == 1 && t.temps[7] == 5 && t.temps[2] == 5,
          "Generic Sims Call 27, 35, 36 and 34");

    // 50: half of family 5's funds (neighbor 14), taken.
    vm.globals[global::BudgetDiv10000] = 0, vm.globals[global::BudgetMod10000] = 0;
    t = fx.run(me.id, 0x1004,
               tree({expr(Temp, 0, Assign, Lit, 1, go(1), E), expr(Temp, 1, Assign, Lit, 14, go(2), E),
                     expr(Temp, 2, Assign, Lit, 50, go(3), E), instr(0x01, T, E, {50})}));
    check(ok(t) && t.temps[3] == 0 && t.temps[4] == 520 && familyFunds(vm, 5) == 520 && familyFunds(vm, 1) == 520,
          "Generic Sims Call 50 moves the money");
    // 5: combine the assets of family 5.
    t = fx.run(me.id, 0x1005, tree({expr(Temp, 0, Assign, Lit, 5, go(1), E), instr(0x01, T, E, {5})}));
    check(ok(t) && familyFunds(vm, 1) == 1040, "Generic Sims Call 5 adds the family's funds");

    // 32: outfits of a type on me (formal = STR# 0xFA 4 -> bit 0x20; swimsuit 2 -> 0x08).
    t = fx.run(me.id, 0x1006,
               tree({expr(Temp, 0, Assign, Lit, 4, go(1), E), instr(0x01, go(2), E, {32}),
                     expr(Temp, 5, Assign, Temp, 1, go(3), E), expr(Temp, 0, Assign, Lit, 5, go(4), E),
                     instr(0x01, T, E, {32})}));
    check(ok(t) && t.temps[5] == 1 && t.temps[1] == 0, "Generic Sims Call 32 counts my outfits of a type");

    // Set to Next 36 (friends of temp 0: variable 1 & temp 1) and 11 (lots).
    t = fx.run(me.id, 0x1007,
               tree({expr(Temp, 0, Assign, Lit, 1, go(1), E), expr(Temp, 1, Assign, Lit, 0x10, go(2), E),
                     expr(Temp, 6, Assign, Lit, 0, go(3), E),
                     instr(0x1F, go(4), go(5), {0, 0, 0, 0, 0x80 | 36, Temp, 0, 6, 0}),
                     expr(Temp, 2, Assign, Temp, 6, go(3), E), // last found
                     expr(Temp, 3, Assign, Lit, 0, go(6), E),
                     instr(0x1F, go(7), go(8), {0, 0, 0, 0, 0x80 | 11, Temp, 0, 3, 0}),
                     expr(Temp, 4, Assign, Temp, 3, go(6), E), expr(Temp, 7, Assign, Lit, 0, T, E)}));
    check(ok(t) && t.temps[2] == 15 && t.temps[4] == 47, "Set to Next 36 and 11");

    // Neighbor data (0x18) of Bella (15) as the stack object; neighborhood data (0x22).
    vm.neighborhoodData[2] = 11;
    t = fx.run(me.id, 0x1008,
               tree({expr(Temp, 0, Assign, Neigh, 1, go(1), E), expr(Temp, 1, Assign, Neigh, 5, go(2), E),
                     expr(Temp, 2, Assign, Neigh, 7, go(3), E), expr(Temp, 3, Assign, Neigh, 8, go(4), E),
                     expr(Temp, 4, Assign, Neigh, 9, go(5), E), expr(Temp, 5, Assign, Neigh, 10, go(6), E),
                     expr(Temp, 6, Assign, HoodData, 2, go(7), E), expr(HoodData, 3, Assign, Lit, 1, T, E)}),
               15);
    check(ok(t) && t.temps[0] == 1 && t.temps[1] == 1 && t.temps[2] == 1 && t.temps[3] == 1 && t.temps[4] == 3 &&
              t.temps[5] == 1 && t.temps[6] == 11 && vm.neighborhoodData[3] == 1,
          "neighbor data: belongs in house, friends, telephone, baby, family friends, computer; neighborhood data");
}

// With the game's data (OS2_DATA): the lot objects' value of the Lots &
// Houses bin's "Ranch Retreat" (cx_00000001) as the game saved it, §1090:
// its five doors (§80) count as Build mode objects through their master's
// build mode type (field 0x45 = 8; the parts have 0), with the windows'
// §690. The value missed the doors (§690), so the lot's price dropped after
// a save (live QA 14: §10.857 for sale, §10.387 after the save).
static void testLotObjectsValue() {
    const char* data = std::getenv("OS2_DATA");
    if (!data)
        return;
    const std::filesystem::path res = std::filesystem::path(data) / "TSData/Res";
    const std::filesystem::path lotPath = res / "UserData/LotCatalog/cx_00000001.package";
    if (!std::filesystem::exists(lotPath))
        return;
    ResourceIndex index(res);
    CodeLibrary code(&index);
    DbpfPackage pkg(lotPath);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    VM vm(code);
    vm.initCreatedObjects = true;
    const LotLoad load = loadLot(vm, lot, saved, charactersDirOf(lotPath));
    startSavedLot(vm, load, saved, false);
    computeLotData(vm);
    check(vm.lotValues.lotObjects == int32_t(saved.simulator.fA8) && saved.simulator.fA8 == 1090,
          "Ranch Retreat: the lot objects' value as the game saved it (doors by their master's build mode type)");
}

// With the game's data (OS2_DATA): two Sims sent to one double bed together
// both get in (N002_Lot10, Jenny 386 and Pollination Tech 160 met head-on
// on the way and the first to stop gave up, "Routing - Goto Head of Bed"
// failing 14), and the player's pick on the partner lying in it ("*Object
// Menu Subqueue" of the person, the bed's "CT - Object Menu Subqueue")
// offers WooHoo, which pushes the bed's pair onto both.
static void testBedTogether() {
    const char* data = std::getenv("OS2_DATA");
    if (!data)
        return;
    const std::filesystem::path res = std::filesystem::path(data) / "TSData/Res";
    const std::filesystem::path lotPath = res / "UserData/Neighborhoods/N002/Lots/N002_Lot10.package";
    if (!std::filesystem::exists(lotPath))
        return;
    ResourceIndex index(res);
    CodeLibrary code(&index);
    DbpfPackage pkg(lotPath);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    VM vm(code);
    vm.initCreatedObjects = true;
    vm.setClockGlobals();
    const bool hood = loadLotNeighborhood(vm, lotPath);
    const LotLoad load = loadLot(vm, lot, saved, charactersDirOf(lotPath));
    if (hood)
        bindLotSims(vm);
    startSavedLot(vm, load, saved, false);
    runLotLoaded(vm);
    const ObjectId jenny = 386, tech = 160, bed = 758;
    auto pick = [&](ObjectId sim, ObjectId object, const std::string& name) {
        for (const auto& e : pieMenu(vm, sim, object))
            if (e.available() && !e.debug && e.name == name)
                if (auto q = interactionFromPie(vm, e)) {
                    q->pushedBy = sim;
                    addAction(vm, *vm.object(sim), *q);
                    return true;
                }
        return false;
    };
    check(vm.object(jenny) && vm.object(tech) && vm.object(bed), "bed together: N002_Lot10's Jenny, Pollination Tech and bed");
    if (!vm.object(jenny) || !vm.object(tech) || !vm.object(bed))
        return;
    // A tick apart, as two clicks are (the same tick, both pause alike and
    // give up together: TODO).
    const bool first = pick(jenny, bed, "Relaxar");
    vm.tick();
    check(first && pick(tech, bed, "Relaxar"), "bed together: both pick Relaxar");
    auto inBed = [&](ObjectId sim) {
        const ObjectId c = vm.object(sim)->data[objdata::ContainerId];
        return c == bed || c == 763; // the two heads, E and F
    };
    for (int i = 0; i < 1300 && !(inBed(jenny) && inBed(tech)); ++i)
        vm.tick();
    check(inBed(jenny) && inBed(tech), "bed together: both get in");
    bool offered = false;
    for (const auto& e : pieMenu(vm, jenny, tech))
        offered = offered || (e.available() && !e.debug && e.name == "Oba-Oba");
    check(offered, "bed together: the partner's menu offers Oba-Oba");
    check(pick(jenny, tech, "Oba-Oba"), "bed together: Oba-Oba picked");
    bool pushed = false;
    for (int i = 0; i < 300 && !pushed; ++i) {
        vm.tick();
        for (ObjectId sim : {jenny, tech}) {
            const Object& o = *vm.object(sim);
            for (const auto* list : {&o.queue, &o.subQueue})
                for (const auto& q : *list)
                    pushed = pushed || ((q.object == bed || q.object == 763) && (q.interaction == 11 || q.interaction == 12));
        }
    }
    check(pushed, "bed together: WooHoo's pair is pushed");
}

int main() {
    testLotObjectsValue();
    testBedTogether();
    testLoad();
    testPrimitives();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("hood vm: all tests passed\n");
    return failures ? 1 : 0;
}
