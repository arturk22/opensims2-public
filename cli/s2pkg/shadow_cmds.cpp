// s2pkg guob: each placed object's ground and wall shadow planes (GUOB) as
// the lot saved them and as nTSSG::cShadow::UpdateGUOBs turns them on for a
// wall mode (formats/object_shadow.hpp, docs/lighting-mac.md 12).

#include "shadow_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/model.hpp"
#include "formats/object_shadow.hpp"
#include "formats/resindex.hpp"
#include "formats/xform.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

namespace os2 {

namespace {

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string bits(uint8_t m) {
    std::string s;
    for (int i = 0; i < kGuobCount; ++i)
        s += (m >> i) & 1 ? "GWNSEW"[i] : '.';
    return s;
}

// The level an object stands on: the highest whose floor is at or under it.
int levelOf(const Lot& lot, float x, float y, float z) {
    int level = 0;
    for (int l = std::max(0, lot.minLevel); l < lot.minLevel + lot.levelCount; ++l)
        if (lot.elevationAt(l, x, y) <= z + 0.05f)
            level = l;
    return level;
}

int cmdGuob(const std::filesystem::path& root, const std::filesystem::path& lotPath, int mode) {
    ResourceIndex index(root);
    DbpfPackage pkg(lotPath);
    const Lot lot = Lot::load(pkg);
    const LotWallEdges edges(lot);
    GuobWorld world;
    world.wallMode = mode;
    world.wallExists = [&](int x0, int y0, int x1, int y1, int level) { return edges(x0, y0, x1, y1, level); };
    std::map<std::string, GuobModelInfo> infos;
    int objects = 0, withPlanes = 0, sameAsSaved = 0, sideOn = 0;
    for (const auto& o : lot.objects) {
        if (o.person || o.model.empty() || o.model == "nullModel" || o.model == "noModel" ||
            (o.position[0] == 0 && o.position[1] == 0))
            continue;
        std::string key = o.model;
        for (const auto& [s, m] : o.materials)
            key += "|" + s + "=" + m;
        auto it = infos.find(key);
        if (it == infos.end()) {
            GuobModelInfo info;
            try {
                info = guobModelInfo(loadObjectModel(index, o.model, o.materials));
            } catch (const std::exception&) {
            }
            it = infos.emplace(key, info).first;
        }
        const GuobModelInfo& info = it->second;
        ++objects;
        // OS2_GUOB_PARTS=<object id>: that object's parts and their boxes (model space).
        if (const char* id = std::getenv("OS2_GUOB_PARTS"); id && unsigned(std::atoi(id)) == (o.instance & 0xFFFF)) {
            const ObjectModel m = loadObjectModel(index, o.model, o.materials);
            for (const auto& p : m.parts) {
                std::array<float, 3> lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (const auto& v : p.mesh.positions)
                    for (int k = 0; k < 3; ++k)
                        lo[size_t(k)] = std::min(lo[size_t(k)], v[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], v[size_t(k)]);
                std::printf("    part %s / %s node %s material %s: (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", p.shape.c_str(),
                            p.mesh.name.c_str(), p.node.c_str(), p.material.c_str(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            }
            for (const auto& b : m.bounds) {
                std::array<float, 3> lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (const auto& v : b.points)
                    for (int k = 0; k < 3; ++k)
                        lo[size_t(k)] = std::min(lo[size_t(k)], v[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], v[size_t(k)]);
                std::printf("    bound %s (%s): (%.4f %.4f %.4f)-(%.4f %.4f %.4f)\n", b.shape.c_str(),
                            b.fromBoundingMesh ? "bounding mesh" : "vertices", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            }
            std::printf("    at (%.2f %.2f %.2f), model box (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", o.position[0], o.position[1],
                        o.position[2], info.lo[0], info.lo[1], info.lo[2], info.hi[0], info.hi[1], info.hi[2]);
        }
        if (!info.present)
            continue;
        ++withPlanes;
        // The saved state: a GUOB subset saved with GUOBOff is off.
        uint8_t saved = 0;
        for (int i = 0; i < kGuobCount; ++i)
            if ((info.present >> i) & 1) {
                auto m = o.materials.find(guobSubsetName(i));
                if (m == o.materials.end() || lower(m->second) != "guoboff")
                    saved |= uint8_t(1u << i);
            }
        const Xform at{{o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]}, {o.position[0], o.position[1], o.position[2]}};
        std::array<float, 3> lo, hi;
        guobWorldBox(at, info.lo, info.hi, lo, hi);
        const GuobTileRect r = guobTileRect(lo, hi);
        if (const char* id = std::getenv("OS2_GUOB_PARTS"); id && unsigned(std::atoi(id)) == (o.instance & 0xFFFF))
            std::printf("    world box (%.7f %.7f)-(%.7f %.7f), rotation (%.9f %.9f %.9f %.9f)\n", lo[0], lo[1], hi[0], hi[1],
                        o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]);
        // Object data 1 from the saved heading (90 - 45 x direction degrees).
        const float yaw = 2 * std::atan2(o.rotation[2], o.rotation[3]) * 180 / 3.14159265f;
        const int direction = (int(std::lround((90 - yaw) / 45)) % 8 + 8) % 8;
        const int level = levelOf(lot, o.position[0], o.position[1], o.position[2]);
        const uint8_t sides = guobWallSides(r, direction, level, true, world);
        const uint8_t on = guobsOn(info.present, true, sides, world);
        sameAsSaved += on == saved;
        sideOn += (on & 0x3C) != 0;
        std::printf("  %5u %-44s dir %d level %d tiles (%d,%d)-(%d,%d) has %s saved %s now %s\n", o.instance & 0xFFFF,
                    o.definition.substr(0, 44).c_str(), direction, level, r.x0, r.y0, r.x1, r.y1, bits(info.present).c_str(),
                    bits(saved).c_str(), bits(on).c_str());
    }
    std::printf("%s, wall mode %d: %d objects, %d with GUOB planes, %d with a wall plane on, %d as saved\n",
                lotPath.filename().string().c_str(), mode, objects, withPlanes, sideOn, sameAsSaved);
    return 0;
}

} // namespace

int shadowCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "guob" && (argc == 4 || argc == 5))
        return cmdGuob(argv[2], argv[3], argc == 5 ? std::stoi(argv[4]) : 2);
    return -1;
}

const char* shadowUsage() {
    return "       s2pkg guob <game-dir> <lot.package> [wall mode, 2 by default]: each object's GUOB planes (G W N S E W) saved and now\n";
}

} // namespace os2
