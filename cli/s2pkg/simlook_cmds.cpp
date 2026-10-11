// sim-model: the parts a character file assembles into (meshes, materials,
// morphs), to work out the render details of Sims.
#include "simlook_cmds.hpp"

#include "formats/resindex.hpp"
#include "formats/sim.hpp"
#include "formats/sim_character.hpp"
#include "formats/skinning.hpp"
#include "formats/txmt.hpp"

#include <cstdio>
#include <set>
#include <string>

namespace os2 {

namespace {

int cmdSimModel(const std::string& res, const std::string& character, uint32_t category) {
    ResourceIndex index(res);
    const auto parts = loadSimParts(index);
    const SkinnedModel sim = loadCharacterSim(index, parts, character, category);
    for (const auto& w : sim.model.warnings)
        std::printf("warning: %s\n", w.c_str());
    for (const auto& p : sim.model.parts) {
        std::printf("%-28s %-40s %5zu verts", p.mesh.name.c_str(), p.material.c_str(), p.mesh.positions.size());
        if (!p.mesh.morphNames.empty()) {
            std::printf(" morphs:");
            for (const auto& m : p.mesh.morphNames)
                std::printf(" %s", m.c_str());
        }
        std::printf("\n");
        auto it = sim.model.materials.find(p.material);
        if (it == sim.model.materials.end())
            continue;
        std::printf("    %s", it->second.materialType.c_str());
        for (const auto& [k, v] : it->second.properties)
            if (k.find("Texture") != std::string::npos || k.find("Blend") != std::string::npos ||
                k.find("Alpha") != std::string::npos || k.find("Layer") != std::string::npos ||
                k.find("Normal") != std::string::npos)
                std::printf(" %s=%s", k.c_str(), v.c_str());
        std::printf("\n");
    }
    return 0;
}

} // namespace

int simLookCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "sim-model" && (argc == 4 || argc == 5))
        return cmdSimModel(argv[2], argv[3], argc == 5 ? uint32_t(std::stoul(argv[4], nullptr, 16)) : 1);
    return -1;
}

const char* simLookUsage() { return "       s2pkg sim-model <Res-dir> <character.package> [category-hex]\n"; }

} // namespace os2
