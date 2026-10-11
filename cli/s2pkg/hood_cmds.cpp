// s2pkg commands for neighborhoods: families, Sims, relationships, family
// tree, lots and inventories.
#include "hood_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/neighborhood.hpp"
#include "formats/hood_terrain.hpp"
#include "formats/resindex.hpp"

#include <algorithm>
#include <memory>

#include <cstdio>
#include <map>
#include <string>

namespace os2 {

namespace {

using namespace hood;

std::string tag(uint32_t type) {
    if (type == 0)
        return "character";
    switch (type) {
    case hoodtype::NGBH: return "NGBH";
    case hoodtype::SDSC: return "SDSC";
    case hoodtype::FAMI: return "FAMI";
    case hoodtype::FamilyTies: return "family ties";
    case hoodtype::SREL: return "SREL";
    case hoodtype::LotInfo: return "lot info";
    case hoodtype::NeighborhoodId: return "NID";
    case hoodtype::SDNA: return "SDNA";
    case hoodtype::BINX: return "BINX wardrobe";
    }
    if (auto n = typeName(type); !n.empty())
        return std::string(n);
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", type);
    return buf;
}

std::string who(const Neighborhood& h, uint16_t id) {
    if (const auto* n = h.neighbor(id))
        return n->name();
    return "#" + std::to_string(id) + "?";
}

void printCounts(const Neighborhood& h) {
    for (const auto& [type, c] : h.decodeCounts) {
        std::printf("  %-14s %5zu decoded %3zu failed", tag(type).c_str(), c.decoded, c.failed);
        if (!c.firstError.empty())
            std::printf("  (%s)", c.firstError.c_str());
        std::printf("\n");
    }
}

int cmdHood(const char* path) {
    const auto h = Neighborhood::load(path);
    std::printf("%s \"%s\"", h.prefix.c_str(), h.name.c_str());
    if (h.info)
        std::printf("  NID v%u id=%u", h.info->version, h.info->id);
    std::printf("  region=%s\n", h.inventories.regionType.c_str());
    std::printf("%zu Sims, %zu families, %zu lots, %zu relationships, %zu family ties, %zu character files\n\n",
                h.neighbors.size(), h.families.size(), h.lots.size(), h.relationships.size(), h.familyTree.size(),
                h.characters.size());

    std::printf("Families\n");
    for (const auto& f : h.families) {
        std::printf("  %5u %-20s funds=%-7d lot=%-4u flags=0x%X members=%zu wardrobe=%zu\n", f.id,
                    f.name.empty() ? "-" : f.name.c_str(), f.record.funds, f.record.lot, f.record.flags,
                    f.members.size(), f.wardrobe.size());
        if (f.id >= 0x7FFE)
            continue; // townies and NPCs: listed under Sims
        for (uint16_t id : f.members) {
            const auto* n = h.neighbor(id);
            const auto& d = n->description;
            std::printf("        %3u %-26s %-7s %s\n", id, n->name().c_str(), lifeStageName(d.age()),
                        d.female() ? "female" : "male");
        }
    }

    std::printf("\nSims\n");
    for (const auto& n : h.neighbors) {
        const auto& p = n.description.personData;
        std::printf("  %3u %-26s %-7s %-6s family=%-5u guid=0x%08X asp=%-3d pers=%d/%d/%d/%d/%d rels=%zu%s\n",
                    n.id, n.name().c_str(), lifeStageName(n.description.age()),
                    n.description.female() ? "female" : "male", n.family, n.description.guid, p[sdsc::Aspiration],
                    p[sdsc::Neat] / 100, p[sdsc::Outgoing] / 100, p[sdsc::Active] / 100, p[sdsc::Playful] / 100,
                    p[sdsc::Nice] / 100, n.description.relations.size(), n.character < 0 ? "  (no character file)" : "");
    }

    std::printf("\nFamily tree\n");
    std::map<uint16_t, std::vector<const FamilyTreeEdge*>> byFrom;
    for (const auto& e : h.familyTree)
        byFrom[e.from].push_back(&e);
    for (const auto& [from, edges] : byFrom) {
        std::printf("  %s:", who(h, from).c_str());
        for (const auto* e : edges)
            std::printf(" %s %s;", tieTypeName(e->type), who(h, e->to).c_str());
        std::printf("\n");
    }

    std::printf("\nRelationships (daily/lifetime, flags, what the other is)\n");
    for (const auto& r : h.relationships) {
        if (r.from == r.to)
            continue;
        std::printf("  %s -> %s: %d/%d flags=0x%X %s\n", who(h, r.from).c_str(), who(h, r.to).c_str(),
                    r.value(relvar::Daily), r.value(relvar::Lifetime), unsigned(r.value(relvar::Flags)),
                    familialName(r.value(relvar::Familial)));
    }

    std::printf("\nLots\n");
    for (const auto& l : h.lots) {
        std::printf("  %3u %-28s %ux%u type=%u at (%u,%u)%s", l.id, l.name.c_str(), l.info.width, l.info.depth,
                    l.info.type, l.info.x, l.info.y, l.package.empty() ? "  (no lot file)" : "");
        for (uint32_t f : l.families)
            if (const auto* fam = h.family(f))
                std::printf("  [%s]", fam->name.c_str());
        std::printf("\n");
    }

    size_t memories = 0, tokens = 0;
    for (const auto* list : {&h.inventories.lots, &h.inventories.families, &h.inventories.neighbors})
        for (const auto& inv : *list) {
            tokens += inv.counted.size() + inv.singular.size();
            for (const auto& t : inv.singular)
                memories += t.flags == 3;
        }
    std::printf("\nInventories: %zu lot, %zu family, %zu neighbor; %zu tokens (%zu with flags 3, the memories)\n",
                h.inventories.lots.size(), h.inventories.families.size(), h.inventories.neighbors.size(), tokens,
                memories);

    std::printf("\nDecoded\n");
    printCounts(h);
    return h.errors.empty() ? 0 : 1;
}

// Every neighborhood under a Neighborhoods folder: decoded / failed per type,
// plus the unknown fields that are not constant.
int cmdHoodSweep(const char* dir) {
    const auto hoods = findNeighborhoods(dir);
    if (hoods.empty()) {
        std::fprintf(stderr, "no neighborhoods under %s\n", dir);
        return 1;
    }
    std::map<uint32_t, DecodeCount> total;
    std::map<std::string, std::map<long, size_t>> unknowns;
    size_t errors = 0, noCharacter = 0, unnamedFamilies = 0, lotsWithoutFile = 0;
    for (const auto& p : hoods) {
        const auto h = Neighborhood::load(p);
        std::printf("%s \"%s\": %zu Sims, %zu families, %zu lots, %zu SREL, %zu ties, %zu characters, %zu errors\n",
                    h.prefix.c_str(), h.name.c_str(), h.neighbors.size(), h.families.size(), h.lots.size(),
                    h.relationships.size(), h.familyTree.size(), h.characters.size(), h.errors.size());
        for (const auto& e : h.errors)
            std::printf("  error: %s\n", e.c_str());
        errors += h.errors.size();
        for (const auto& [t, c] : h.decodeCounts) {
            total[t].decoded += c.decoded;
            total[t].failed += c.failed;
            if (total[t].firstError.empty())
                total[t].firstError = c.firstError;
        }
        const auto& inv = h.inventories;
        ++unknowns["NGBH header"][long(inv.header[0]) << 16 | inv.header[1] << 8 | inv.header[2]];
        ++unknowns["NGBH trailer"][inv.trailer];
        for (const auto& n : h.neighbors) {
            ++unknowns["SDSC u32 @0"][n.description.unknown0];
            ++unknowns["SDSC u32 @8"][n.description.unknown2];
            ++unknowns["SDSC u32 after GUID"][n.description.unknown3];
            ++unknowns["SDSC trailer"][n.description.trailer];
            noCharacter += n.character < 0;
            // Every Sim listed in the SDSC has an SREL from this Sim.
            bool listed = true;
            for (uint32_t to : n.description.relations)
                listed = listed && h.relationship(n.id, uint16_t(to));
            ++unknowns["SDSC relations have an SREL"][listed];
        }
        // Familial relation (SREL variable 3) against the family tree edge
        // between the same two Sims: key = value << 8 | tie type (0xFF none).
        for (const auto& r : h.relationships) {
            const int32_t v = r.value(relvar::Familial);
            if (!v)
                continue;
            long tie = 0xFF;
            for (const auto& e : h.familyTree)
                if (e.from == r.from && e.to == r.to)
                    tie = long(e.type);
            ++unknowns["SREL familial << 8 | tie from->to"][long(v) << 8 | tie];
        }
        // Gender of the other Sim per tie type: key = type << 8 | female.
        for (const auto& e : h.familyTree)
            if (const auto* o = h.neighbor(e.to))
                ++unknowns["tie type << 8 | other is female"][long(e.type) << 8 | o->description.female()];
        for (const auto& f : h.families) {
            ++unknowns["FAMI u32 @8"][f.record.unknown];
            ++unknowns["FAMI flags"][f.record.flags];
            unnamedFamilies += f.name.empty();
        }
        for (const auto& r : h.relationships) {
            ++unknowns["SREL u32 @0"][r.unknown];
            ++unknowns["SREL count"][long(r.values.size())];
        }
        for (const auto& l : h.lots) {
            ++unknowns["lot info rest size"][long(l.info.rest.size())];
            lotsWithoutFile += l.package.empty();
        }
    }
    std::printf("\nDecoded (all neighborhoods)\n");
    for (const auto& [type, c] : total)
        std::printf("  %-14s %6zu decoded %3zu failed%s%s\n", tag(type).c_str(), c.decoded, c.failed,
                    c.firstError.empty() ? "" : "  ", c.firstError.c_str());
    std::printf("\nUnknown fields (value: count)\n");
    for (const auto& [what, values] : unknowns) {
        std::printf("  %-38s", what.c_str());
        for (const auto& [v, n] : values)
            std::printf(" 0x%lX:%zu", v, n);
        std::printf("\n");
    }
    std::printf("\nSims without a character file: %zu; families without a name: %zu; lots without a file: %zu\n",
                noCharacter, unnamedFamilies, lotsWithoutFile);
    return errors ? 1 : 0;
}

// The neighborhood view's data: terrain, occupants (trees, roads, bridges,
// props) with their models, and the saved camera.
int cmdHoodTerrain(const std::filesystem::path& path, const char* resDir) {
    std::filesystem::path pkgPath = path;
    if (std::filesystem::is_directory(path))
        for (const auto& e : std::filesystem::directory_iterator(path))
            if (e.path().filename().string().find("_Neighborhood.package") != std::string::npos)
                pkgPath = e.path();
    DbpfPackage pkg(pkgPath);
    std::map<uint32_t, std::string> objects;
    std::unique_ptr<ResourceIndex> index;
    if (resDir) {
        index = std::make_unique<ResourceIndex>(resDir);
        objects = loadNeighborhoodObjects(*index);
        std::printf("%zu neighborhood objects in the catalog\n", objects.size());
    }
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == hoodtype::Terrain) {
            const Terrain t = Terrain::parse(pkg.read(e));
            float lo = 1e9f, hi = -1e9f;
            size_t under = 0;
            for (float h : t.heights) {
                lo = std::min(lo, h), hi = std::max(hi, h);
                under += h < t.seaLevel;
            }
            const size_t nonzero = size_t(std::count_if(t.vertexBytes.begin(), t.vertexBytes.end(), [](uint8_t b) { return b != 0; }));
            std::printf("terrain v%u: %ux%u cells (%ux%u vertices), %s, sea level %.2f, heights %.1f..%.1f, %zu "
                        "vertices under water, %zu vertex bytes (%zu non-zero)\n",
                        t.version, t.width, t.height, t.gridWidth, t.gridHeight, t.type.c_str(), t.seaLevel, lo, hi,
                        under, t.vertexBytes.size(), nonzero);
        } else if (e.tgi.type == hoodtype::Occupants) {
            const Occupants o = Occupants::parse(pkg.read(e));
            std::printf("occupants v%u: %zu flora (v%u), %zu roads (v%u), %zu bridges (v%u), %zu props (v%u)\n",
                        o.version, o.flora.size(), o.floraVersion, o.roads.size(), o.roadVersion, o.bridges.size(),
                        o.bridgeVersion, o.props.size(), o.propVersion);
            std::map<uint32_t, int> pieces, guids, flags, connections;
            for (const auto& r : o.roads)
                ++pieces[r.piece], ++flags[r.flags], ++connections[r.connections];
            for (const auto& b : o.bridges)
                ++pieces[b.road.piece];
            for (const auto& [p, n] : pieces)
                std::printf("  road piece %08x x%d\n", p, n);
            for (const auto& [f, n] : flags)
                std::printf("  road flags %02x x%d\n", f, n);
            for (const auto& f : o.flora)
                ++guids[f.guid];
            for (const auto& p : o.props)
                ++guids[p.guid];
            for (const auto& [g, n] : guids) {
                auto it = objects.find(g);
                std::printf("  object %08x x%d %s\n", g, n, it == objects.end() ? "?" : it->second.c_str());
            }
            for (const auto& b : o.bridges)
                std::printf("  bridge %04x-bridge at (%.1f, %.1f, %.1f)\n", b.road.piece >> 16, b.road.at.position[0],
                            b.road.at.position[1], b.road.at.position[2]);
        } else if (e.tgi.type == hoodtype::SavedCamera) {
            SavedCamera c;
            if (SavedCamera::parse(pkg.read(e), c))
                std::printf("camera: distance %.1f, heading %.1f deg, pitch %.1f deg, pivot (%.1f, %.1f, %.1f)\n",
                            c.distance, c.heading * 57.2958f, c.pitch * 57.2958f, c.pivot[0], c.pivot[1], c.pivot[2]);
        }
    }
    return 0;
}

} // namespace

const char* hoodUsage() {
    return "       s2pkg hood <neighborhood-dir|_Neighborhood.package>\n"
           "       s2pkg hood-sweep <UserData/Neighborhoods>\n"
           "       s2pkg hood-terrain <neighborhood-dir|_Neighborhood.package> [Res-dir]\n";
}

int hoodCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "hood" && argc == 3)
        return cmdHood(argv[2]);
    if (cmd == "hood-terrain" && (argc == 3 || argc == 4))
        return cmdHoodTerrain(argv[2], argc == 4 ? argv[3] : nullptr);
    if (cmd == "hood-sweep" && argc == 3)
        return cmdHoodSweep(argv[2]);
    return -1;
}

} // namespace os2
