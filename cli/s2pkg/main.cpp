// s2pkg: inspect and extract The Sims 2 .package (DBPF) files.
#include "describe.hpp"
#include "formats/dbpf.hpp"
#include "formats/gmdc.hpp"
#include "formats/image.hpp"
#include "formats/lot.hpp"
#include "formats/lot_model.hpp"
#include "formats/model.hpp"
#include "formats/simantics.hpp"
#include "formats/resindex.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace os2;

namespace {

std::string typeLabel(uint32_t t) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", t);
    if (auto n = typeName(t); !n.empty())
        return std::string(n);
    // Many Sims 1-era types are big-endian FourCCs ("SPR2", "DGRP", "BMP_").
    std::string cc;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = char((t >> shift) & 0xFF);
        if (c < 0x20 || c > 0x7E)
            return hex;
        cc += c;
    }
    return cc;
}

std::string tgiString(const Tgi& k) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%08X-%08X-%08X-%08X", k.type, k.group, k.instanceHi, k.instance);
    return buf;
}

int cmdLs(DbpfPackage& pkg) {
    for (const auto& e : pkg.entries()) {
        std::printf("%-6s %s  off=%-10u size=%-9u", typeLabel(e.tgi.type).c_str(), tgiString(e.tgi).c_str(),
                    e.offset, e.size);
        if (e.rawSize)
            std::printf(" raw=%u", *e.rawSize);
        std::printf("\n");
    }
    return 0;
}

int cmdStats(DbpfPackage& pkg) {
    struct Acc { size_t count = 0; uint64_t bytes = 0; size_t compressed = 0; };
    std::map<uint32_t, Acc> byType;
    for (const auto& e : pkg.entries()) {
        auto& a = byType[e.tgi.type];
        ++a.count;
        a.bytes += e.size;
        a.compressed += e.compressed();
    }
    const auto& h = pkg.header();
    std::printf("DBPF %u.%u  index %u.%u  entries=%u\n", h.major, h.minor, h.indexMajor,
                h.indexMinor ? h.indexMinor - 1 : 0, h.indexCount);
    std::printf("%-12s %8s %12s %10s\n", "type", "count", "bytes", "qfs");
    for (const auto& [t, a] : byType)
        std::printf("%-12s %8zu %12llu %10zu\n", typeLabel(t).c_str(), a.count, (unsigned long long)a.bytes,
                    a.compressed);
    return 0;
}

int cmdVerify(DbpfPackage& pkg) {
    size_t ok = 0, failed = 0;
    for (const auto& e : pkg.entries()) {
        try {
            pkg.read(e);
            ++ok;
        } catch (const std::exception& ex) {
            ++failed;
            std::fprintf(stderr, "%s: %s\n", tgiString(e.tgi).c_str(), ex.what());
        }
    }
    std::printf("%s: %zu ok, %zu failed\n", pkg.path().string().c_str(), ok, failed);
    return failed ? 1 : 0;
}

Tgi parseTgi(const std::string& s) {
    Tgi k;
    if (std::sscanf(s.c_str(), "%x-%x-%x-%x", &k.type, &k.group, &k.instanceHi, &k.instance) != 4)
        throw std::runtime_error("bad TGI '" + s + "' (expected TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII)");
    return k;
}

int cmdDump(DbpfPackage& pkg, const Tgi& tgi) {
    const auto* e = pkg.find(tgi);
    if (!e)
        throw std::runtime_error("resource not found");
    std::fputs(describeResource(tgi.type, pkg.read(*e)).c_str(), stdout);
    return 0;
}

int cmdDecodeAll(DbpfPackage& pkg) {
    std::map<std::string, std::pair<size_t, size_t>> byType; // ok, failed
    for (const auto& e : pkg.entries()) {
        if (!hasDecoder(e.tgi.type))
            continue;
        auto& acc = byType[typeLabel(e.tgi.type)];
        try {
            describeResource(e.tgi.type, pkg.read(e));
            ++acc.first;
        } catch (const std::exception& ex) {
            ++acc.second;
            std::fprintf(stderr, "%s %s: %s\n", typeLabel(e.tgi.type).c_str(), tgiString(e.tgi).c_str(), ex.what());
        }
    }
    size_t failed = 0;
    for (const auto& [t, acc] : byType) {
        std::printf("%-10s %8zu ok %6zu failed  %s\n", t.c_str(), acc.first, acc.second,
                    pkg.path().filename().string().c_str());
        failed += acc.second;
    }
    return failed ? 1 : 0;
}

// The TSData/Res directory a package lives under, used to resolve cross-package references.
std::filesystem::path resRootOf(const std::filesystem::path& pkg) {
    for (auto p = std::filesystem::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    return {};
}

int cmdTexture(DbpfPackage& pkg, const Tgi& tgi, const std::filesystem::path& out) {
    const auto* e = pkg.find(tgi);
    if (!e)
        throw std::runtime_error("resource not found");
    const auto data = pkg.read(*e);
    std::unique_ptr<ResourceIndex> index;
    RgbaImage img;
    try {
        img = loadTexture(data, nullptr);
    } catch (const FormatError&) {
        const auto root = resRootOf(pkg.path());
        if (root.empty())
            throw;
        index = std::make_unique<ResourceIndex>(root);
        img = loadTexture(data, index.get());
    }
    writePng(out, img);
    std::printf("%ux%u -> %s\n", img.width, img.height, out.string().c_str());
    return 0;
}

int cmdMesh(DbpfPackage& pkg, const Tgi& tgi, const std::filesystem::path& out) {
    const auto* e = pkg.find(tgi);
    if (!e)
        throw std::runtime_error("resource not found");
    const auto data = pkg.read(*e);
    Rcol rc(data);
    const auto* block = rc.find("cGeometryDataContainer");
    if (!block)
        throw std::runtime_error("not a GMDC");
    const auto meshes = buildMeshes(GeometryData::parse(block->body));
    std::ofstream f(out);
    if (!f)
        throw std::runtime_error("cannot write " + out.string());
    ObjWriter obj{f};
    size_t tris = 0;
    for (const auto& m : meshes) {
        obj.write(m, "");
        tris += m.triangles.size() / 3;
    }
    std::printf("%zu groups, %zu vertices, %zu triangles -> %s\n", meshes.size(), obj.vBase, tris,
                out.string().c_str());
    return 0;
}

int cmdObject(const std::filesystem::path& root, const std::string& query, const std::filesystem::path& outDir) {
    ResourceIndex index(root);
    std::string match;
    const auto cres = resolveObjectCres(index, query, &match);
    std::printf("%s\n", match.c_str());
    const auto model = loadObjectModel(index, cres);
    std::filesystem::create_directories(outDir);
    const std::string base = model.name.substr(0, model.name.size() - 5); // without "_cres"

    // Materials and their base textures.
    std::ofstream mtl(outDir / (base + ".mtl"));
    size_t textures = 0;
    for (const auto& [mname, m] : model.materials) {
        auto diffuse = m.property("stdMatDiffCoef");
        for (auto& c : diffuse)
            if (c == ',')
                c = ' ';
        mtl << "newmtl " << mname << "\nKd " << (diffuse.empty() ? "1 1 1" : diffuse) << "\n";
        const auto blend = m.property("stdMatAlphaBlendMode");
        const bool alpha = (!blend.empty() && blend != "none") || m.property("stdMatAlphaTestEnabled") == "true";
        if (alpha && !m.property("stdMatAlphaMultiplier").empty())
            mtl << "d " << m.property("stdMatAlphaMultiplier") << "\n";
        const auto tex = baseTextureName(m);
        if (tex.empty())
            continue;
        auto loc = index.findByName(tex, 0x1C4A276C);
        if (!loc) {
            std::fprintf(stderr, "warning: TXTR not found: %s\n", tex.c_str());
            continue;
        }
        const auto png = tex + ".png";
        writePng(outDir / png, loadTexture(index.read(*loc), &index));
        ++textures;
        mtl << "map_Kd " << png << "\n";
        if (alpha)
            mtl << "map_d " << png << "\n";
    }

    std::ofstream obj(outDir / (base + ".obj"));
    obj << "# " << model.name << " exported by s2pkg\nmtllib " << base << ".mtl\n";
    ObjWriter writer{obj};
    size_t tris = 0;
    for (const auto& part : model.parts) {
        writer.write(part.mesh, part.material);
        tris += part.mesh.triangles.size() / 3;
    }
    for (const auto& w : model.warnings)
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    std::printf("%s: %zu parts, %zu vertices, %zu triangles, %zu materials, %zu textures -> %s\n", model.name.c_str(),
                model.parts.size(), writer.vBase, tris, model.materials.size(), textures,
                (outDir / (base + ".obj")).string().c_str());
    return 0;
}

int cmdFind(const std::string& root, const std::string& name, const std::string& type) {
    ResourceIndex idx(root);
    std::printf("%zu packages, %zu resources, %zu duplicate TGIs\n", idx.packageCount(), idx.resourceCount(),
                idx.duplicateCount());
    const uint32_t t = uint32_t(std::stoul(type, nullptr, 16));
    std::printf("hash: instance=0x%08X instanceHi=0x%08X\n", nameInstanceHash(name), nameInstanceHiHash(name));
    auto loc = idx.findByName(name, t);
    if (!loc) {
        std::printf("not found\n");
        return 1;
    }
    std::printf("%s in %s\n", tgiString(loc->entry->tgi).c_str(), loc->package->path().string().c_str());
    return 0;
}

int cmdExtract(DbpfPackage& pkg, const std::filesystem::path& outDir) {
    std::filesystem::create_directories(outDir);
    for (const auto& e : pkg.entries()) {
        auto data = pkg.read(e);
        auto name = typeLabel(e.tgi.type) + "_" + tgiString(e.tgi) + ".bin";
        std::ofstream(outDir / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                             std::streamsize(data.size()));
    }
    std::printf("extracted %zu resources to %s\n", pkg.entries().size(), outDir.string().c_str());
    return 0;
}

// Builds the terrain/floor/wall model of each lot and reports what it holds
// and what could not be resolved (patterns, TXMTs, textures).
int cmdLot(const std::filesystem::path& root, const std::vector<std::filesystem::path>& lots) {
    ResourceIndex index(root);
    const PatternCatalog patterns(index, root);
    std::printf("%zu catalog patterns\n", patterns.catalogSize());
    int failed = 0;
    for (const auto& path : lots) {
        try {
            DbpfPackage pkg(path);
            const Lot lot = Lot::load(pkg);
            const ObjectModel model = buildLotModel(lot, index, patterns);
            size_t tris = 0, missingTextures = 0;
            for (const auto& part : model.parts)
                tris += part.mesh.triangles.size() / 3;
            for (const auto& [name, m] : model.materials) {
                const auto tex = baseTextureName(m);
                if (!tex.empty() && !index.findByName(tex, 0x1C4A276C)) {
                    std::printf("  missing texture %s (%s)\n", tex.c_str(), name.c_str());
                    ++missingTextures;
                }
            }
            std::printf("%s: %ux%u, levels %d..%d, %zu walls, %zu floor / %zu wall patterns, %zu paints, "
                        "%zu parts, %zu triangles, %zu materials\n",
                        path.filename().string().c_str(), lot.width, lot.height, lot.minLevel,
                        lot.minLevel + lot.levelCount - 1, lot.walls.size(), lot.floorPatterns.size(),
                        lot.wallPatterns.size(), lot.paints.size(), model.parts.size(), tris, model.materials.size());
            size_t placed = 0, people = 0;
            for (const auto& o : lot.objects) {
                people += o.person;
                placed += !o.person && !o.model.empty() && o.model != "nullModel" && o.model != "noModel" &&
                          (o.position[0] != 0 || o.position[1] != 0);
            }
            std::printf("  %zu objects: %zu with a model in the world, %zu Sims\n", lot.objects.size(), placed, people);
            for (const auto& w : lot.warnings)
                std::printf("  %s\n", w.c_str());
            for (const auto& w : model.warnings)
                std::printf("  %s\n", w.c_str());
        } catch (const std::exception& e) {
            std::printf("%s: error: %s\n", path.filename().string().c_str(), e.what());
            ++failed;
        }
    }
    return failed ? 1 : 0;
}

int usage() {
    std::fprintf(stderr, "usage: s2pkg ls|stats|verify|decode-all <file.package>\n"
                         "       s2pkg dump <file.package> <TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII>\n"
                         "       s2pkg extract <file.package> <out-dir>\n"
                         "       s2pkg texture <file.package> <tgi> <out.png>\n"
                         "       s2pkg mesh <file.package> <tgi> <out.obj>\n"
                         "       s2pkg find <Res-dir> <name> <type-hex>\n"
                         "       s2pkg object <Res-dir> <cres-name|0xGUID|object-name> <out-dir>\n"
                         "       s2pkg lot <Res-dir> <lot.package>...\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3)
        return usage();
    const std::string cmd = argv[1];
    try {
        if (cmd == "find" && argc == 5) return cmdFind(argv[2], argv[3], argv[4]);
        if (cmd == "object" && argc == 5) return cmdObject(argv[2], argv[3], argv[4]);
        if (cmd == "lot" && argc >= 4) return cmdLot(argv[2], {argv + 3, argv + argc});
        DbpfPackage pkg(argv[2]);
        if (cmd == "ls") return cmdLs(pkg);
        if (cmd == "stats") return cmdStats(pkg);
        if (cmd == "verify") return cmdVerify(pkg);
        if (cmd == "decode-all") return cmdDecodeAll(pkg);
        if (cmd == "dump" && argc == 4) return cmdDump(pkg, parseTgi(argv[3]));
        if (cmd == "extract" && argc == 4) return cmdExtract(pkg, argv[3]);
        if (cmd == "texture" && argc == 5) return cmdTexture(pkg, parseTgi(argv[3]), argv[4]);
        if (cmd == "mesh" && argc == 5) return cmdMesh(pkg, parseTgi(argv[3]), argv[4]);
        return usage();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
