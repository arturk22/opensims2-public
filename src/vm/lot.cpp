#include "lot.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"

#include <cmath>
#include <map>
#include <set>

namespace os2::vm {

namespace {

// Groups given to the code of a package loaded on its own (a Sim's character
// file): 0xFE000000 | CRC24 of the file name, out of the way of the game's
// object (0x7F......) and scenegraph groups.
uint32_t characterGroup(const std::string& stem) { return 0xFE000000 | (nameInstanceHash(stem) & 0x00FFFFFF); }

const std::string kObjtSuffix = "_objt";

} // namespace

std::filesystem::path charactersDirOf(const std::filesystem::path& lotPackage) {
    auto dir = std::filesystem::absolute(lotPackage).parent_path();
    if (dir.filename() != "Lots")
        return {};
    auto characters = dir.parent_path() / "Characters";
    return std::filesystem::is_directory(characters) ? characters : std::filesystem::path{};
}

LotLoad loadLot(VM& vm, const Lot& lot, const LotObjects& saved, const std::filesystem::path& charactersDir) {
    CodeLibrary& code = vm.code();
    std::map<uint32_t, const Lot::Object*> cObjects;
    for (const auto& o : lot.objects)
        cObjects[o.instance] = &o;

    // Rooms: their level (as the saved objects number levels: the arrays'
    // first level is 1) and area in tiles (a quarter tile counts a quarter;
    // inferred). Light, outside, pool and windows are not known.
    if (!lot.rooms.empty()) {
        std::map<int16_t, std::pair<int, size_t>> rooms; // level, quarter tiles
        for (int li = 0; li < lot.levelCount; ++li)
            for (uint32_t x = 0; x < lot.width; ++x)
                for (uint32_t y = 0; y < lot.height; ++y)
                    for (uint16_t room : lot.rooms[(size_t(li) * lot.width + x) * lot.height + y])
                        if (room) {
                            auto& r = rooms[int16_t(room)];
                            r.first = li + 1;
                            ++r.second;
                        }
        // Outside and pool (inferred): a room is a pool when one of its tiles
        // is in a POOL's rectangle on its level, and outside when it is a pool
        // or reaches the edge of the lot on the ground (the terrain's room).
        std::set<int16_t> outside, pool;
        for (int li = 0; li < lot.levelCount; ++li)
            for (uint32_t x = 0; x < lot.width; ++x)
                for (uint32_t y = 0; y < lot.height; ++y)
                    for (uint16_t room : lot.rooms[(size_t(li) * lot.width + x) * lot.height + y]) {
                        if (!room)
                            continue;
                        const int level = li + lot.minLevel;
                        if (level == 0 && (x == 0 || y == 0 || x + 1 == lot.width || y + 1 == lot.height))
                            outside.insert(int16_t(room));
                        for (const auto& p : lot.pools)
                            if (p.level == level && float(x) >= p.x && float(x) < p.x + float(p.width) &&
                                float(y) >= p.y && float(y) < p.y + float(p.height))
                                pool.insert(int16_t(room)), outside.insert(int16_t(room));
                    }
        for (const auto& [id, r] : rooms) {
            VM::Room& room = vm.rooms[id];
            room.values[1] = int16_t(outside.count(id));
            room.values[2] = int16_t(r.first);
            room.values[3] = int16_t(r.second / 4);
            room.values[4] = int16_t(pool.count(id));
            room.known = 0x1E;
        }
    }

    // The world: walls, floors and rooms, with the walls opened where the
    // lot's doors (and gates) are. Doors are taken from the cObjects, whose
    // positions match the walls; some saved door states do not (in
    // N001_Lot11 all eight are elsewhere: leftovers?). The direction comes
    // from the model's rotation: 90 - 45 * direction degrees about Z.
    vm.world = World::fromLot(lot);
    for (const auto& o : lot.objects) {
        std::string name = o.definition;
        if (name.size() > kObjtSuffix.size() &&
            name.compare(name.size() - kObjtSuffix.size(), kObjtSuffix.size(), kObjtSuffix) == 0)
            name.resize(name.size() - kObjtSuffix.size());
        const auto def = name.empty() ? std::nullopt : code.definitionByName(name);
        const ObjectDefinition* d = def ? code.definition(def->first, def->second) : nullptr;
        if (!d || d->objectType() != kObjectTypeDoor)
            continue;
        const auto& q = o.rotation;
        const double yaw = std::atan2(2 * (q[3] * q[2] + q[0] * q[1]), 1 - 2 * (q[1] * q[1] + q[2] * q[2])) * 180 / M_PI;
        const int dir = ((int(std::lround((90 - yaw) / 45)) % 8) + 8) % 8;
        // The level is the one whose floor height at the door is nearest
        // the cObject's (a foundation lifts the ground floor to 0.75).
        const int x = int(std::floor(o.position[0])), y = int(std::floor(o.position[1]));
        int level = 0;
        for (int l = lot.minLevel; l < lot.minLevel + lot.levelCount; ++l)
            if (std::fabs(lot.elevation(l, x, y) - o.position[2]) < std::fabs(lot.elevation(level, x, y) - o.position[2]))
                level = l;
        vm.world->openDoor({x, y, level + 1}, dir);
    }

    LotLoad out;
    for (const auto& s : saved.objects) {
        LotLoad::Entry e;
        e.id = s.id;
        e.person = s.person;
        std::optional<std::pair<uint32_t, uint16_t>> def;
        std::string cName;
        if (auto it = cObjects.find(uint32_t(uint16_t(s.id))); it != cObjects.end()) {
            cName = it->second->definition;
            if (cName.size() > kObjtSuffix.size() &&
                cName.compare(cName.size() - kObjtSuffix.size(), kObjtSuffix.size(), kObjtSuffix) == 0)
                cName.resize(cName.size() - kObjtSuffix.size());
        }
        e.name = cName;
        try {
            if (s.person) {
                // Person states are kept for Sims that left the lot too; the
                // ones on it have a cPerson. "N001_User00011 - Don": the
                // character file is the part before " - ".
                auto c = cObjects.find(uint32_t(uint16_t(s.id)));
                if (c == cObjects.end() || !c->second->person)
                    throw VMError("person state without a Sim on the lot (no cPerson)");
                const std::string stem = cName.substr(0, cName.find(" - "));
                if (stem.empty())
                    throw VMError("Sim without a cObject naming its character file");
                if (charactersDir.empty())
                    throw VMError("no character directory for " + stem);
                const auto file = charactersDir / (stem + ".package");
                if (!std::filesystem::exists(file))
                    throw VMError("character file " + file.filename().string() + " not found");
                DbpfPackage pkg(file);
                const uint32_t group = characterGroup(stem);
                const auto definitions = code.addPackage(pkg, group);
                if (definitions.size() != 1)
                    throw VMError("character file " + file.filename().string() + " has " +
                                  std::to_string(definitions.size()) + " OBJDs");
                def = std::pair{group, definitions.front()};
            } else if (const uint32_t guid = saved.guidOf(s.id)) {
                def = code.definitionByGuid(guid);
                if (!def)
                    throw VMError("no OBJD with GUID " + hexString(guid));
            } else if (!cName.empty()) {
                def = code.definitionByName(cName);
                if (!def)
                    throw VMError("no OBJD named \"" + cName + "\"");
            } else {
                throw VMError("neither the object module nor a cObject says what object it is");
            }
            if (const ObjectDefinition* d = code.definition(def->first, def->second)) {
                // The packages keep leftovers of objects that are gone (N001_Lot11
                // has four "Piano Upright 0,0" cObjects, three of them on the
                // same tile), and a leftover may have the ID the object module
                // gives to another object: when the two disagree, it is not
                // known which one is current.
                if (!s.person && !cName.empty() && d->fileName != cName)
                    throw VMError("the object module and the cObject disagree (\"" + d->fileName + "\", \"" +
                                  cName + "\")");
                e.name = d->fileName;
            }
            Object& o = vm.instantiatePart(def->first, def->second, s.id);
            o.position = {s.x, s.y, s.level};
            for (int16_t field : {objdata::Direction, objdata::ContainerId, objdata::SlotNumber, objdata::Room})
                if (size_t(field) < s.data.size())
                    o.data[size_t(field)] = s.data[size_t(field)];
            e.created = true;
            ++out.created;
        } catch (const std::exception& ex) {
            e.error = ex.what();
        }
        out.objects.push_back(std::move(e));
    }
    // The lot's relationships, by object ID (docs/data-findings.md §7). Two
    // values: ownership, from an object to a neighbor ID (Relationship's flag
    // 0x08 reads variable 1 of these; their "to" are the owning family's
    // neighbor IDs, not the lot's objects with those IDs: a phone handset and
    // two controllers in N001_Lot11; inferred from the count). The rest name
    // lot objects on both sides, Sims by their neighbor ID as everywhere in
    // the VM; Sim-to-Sim ones are left to the neighborhood's.
    auto key = [&](int16_t id) {
        const Object* o = vm.object(id);
        return o ? vm.relationshipKey(id) : VM::RelationshipKey{false, id};
    };
    for (const auto& r : saved.relationships) {
        std::pair<VM::RelationshipKey, VM::RelationshipKey> k;
        if (r.values.size() == 2) {
            k = {{false, r.from}, {true, r.to}};
        } else {
            k = {key(r.from), key(r.to)};
            if (k.first.neighbor && k.second.neighbor)
                continue;
        }
        auto& values = vm.relationships[k];
        values.clear();
        for (int32_t v : r.values)
            values.push_back(int16_t(v));
        if (values.size() > kMaxRelationshipVars)
            values.resize(kMaxRelationshipVars);
        ++out.relationships;
    }
    return out;
}

} // namespace os2::vm
