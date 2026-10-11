// Writing the neighborhood back after a lot save (hood_save.hpp).

#include "hood_save.hpp"

#include "hood_bridge.hpp"
#include "wants.hpp"

#include "formats/dbpf.hpp"
#include "formats/binwriter.hpp"
#include "formats/character_write.hpp"
#include "formats/neighborhood.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace os2::vm {

namespace {

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
        v.count = t.data.size() > 0 ? int16_t(t.data[0]) : int16_t(0);
        v.category = t.data.size() > 1 ? int16_t(t.data[1]) : int16_t(0);
        out.counted.push_back(std::move(v));
    }
    return out;
}

bool same(const Inventory& a, const Inventory& b) {
    auto eq = [](const std::vector<InventoryToken>& x, const std::vector<InventoryToken>& y, bool counted) {
        if (x.size() != y.size())
            return false;
        for (size_t i = 0; i < x.size(); ++i) {
            if (x[i].guid != y[i].guid || x[i].flags != y[i].flags)
                return false;
            if (counted ? (x[i].count != y[i].count || x[i].category != y[i].category)
                        : x[i].properties != y[i].properties)
                return false;
        }
        return true;
    };
    return eq(a.singular, b.singular, false) && eq(a.counted, b.counted, true);
}

// A VM inventory in the package's form; tokens the VM did not change keep
// their saved values (a counted token's values past the count and category).
hood::Inventory fromVm(uint32_t id, const Inventory& in, const hood::Inventory* was) {
    hood::Inventory out;
    out.id = id;
    for (const auto& t : in.singular) {
        hood::InventoryToken v{t.guid, uint16_t(t.flags), {}};
        v.data.assign(t.properties.begin(), t.properties.end());
        out.singular.push_back(std::move(v));
    }
    for (size_t i = 0; i < in.counted.size(); ++i) {
        const auto& t = in.counted[i];
        hood::InventoryToken v{t.guid, uint16_t(t.flags), {uint16_t(t.count), uint16_t(t.category)}};
        if (was && i < was->counted.size() && was->counted[i].guid == t.guid && was->counted[i].data.size() > 2)
            v.data.insert(v.data.end(), was->counted[i].data.begin() + 2, was->counted[i].data.end());
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

std::vector<int32_t> trimmed(std::vector<int32_t> v) {
    while (!v.empty() && v.back() == 0)
        v.pop_back();
    return v;
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
        if (funds == f.record.funds && !firstDay)
            continue;
        hood::FamilyRecord r = f.record;
        r.funds = funds;
        if (firstDay)
            r.flags &= ~0x04u;
        replaced[{hoodtype::FAMI, f.id}] = r.write();
        ++res.families;
    }
    // The lot info of the lot saved (cTSSaveLotController::SaveLot, BV
    // 0x884ec4): flag 1 set ("saved"), 4 (first day) and 8 (evicted family,
    // whose things the load cleaned up) cleared.
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
        for (const auto& [_, o] : vm.objects())
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

    // Sims born in play (Make New Character, characters.cpp): an SDSC as the
    // game writes a new neighbor's (version 0x20, header words as the CAS
    // families', family_write.hpp), its SDNA, its GUID among its family's
    // members (FAMI) and its parents in the family tree. Their character
    // files are written beside (vm/baby_character.hpp; the app's save copies
    // them into Characters).
    std::set<uint16_t> bornIds;
    std::map<uint16_t, hood::SimDescription> born;
    std::map<uint32_t, hood::FamilyRecord> famiChanged;
    for (const auto& [id, n] : vm.neighbors) {
        if (id <= 0 || h.neighbor(uint16_t(id)) || !n.guid || !n.parentA)
            continue;
        bornIds.insert(uint16_t(id));
        hood::SimDescription d;
        d.unknown0 = 0x70;
        d.version = 0x20;
        d.unknown2 = 0x20;
        d.unknown3 = 3;
        d.trailer = 1;
        d.neighborId = uint16_t(id);
        d.guid = n.guid;
        const Object* sim = nullptr;
        for (const auto& [_, o] : vm.objects())
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
                auto it = famiChanged.find(family);
                if (it == famiChanged.end())
                    it = famiChanged.emplace(family, f.record).first;
                if (auto r = replaced.find({hoodtype::FAMI, family}); r != replaced.end())
                    it->second.funds = familyFunds(vm, int16_t(family));
                if (std::find(it->second.members.begin(), it->second.members.end(), n.guid) == it->second.members.end())
                    it->second.members.push_back(n.guid);
            }
    }
    for (auto& [family, r] : famiChanged)
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
            const auto values = keepWide(v16, it->second->values);
            if (trimmed(values) == trimmed(it->second->values))
                continue;
            hood::Relationship r = *it->second;
            r.values = values;
            if (r.values.size() < it->second->values.size())
                r.values.resize(it->second->values.size(), 0);
            replaced[{hoodtype::SREL, instance}] = r.write();
            ++res.relationshipsChanged;
            continue;
        }
        std::vector<int32_t> values(v16.begin(), v16.end());
        if (trimmed(values).empty())
            continue;
        const hood::Neighbor* a = h.neighbor(from);
        if (bornIds.count(from) || bornIds.count(to)) {
            if ((a || bornIds.count(from)) && (h.neighbor(to) || bornIds.count(to))) {
                hood::Relationship r;
                r.from = from;
                r.to = to;
                r.unknown = 2;
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
        r.unknown = 2;
        r.values = values;
        added.push_back({Tgi{hoodtype::SREL, 0xFFFFFFFF, instance, 0}, r.write()});
        auto& d = description(*a);
        if (std::find(d.relations.begin(), d.relations.end(), to) == d.relations.end())
            d.relations.push_back(to);
        ++res.relationshipsNew;
    }
    for (const auto& [id, d] : descriptions) {
        const hood::Neighbor* n = h.neighbor(id);
        if (n && (d.personData != n->description.personData || d.relations != n->description.relations)) {
            replaced[{hoodtype::SDSC, id}] = d.write();
            ++res.descriptions;
        }
    }

    // Inventories.
    {
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
        if (changed && inv.version) {
            hood::NeighborhoodInventories n = inv;
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
            replaced[{hoodtype::NGBH, 0}] = n.write();
            res.inventories = true;
        }
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

    // The package: every resource as it is but the replaced ones.
    DbpfPackage pkg(source);
    DbpfWriter w;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == type::DIR)
            continue;
        const uint32_t instance = e.tgi.type == hoodtype::NGBH ? 0 : e.tgi.instance;
        if (auto it = replaced.find({e.tgi.type, instance}); it != replaced.end()) {
            w.add(e.tgi, std::move(it->second), e.compressed());
            replaced.erase(it);
        } else if (auto sw = e.tgi.type == wants::kTypeSwaf ? swaf.find(e.tgi.instance) : swaf.end(); sw != swaf.end()) {
            w.add(e.tgi, std::move(sw->second), e.compressed());
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
