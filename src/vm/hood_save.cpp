// Writing the neighborhood back after a lot save (hood_save.hpp).

#include "hood_save.hpp"

#include "hood_bridge.hpp"
#include "family_write.hpp"
#include "wants.hpp"

#include "formats/dbpf.hpp"
#include "formats/binwriter.hpp"
#include "formats/character_write.hpp"
#include "formats/neighborhood.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <variant>

namespace os2::vm {

namespace {

// A binary property set (cGZPropertySet, magic 0xCBE750E0 version 2) as the
// neighborhood's BINX entries are stored: u32 values and strings in the
// order given.
std::vector<uint8_t> binaryPropertySet(const std::vector<std::pair<std::string, std::variant<uint32_t, std::string>>>& props) {
    BinWriter w;
    w.u32(0xCBE750E0);
    w.u16(2);
    w.u32(uint32_t(props.size()));
    auto str = [&](const std::string& t) {
        w.u32(uint32_t(t.size()));
        w.raw(std::span(reinterpret_cast<const uint8_t*>(t.data()), t.size()));
    };
    for (const auto& [name, v] : props) {
        if (const uint32_t* n = std::get_if<uint32_t>(&v)) {
            w.u32(0xEB61E4F7);
            str(name);
            w.u32(*n);
        } else {
            w.u32(0x0B8BEA18);
            str(name);
            str(std::get<std::string>(v));
        }
    }
    return std::move(w.bytes());
}

// The VM's view of a neighborhood inventory, as loadNeighborhood makes it.
Inventory toVm(const hood::Inventory& in) {
    Inventory out;
    for (const auto& t : in.singular) {
        InventoryToken v{.guid = t.guid, .flags = int16_t(t.flags)};
        v.properties.assign(t.data.begin(), t.data.end());
        out.singular.push_back(std::move(v));
    }
    for (const auto& t : in.counted) {
        InventoryToken v{.guid = t.guid, .flags = int16_t(t.flags)};
        v.properties.assign(t.data.begin(), t.data.end());
        out.counted.push_back(std::move(v));
    }
    return out;
}

bool same(const Inventory& a, const Inventory& b) {
    auto eq = [](const std::vector<InventoryToken>& x, const std::vector<InventoryToken>& y) {
        if (x.size() != y.size())
            return false;
        for (size_t i = 0; i < x.size(); ++i) {
            if (x[i].guid != y[i].guid || x[i].flags != y[i].flags || x[i].properties != y[i].properties)
                return false;
        }
        return true;
    };
    return eq(a.singular, b.singular) && eq(a.counted, b.counted);
}

// A VM inventory in the package's form (the VM keeps every saved value of a
// token as its properties, counted tokens too).
hood::Inventory fromVm(uint32_t id, const Inventory& in, [[maybe_unused]] const hood::Inventory* was) {
    hood::Inventory out;
    out.id = id;
    for (const auto& t : in.singular) {
        hood::InventoryToken v{t.guid, uint16_t(t.flags), {}};
        v.data.assign(t.properties.begin(), t.properties.end());
        out.singular.push_back(std::move(v));
    }
    for (size_t i = 0; i < in.counted.size(); ++i) {
        const auto& t = in.counted[i];
        hood::InventoryToken v{t.guid, uint16_t(t.flags), {}};
        v.data.assign(t.properties.begin(), t.properties.end());
        out.counted.push_back(std::move(v));
    }
    return out;
}

std::vector<int32_t> keepWide(const std::vector<int16_t>& vm, const std::vector<int32_t>& saved) {
    std::vector<int32_t> out;
    for (size_t i = 0; i < vm.size(); ++i)
        out.push_back(i < saved.size() && int16_t(saved[i]) == vm[i] ? saved[i] : vm[i]);
    for (size_t i = out.size(); i < saved.size() && vm.size() >= kMaxRelationshipVars; ++i)
        out.push_back(saved[i]);
    return out;
}

} // namespace

const std::vector<size_t>& persistentPersonData() {
    // cTSNeighbor::sGetPersistentDataFields (BV 0x492524): the pairs whose
    // SDSC version is <= 0x20 (docs/save-mac.md §6).
    static const std::vector<size_t> fields = [] {
        std::vector<size_t> f;
        auto range = [&](size_t a, size_t b) {
            for (size_t i = a; i <= b; ++i)
                f.push_back(i);
        };
        range(2, 7);
        f.push_back(70);
        range(9, 18);
        for (size_t i : {58, 60, 61, 56, 57, 63, 65})
            f.push_back(i);
        range(67, 69);
        f.push_back(74);
        range(81, 101);
        range(106, 153);
        for (size_t i : {155, 156, 159})
            f.push_back(i);
        range(46, 51);
        for (size_t i : {22, 23})
            f.push_back(i);
        range(160, 162);
        f.push_back(165);
        std::sort(f.begin(), f.end());
        f.erase(std::unique(f.begin(), f.end()), f.end());
        return f;
    }();
    return fields;
}

std::vector<std::pair<Tgi, std::vector<uint8_t>>>
familyClothingEntries(const DbpfPackage& pkg, uint32_t family, const std::vector<Tgi>& gzps,
                      const std::vector<std::pair<Tgi, std::vector<uint8_t>>>& pending) {
    // cTSFamily::SaveFamilyClothing writes each new item as a BINX
    // (0x0C560F39, CPF: binidx 1 and objectidx 2 into its 3IDR, the rest 0
    // as in the shipped ones) whose 3IDR of the same instance lists nothing,
    // the family's bin (0x6C4F359D, instance = family) and the GZPS.
    // Instances: one past the package's highest BINX/3IDR and those pending
    // (inferred; the game's own come from its instance allocator).
    std::vector<std::pair<Tgi, std::vector<uint8_t>>> out;
    uint32_t nextBinx = 0;
    bool hasBin = false;
    auto see = [&](const Tgi& t) {
        if (t.type == hoodtype::BINX || t.type == hoodtype::ReferenceList)
            nextBinx = std::max(nextBinx, t.instance + 1);
        if (t.type == hoodtype::Wardrobe && t.instance == family)
            hasBin = true;
    };
    for (const auto& e : pkg.entries())
        see(e.tgi);
    for (const auto& [t, bytes] : pending)
        see(t);
    std::vector<Tgi> done;
    for (const Tgi& k : gzps) {
        if (std::find(done.begin(), done.end(), k) != done.end())
            continue; // cTSFamily::AddClothing keeps one of each
        done.push_back(k);
        const uint32_t inst = nextBinx++;
        out.push_back({Tgi{hoodtype::BINX, 0xFFFFFFFF, inst, 0},
                       binaryPropertySet({{"binidx", 1u},
                                          {"creatorid", std::string("00000000-0000-0000-0000-000000000000")},
                                          {"iconidx", 0u},
                                          {"objectidx", 2u},
                                          {"sortindex", 0u},
                                          {"stringindex", 0u},
                                          {"stringsetidx", 0u}})});
        out.push_back({Tgi{hoodtype::ReferenceList, 0xFFFFFFFF, inst, 0},
                       cas::writeReferenceList({Tgi{}, Tgi{hoodtype::Wardrobe, 0xFFFFFFFF, family, 0}, k})});
    }
    if (!hasBin && !done.empty())
        // A family without a bin yet gets one as the shipped ones are.
        out.push_back({Tgi{hoodtype::Wardrobe, 0xFFFFFFFF, family, 0},
                       binaryPropertySet({{"creatorid", std::string("00000000-0000-0000-0000-000000000000")},
                                          {"flags", 1u},
                                          {"iconidx", 0u},
                                          {"sortindex", 0u},
                                          {"stringindex", 0u},
                                          {"stringsetidx", 0u},
                                          {"type", std::string("clothing")}})});
    return out;
}

HoodSaveResult saveNeighborhood(VM& vm, const hood::Neighborhood& h, const std::filesystem::path& source,
                                const std::filesystem::path& out) {
    namespace fs = std::filesystem;
    if (fs::exists(out) && fs::equivalent(out, source))
        throw VMError("refusing to write a neighborhood over its source package");
    HoodSaveResult res;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> replaced; // (type, instance) -> bytes
    std::vector<std::pair<Tgi, std::vector<uint8_t>>> added;

    // FAMI funds; the lot's family's first day was set up when the lot
    // loaded (cTSNeighborhood::SetupFamilyFirstDay: SetNewLot(0), FAMI flag
    // 0x04, formats/lot_arrivals.hpp).
    const int16_t lotFamily = vm.globals[global::CurrentFamily];
    for (const auto& f : h.families) {
        const int32_t funds = familyFunds(vm, int16_t(f.id));
        const bool firstDay = lotFamily && f.id == uint32_t(lotFamily) && (f.record.flags & 0x04);
        // A family the VM evicted (VM::evictFamily) has no lot now.
        const auto vf = vm.families.find(int16_t(f.id));
        const bool evicted = f.record.lot && (vf == vm.families.end() || vf->second.house == 0) &&
                             vm.evictedLots.count(int16_t(f.record.lot));
        if (funds == f.record.funds && !firstDay && !evicted)
            continue;
        hood::FamilyRecord r = f.record;
        r.funds = funds;
        if (evicted)
            r.lot = 0;
        if (firstDay)
            r.flags &= ~0x04u;
        replaced[{hoodtype::FAMI, f.id}] = r.write();
        ++res.families;
    }
    // The lot info of the lot saved (cTSSaveLotController::SaveLot, BV
    // 0x884ec4): flag 1 set ("saved"), 4 (first day) and 8 (evicted family,
    // whose things the load cleaned up) cleared.
    for (const int16_t lotId : vm.evictedLots)
        if (const hood::Lot* lot = h.lot(uint32_t(lotId)); lot && lotId != vm.globals[global::CurrentHouse]) {
            hood::LotInfo info = lot->info;
            info.flags |= hood::LotInfo::kFlagEvicted;
            replaced[{hoodtype::LotInfo, uint32_t(lotId)}] = info.write();
        }
    if (const int16_t lotId = vm.globals[global::CurrentHouse]; lotId > 0)
        if (const hood::Lot* lot = h.lot(uint32_t(lotId))) {
            hood::LotInfo info = lot->info;
            info.flags = (info.flags | hood::LotInfo::kFlagSaved) &
                         ~(hood::LotInfo::kFlagFirstDay | hood::LotInfo::kFlagEvicted);
            if (info.flags != lot->info.flags)
                replaced[{hoodtype::LotInfo, uint32_t(lotId)}] = info.write();
        }

    // SDSC person data and relations.
    std::map<uint16_t, hood::SimDescription> descriptions;
    auto description = [&](const hood::Neighbor& n) -> hood::SimDescription& {
        auto it = descriptions.find(n.id);
        if (it == descriptions.end())
            it = descriptions.emplace(n.id, n.description).first;
        return it->second;
    };
    for (const auto& n : h.neighbors) {
        const Object* sim = nullptr;
        for (const auto& [_, o] : vm.eachObject())
            if (o.person && n.description.guid && o.guid() == n.description.guid) {
                sim = &o;
                break;
            }
        const auto vmn = vm.neighbors.find(int16_t(n.id));
        for (size_t i : persistentPersonData()) {
            if (i >= hood::kSdscPersonDataCount)
                continue;
            int16_t v = n.description.personData[i];
            if (sim) {
                v = sim->person->data[i];
            } else if (vmn != vm.neighbors.end()) {
                // Off the lot: what the VM changed of what it was given
                // (it was given the neighbor ID and family in 31/61).
                const int16_t given = i == persondata::FamilyNumber ? int16_t(n.family) : n.description.personData[i];
                if (vmn->second.personData[i] != given)
                    v = vmn->second.personData[i];
            }
            if (v != n.description.personData[i])
                description(n).personData[i] = v;
        }
    }

    // Neighbors made in play (Make New Character, characters.cpp: the
    // babies born, the NPCs and townies made from a template; or a Sim the
    // VM made the neighbor of): cTSNeighborManager::WriteNeighbors (BV
    // 0x53a76c) writes every neighbor, new ones too. An SDSC as the game
    // writes a new neighbor's (version 0x20, header words as the CAS
    // families', family_write.hpp), its SDNA, its GUID among its family's
    // members (FAMI: the NPCs' 0x7FFF, by Generic Sims Call 4) and, for a
    // baby, its parents in the family tree. Their character files are
    // written beside (vm/baby_character.hpp; the app's save copies them
    // into Characters).
    std::set<uint16_t> newIds, bornIds;
    std::map<uint16_t, hood::SimDescription> born; // every new neighbor's SDSC
    std::map<uint32_t, hood::FamilyRecord> famiChanged;
    // A family's FAMI as this save writes it: what the funds/eviction pass
    // above made of it, else the package's.
    auto familyRecord = [&](const hood::Family& f) -> hood::FamilyRecord& {
        auto it = famiChanged.find(f.id);
        if (it == famiChanged.end()) {
            auto r = replaced.find({hoodtype::FAMI, f.id});
            it = famiChanged.emplace(f.id, r != replaced.end() ? hood::FamilyRecord::parse(r->second) : f.record).first;
        }
        return it->second;
    };
    for (const auto& [id, n] : vm.neighbors) {
        if (id <= 0 || h.neighbor(uint16_t(id)) || !n.guid)
            continue;
        newIds.insert(uint16_t(id));
        if (n.parentA)
            bornIds.insert(uint16_t(id));
        else
            ++res.npcs;
        hood::SimDescription d;
        d.unknown0 = 0x70;
        d.version = 0x20;
        d.unknown2 = 0x20;
        d.unknown3 = 3;
        d.trailer = 1;
        d.neighborId = uint16_t(id);
        d.guid = n.guid;
        const Object* sim = nullptr;
        for (const auto& [_, o] : vm.eachObject())
            if (o.person && o.guid() == n.guid)
                sim = &o;
        for (size_t i : persistentPersonData())
            if (i < hood::kSdscPersonDataCount)
                d.personData[i] = sim ? sim->person->data[i] : n.personData[i];
        born[uint16_t(id)] = d;
        if (!n.dna.empty()) {
            std::map<std::string, cas::PropertyValue> props;
            for (const auto& [k, v] : n.dna)
                props[k] = {cas::PropertyValue::String, v};
            added.push_back({Tgi{hoodtype::SDNA, 0xFFFFFFFF, uint32_t(id), 0}, cas::writePropertySetXml(props)});
        }
        const uint32_t family = uint32_t(uint16_t(n.personData[persondata::FamilyNumber]));
        for (const auto& f : h.families)
            if (f.id == family) {
                auto& members = familyRecord(f).members;
                if (std::find(members.begin(), members.end(), n.guid) == members.end())
                    members.push_back(n.guid);
            }
    }
    // Family membership the VM changed (Generic Sims Call 4 "add to family"
    // and 6 "remove from family", cTSFamilyManager::AddToFamily/
    // RemoveFromFamily, BV 0x4fddcc/0x4fe800): a neighbor of the package
    // whose family number is now another leaves the old FAMI's members and
    // joins the new one's; a family the VM made (MakeNewFamily) gets its
    // FAMI, named after its first member's last name as the binary does
    // (and its STR#), and a family the VM removed (RemoveFamily, the last
    // member gone) loses its FAMI.
    auto currentFamily = [&](const hood::Neighbor& n) -> int64_t { // -1: the VM does not know
        for (const auto& [_, o] : vm.eachObject())
            if (o.person && n.description.guid && o.guid() == n.description.guid)
                return uint16_t(o.person->data[persondata::FamilyNumber]);
        if (auto it = vm.neighbors.find(int16_t(n.id)); it != vm.neighbors.end())
            return uint16_t(it->second.personData[persondata::FamilyNumber]);
        return -1;
    };
    std::map<uint32_t, std::vector<uint32_t>> joined; // family -> GUIDs of neighbors of the package now in it
    std::map<uint32_t, std::string> lastNames;
    for (const auto& n : h.neighbors) {
        const int64_t now = currentFamily(n);
        if (now < 0 || !n.description.guid || uint32_t(now) == n.family)
            continue;
        if (n.family)
            for (const auto& f : h.families)
                if (f.id == n.family) {
                    auto& members = familyRecord(f).members;
                    members.erase(std::remove(members.begin(), members.end(), n.description.guid), members.end());
                }
        if (now) {
            joined[uint32_t(now)].push_back(n.description.guid);
            lastNames.emplace(uint32_t(now), n.lastName);
        }
    }
    std::set<uint32_t> removedFamilies;
    for (const auto& f : h.families) {
        if (vm.removedFamilies.count(int16_t(f.id)) && !vm.families.count(int16_t(f.id))) {
            removedFamilies.insert(f.id);
            continue;
        }
        if (auto j = joined.find(f.id); j != joined.end()) {
            auto& members = familyRecord(f).members;
            for (uint32_t g : j->second)
                if (std::find(members.begin(), members.end(), g) == members.end())
                    members.push_back(g);
        }
    }
    for (const auto& [id, vf] : vm.families) {
        if (id <= 0 || h.family(uint32_t(uint16_t(id))))
            continue;
        hood::FamilyRecord r; // as the CAS families' (family_write.cpp)
        r.version = 0x4E;
        r.lot = uint32_t(uint16_t(vf.house));
        r.simGroupId = uint32_t(uint16_t(id));
        r.funds = familyFunds(vm, id);
        r.friends = uint32_t(uint16_t(vf.friends));
        r.flags = vf.flags ? vf.flags : 1;
        if (auto j = joined.find(uint32_t(uint16_t(id))); j != joined.end())
            r.members = j->second;
        for (const auto& [nid, n] : vm.neighbors) // new neighbors (made in play) in it
            if (nid > 0 && !h.neighbor(uint16_t(nid)) && n.guid &&
                n.personData[persondata::FamilyNumber] == id &&
                std::find(r.members.begin(), r.members.end(), n.guid) == r.members.end())
                r.members.push_back(n.guid);
        if (r.members.empty())
            continue;
        const uint32_t fid = uint32_t(uint16_t(id));
        added.push_back({Tgi{hoodtype::FAMI, 0xFFFFFFFF, fid, 0}, r.write()});
        auto ln = lastNames.find(fid);
        added.push_back({Tgi{kTypeStr, 0xFFFFFFFF, fid, 0}, writeFamilyStrings({ln != lastNames.end() ? ln->second : std::string(), std::string()})});
        ++res.families;
    }
    for (auto& [family, r] : famiChanged)
        if (!removedFamilies.count(family))
            replaced[{hoodtype::FAMI, family}] = r.write();
    if (!bornIds.empty()) {
        // The family tree (u32 version, u32 count, per Sim u16 neighbor ID,
        // u32 1, u32 n, n x (u32 type, u16 neighbor ID)), the parents gaining
        // the child (4) and the child its parents (0, 1).
        std::map<uint16_t, std::vector<std::pair<uint32_t, uint16_t>>> rows;
        std::vector<uint16_t> order;
        uint32_t version = 1;
        Tgi tiesTgi{hoodtype::FamilyTies, 0xFFFFFFFF, 0, 0};
        {
            DbpfPackage src(source);
            for (const auto& e : src.entries())
                if (e.tgi.type == hoodtype::FamilyTies) {
                    tiesTgi = e.tgi;
                    const auto old = hood::FamilyTies::parse(src.read(e));
                    version = old.version ? old.version : 1;
                    for (const auto& sm : old.sims) {
                        order.push_back(sm.neighborId);
                        for (const auto& t : sm.ties)
                            rows[sm.neighborId].push_back({uint32_t(t.type), t.other});
                    }
                    break;
                }
        }
        auto addTie = [&](uint16_t who, uint32_t type, uint16_t other) {
            if (!rows.count(who))
                order.push_back(who);
            rows[who].push_back({type, other});
        };
        for (uint16_t id : bornIds) {
            const Neighbor& n = vm.neighbors.at(int16_t(id));
            addTie(id, 0, uint16_t(n.parentA));
            if (n.parentB && n.parentB != n.parentA)
                addTie(id, 1, uint16_t(n.parentB));
            addTie(uint16_t(n.parentA), 4, id);
            if (n.parentB && n.parentB != n.parentA)
                addTie(uint16_t(n.parentB), 4, id);
        }
        BinWriter tw;
        tw.u32(version);
        tw.u32(uint32_t(order.size()));
        for (uint16_t who : order) {
            tw.u16(who);
            tw.u32(1);
            tw.u32(uint32_t(rows[who].size()));
            for (const auto& [type, other] : rows[who]) {
                tw.u32(type);
                tw.u16(other);
            }
        }
        replaced[{hoodtype::FamilyTies, tiesTgi.instance}] = std::move(tw.bytes());
        res.born = bornIds.size();
    }

    // Sim-to-Sim relationships.
    std::map<std::pair<uint16_t, uint16_t>, const hood::Relationship*> saved;
    for (const auto& r : h.relationships)
        saved[{r.from, r.to}] = &r;
    for (const auto& [k, v16] : vm.relationships) {
        if (!k.first.neighbor || !k.second.neighbor)
            continue;
        const auto from = uint16_t(k.first.id), to = uint16_t(k.second.id);
        const uint32_t instance = uint32_t(from) << 16 | to;
        if (auto it = saved.find({from, to}); it != saved.end()) {
            // The entry as the VM has it, as long as the game's (stale
            // bytes after the values are dropped, as ResourceWrite does).
            const auto values = keepWide(v16, it->second->values);
            if (values == it->second->values)
                continue;
            hood::Relationship r = *it->second;
            r.values = values;
            replaced[{hoodtype::SREL, instance}] = r.write();
            ++res.relationshipsChanged;
            continue;
        }
        // A new entry, even of zeros: the game's table has the key (it
        // made the entry), so the SDSC lists it and the record is written.
        std::vector<int32_t> values(v16.begin(), v16.end());
        const hood::Neighbor* a = h.neighbor(from);
        if (newIds.count(from) || newIds.count(to)) {
            if ((a || newIds.count(from)) && (h.neighbor(to) || newIds.count(to))) {
                hood::Relationship r;
                r.from = from;
                r.to = to;
                r.values = values;
                added.push_back({Tgi{hoodtype::SREL, 0xFFFFFFFF, instance, 0}, r.write()});
                auto& d = a ? description(*a) : born[from];
                if (std::find(d.relations.begin(), d.relations.end(), to) == d.relations.end())
                    d.relations.push_back(to);
                ++res.relationshipsNew;
            }
            continue;
        }
        if (!a || !h.neighbor(to)) {
            res.warnings.push_back("relationship " + std::to_string(from) + " -> " + std::to_string(to) +
                                   ": not between neighbors of the package");
            continue;
        }
        hood::Relationship r;
        r.from = from;
        r.to = to;
        r.values = values;
        added.push_back({Tgi{hoodtype::SREL, 0xFFFFFFFF, instance, 0}, r.write()});
        auto& d = description(*a);
        if (std::find(d.relations.begin(), d.relations.end(), to) == d.relations.end())
            d.relations.push_back(to);
        ++res.relationshipsNew;
    }
    // A table with new keys is written in its hash order: the keys it read,
    // in the order read, then the new ones (relationshipTableOrder).
    for (auto& [id, d] : descriptions)
        if (const hood::Neighbor* n = h.neighbor(id); n && d.relations != n->description.relations)
            d.relations = hood::relationshipTableOrder(d.relations);
    for (auto& [id, d] : born)
        d.relations = hood::relationshipTableOrder(d.relations);
    for (const auto& [id, d] : descriptions) {
        const hood::Neighbor* n = h.neighbor(id);
        if (n && (d.personData != n->description.personData || d.relations != n->description.relations)) {
            replaced[{hoodtype::SDSC, id}] = d.write();
            ++res.descriptions;
        }
    }

    // Inventories. The game writes the NGBH at every neighborhood save
    // (cTSNeighborhood::Save) after SanifyInventories (docs/save-mac.md §9.1,
    // §9.3): the VM's inventories when they changed, then one per lot,
    // family and neighbor of the neighborhood, by ID.
    if (h.inventories.version) {
        const auto& inv = h.inventories;
        bool changed = false;
        std::map<std::pair<InventoryKind, int16_t>, const hood::Inventory*> was;
        was[{InventoryKind::Global, 0}] = &inv.global;
        for (const auto& [kind, list] : {std::pair{InventoryKind::Lot, &inv.lots},
                                         std::pair{InventoryKind::Family, &inv.families},
                                         std::pair{InventoryKind::Neighbor, &inv.neighbors}})
            for (const auto& i : *list)
                was[{kind, int16_t(i.id)}] = &i;
        for (const auto& [key, w] : was) {
            auto it = vm.inventories.find(key);
            const Inventory empty;
            if (!same(it == vm.inventories.end() ? empty : it->second, toVm(*w)))
                changed = true;
        }
        for (const auto& [key, i] : vm.inventories)
            if (!was.count(key) && (!i.singular.empty() || !i.counted.empty()))
                changed = true;
        if (vm.neighborhoodData != inv.data)
            changed = true;
        hood::NeighborhoodInventories n = inv;
        if (changed) {
            n.data = vm.neighborhoodData;
            auto rebuild = [&](InventoryKind kind, std::vector<hood::Inventory>& list) {
                std::vector<hood::Inventory> out;
                for (const auto& i : list) {
                    auto it = vm.inventories.find({kind, int16_t(i.id)});
                    out.push_back(it == vm.inventories.end() ? hood::Inventory{i.id, {}, {}}
                                                             : fromVm(i.id, it->second, &i));
                }
                for (const auto& [key, i] : vm.inventories)
                    if (key.first == kind && !was.count(key) && (!i.singular.empty() || !i.counted.empty()))
                        out.push_back(fromVm(uint32_t(key.second), i, nullptr));
                list = std::move(out);
            };
            if (auto it = vm.inventories.find({InventoryKind::Global, 0}); it != vm.inventories.end())
                n.global = fromVm(0, it->second, &inv.global);
            rebuild(InventoryKind::Lot, n.lots);
            rebuild(InventoryKind::Family, n.families);
            rebuild(InventoryKind::Neighbor, n.neighbors);
            res.inventories = true;
        }
        // The lots (LotInfos), families (the family manager's: the
        // package's, less those removed, and those made in play) and
        // neighbors (the package's and those made in play) of the
        // neighborhood.
        std::vector<uint32_t> lotIds, familyIds, neighborIds;
        for (const auto& l : h.lots)
            lotIds.push_back(l.id);
        for (const auto& f : h.families)
            if (!removedFamilies.count(f.id))
                familyIds.push_back(f.id);
        for (const auto& [tgi, bytes] : added) // FAMIs made above
            if (tgi.type == hoodtype::FAMI)
                familyIds.push_back(tgi.instance);
        for (const auto& nb : h.neighbors)
            neighborIds.push_back(nb.id);
        for (uint16_t id : newIds)
            neighborIds.push_back(id);
        n.sanify(lotIds, familyIds, neighborIds);
        replaced[{hoodtype::NGBH, 0}] = n.write();
    }

    // SWAF: each Sim's wants, fears and achievement history, for those whose
    // state changed (pending want updates are run first, as WritePerson does).
    std::map<uint32_t, std::vector<uint8_t>> swaf;
    if (WantSystem* ws = wantSystem(vm)) {
        flushWants(vm);
        for (const auto& [id, s] : ws->sims)
            if (wantRecordChanged(vm, id)) {
                swaf[uint32_t(uint16_t(id))] = wantRecord(vm, id).write();
                ++res.wants;
            }
    }

    // The family clothing bins: what the clothing rack added (the VM's
    // family wardrobe past the hood's), as familyClothingEntries writes it.
    std::map<uint32_t, std::vector<Tgi>> newClothing; // family -> GZPS
    for (const auto& f : h.families) {
        auto vf = vm.families.find(int16_t(f.id));
        if (vf == vm.families.end())
            continue;
        for (const Tgi& k : vf->second.wardrobe)
            if (std::find(f.wardrobe.begin(), f.wardrobe.end(), k) == f.wardrobe.end()) {
                newClothing[f.id].push_back(k);
                ++res.clothing;
            }
    }

    // The package: every resource as it is but the replaced ones.
    DbpfPackage pkg(source);
    for (const auto& [family, items] : newClothing)
        for (auto& e : familyClothingEntries(pkg, family, items, added))
            added.push_back(std::move(e));
    DbpfWriter w;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == type::DIR)
            continue;
        const uint32_t instance = e.tgi.type == hoodtype::NGBH ? 0 : e.tgi.instance;
        if (e.tgi.type == hoodtype::FAMI && removedFamilies.count(e.tgi.instance))
            continue; // RemoveFamily
        if (auto it = replaced.find({e.tgi.type, instance}); it != replaced.end()) {
            w.add(e.tgi, std::move(it->second), true);
            replaced.erase(it);
        } else if (auto sw = e.tgi.type == wants::kTypeSwaf ? swaf.find(e.tgi.instance) : swaf.end(); sw != swaf.end()) {
            w.add(e.tgi, std::move(sw->second), true);
            swaf.erase(sw);
        } else {
            w.addStored(e.tgi, pkg.readRaw(e), e.rawSize);
        }
    }
    for (auto& [tgi, bytes] : added)
        w.add(tgi, std::move(bytes), true);
    for (const auto& [id, d] : born)
        w.add(Tgi{hoodtype::SDSC, 0xFFFFFFFF, id, 0}, d.write(), true);
    // Replaced records the package did not have (a first family tree).
    for (auto& [key, bytes] : replaced)
        if (key.first == hoodtype::FamilyTies)
            w.add(Tgi{key.first, 0xFFFFFFFF, key.second, 0}, std::move(bytes), true);
    for (auto& [instance, bytes] : swaf) // Sims that had no SWAF
        w.add(Tgi{wants::kTypeSwaf, 0xFFFFFFFF, instance, 0}, std::move(bytes), true);
    pkg.closeFile();
    w.save(out);
    return res;
}

std::vector<std::string> compareNeighborhoodState(VM& a, VM& b, size_t limit) {
    std::vector<std::string> out;
    size_t more = 0;
    auto diff = [&](const std::string& s) {
        if (out.size() < limit)
            out.push_back(s);
        else
            ++more;
    };
    for (const auto& [id, f] : a.families)
        if (familyFunds(a, id) != familyFunds(b, id))
            diff("family " + std::to_string(id) + " funds " + std::to_string(familyFunds(a, id)) + " vs " +
                 std::to_string(familyFunds(b, id)));
    auto rels = [](VM& m) {
        std::map<std::pair<int16_t, int16_t>, std::vector<int16_t>> r;
        for (const auto& [k, v] : m.relationships)
            if (k.first.neighbor && k.second.neighbor) {
                auto t = v;
                while (!t.empty() && t.back() == 0)
                    t.pop_back();
                if (!t.empty())
                    r[{k.first.id, k.second.id}] = t;
            }
        return r;
    };
    const auto ra = rels(a), rb = rels(b);
    for (const auto& [k, v] : ra)
        if (auto it = rb.find(k); it == rb.end() || it->second != v)
            diff("relationship " + std::to_string(k.first) + " -> " + std::to_string(k.second));
    for (const auto& [k, v] : rb)
        if (!ra.count(k))
            diff("relationship " + std::to_string(k.first) + " -> " + std::to_string(k.second) + " only in the second");
    for (const auto& [key, i] : a.inventories) {
        auto it = b.inventories.find(key);
        const Inventory empty;
        if (!same(i, it == b.inventories.end() ? empty : it->second))
            diff("inventory " + std::to_string(int(key.first)) + "/" + std::to_string(key.second));
    }
    auto data = [](VM& m, const Neighbor& n) -> const std::array<int16_t, kPersonDataCount>& {
        for (const auto& [_, o] : m.objects())
            if (o.person && n.guid && o.guid() == n.guid)
                return o.person->data;
        return n.personData;
    };
    for (const auto& [id, n] : a.neighbors) {
        auto it = b.neighbors.find(id);
        if (it == b.neighbors.end()) {
            diff("neighbor " + std::to_string(id) + " missing from the second");
            continue;
        }
        if (n.firstName != it->second.firstName || n.lastName != it->second.lastName)
            diff("neighbor " + std::to_string(id) + " name \"" + n.firstName + " " + n.lastName + "\" vs \"" +
                 it->second.firstName + " " + it->second.lastName + "\"");
        if (n.dna != it->second.dna)
            diff("neighbor " + std::to_string(id) + " DNA");
        const auto& x = data(a, n);
        const auto& y = data(b, it->second);
        for (size_t i : persistentPersonData())
            if (i < x.size() && x[i] != y[i]) {
                diff("neighbor " + std::to_string(id) + " person data " + std::to_string(i) + " " +
                     std::to_string(x[i]) + " vs " + std::to_string(y[i]));
                break;
            }
    }
    for (const auto& d : compareWantState(a, b, limit))
        diff(d);
    if (more)
        out.push_back("... and " + std::to_string(more) + " more");
    return out;
}

} // namespace os2::vm
