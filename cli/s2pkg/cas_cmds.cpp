// s2pkg cas-catalog: the Create-a-Sim catalog (formats/cas_catalog.hpp) for
// one age and gender.

#include "cas_cmds.hpp"

#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "formats/character_write.hpp"
#include "formats/dbpf.hpp"
#include "formats/gmdc.hpp"
#include "formats/lxnr.hpp"
#include "formats/property_set.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_character.hpp"
#include "vm/family_write.hpp"
#include "vm/genetics.hpp"

#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace os2 {

namespace {

uint32_t parseAge(const std::string& s) {
    static const std::map<std::string, uint32_t> names = {
        {"baby", cas::age::Baby},   {"toddler", cas::age::Toddler}, {"child", cas::age::Child},
        {"teen", cas::age::Teen},   {"adult", cas::age::Adult},     {"elder", cas::age::Elder},
    };
    if (auto it = names.find(s); it != names.end())
        return it->second;
    throw std::runtime_error("unknown age " + s + " (baby, toddler, child, teen, adult, elder)");
}

uint32_t parseGender(const std::string& s) {
    if (s == "f" || s == "female")
        return cas::gender::Female;
    if (s == "m" || s == "male")
        return cas::gender::Male;
    throw std::runtime_error("unknown gender " + s + " (f, m)");
}

const char* overlayName(uint32_t subtype) {
    static const char* names[] = {"facial hair", "brows", "lipstick", "eyes", "costume makeup", "mask", "blush",
                                  "eye shadow/liner"};
    return subtype < 8 ? names[subtype] : "?";
}

int cmdCasCatalog(const std::string& res, uint32_t ageBit, uint32_t genderBit, const std::string& only) {
    ResourceIndex index(res);
    const cas::CasCatalog c = cas::CasCatalog::load(index);
    std::printf("skin catalog: %zu skin tones, %zu hair tones, %zu archetypes, %zu modifiers, %zu regions, "
                "%zu neutral faces, %zu texture overlays, %zu mesh overlays, %zu outfit parts\n",
                c.skinTones.size(), c.hairTones.size(), c.archetypes.size(), c.modifiers.size(), c.regions.size(),
                c.neutrals.size(), c.textureOverlays.size(), c.meshOverlays.size(), c.parts.size());
    std::printf("for %s %s (%s):\n", cas::ageName(ageBit), genderBit == cas::gender::Female ? "female" : "male",
                cas::agePrefix(ageBit, genderBit).c_str());
    const auto want = [&](const char* k) { return only.empty() || only == k; };
    if (want("tones")) {
        std::printf("skin tones:\n");
        for (const auto& e : c.skinTones)
            std::printf("  %-14s s%d genetic %.2f\n", e.name().c_str(), cas::toneNumber(cas::family(e)), cas::genetic(e));
        std::printf("hair tones:\n");
        for (const auto& e : c.hairTones)
            std::printf("  %-14s %d genetic %.2f\n", e.name().c_str(), cas::toneNumber(cas::family(e)), cas::genetic(e));
    }
    if (want("face")) {
        if (const SimPart* n = c.neutralFor(ageBit, genderBit))
            std::printf("neutral face: %s\n", n->name().c_str());
        const auto arch = c.archetypesFor(ageBit, genderBit);
        std::printf("archetypes (%zu):", arch.size());
        for (const auto* e : arch)
            std::printf(" %s", e->name().c_str());
        std::printf("\n");
        const auto mods = c.modifiersFor(ageBit, genderBit);
        std::printf("modifiers (%zu):\n", mods.size());
        for (const auto* e : mods)
            std::printf("  %-32s group %u  %+.2f..%+.2f  scale %.2f/%.2f  vs %s\n", e->name().c_str(),
                        e->number("groupindex"), cas::floatProperty(*e, "negativelimit"),
                        cas::floatProperty(*e, "positivelimit"), cas::floatProperty(*e, "negativescale"),
                        cas::floatProperty(*e, "positivescale"),
                        cas::agePrefix(e->number("compareage"), e->number("comparegender"), false).c_str());
        std::printf("regions:");
        for (const auto& e : c.regions)
            std::printf(" %s", e.name().c_str());
        std::printf("\n");
    }
    if (want("overlays")) {
        for (uint32_t s = 0; s < 8; ++s) {
            const auto list = c.overlaysFor(s, ageBit, genderBit);
            std::printf("%s (%zu):", overlayName(s), list.size());
            for (const auto* e : list)
                std::printf(" %s", e->name().c_str());
            std::printf("\n");
        }
        const auto mesh = c.meshOverlaysFor(ageBit, genderBit);
        std::printf("mesh overlays (%zu):", mesh.size());
        for (const auto* e : mesh)
            std::printf(" %s", e->name().c_str());
        std::printf("\n");
    }
    if (want("hair")) {
        const auto hair = c.hairStyles(ageBit, genderBit, cas::toneFamily(2));
        std::printf("hair styles in brown (%zu):", hair.size());
        for (const auto* e : hair)
            std::printf(" %s", e->name().c_str());
        std::printf("\n");
    }
    if (want("clothes")) {
        const std::pair<uint32_t, const char*> cats[] = {
            {cas::category::Everyday, "everyday"}, {cas::category::Formal, "formal"},
            {cas::category::Underwear, "underwear"}, {cas::category::Pajamas, "pajamas"},
            {cas::category::Swimwear, "swimwear"}, {cas::category::Athletic, "athletic"},
            {cas::category::Naked, "naked"},       {cas::category::Maternity, "maternity"},
        };
        const std::pair<uint32_t, const char*> partsList[] = {
            {cas::part::Top, "tops"}, {cas::part::Bottom, "bottoms"}, {cas::part::Body, "outfits"}};
        for (const auto& [cat, cname] : cats)
            for (const auto& [p, pname] : partsList) {
                const auto list = c.outfitParts(p, ageBit, genderBit, cat, false, cat != cas::category::Naked);
                if (list.empty())
                    continue;
                std::printf("%s %s (%zu):", cname, pname, list.size());
                for (size_t i = 0; i < list.size() && i < 12; ++i)
                    std::printf(" %s", list[i]->name().c_str());
                std::printf("%s\n", list.size() > 12 ? " ..." : "");
            }
    }
    return 0;
}

// A catalog entry by name or family, case-insensitive.
const SimPart* entryNamed(const std::vector<SimPart>& list, const std::string& key) {
    std::string k = key;
    for (auto& ch : k)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (const auto& e : list) {
        std::string n = e.name();
        for (auto& ch : n)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (n == k || cas::family(e) == k)
            return &e;
    }
    return nullptr;
}

int cmdCasMake(int argc, char** argv) {
    const std::string res = argv[2];
    const std::filesystem::path out = argv[3];
    ResourceIndex index(res);
    const cas::CasCatalog catalog = cas::CasCatalog::load(index);
    cas::CharacterSpec spec;
    spec.firstName = "Novo";
    spec.lastName = "Sim";
    std::string objectName = out.stem().string();
    uint32_t seed = 0;
    std::string hoodSource, hoodOut, familyName;
    std::vector<std::string> members; // --member first:age:gender:tie (more Sims of the family)
    std::vector<std::pair<std::string, std::string>> deferred; // options that need the age and gender
    for (int i = 4; i + 1 < argc; i += 2) {
        const std::string a = argv[i], v = argv[i + 1];
        if (a == "--age") spec.age = parseAge(v);
        else if (a == "--gender") spec.gender = parseGender(v);
        else if (a == "--skin") spec.skinTone = std::stoi(v);
        else if (a == "--hair-tone") spec.hairTone = std::stoi(v);
        else if (a == "--first") spec.firstName = v;
        else if (a == "--last") spec.lastName = v;
        else if (a == "--object") objectName = v;
        else if (a == "--seed") seed = uint32_t(std::stoul(v));
        else if (a == "--guid") spec.guid = uint32_t(std::stoul(v, nullptr, 16));
        else if (a == "--hood") hoodSource = v;
        else if (a == "--hood-out") hoodOut = v;
        else if (a == "--family") familyName = v;
        else if (a == "--member") members.push_back(v);
        else deferred.emplace_back(a, v);
    }
    for (const auto& [a, v] : deferred) {
        auto& everyday = spec.outfits[cas::category::Everyday];
        if (a == "--hair") everyday[cas::part::Hair] = v;
        else if (a == "--top") everyday[cas::part::Top] = v;
        else if (a == "--bottom") everyday[cas::part::Bottom] = v;
        else if (a == "--body") everyday[cas::part::Body] = v;
        else if (a == "--formal") spec.outfits[cas::category::Formal][cas::part::Body] = v;
        else if (a == "--archetype") {
            const SimPart* e = entryNamed(catalog.archetypes, v);
            if (!e) throw std::runtime_error("no archetype " + v);
            spec.face.archetype = cas::family(*e);
        } else if (a == "--mod") {
            const auto eq = v.rfind('=');
            if (eq == std::string::npos) throw std::runtime_error("--mod name=weight");
            const SimPart* e = entryNamed(catalog.modifiers, v.substr(0, eq));
            if (!e) throw std::runtime_error("no modifier " + v.substr(0, eq));
            spec.face.modifiers[cas::family(*e)] = std::stof(v.substr(eq + 1));
        } else if (a == "--eyes") {
            const SimPart* e = entryNamed(catalog.textureOverlays, v);
            if (!e) throw std::runtime_error("no eye overlay " + v);
            spec.eyes = cas::family(*e);
        } else if (a == "--overlay") {
            const SimPart* e = entryNamed(catalog.textureOverlays, v);
            if (!e) throw std::runtime_error("no overlay " + v);
            spec.overlays.push_back(cas::family(*e));
        } else {
            throw std::runtime_error("unknown option " + a);
        }
    }
    cas::completeSpec(catalog, spec, seed);
    cas::FaceBaker baker(index, catalog);
    if (!hoodSource.empty()) {
        // A one-Sim family added to a copy of the neighborhood (vm/family_write.hpp).
        if (hoodOut.empty())
            throw std::runtime_error("--hood needs --hood-out <folder>");
        // The first Sim, then each --member first:age:gender:tie, tie to the
        // first Sim: spouse, child (of the first and its spouse), sibling,
        // roommate (family_write.hpp: 2 married, 3 sibling, 4/0/1 child/parent).
        std::vector<cas::CharacterSpec> specs{spec};
        std::vector<std::string> ties{""};
        for (const std::string& m : members) {
            std::vector<std::string> f;
            for (size_t a = 0, b; a <= m.size(); a = b + 1) {
                b = m.find(':', a);
                if (b == std::string::npos)
                    b = m.size();
                f.push_back(m.substr(a, b - a));
            }
            if (f.size() != 4)
                throw std::runtime_error("--member first:age:gender:tie");
            cas::CharacterSpec o;
            o.firstName = f[0];
            o.lastName = spec.lastName;
            o.age = parseAge(f[1]);
            o.gender = parseGender(f[2]);
            o.skinTone = spec.skinTone;
            cas::completeSpec(catalog, o, (seed ? seed : 1) + uint32_t(specs.size()));
            specs.push_back(o);
            ties.push_back(f[3]);
        }
        std::vector<vm::NewSim> sims;
        uint32_t s = seed ? seed : 1;
        for (size_t k = 0; k < specs.size(); ++k) {
            const cas::CharacterSpec& sp = specs[k];
            vm::NewSim sim;
            sim.firstName = sp.firstName;
            sim.lastName = sp.lastName;
            sim.ageBit = sp.age;
            sim.genderBit = sp.gender;
            sim.fitness = sp.fitness;
            sim.zodiac = cas::zodiacSign(sim.personality);
            sim.aspiration = sp.age <= cas::age::Child ? cas::aspiration::GrowUp : cas::suggestedAspiration(sim.zodiac);
            sim.dna = genetics::dnaFromLook(cas::toneFamily(sp.hairTone), cas::toneFamily(sp.skinTone), sp.eyes,
                                            sp.gender == cas::gender::Female,
                                            [&s](uint32_t n) { s = s * 1103515245u + 12345u; return n ? (s >> 8) % n : 0u; })
                          .toProperties();
            sim.clothing = cas::familyBinClothing(catalog, sp); // the family's clothing bin
            sim.writeCharacter = [&index, &catalog, &baker, &specs, k](const std::string& name, const std::filesystem::path& file) {
                return cas::writeCharacter(index, catalog, baker, specs[k], name, file).guid;
            };
            sims.push_back(std::move(sim));
        }
        size_t spouse = 0;
        for (size_t k = 1; k < sims.size(); ++k)
            if (ties[k] == "spouse") {
                sims[0].ties.push_back({k, 2});
                sims[k].ties.push_back({0, 2});
                spouse = k;
            }
        for (size_t k = 1; k < sims.size(); ++k) {
            if (ties[k] == "child") {
                sims[k].ties.push_back({0, 0});
                sims[0].ties.push_back({k, 4});
                if (spouse) {
                    sims[k].ties.push_back({spouse, 1});
                    sims[spouse].ties.push_back({k, 4});
                }
            } else if (ties[k] == "sibling") {
                sims[k].ties.push_back({0, 3});
                sims[0].ties.push_back({k, 3});
            }
        }
        const auto fr = vm::writeNewFamily(hoodSource, std::filesystem::path(hoodOut) / std::filesystem::path(hoodSource).filename(),
                                           familyName.empty() ? spec.lastName : familyName, "", std::move(sims), seed ? seed : 1);
        std::printf("family %u written to %s:", fr.family, fr.neighborhood.string().c_str());
        for (size_t k = 0; k < fr.neighbors.size(); ++k)
            std::printf(" neighbor %u (%s, %s)", unsigned(fr.neighbors[k]), specs[k].firstName.c_str(),
                        fr.characters[k].filename().string().c_str());
        std::printf("\n");
        return 0;
    }
    const auto r = cas::writeCharacter(index, catalog, baker, spec, objectName, out);
    std::printf("wrote %s: %zu resources, GUID 0x%08X, %s %s %s, skin s%d, hair tone %d\n", out.string().c_str(),
                r.resources, r.guid, spec.firstName.c_str(), spec.lastName.c_str(), cas::ageName(spec.age), spec.skinTone,
                spec.hairTone);
    for (const auto& [cat, parts] : spec.outfits) {
        std::printf("  outfit 0x%03X:", cat);
        for (const auto& [bit, name] : parts)
            std::printf(" %s", name.c_str());
        std::printf("\n");
    }
    for (const auto& w : r.warnings)
        std::printf("  warning: %s\n", w.c_str());
    return 0;
}

// The character's LxNR: its keys, and the face baked from it at the
// character's age against the character's own face meshes (by texture
// coordinate).
int cmdCasLxnr(const char* resDir, const char* file) {
    ResourceIndex index(resDir);
    DbpfPackage pkg(file);
    const DbpfEntry* lx = nullptr;
    for (const auto& e : pkg.entries())
        if (e.tgi.type == cas::kTypeLxnr)
            lx = &e;
    if (!lx) {
        std::printf("no LxNR\n");
        return 1;
    }
    const auto bytes = pkg.read(*lx);
    auto fs = std::make_shared<cas::FaceStructure>(cas::FaceStructure::parse(bytes));
    std::printf("LxNR version %u, %zu keys:", fs->version, fs->keys.size());
    for (uint32_t k : fs->keys)
        std::printf(" %x(%zu)", k, fs->data[k].size());
    std::printf("\n");
    const auto again = fs->write();
    const auto re = cas::FaceStructure::parse(again);
    std::printf("rewritten: %zu bytes (stored %zu), %s\n", again.size(), bytes.size(),
                re.data == fs->data ? "same data" : "DIFFERENT");
    const Character ch = readCharacter(pkg, index);
    std::printf("character: age %u gender %u\n", ch.age, ch.gender);
    const auto catalog = cas::CasCatalog::load(index);
    cas::FaceBaker baker(index, catalog);
    for (bool low : {false, true}) {
        cas::FaceSpec spec;
        spec.structure = fs;
        cas::FaceBaker::Baked b;
        try {
            b = baker.bake(spec, ch.age, ch.gender, low);
        } catch (const std::exception& e) {
            std::printf("bake: %s\n", e.what());
            continue;
        }
        if (const auto* sv = fs->find(ch.age, ch.gender, low)) {
            size_t same = 0;
            for (size_t i = 0; i < sv->size() && i < b.structure.size(); ++i)
                same += std::fabs((*sv)[i].u - b.structure[i].u) < 1e-4f && std::fabs((*sv)[i].v - b.structure[i].v) < 1e-4f;
            std::printf("%s detail: stored %zu corners, neutral mesh %zu, same uv in order %zu\n", low ? "low" : "high",
                        sv->size(), b.structure.size(), same);
        }
        Rcol brc(b.gmdc);
        const auto mine = buildMeshes(GeometryData::parse(brc.find("cGeometryDataContainer")->body));
        // Each GMDC of the character file: the mean distance per matched texture coordinate.
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type != 0xAC4F8687)
                continue;
            try {
                const auto gb = pkg.read(e);
                Rcol rc(gb);
                const auto* blk = rc.find("cGeometryDataContainer");
                if (!blk)
                    continue;
                const auto theirs = buildMeshes(GeometryData::parse(blk->body));
                std::multimap<std::pair<long, long>, std::array<float, 3>> byUv;
                size_t n = 0;
                for (const auto& g : theirs)
                    for (size_t i = 0; i < g.positions.size() && i < g.uvs.size(); ++i)
                        byUv.emplace(std::make_pair(std::lround(g.uvs[i][0] * 4096), std::lround(g.uvs[i][1] * 4096)), g.positions[i]), ++n;
                double sum = 0, worst = 0;
                size_t matched = 0;
                for (const auto& g : mine)
                    for (size_t i = 0; i < g.positions.size() && i < g.uvs.size(); ++i) {
                        auto [lo, hi] = byUv.equal_range({std::lround(g.uvs[i][0] * 4096), std::lround(g.uvs[i][1] * 4096)});
                        double best = -1;
                        for (auto it = lo; it != hi; ++it) {
                            double d = 0;
                            for (int c = 0; c < 3; ++c)
                                d += double(it->second[c] - g.positions[i][c]) * double(it->second[c] - g.positions[i][c]);
                            if (best < 0 || d < best)
                                best = d;
                        }
                        if (best >= 0) {
                            sum += std::sqrt(best), worst = std::max(worst, std::sqrt(best)), ++matched;
                        }
                    }
                if (matched)
                    std::printf("%s detail vs GMDC %08x (%zu vertices): %zu matched, mean %.6f, worst %.6f\n",
                                low ? "low" : "high", e.tgi.instance, n, matched, sum / double(matched), worst);
            } catch (const std::exception&) {
            }
        }
        for (const auto& w : b.warnings)
            std::printf("  warning: %s\n", w.c_str());
    }
    return 0;
}

} // namespace

const char* casUsage() {
    return "       s2pkg cas-catalog <Res-dir> [--age adult] [--gender f|m] [tones|face|overlays|hair|clothes]\n"
           "       s2pkg cas-make <Res-dir> <out.package> [--age adult] [--gender f|m] [--skin 1-4] [--hair-tone 1-5]\n"
           "              [--hair|--top|--bottom|--body|--formal part] [--archetype name] [--mod name=weight]...\n"
           "              [--eyes overlay] [--overlay overlay]... [--first name] [--last name] [--object N001_User00100] [--seed N]\n"
           "              [--hood <neighborhood dir> --hood-out <dir> [--family name] [--member first:age:gender:spouse|child|sibling|roommate]...]  (adds the family to a copy)\n"
           "       s2pkg cas-lxnr <Res-dir> <character.package>  (the LxNR face structure, checked against the face meshes)\n"
           "       s2pkg cas-age <Res-dir> <character.package> <age> <out.package>  (grown up as the Age primitive does)\n";
}

int casCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "cas-catalog" && argc >= 3) {
        uint32_t ageBit = cas::age::Adult, genderBit = cas::gender::Female;
        std::string only;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--age" && i + 1 < argc)
                ageBit = parseAge(argv[++i]);
            else if (a == "--gender" && i + 1 < argc)
                genderBit = parseGender(argv[++i]);
            else
                only = a;
        }
        return cmdCasCatalog(argv[2], ageBit, genderBit, only);
    }
    if (cmd == "cas-lxnr" && argc >= 4)
        return cmdCasLxnr(argv[2], argv[3]);
    if (cmd == "cas-make" && argc >= 4)
        return cmdCasMake(argc, argv);
    if (cmd == "cas-age" && argc >= 6) {
        // A character grown into another age as the Age primitive does it
        // (cas::writeAgedCharacter): the new age's face from its LxNR.
        ResourceIndex index(argv[2]);
        const cas::CasCatalog catalog = cas::CasCatalog::load(index);
        cas::FaceBaker baker(index, catalog);
        const auto r = cas::writeAgedCharacter(index, catalog, baker, argv[3], parseAge(argv[4]), argv[5]);
        std::printf("wrote %s: %zu resources, GUID 0x%08X\n", argv[5], r.resources, r.guid);
        for (const auto& w : r.warnings)
            std::printf("  warning: %s\n", w.c_str());
        return 0;
    }
    return -1;
}

} // namespace os2
