// s2pkg cas-catalog: the Create-a-Sim catalog (formats/cas_catalog.hpp) for
// one age and gender.

#include "cas_cmds.hpp"

#include "formats/cas_catalog.hpp"
#include "formats/resindex.hpp"

#include <cstdio>
#include <cstring>
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

} // namespace

const char* casUsage() {
    return "       s2pkg cas-catalog <Res-dir> [--age adult] [--gender f|m] [tones|face|overlays|hair|clothes]\n";
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
    return -1;
}

} // namespace os2
