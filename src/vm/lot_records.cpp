// The records of a lot save outside the object module (lot_records.hpp).

#include "lot_records.hpp"

#include "formats/binreader.hpp"
#include "formats/binwriter.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/neighborhood.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace os2::vm {

namespace {

constexpr uint32_t kTypeSceneObject = 0xFA1C39F7, kTypeRouting = 0xBC66BAEC;
constexpr size_t kFileNameSize = 64;

// Where a cObject keeps its position and rotation (7 floats, twice: before
// and after the definition's name), as Lot's reader walks it.
struct SceneLayout {
    size_t first = 0, second = 0; // byte offsets; second = 0 if the copy is not there
    std::string definition;       // "Trash Can - Outside_objt"
};

std::optional<SceneLayout> sceneLayout(std::span<const uint8_t> bytes) {
    try {
        BinReader r(bytes);
        r.skip(kFileNameSize);
        uint32_t version = 0;
        for (int depth = 0;; ++depth) {
            if (depth == 8)
                return std::nullopt;
            r.u32();
            version = r.u32();
            if (r.str7() == "cObject")
                break;
        }
        r.str7();
        for (uint32_t n = r.u32(); n; --n) {
            r.str7();
            if (version >= 0x11)
                r.u32();
            for (uint32_t m = r.u32(); m; --m) {
                r.str7();
                r.str7();
            }
        }
        SceneLayout l;
        l.first = r.pos();
        r.skip(28);
        r.u32();
        l.definition = r.str7();
        if (r.remaining() >= 28 && std::memcmp(&bytes[l.first], &bytes[r.pos()], 28) == 0)
            l.second = r.pos();
        return l;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::array<float, 7> readPose(std::span<const uint8_t> bytes, size_t at) {
    std::array<float, 7> p{};
    std::memcpy(p.data(), &bytes[at], 28);
    return p;
}

void writePose(std::vector<uint8_t>& bytes, const SceneLayout& l, const std::array<float, 7>& p) {
    std::memcpy(&bytes[l.first], p.data(), 28);
    if (l.second)
        std::memcpy(&bytes[l.second], p.data(), 28);
}

int16_t directionOf(const LotObjects::State& s) {
    return s.data.size() > size_t(objdata::Direction) ? s.data[objdata::Direction] : 0;
}

bool inWorld(const LotObjects::State& s) { return s.x >= 0 && s.y >= 0; }

// The pose the game gives a state: position in tiles with the floor's
// elevation plus the object's (out of the world: the origin), rotation
// about Z by 90° - 45° × direction. What did not change keeps the old
// pose's values (the game's quaternions have either sign).
std::array<float, 7> poseOf(const Lot& world, const LotObjects::State& s, const LotObjects::State* was,
                            const std::array<float, 7>* old) {
    std::array<float, 7> p = old ? *old : std::array<float, 7>{0, 0, 0, 0, 0, 0, 1};
    if (!inWorld(s)) {
        p[0] = p[1] = p[2] = 0;
    } else {
        p[0] = s.x;
        p[1] = s.y;
        if (old && was && inWorld(*was) && was->level == s.level)
            p[2] = (*old)[2] - was->elevation + s.elevation;
        else
            p[2] = (world.hasLevel(s.level - 1) ? world.elevationAt(s.level - 1, s.x, s.y) : 3.0f * float(s.level - 1)) +
                   s.elevation;
    }
    if (!old || !was || directionOf(*was) != directionOf(s)) {
        const double a = (90.0 - 45.0 * directionOf(s)) * M_PI / 180.0;
        p[3] = p[4] = 0;
        p[5] = float(std::sin(a / 2));
        p[6] = float(std::cos(a / 2));
    }
    return p;
}

bool moved(const LotObjects::State& a, const LotObjects::State& b) {
    return a.x != b.x || a.y != b.y || a.level != b.level || a.elevation != b.elevation ||
           directionOf(a) != directionOf(b);
}

std::vector<uint8_t> writeRelationship(uint32_t unknown, const std::vector<int32_t>& values,
                                       const std::vector<int32_t>& stale) {
    BinWriter w;
    w.u32(unknown);
    w.u32(uint32_t(values.size()));
    for (int32_t v : values)
        w.i32(v);
    for (int32_t v : stale)
        w.i32(v);
    return std::move(w.bytes());
}

} // namespace

RecordsResult captureLotRecords(VM& vm, const Lot& world, DbpfPackage& source, const LotObjects& before,
                                LotObjects& lot, const std::vector<std::filesystem::path>& templatePackages) {
    RecordsResult out;
    std::map<uint32_t, const DbpfEntry*> scene, routing;
    std::map<uint32_t, const DbpfEntry*> relationships;
    for (const auto& e : source.entries()) {
        if (e.tgi.type == kTypeSceneObject)
            scene[e.tgi.instance] = &e;
        else if (e.tgi.type == kTypeRouting)
            routing[e.tgi.instance] = &e;
        else if (e.tgi.type == hoodtype::SREL)
            relationships[e.tgi.instance] = &e;
    }
    auto addRecord = [&](uint32_t type, uint32_t group, uint32_t instance, std::vector<uint8_t> bytes) {
        lot.otherRecords.push_back({type, group, instance, std::move(bytes)});
    };

    // Scene graph objects.
    std::map<std::string, std::vector<uint8_t>> templates; // definition name -> a cObject of it
    bool templatesRead = false;
    auto templateOf = [&](const std::string& definition) -> const std::vector<uint8_t>* {
        if (!templatesRead) {
            templatesRead = true;
            for (const auto& path : templatePackages) {
                try {
                    DbpfPackage pkg(path);
                    for (const auto& e : pkg.entries())
                        if (e.tgi.type == kTypeSceneObject) {
                            auto bytes = pkg.read(e);
                            if (auto l = sceneLayout(bytes); l && !l->definition.empty())
                                templates.emplace(l->definition, std::move(bytes));
                        }
                } catch (const std::exception&) {
                }
            }
        }
        auto it = templates.find(definition);
        return it == templates.end() ? nullptr : &it->second;
    };
    for (const auto& s : lot.objects) {
        const LotObjects::State* was = before.state(s.id);
        const bool known = was && before.guidOf(s.id) == lot.guidOf(s.id);
        const uint32_t instance = uint32_t(uint16_t(s.id));
        auto it = scene.find(instance);
        if (known) {
            if (!moved(s, *was) || it == scene.end())
                continue;
            auto bytes = source.read(*it->second);
            const auto l = sceneLayout(bytes);
            if (!l)
                continue;
            const auto old = readPose(bytes, l->first);
            writePose(bytes, *l, poseOf(world, s, was, &old));
            addRecord(kTypeSceneObject, it->second->tgi.group, instance, std::move(bytes));
            ++out.sceneMoved;
            continue;
        }
        // A new object: only the lead part of a multi-tile object (or a
        // single-tile one, or a Sim) has a cObject naming its definition.
        const Object* o = vm.object(s.id);
        if (!o || !o->definition || (s.kind == LotObjects::Kind::MultiTile && multiTileGroup(*o) != o->id))
            continue;
        const std::string name = o->definition->fileName + "_objt";
        const std::vector<uint8_t>* t = templateOf(name);
        std::vector<uint8_t> bytes;
        if (t) {
            bytes = *t;
        } else {
            // No lot has one (an object bought in Buy mode): a bare cObject
            // as the game writes for objects without an animatable part
            // (364 of N001_Lot11's 834, "Social - Talk - Chat_objt"): no
            // name, class cObject version 0x11, the model with no design
            // choices, the pose, 1, the definition, the pose again, 0.
            // Inferred: the cAnimatable data of animated objects is left out.
            std::string model;
            if (ResourceIndex* index = vm.code().index())
                model = objectCresName(*index, Tgi{kTypeObjd, o->group, o->definitionInstance, 0});
            if (model.size() > 5 && model.compare(model.size() - 5, 5, "_cres") == 0)
                model.resize(model.size() - 5);
            BinWriter w;
            w.raw(std::vector<uint8_t>(kFileNameSize, 0));
            w.u32(kTypeSceneObject);
            w.u32(0x11);
            w.str7("cObject");
            w.str7(model);
            w.u32(model.empty() ? 0 : 1);
            if (!model.empty()) {
                w.str7(model);
                w.u32(12);
                w.u32(0);
            }
            for (int k = 0; k < 6; ++k)
                w.f32(0);
            w.f32(1);
            w.u32(1);
            w.str7(name);
            for (int k = 0; k < 6; ++k)
                w.f32(0);
            w.f32(1);
            w.u32(0);
            bytes = std::move(w.bytes());
            out.warnings.push_back("object " + std::to_string(s.id) + ": no cObject of \"" + name +
                                   "\" in the other lots to copy; wrote a bare one");
        }
        const auto l = sceneLayout(bytes);
        writePose(bytes, *l, poseOf(world, s, nullptr, nullptr));
        addRecord(kTypeSceneObject, 0xFFFFFFFF, instance, std::move(bytes));
        ++out.sceneNew;
    }

    // Routing states of new Sims: idle, as the lot's own idle ones (else the
    // base game's version 4 with nothing set).
    std::vector<uint8_t> idle;
    for (const auto& [_, e] : routing) {
        auto bytes = source.read(*e);
        // Idle: after the class name, only zeros.
        BinReader r(bytes);
        try {
            r.skip(kFileNameSize + 8);
            r.str7();
            bool zeros = r.remaining() > 0;
            for (size_t i = r.pos(); i < bytes.size(); ++i)
                zeros = zeros && bytes[i] == 0;
            if (zeros) {
                idle = std::move(bytes);
                break;
            }
        } catch (const FormatError&) {
        }
    }
    if (idle.empty()) {
        BinWriter w;
        w.raw(std::vector<uint8_t>(kFileNameSize, 0));
        w.u32(kTypeRouting);
        w.u32(4);
        w.str7("cTSRoutingObjectAggregate");
        w.raw(std::vector<uint8_t>(12, 0));
        idle = std::move(w.bytes());
    }
    for (const auto& s : lot.objects)
        if (s.kind == LotObjects::Kind::Person && !routing.count(uint32_t(uint16_t(s.id)))) {
            addRecord(kTypeRouting, 0xFFFFFFFF, uint32_t(uint16_t(s.id)), idle);
            ++out.routing;
        }

    // Relationships: the lot's SRELs become exactly the VM's relationships
    // with a lot object on one side at least, keyed as loadLot keys them
    // (two values: ownership, "to" a neighbor ID; otherwise object IDs on
    // both sides, a Sim by its neighbor ID). Unchanged records stay as
    // they are; records whose relationship the VM no longer has go.
    using Key = std::pair<VM::RelationshipKey, VM::RelationshipKey>;
    auto isSim = [&](int16_t id) {
        const Object* o = vm.object(id);
        return o && o->person;
    };
    auto trimmed = [](std::vector<int32_t> v) {
        while (!v.empty() && v.back() == 0)
            v.pop_back();
        return v;
    };
    std::map<Key, std::vector<uint32_t>> instancesOf;
    std::map<uint32_t, hood::Relationship> read;
    for (const auto& [instance, e] : relationships) {
        try {
            auto rel = hood::Relationship::parse(source.read(*e), instance);
            const auto from = int16_t(rel.from), to = int16_t(rel.to);
            Key k;
            if (rel.values.size() == 2) {
                k = {{false, from}, {true, to}};
            } else {
                if (isSim(from) && isSim(to))
                    continue; // the neighborhood's
                k = {{false, from}, {false, to}};
            }
            instancesOf[k].push_back(instance);
            read.emplace(instance, std::move(rel));
        } catch (const std::exception&) {
        }
    }
    auto objectIdOf = [&](const VM::RelationshipKey& k) -> int16_t {
        if (!k.neighbor)
            return k.id;
        const Object* sim = vm.neighborSim(k.id);
        return sim ? int16_t(sim->id) : int16_t(0);
    };
    std::set<uint32_t> kept;
    // First the relationships the lot had (their records), then new ones.
    for (int pass = 0; pass < 2; ++pass)
    for (const auto& [k, v16] : vm.relationships) {
        if (k.first.neighbor && k.second.neighbor)
            continue;
        std::vector<int32_t> values(v16.begin(), v16.end());
        if (trimmed(values).empty())
            continue;
        auto it = instancesOf.find(k);
        if ((it != instancesOf.end()) != (pass == 0))
            continue;
        if (it != instancesOf.end()) {
            for (uint32_t instance : it->second) {
                kept.insert(instance);
                const auto& rel = read.at(instance);
                std::vector<int32_t> out32;
                for (size_t i = 0; i < values.size(); ++i)
                    out32.push_back(i < rel.values.size() && int16_t(rel.values[i]) == values[i] ? rel.values[i]
                                                                                              : values[i]);
                // The VM keeps at most 32 variables: the rest stay.
                for (size_t i = out32.size(); i < rel.values.size() && values.size() >= kMaxRelationshipVars; ++i)
                    out32.push_back(rel.values[i]);
                // As many as were saved while only zeros differ; the count
                // tells ownership (2) from the rest.
                if (trimmed(out32) == trimmed(rel.values))
                    continue;
                const bool ownership = rel.values.size() == 2;
                if (ownership)
                    out32.resize(2, 0);
                else if (out32.size() == 2)
                    out32.push_back(0);
                addRecord(hoodtype::SREL, relationships.at(instance)->tgi.group, instance,
                          writeRelationship(rel.unknown, out32, rel.stale));
                ++out.relationshipsChanged;
            }
            continue;
        }
        // Sim-to-Sim ones keyed by object (a Sim with no neighbor ID)
        // would read back as the neighborhood's: not saved here.
        if (!k.first.neighbor && !k.second.neighbor && isSim(k.first.id) && isSim(k.second.id))
            continue;
        const int16_t from = objectIdOf(k.first);
        const bool ownership = k.second.neighbor;
        const int16_t to = ownership ? k.second.id : objectIdOf(k.second);
        if (from <= 0 || to <= 0)
            continue;
        if (ownership)
            values.resize(2, 0);
        else if (values.size() == 2)
            values.push_back(0);
        const uint32_t instance = uint32_t(uint16_t(from)) << 16 | uint16_t(to);
        if (kept.count(instance)) {
            out.warnings.push_back("relationship " + std::to_string(from) + " -> " + std::to_string(to) +
                                   ": an object one and an ownership one share the record; one kept");
            continue;
        }
        kept.insert(instance);
        addRecord(hoodtype::SREL, 0xFFFFFFFF, instance, writeRelationship(2, values, {}));
        ++out.relationshipsNew;
    }
    // (Records of only zeros mean what no record means: they stay.)
    for (const auto& [instance, rel] : read)
        if (!kept.count(instance) && !trimmed(rel.values).empty()) {
            lot.deletedRecords.push_back({hoodtype::SREL, relationships.at(instance)->tgi.group, instance, {}});
            ++out.relationshipsGone;
        }
    return out;
}

} // namespace os2::vm
