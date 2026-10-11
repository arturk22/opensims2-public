// Writing the neighborhood back after a lot save (hood_save.hpp).

#include "hood_save.hpp"

#include "hood_bridge.hpp"
#include "wants.hpp"

#include "formats/dbpf.hpp"
#include "formats/neighborhood.hpp"

#include <algorithm>
#include <map>
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

    // FAMI funds.
    for (const auto& f : h.families) {
        const int32_t funds = familyFunds(vm, int16_t(f.id));
        if (funds == f.record.funds)
            continue;
        hood::FamilyRecord r = f.record;
        r.funds = funds;
        replaced[{hoodtype::FAMI, f.id}] = r.write();
        ++res.families;
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
