#include "baby_character.hpp"

#include "genetics.hpp"
#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "formats/character_write.hpp"
#include "formats/dbpf.hpp"
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
            if (it == st->byGuid.end())
                throw std::runtime_error("no character file for neighbor GUID " + hexString(n.guid));
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
            // The next character number of the neighborhood (either folder).
            std::string prefix = "N001";
            int next = 0;
            const std::regex re("([A-Za-z0-9]+)_User([0-9]+)\\.package", std::regex::icase);
            for (const fs::path& dir : {st->chars, st->out})
                for (const auto& e : fs::directory_iterator(dir, ec)) {
                    std::smatch m;
                    const std::string name = e.path().filename().string();
                    if (std::regex_match(name, m, re)) {
                        prefix = m[1];
                        next = std::max(next, std::stoi(m[2]) + 1);
                    }
                }
            fs::create_directories(st->out);
            char stem[64];
            std::snprintf(stem, sizeof stem, "%s_User%05d", prefix.c_str(), next);
            const fs::path file = st->out / (std::string(stem) + ".package");
            cas::writeCharacter(*st->index, *st->catalog, *st->baker, s, stem, file, true);
            st->byGuid[baby->guid] = file;
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
}

} // namespace os2::vm
