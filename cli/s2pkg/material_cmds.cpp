// s2pkg material-survey: TXMT material types and the properties each uses.
#include "material_cmds.hpp"

#include "formats/image.hpp"
#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/txmt.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace os2 {

namespace {

constexpr uint32_t kTypeTxmt = 0x49596978;

// Every TXMT under `root` (unique TGIs): count per material type, then per
// type the properties seen with how often they occur and their commonest
// values. `filter` keeps only the types containing it.
int cmdMaterialSurvey(const std::string& root, const std::string& filter, bool list = false) {
    ResourceIndex index(root);
    struct TypeStats {
        size_t count = 0;
        std::map<std::string, std::map<std::string, size_t>> props; // name -> value -> count
        std::vector<std::string> examples;
    };
    std::map<std::string, TypeStats> types;
    size_t total = 0, failed = 0;
    for (const auto& loc : index.ofType(kTypeTxmt)) {
        try {
            const auto bytes = index.read(loc);
            Rcol rc(bytes);
            const auto* b = rc.find("cMaterialDefinition");
            if (!b)
                throw std::runtime_error("no cMaterialDefinition");
            const auto m = MaterialDefinition::parse(b->body);
            ++total;
            if (!filter.empty() && m.materialType.find(filter) == std::string::npos)
                continue;
            if (list) {
                // Wall masks also print their mirror flag (tsMirrorWallMask,
                // which CombinedWallMaterial passes on as wallMaskMirrorFlag).
                const std::string mirror = m.property("tsMirrorWallMask");
                std::printf("%s %s%s%s\n", m.resource.fileName.c_str(), m.property("stdMatBaseTextureName").c_str(),
                            mirror.empty() ? "" : " mirror=", mirror.c_str());
            }
            auto& t = types[m.materialType];
            ++t.count;
            if (t.examples.size() < 3)
                t.examples.push_back(m.resource.fileName);
            for (const auto& [k, v] : m.properties)
                ++t.props[k][v];
        } catch (const std::exception&) {
            ++failed;
        }
    }
    std::vector<std::pair<std::string, const TypeStats*>> order;
    for (const auto& [name, t] : types)
        order.emplace_back(name, &t);
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second->count > b.second->count; });
    std::printf("%zu TXMTs (%zu unreadable), %zu material types\n", total, failed, types.size());
    for (const auto& [name, t] : order)
        std::printf("%7zu  %s\n", t->count, name.c_str());
    for (const auto& [name, t] : order) {
        std::printf("\n== %s (%zu; e.g.", name.c_str(), t->count);
        for (const auto& e : t->examples)
            std::printf(" %s", e.c_str());
        std::printf(")\n");
        for (const auto& [prop, values] : t->props) {
            size_t n = 0;
            for (const auto& [v, c] : values)
                n += c;
            std::vector<std::pair<size_t, std::string>> top;
            for (const auto& [v, c] : values)
                top.emplace_back(c, v);
            std::sort(top.rbegin(), top.rend());
            std::printf("  %6zu %-44s", n, prop.c_str());
            for (size_t i = 0; i < top.size() && i < 4; ++i)
                std::printf(" %s(%zu)", top[i].second.substr(0, 40).c_str(), top[i].first);
            if (top.size() > 4)
                std::printf(" ... %zu values", top.size());
            std::printf("\n");
        }
    }
    return 0;
}

// Every texture of a TXTR found by name (e.g. the six faces of an env cube),
// largest level, as <out-prefix><i>.png.
int cmdTextureFaces(const std::string& root, const std::string& name, const std::string& prefix) {
    ResourceIndex index(root);
    auto loc = index.findByName(name, 0x1C4A276C);
    if (!loc)
        throw std::runtime_error("TXTR not found: " + name);
    const auto all = loadAllTextureLevels(index.read(*loc), &index);
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& l = all[i].levels.front();
        const auto path = prefix + std::to_string(i) + ".png";
        writePng(path, decodeTexture(all[i].format, l.width, l.height, l.data));
        std::printf("%s: %ux%u %s\n", path.c_str(), l.width, l.height, texFormatName(all[i].format));
    }
    return 0;
}

// The materials of an object (CRES name, 0xGUID or object name) as the
// renderer sees them.
int cmdObjectMaterials(const std::string& root, const std::string& query) {
    ResourceIndex index(root);
    const auto model = loadObjectModel(index, resolveObjectCres(index, query));
    for (const auto& part : model.parts)
        std::printf("part %s/%s: %s, %zu vertices%s\n", part.shape.c_str(), part.mesh.name.c_str(), part.material.c_str(),
                    part.mesh.positions.size(), part.mesh.colors.empty() ? "" : ", vertex colours");
    static const char* shading[] = {"standard", "sim", "cel", "vertexcolor"};
    static const char* blend[] = {"none", "alpha", "additive"};
    static const char* anim[] = {"none", "transform", "tile"};
    for (const auto& [name, def] : model.materials) {
        const auto m = standardMaterial(def);
        std::printf("%s: %s -> %s%s, layer %d, blend %s%s, lit %d, base %s\n", name.c_str(), def.materialType.c_str(),
                    shading[int(m.shading)], m.visible ? "" : " (hidden)", m.layer, blend[int(m.blend)],
                    m.alphaTest ? " +test" : "", m.lighting, m.baseTexture.c_str());
        std::printf("    diffuse %g,%g,%g alpha x%g spec %g,%g,%g ^%g emissive %g,%g,%g\n", m.diffuse[0], m.diffuse[1],
                    m.diffuse[2], m.alphaMultiplier, m.specular[0], m.specular[1], m.specular[2], m.specularPower,
                    m.emissive[0], m.emissive[1], m.emissive[2]);
        if (!m.envCube.empty())
            std::printf("    env cube %s x %g,%g,%g blur %g%s\n", m.envCube.c_str(), m.envCubeCoef[0], m.envCubeCoef[1],
                        m.envCubeCoef[2], m.envCubeBlur, m.envCubeLockedToCamera ? " locked to camera" : "");
        if (!m.normalMap.empty())
            std::printf("    normal map %s\n", m.normalMap.c_str());
        if (m.texAnim != StandardMaterial::TexAnim::None)
            std::printf("    texture animation %s\n", anim[int(m.texAnim)]);
    }
    return 0;
}

} // namespace

int materialCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "material-survey" && (argc == 3 || argc == 4))
        return cmdMaterialSurvey(argv[2], argc == 4 ? argv[3] : "");
    if (cmd == "material-survey" && argc == 5 && std::string(argv[4]) == "--list")
        return cmdMaterialSurvey(argv[2], argv[3], true);
    if (cmd == "object-materials" && argc == 4)
        return cmdObjectMaterials(argv[2], argv[3]);
    if (cmd == "texture-faces" && argc == 5)
        return cmdTextureFaces(argv[2], argv[3], argv[4]);
    return -1;
}

const char* materialUsage() { return "       s2pkg material-survey <Res-dir> [type-filter [--list]]\n"
                                    "       s2pkg object-materials <Res-dir> <cres-name|0xGUID|object-name>\n"
                                    "       s2pkg texture-faces <Res-dir> <txtr-name> <out-prefix>\n"; }

} // namespace os2
