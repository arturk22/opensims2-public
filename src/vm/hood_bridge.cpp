#include "hood_bridge.hpp"
#include "wants.hpp"

#include "formats/binreader.hpp"
#include "formats/neighborhood.hpp"
#include "formats/hood_moves.hpp"

#include <memory>

#include <algorithm>
#include <cmath>

namespace os2::vm {

namespace {

// Relationship variable 1 ("UI flags") bit of a friend: "Rel - Friend - Get
// Next Friend" tests flag 5 (global BCON 0x11C "G - Relationship - UI
// Flags"), and the SRELs with it set have high daily scores (inferred).
constexpr int16_t kRelFriendFlag = 0x10;
// FAMI flags (SimPE's naming; inferred).
constexpr uint32_t kFamilyHasPhone = 0x01, kFamilyHasComputer = 0x08;
// Person Age (person data 58) of a baby.
constexpr int16_t kAgeBaby = 1;

// Person data of a neighbor: its Sim's while it is on the lot, else the
// neighborhood's copy.
const std::array<int16_t, kPersonDataCount>& personDataOf(VM& vm, int16_t id, const Neighbor& n) {
    if (Object* sim = vm.neighborSim(id))
        return sim->person->data;
    return n.personData;
}

int16_t familyOf(VM& vm, int16_t id) {
    const Neighbor* n = vm.neighbor(id);
    return n ? personDataOf(vm, id, *n)[persondata::FamilyNumber] : int16_t(0);
}

Inventory toInventory(const hood::Inventory& in) {
    Inventory out;
    for (const auto& t : in.singular) {
        InventoryToken v{.guid = t.guid, .flags = int16_t(t.flags)};
        v.properties.assign(t.data.begin(), t.data.end());
        out.singular.push_back(std::move(v));
    }
    // Counted tokens hold the count and the category (the byte the trees add
    // them with: games 3, career rewards 7, skill and pajama tokens 0) as
    // properties 0 and 1, as cTSInventory::AddCountedToken (BV 0x496ac4).
    for (const auto& t : in.counted) {
        InventoryToken v{.guid = t.guid, .flags = int16_t(t.flags)};
        v.properties.assign(t.data.begin(), t.data.end());
        out.counted.push_back(std::move(v));
    }
    return out;
}

} // namespace

uint32_t outfitCategoryBit(int16_t outfitType) {
    // The outfit keys' category bits follow the order of the outfit names in
    // Sims2.exe (casual1, casual2, casual3, swim, sleep, formal, underwear,
    // naked, maternity, gym, tryon): every Sim with a character file has
    // 0x001, 0x008, ..., 0x400 and only three have 0x002/0x004 (casual 2/3);
    // "naked" (0x080) is the one made of top and bottom (inferred).
    // STR# 0xFA: 0 Casual 1, 1 Naked, 2 Swimsuit, 3 Gym, 4 Formal, 5 Pajamas,
    // 6 Try On, 7 invalid, 8 Maternity, 9 Casual 2, 10 Casual 3, 11 Underwear.
    static constexpr uint32_t bits[] = {0x001, 0x080, 0x008, 0x200, 0x020, 0x010,
                                        0x400, 0,     0x100, 0x002, 0x004, 0x040};
    return outfitType >= 0 && size_t(outfitType) < std::size(bits) ? bits[outfitType] : 0;
}

NeighborhoodLoad loadNeighborhood(VM& vm, const hood::Neighborhood& h) {
    NeighborhoodLoad out;

    vm.families.clear();
    for (const auto& f : h.families) {
        vm.families[int16_t(f.id)] = {.funds = f.record.funds,
                                      .house = int16_t(f.record.lot),
                                      .flags = f.record.flags,
                                      .friends = int16_t(f.record.friends),
                                      .wardrobe = f.wardrobe};
        ++out.families;
    }

    // Message 0xEC1AAC03 (cTSGameStateController::EvictFamily, BV 0x139740;
    // formats/hood_moves.hpp): the family gets its lot's price back (not
    // for lot types 2 and 3), its lot becomes 0 and the lot is flagged
    // evicted when the neighborhood is saved.
    vm.evictedLots.clear();
    vm.removedFamilies.clear();
    vm.evictFamily = [&vm, lots = std::make_shared<std::vector<hood::Lot>>(h.lots)](int16_t family) {
        auto f = vm.families.find(family);
        if (f == vm.families.end() || !f->second.house)
            return;
        for (const auto& l : *lots)
            if (int16_t(l.id) == f->second.house) {
                if (l.info.type != 2 && l.info.type != 3)
                    setFamilyFunds(vm, family, int32_t(int64_t(familyFunds(vm, family)) + hood::lotFileInfo(l).price));
                vm.evictedLots.insert(f->second.house);
            }
        for (auto& [id, n] : vm.neighbors)
            if (n.house == f->second.house)
                n.house = 0;
        f->second.house = 0;
    };

    vm.lots.clear();
    for (const auto& l : h.lots) {
        vm.lots[int16_t(l.id)] = {.zoning = l.info.type, .x = float(l.info.x), .y = float(l.info.y), .name = l.name.empty() ? l.info.name : l.name};
        ++out.lots;
    }

    vm.neighbors.clear();
    for (const auto& n : h.neighbors) {
        Neighbor v;
        v.guid = n.description.guid;
        std::copy(n.description.personData.begin(), n.description.personData.end(), v.personData.begin());
        // The SDSC's own person data 31 is not the neighbor ID (it differs in
        // all 175 of N001); the VM finds Sims by it.
        v.personData[persondata::NeighborId] = int16_t(n.id);
        v.personData[persondata::FamilyNumber] = int16_t(n.family);
        if (auto f = vm.families.find(int16_t(n.family)); f != vm.families.end())
            v.house = f->second.house;
        if (n.character >= 0)
            for (const auto& o : h.characters[size_t(n.character)].outfits)
                v.outfits.push_back(o.key);
        v.firstName = n.firstName;
        v.lastName = n.lastName;
        v.dna = n.dna;
        vm.neighbors[int16_t(n.id)] = std::move(v);
        ++out.neighbors;
    }

    // Each neighbor's relationship table is the list its SDSC keeps (u32 3,
    // count, the neighbor IDs: cTSRelationshipTable::Write, BV 0xac6f04):
    // only the SRELs it lists count, and a listed one with no record (or not
    // version 2) is an entry with no variables (docs/relationships-mac.md §1).
    std::erase_if(vm.relationships, [](const auto& r) { return r.first.first.neighbor && r.first.second.neighbor; });
    std::map<std::pair<uint16_t, uint16_t>, const hood::Relationship*> srel;
    for (const auto& r : h.relationships)
        srel[{r.from, r.to}] = &r;
    for (const auto& n : h.neighbors)
        for (uint32_t to : n.description.relations) {
            auto& values = vm.relationships[{{true, int16_t(n.id)}, {true, int16_t(to)}}];
            values.clear();
            if (auto it = srel.find({uint16_t(n.id), uint16_t(to)}); it != srel.end() && it->second->version == 2)
                for (int32_t v : it->second->values) // flags (0xC000, ...) are stored as u32; the low 16 bits are the same
                    values.push_back(int16_t(v));
            if (values.size() > kMaxRelationshipVars)
                values.resize(kMaxRelationshipVars);
            ++out.relationships;
        }

    vm.familyTree.clear();
    for (const auto& e : h.familyTree) {
        const auto from = int16_t(e.from), to = int16_t(e.to);
        switch (e.type) {
        case hood::TieType::Parent:
        case hood::TieType::OtherParent: vm.familyTree[from].parents.insert(to); break;
        case hood::TieType::Child: vm.familyTree[from].children.insert(to); break;
        case hood::TieType::Spouse: vm.familyTree[from].spouses.insert(to); break;
        case hood::TieType::Sibling: vm.familyTree[from].siblings.insert(to); break;
        }
        ++out.familyTies;
    }

    vm.neighborhoodData = h.inventories.data;
    vm.inventories.clear();
    vm.inventory(InventoryKind::Global, 0) = toInventory(h.inventories.global);
    ++out.inventories;
    for (const auto& [kind, list] : {std::pair{InventoryKind::Lot, &h.inventories.lots},
                                     std::pair{InventoryKind::Family, &h.inventories.families},
                                     std::pair{InventoryKind::Neighbor, &h.inventories.neighbors}})
        for (const auto& inv : *list) {
            vm.inventory(kind, int16_t(inv.id)) = toInventory(inv);
            ++out.inventories;
        }
    loadWants(vm, h); // wants.cpp: the want library and the SWAF records
    return out;
}

std::filesystem::path neighborhoodPackageOf(const std::filesystem::path& lotPackage) {
    const auto dir = std::filesystem::absolute(lotPackage).parent_path();
    if (dir.filename() != "Lots")
        return {};
    const auto hoodDir = dir.parent_path();
    const auto package = hoodDir / (hoodDir.filename().string() + "_Neighborhood.package");
    return std::filesystem::exists(package) ? package : std::filesystem::path{};
}

int16_t lotIdOf(const std::filesystem::path& lotPackage) {
    const std::string stem = lotPackage.stem().string();
    const auto p = stem.rfind("_Lot");
    if (p == std::string::npos)
        return 0;
    const std::string digits = stem.substr(p + 4);
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        return 0;
    return int16_t(std::stoi(digits));
}

int32_t familyFunds(VM& vm, int16_t family) {
    if (family != 0 && family == vm.globals[global::CurrentFamily])
        return int32_t(vm.globals[global::BudgetDiv10000]) * 10000 + vm.globals[global::BudgetMod10000];
    auto it = vm.families.find(family);
    return it == vm.families.end() ? 0 : it->second.funds;
}

void setFamilyFunds(VM& vm, int16_t family, int32_t funds) {
    if (auto it = vm.families.find(family); it != vm.families.end())
        it->second.funds = funds;
    if (family != 0 && family == vm.globals[global::CurrentFamily]) {
        vm.globals[global::BudgetDiv10000] = int16_t(funds / 10000);
        vm.globals[global::BudgetMod10000] = int16_t(funds % 10000);
    }
}

void enterLot(VM& vm, int16_t lot, int16_t neighborhoodId) {
    int16_t family = 0;
    for (const auto& [id, f] : vm.families)
        if (id != 0 && f.house == lot) {
            family = id;
            break;
        }
    vm.globals[global::CurrentHouse] = lot;
    vm.globals[global::CurrentFamily] = family;
    vm.globals[global::CurrentNeighborhoodId] = neighborhoodId;
    auto l = vm.lots.find(lot);
    vm.globals[global::LotZoning] = l == vm.lots.end() ? int16_t(0) : l->second.zoning;
    const auto f = vm.families.find(family);
    setFamilyFunds(vm, family, f == vm.families.end() ? 0 : f->second.funds);
}

size_t bindLotSims(VM& vm) {
    size_t bound = 0;
    std::vector<ObjectId> sims;
    for (const auto& [id, o] : vm.objects())
        if (o.person)
            sims.push_back(id);
    for (ObjectId id : sims) {
        Object& o = *vm.object(id);
        const uint32_t guid = o.guid();
        for (const auto& [nid, n] : vm.neighbors)
            if (guid && n.guid == guid) {
                o.person->data = n.personData;
                ++bound;
                break;
            }
    }
    return bound;
}

std::optional<Exit> neighborhoodSimsCall(Context& c, uint8_t call) {
    VM& vm = c.vm;
    auto& temps = c.thread.temps;
    switch (call) {
    case 5: { // combine assets of the family in temp 0 into the current family (its funds; FreeSO adds the house value too)
        if (!vm.families.count(temps[0]))
            return Exit::False;
        const int16_t current = vm.globals[global::CurrentFamily];
        setFamilyFunds(vm, current, familyFunds(vm, current) + familyFunds(vm, temps[0]));
        return Exit::True;
    }
    case 27: // number of community lots into temp 0
        temps[0] = int16_t(std::count_if(vm.lots.begin(), vm.lots.end(), [](const auto& l) { return l.second.zoning == 1; }));
        return Exit::True;
    case 28: // perform money effect: only something to show
        vm.request({call, c.thread.me, c.frame.stackObject, temps});
        return Exit::True;
    case 32: { // number of my outfits of the type in temp 0 into temp 1
        // Only the Sim's own outfits (its character file) are counted, not
        // the family wardrobe; "Clothes - Do I have Spares?" passes age and
        // gender in temps 1 and 2, which would filter the wardrobe's
        // (inferred).
        const Object& me = c.me();
        const Neighbor* n = me.person ? vm.neighbor(me.person->data[persondata::NeighborId]) : nullptr;
        const uint32_t bit = outfitCategoryBit(temps[0]);
        int16_t count = 0;
        if (n && bit) {
            // One outfit per category: the lists of its parts share the key's category.
            for (uint32_t key : n->outfits)
                if ((key >> 16) & bit) {
                    count = 1;
                    break;
                }
        }
        temps[1] = count;
        return Exit::True;
    }
    case 34: { // distance between lots temp 0 and temp 1 into temp 2 (in neighborhood tiles; inferred)
        auto a = vm.lots.find(temps[0]), b = vm.lots.find(temps[1]);
        if (a == vm.lots.end() || b == vm.lots.end())
            return Exit::False;
        temps[2] = int16_t(std::lround(std::hypot(a->second.x - b->second.x, a->second.y - b->second.y)));
        return Exit::True;
    }
    case 35: { // zoning type of lot temp 0 into temp 1
        auto l = vm.lots.find(temps[0]);
        if (l == vm.lots.end())
            return Exit::False;
        temps[1] = l->second.zoning;
        return Exit::True;
    }
    case 36: // family living on lot temp 0 into temp 1, -1 if none
        temps[1] = -1;
        for (const auto& [id, f] : vm.families)
            if (id != 0 && f.house == temps[0]) {
                temps[1] = id;
                break;
            }
        return Exit::True;
    case 50: { // money from the family of neighbor temp 1: temp 2 percent, into temps 3/4
        // temps 3 and 4 are the amount / 10000 and % 10000, like the budget
        // globals ("Memory - Is Other Sim Rich?" compares temp 3 with 5);
        // with temp 0 = 0 the amount is only computed (inferred).
        if (temps[5] != 0)
            throw VMError("Generic Sims Call: 50 from the lot assets (temp 5) not implemented");
        const int16_t family = familyOf(vm, temps[1]);
        if (!vm.neighbor(temps[1]) || family == 0)
            return Exit::False;
        const int32_t amount = int32_t(int64_t(familyFunds(vm, family)) * temps[2] / 100);
        if (temps[0] != 0) {
            setFamilyFunds(vm, family, familyFunds(vm, family) - amount);
            const int16_t current = vm.globals[global::CurrentFamily];
            setFamilyFunds(vm, current, familyFunds(vm, current) + amount);
        }
        temps[3] = int16_t(amount / 10000);
        temps[4] = int16_t(amount % 10000);
        return Exit::True;
    }
    default: return std::nullopt;
    }
}

std::optional<int16_t> nextInNeighborhood(Context& c, uint8_t type, int16_t current) {
    VM& vm = c.vm;
    switch (type) {
    case 11: { // lot ID (house number), from 0
        auto it = vm.lots.upper_bound(current);
        while (it != vm.lots.end() && it->first <= 0)
            ++it;
        return it == vm.lots.end() ? std::nullopt : std::optional<int16_t>(it->first);
    }
    case 36: { // neighbor whose relationship from the neighbor in temp 0 has variable 1 & temp 1 (direction inferred)
        const VM::RelationshipKey from{true, c.thread.temps[0]};
        const int16_t mask = c.thread.temps[1];
        for (auto it = vm.neighbors.upper_bound(current); it != vm.neighbors.end(); ++it) {
            auto rel = vm.relationships.find({from, VM::RelationshipKey{true, it->first}});
            if (rel != vm.relationships.end() && rel->second.size() > 1 && (rel->second[1] & mask))
                return it->first;
        }
        return std::nullopt;
    }
    default: throw VMError("Set to Next: search type " + std::to_string(type) + " not implemented");
    }
}

int16_t neighborhoodNeighborField(VM& vm, int16_t id, const Neighbor& n, int16_t field) {
    const int16_t family = personDataOf(vm, id, n)[persondata::FamilyNumber];
    const auto f = vm.families.find(family);
    const VM::Family* fam = family != 0 && f != vm.families.end() ? &f->second : nullptr;
    switch (field) {
    case 1: return family != 0 && family == vm.globals[global::CurrentFamily]; // belongs in house (as in FreeSO)
    case 5: { // friend count: neighbors this one has the friend flag for
        int16_t count = 0;
        for (const auto& [key, values] : vm.relationships)
            if (key.first.neighbor && key.first.id == id && key.second.neighbor && values.size() > 1 &&
                (values[1] & kRelFriendFlag))
                ++count;
        return count;
    }
    case 7: return fam && (fam->flags & kFamilyHasPhone);
    case 8: { // has baby: someone of its family is a baby
        if (!fam)
            return 0;
        for (const auto& [other, m] : vm.neighbors) {
            const auto& data = personDataOf(vm, other, m);
            if (data[persondata::FamilyNumber] == family && data[persondata::PersonAge] == kAgeBaby)
                return 1;
        }
        return 0;
    }
    case 9: return fam ? fam->friends : int16_t(0);
    case 10: return fam && (fam->flags & kFamilyHasComputer);
    default: throw VMError("neighbor data " + std::to_string(field) + " not implemented");
    }
}

namespace {

// Alter Budget (0x19), with the operand of The Sims 1's (FreeSO's
// VMTransferFundsOperand): byte 0 where the amount is (0 literal, 1
// parameter, 2 local, 3 the owner in byte 1), u16 data at 2, flags at 4
// (0x01 only test, 0x02 add instead of subtract, 0x04 not understood),
// expense type at 6 (STR# 0xF0, not kept). False when the current family
// cannot pay (funds would go below 0), and then nothing changes.
Exit alterBudget(Context& c) {
    const auto& o = c.instruction.operands;
    static constexpr uint8_t oldOwners[] = {0x07, 0x09, 0x19};
    const uint8_t owner = o[0] < 3 ? oldOwners[o[0]] : o[1];
    const int32_t amount = c.get(owner, int16_t(le16(&o[2])));
    const uint8_t flags = o[4];
    VM& vm = c.vm;
    const int16_t family = vm.globals[global::CurrentFamily];
    const int32_t funds = familyFunds(vm, family);
    const int32_t after = flags & 0x02 ? funds + amount : funds - amount;
    if (after < 0 && !(flags & 0x02))
        return Exit::False;
    if (!(flags & 0x01))
        setFamilyFunds(vm, family, after);
    return Exit::True;
}

} // namespace

void registerNeighborhoodPrimitives(VM& vm) { vm.setPrimitive(0x19, alterBudget); }

} // namespace os2::vm
