#include "baby_character.hpp"

#include "genetics.hpp"
#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "formats/character_write.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"
#include "formats/simantics.hpp"
#include "formats/binreader.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>
#include <regex>

namespace os2::vm {


namespace fs = std::filesystem;

std::function<void(VM&, const BabyRequest&)>& babyCharacterWriter() {
    static std::function<void(VM&, const BabyRequest&)> writer;
    return writer;
}

std::function<void(VM&, const NpcRequest&)>& npcCharacterWriter() {
    static std::function<void(VM&, const NpcRequest&)> writer;
    return writer;
}

namespace {

constexpr uint32_t kTypeAged = 0xAC598EAC;
constexpr const char* kNpcFolder = "NPCs";

// The next character file of the neighborhood (either folder): "<P>_User<NNNNN>".
std::string nextCharacterStem(const fs::path& a, const fs::path& b, const fs::path& c) {
    std::string prefix = "N001";
    int next = 0;
    std::error_code ec;
    const std::regex re("([A-Za-z0-9]+)_User([0-9]+)\\.package", std::regex::icase);
    for (const fs::path& dir : {a, b, c})
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::smatch m;
            const std::string name = e.path().filename().string();
            if (std::regex_match(name, m, re)) {
                prefix = m[1];
                next = std::max(next, std::stoi(m[2]) + 1);
            }
        }
    char stem[64];
    std::snprintf(stem, sizeof stem, "%s_User%05d", prefix.c_str(), next);
    return stem;
}

// A random look for a new Sim of the spec's age and gender, as the CAS
// randomize button draws it (CasMode::randomize): an archetype face, skin
// and hair tones, eyes (not the alien ones), then hair, brows and clothes
// (completeSpec with a seed).
void randomLook(const cas::CasCatalog& catalog, cas::CharacterSpec& s, std::mt19937& rng) {
    const auto arch = catalog.archetypesFor(s.age, s.gender, cas::flag::Hidden | cas::flag::NoRandom);
    if (!arch.empty())
        s.face.archetype = cas::family(*arch[rng() % arch.size()]);
    s.hairTone = int(rng() % 4) + 1;
    const auto eyes = catalog.overlaysFor(cas::overlay::Eyes, s.age, s.gender);
    if (!eyes.empty())
        s.eyes = cas::family(*eyes[rng() % std::min<size_t>(eyes.size(), 5)]);
    cas::completeSpec(catalog, s, uint32_t(rng() | 1));
}

// The in-memory OBJD Make New Character made for the neighbor takes the
// name of the character file written for it ("<stem> - <first name>", as
// writeCharacter names the file's OBJD), so the lot's cPerson names that
// file (lot_records.cpp) and the reload finds it (lot.cpp).
void nameDefinition(VM& vm, uint32_t guid, const std::string& stem, const std::string& firstName) {
    if (const auto at = vm.code().definitionByGuid(guid))
        vm.code().renameDefinition(at->first, at->second, stem + " - " + firstName);
}

} // namespace

void installBabyCharacterWriter(ResourceIndex& index, const fs::path& charactersDir, const fs::path& outDir,
                                std::map<int16_t, fs::path>* written) {
    struct State {
        ResourceIndex* index;
        fs::path chars, out;
        std::unique_ptr<cas::CasCatalog> catalog;
        std::unique_ptr<cas::FaceBaker> baker;
        std::map<uint32_t, fs::path> byGuid; // character files by OBJD GUID
        std::map<int16_t, fs::path>* written;
    };
    auto st = std::make_shared<State>();
    st->index = &index;
    st->chars = charactersDir;
    st->out = outDir;
    st->written = written;
    babyCharacterWriter() = [st](VM& vm, const BabyRequest& r) {
        if (!st->catalog) {
            st->catalog = std::make_unique<cas::CasCatalog>(cas::CasCatalog::load(*st->index));
            st->baker = std::make_unique<cas::FaceBaker>(*st->index, *st->catalog);
        }
        std::error_code ec;
        if (st->byGuid.empty())
            for (const auto& e : fs::directory_iterator(st->chars, ec))
                if (e.path().extension() == ".package")
                    try {
                        DbpfPackage pkg(e.path());
                        for (const auto& x : pkg.entries())
                            if (x.tgi.type == 0x4F424A44) {
                                st->byGuid[ObjectDefinition::parse(pkg.read(x)).guid()] = e.path();
                                break;
                            }
                    } catch (const std::exception&) {
                    }
        const Neighbor* a = vm.neighbor(r.parentA);
        const Neighbor* b = vm.neighbor(r.parentB);
        const Neighbor* baby = vm.neighbor(r.baby);
        if (!a || !b || !baby)
            return;
        auto specOf = [&](const Neighbor& n) {
            auto it = st->byGuid.find(n.guid);
            if (it == st->byGuid.end()) {
                // A unique NPC (the Pollination Technician fathers the
                // abducted Sim's baby) keeps its character in its object.
                char stem[32];
                std::snprintf(stem, sizeof stem, "Unique_%08X.package", n.guid);
                const fs::path file = st->out / kNpcFolder / stem;
                if (!fs::exists(file) && !cas::writeEmbeddedCharacter(*st->index, n.guid, file))
                    throw std::runtime_error("no character file for neighbor GUID " + hexString(n.guid));
                it = st->byGuid.emplace(n.guid, file).first;
            }
            DbpfPackage pkg(it->second);
            return cas::readCharacterSpec(pkg, 0xFFFFFFFF, *st->catalog);
        };
        try {
            const cas::CharacterSpec sa = specOf(*a), sb = specOf(*b);
            const cas::FaceStructure fa = sa.face.structure ? *sa.face.structure : st->baker->structureOf(sa.face, sa.gender);
            const cas::FaceStructure fb = sb.face.structure ? *sb.face.structure : st->baker->structureOf(sb.face, sb.gender);
            cas::CharacterSpec s;
            s.age = cas::age::Baby;
            s.gender = r.female ? cas::gender::Female : cas::gender::Male;
            s.face.structure = std::make_shared<cas::FaceStructure>(st->baker->descendant(fa, fb, r.regions));
            const os2::genetics::Dna dna = os2::genetics::Dna::fromProperties(r.dna);
            if (int t = cas::toneNumber(dna.expressed(os2::genetics::gene::Hair)))
                s.hairTone = std::clamp(t, 1, 4);
            s.skinTone = sa.skinTone;
            s.skinFamily = sa.skinFamily;
            if (int t = cas::toneNumber(dna.expressed(os2::genetics::gene::Skin)); t >= 1 && t <= 4 && sa.skinFamily.empty())
                s.skinTone = t;
            if (!dna.expressed(os2::genetics::gene::Eyes).empty())
                s.eyes = dna.expressed(os2::genetics::gene::Eyes);
            s.lastName = baby->lastName;
            s.firstName = baby->firstName;
            s.guid = baby->guid;
            cas::completeSpec(*st->catalog, s, uint32_t(r.baby) * 2654435761u | 1); // a baby outfit at random
            const std::string stem = nextCharacterStem(st->chars, st->out, st->out / kNpcFolder);
            fs::create_directories(st->out);
            const fs::path file = st->out / (std::string(stem) + ".package");
            cas::writeCharacter(*st->index, *st->catalog, *st->baker, s, stem, file, true);
            st->byGuid[baby->guid] = file;
            nameDefinition(vm, baby->guid, stem, s.firstName);
            if (st->written)
                (*st->written)[r.baby] = file;
            std::printf("baby %d: character %s (face: parents' LxNR by region", r.baby, file.filename().string().c_str());
            for (const auto& [region, parent] : r.regions)
                std::printf(" %s<%d", region.c_str(), parent);
            std::printf(")\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "baby %d: character not written: %s\n", r.baby, e.what());
        }
    };
    npcCharacterWriter() = [st](VM& vm, const NpcRequest& r) {
        const Neighbor* n = vm.neighbor(r.neighbor);
        if (!n)
            return;
        try {
            if (!st->catalog) {
                st->catalog = std::make_unique<cas::CasCatalog>(cas::CasCatalog::load(*st->index));
                st->baker = std::make_unique<cas::FaceBaker>(*st->index, *st->catalog);
            }
            cas::CharacterSpec s;
            s.age = uint32_t(r.ageFlag);
            s.gender = r.female ? cas::gender::Female : cas::gender::Male;
            s.skinTone = std::clamp(int(r.skin) + 1, 1, 4);
            // Seeded by the neighbor's GUID (itself drawn from the VM's
            // random numbers), so the VM's own draws stay as they were.
            std::mt19937 rng(n->guid * 2654435761u + uint32_t(r.neighbor));
            randomLook(*st->catalog, s, rng);
            // Its DNA from that look, as the CAS gives a new (non-baby)
            // Sim (SetFromOutfitData; the neighborhood keeps it in SDNA).
            vm.neighbors[r.neighbor].dna =
                os2::genetics::dnaFromLook(cas::toneFamily(s.hairTone), cas::toneFamily(s.skinTone), s.eyes, r.female,
                                           [&](uint32_t k) { return k ? uint32_t(rng() % k) : 0u; })
                    .toProperties();
            // The outfit object's clothes for this age and gender (one of
            // its AGEDs at random when several fit).
            std::string uniform;
            if (r.outfitGroup) {
                std::vector<std::pair<DbpfPackage*, uint32_t>> fitting;
                for (const auto& loc : st->index->ofType(kTypeAged)) {
                    if (loc.entry->tgi.group != r.outfitGroup)
                        continue;
                    try {
                        const cas::CharacterSpec o =
                            cas::readCharacterSpec(*loc.package, r.outfitGroup, *st->catalog, loc.entry->tgi.instance);
                        if ((o.age & s.age) && o.gender == s.gender)
                            fitting.push_back({loc.package, loc.entry->tgi.instance});
                    } catch (const std::exception&) {
                    }
                }
                if (!fitting.empty()) {
                    const auto& [pkg, inst] = fitting[rng() % fitting.size()];
                    const cas::CharacterSpec o = cas::readCharacterSpec(*pkg, r.outfitGroup, *st->catalog, inst);
                    for (const auto& [category, parts] : o.outfits) {
                        auto& mine = s.outfits[category];
                        // A whole outfit replaces top and bottom, and the other way round.
                        if (parts.count(cas::part::Body))
                            mine.erase(cas::part::Top), mine.erase(cas::part::Bottom);
                        if (parts.count(cas::part::Top) || parts.count(cas::part::Bottom))
                            mine.erase(cas::part::Body);
                        for (const auto& [bit, name] : parts)
                            mine[bit] = name;
                    }
                    uniform = hexString(r.outfitGroup) + ":" + std::to_string(inst);
                }
            }
            s.firstName = n->firstName;
            s.lastName = n->lastName;
            s.guid = n->guid;
            s.templateGuid = r.templateGuid;
            // In the session's NPCs folder: the save copies the <P>_User
            // files from there to the neighborhood's Characters with the
            // neighbors (app/game_save.cpp).
            const fs::path dir = st->out / kNpcFolder;
            const std::string stem = nextCharacterStem(st->chars, st->out, dir);
            fs::create_directories(dir);
            const fs::path file = dir / (stem + ".package");
            cas::writeCharacter(*st->index, *st->catalog, *st->baker, s, stem, file, true);
            nameDefinition(vm, n->guid, stem, s.firstName);
            if (st->written)
                (*st->written)[r.neighbor] = file;
            std::printf("NPC %d: character %s (template %s, %s %s, outfit %s)\n", r.neighbor, file.filename().string().c_str(),
                        hexString(r.templateGuid).c_str(), cas::ageName(s.age), r.female ? "female" : "male",
                        uniform.empty() ? "random" : uniform.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "NPC %d: character not written: %s\n", r.neighbor, e.what());
        }
    };
}

} // namespace os2::vm
