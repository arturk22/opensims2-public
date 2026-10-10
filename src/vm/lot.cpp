#include "lot.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"

#include <map>

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
    return out;
}

} // namespace os2::vm
