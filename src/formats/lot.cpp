#include "lot.hpp"

#include "binreader.hpp"
#include "dbpf.hpp"

#include <algorithm>
#include <optional>

namespace os2 {

namespace {

constexpr uint32_t kType2DArray = 0x6B943B43, kType3DArray = 0x2A51171B, kTypeStringMap = 0xCAC4FC40,
                   kTypeWallGraph = 0x0A284D0B, kTypeWallLayer = 0x8A84D7B0, kTypeLotTexture = 0x4B58975B,
                   kTypeObject = 0xFA1C39F7;
constexpr uint32_t kArrayFloors = 0x00, kArrayElevation = 0x01, kSmapWalls = 0x0D, kSmapFloors = 0x0E,
                   kWallLayerPartitions = 0x04, kPaintMaskFirst = 0x5CBC;
constexpr size_t kFileNameSize = 64;

// Every lot resource: 64-byte file name, block id, version.
BinReader blockReader(const std::vector<uint8_t>& bytes, uint32_t blockId, uint32_t* version = nullptr) {
    BinReader r(bytes);
    r.skip(kFileNameSize);
    const uint32_t id = r.u32();
    if (id != blockId)
        throw FormatError("block id " + hexString(id) + ", expected " + hexString(blockId));
    const uint32_t v = r.u32();
    if (version)
        *version = v;
    return r;
}

void expectName(BinReader& r, const char* name) {
    const std::string n = r.str7();
    if (n != name)
        throw FormatError("block name \"" + n + "\", expected \"" + name + "\"");
}

struct Array3 {
    uint32_t x = 0, y = 0, z = 0;
    std::span<const uint8_t> data;
};

Array3 readArray3(BinReader& r, size_t elementSize) {
    expectName(r, "c3DArray");
    Array3 a;
    a.x = r.u32(), a.y = r.u32(), a.z = r.u32();
    a.data = r.bytes(size_t(a.x) * a.y * a.z * elementSize);
    return a;
}

std::map<uint16_t, std::string> readStringMap(BinReader& r) {
    expectName(r, "cStringMap");
    std::map<uint16_t, std::string> out;
    for (uint32_t n = r.u32(); n; --n) {
        std::string s = r.str7();
        const uint16_t ref = r.u16();
        r.u32(); // usage count
        out[ref] = std::move(s);
    }
    return out;
}

std::optional<std::vector<uint8_t>> readResource(DbpfPackage& pkg, uint32_t type, uint32_t instance) {
    for (const auto& e : pkg.entries())
        if (e.tgi.type == type && e.tgi.instance == instance)
            return pkg.read(e);
    return std::nullopt;
}

// cObject, possibly wrapped by subclass blocks (cAnimatable, cLocomotable,
// cPerson: id, version, name, outermost first) whose own data follows it.
Lot::Object readObject(const std::vector<uint8_t>& bytes) {
    BinReader r(bytes);
    r.skip(kFileNameSize);
    Lot::Object o;
    uint32_t version = 0;
    for (int depth = 0;; ++depth) {
        if (depth == 8)
            throw FormatError("no cObject block");
        r.u32();
        version = r.u32();
        const std::string cls = r.str7();
        if (cls == "cPerson")
            o.person = true;
        if (cls == "cObject")
            break;
    }
    o.model = r.str7();
    // Models with their material choices; a Sim lists one per body part, an
    // object at most one (the same name again).
    for (uint32_t n = r.u32(); n; --n) {
        r.str7();
        if (version >= 0x11)
            r.u32(); // 12 for objects, 10 for Sims
        for (uint32_t m = r.u32(); m; --m) {
            std::string material = r.str7();
            o.materials[r.str7()] = std::move(material);
        }
    }
    for (auto& v : o.position)
        v = r.f32();
    for (auto& v : o.rotation)
        v = r.f32();
    r.u32();
    o.definition = r.str7();
    // Then the position and rotation again, and the subclass data.
    return o;
}

} // namespace

float Lot::elevation(int level, int x, int y) const {
    const int w = int(width) + 1, h = int(height) + 1;
    if (!hasLevel(level) || elevations.empty())
        return 3.0f * float(level);
    x = std::clamp(x, 0, w - 1), y = std::clamp(y, 0, h - 1);
    return elevations[(size_t(level - minLevel) * w + size_t(x)) * h + size_t(y)];
}

const std::array<uint16_t, 4>& Lot::floor(int level, uint32_t x, uint32_t y) const {
    static const std::array<uint16_t, 4> none{};
    if (!hasLevel(level) || x >= width || y >= height || floors.empty())
        return none;
    return floors[(size_t(level - minLevel) * width + x) * height + y];
}

Lot Lot::load(DbpfPackage& pkg) {
    Lot lot;

    // Wall graph first: it holds the lowest level. Of its instances (walls
    // flagged for light blocking, load bearing, ...) the largest lists all of them.
    struct Graph {
        std::map<uint32_t, WallVertex> vertices;
        std::vector<std::array<uint32_t, 3>> walls; // id, vertex a, vertex b
    };
    std::optional<Graph> graph;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != kTypeWallGraph)
            continue;
        try {
            const auto bytes = pkg.read(e);
            BinReader r = blockReader(bytes, kTypeWallGraph);
            expectName(r, "cWallGraph");
            r.skip(8);
            const int minLevel = r.i32();
            r.skip(20); // width, height (version-dependent units), max level, two unknowns
            Graph g;
            for (uint32_t n = r.u32(); n; --n) {
                const uint32_t id = r.u32();
                WallVertex v;
                v.x = r.f32(), v.y = r.f32(), v.level = r.i32();
                g.vertices[id] = v;
            }
            r.skip(size_t(r.u32()) * 4); // room ids
            for (uint32_t n = r.u32(); n; --n) {
                const uint32_t id = r.u32(), a = r.u32();
                r.u32();
                const uint32_t b = r.u32();
                r.u32();
                g.walls.push_back({id, a, b});
            }
            lot.minLevel = minLevel;
            if (!graph || g.walls.size() > graph->walls.size())
                graph = std::move(g);
        } catch (const std::exception& ex) {
            lot.warnings.push_back("wall graph " + hexString(e.tgi.instance) + ": " + ex.what());
        }
    }

    if (auto bytes = readResource(pkg, kType3DArray, kArrayElevation)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 4);
        if (a.x < 2 || a.y < 2)
            throw FormatError("elevation array " + std::to_string(a.x) + "x" + std::to_string(a.y));
        lot.width = a.x - 1, lot.height = a.y - 1, lot.levelCount = int(a.z);
        lot.elevations.resize(size_t(a.x) * a.y * a.z);
        for (size_t i = 0; i < lot.elevations.size(); ++i)
            lot.elevations[i] = std::bit_cast<float>(le32(&a.data[i * 4]));
    } else {
        throw FormatError("no elevation array (3ARY 0x01): not a lot package?");
    }

    if (auto bytes = readResource(pkg, kType3DArray, kArrayFloors)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 8);
        if (a.x != lot.width || a.y != lot.height || int(a.z) != lot.levelCount) {
            lot.warnings.push_back("floor array size does not match the elevations");
        } else {
            lot.floors.resize(size_t(a.x) * a.y * a.z);
            for (size_t i = 0; i < lot.floors.size(); ++i)
                for (int q = 0; q < 4; ++q)
                    lot.floors[i][size_t(q)] = le16(&a.data[i * 8 + size_t(q) * 2]);
        }
    }

    for (auto [instance, out] : {std::pair{kSmapFloors, &lot.floorPatterns}, std::pair{kSmapWalls, &lot.wallPatterns}})
        if (auto bytes = readResource(pkg, kTypeStringMap, instance)) {
            BinReader r = blockReader(*bytes, kTypeStringMap);
            *out = readStringMap(r);
        }

    if (graph) {
        std::map<uint32_t, std::pair<uint32_t, std::array<uint16_t, 2>>> layer; // id -> type, coverings
        if (auto bytes = readResource(pkg, kTypeWallLayer, kWallLayerPartitions)) {
            BinReader r = blockReader(*bytes, kTypeWallLayer);
            expectName(r, "cWallLayer");
            for (uint32_t n = r.u32(); n; --n) {
                const uint32_t id = r.u32(), type = r.u32();
                const uint16_t right = r.u16(), left = r.u16();
                layer[id] = {type, {right, left}};
            }
        }
        size_t missing = 0;
        for (const auto& [id, a, b] : graph->walls) {
            auto va = graph->vertices.find(a), vb = graph->vertices.find(b);
            if (va == graph->vertices.end() || vb == graph->vertices.end()) {
                ++missing;
                continue;
            }
            Wall w;
            w.id = id;
            w.a = va->second, w.b = vb->second;
            if (auto it = layer.find(id); it != layer.end())
                w.type = it->second.first, w.patterns = it->second.second;
            lot.walls.push_back(w);
        }
        if (missing)
            lot.warnings.push_back(std::to_string(missing) + " walls with unknown vertices");
    }

    if (auto bytes = readResource(pkg, kTypeLotTexture, 0)) {
        BinReader r = blockReader(*bytes, kTypeLotTexture);
        r.skip(8); // width, height in tiles
        lot.terrainBase = r.str7();
        const uint32_t n = r.u32();
        for (uint32_t i = 0; i < n; ++i) {
            TerrainPaint p;
            p.texture = r.str7();
            if (auto mask = readResource(pkg, kType2DArray, kPaintMaskFirst + i)) {
                BinReader m = blockReader(*mask, kType2DArray);
                expectName(m, "c2DArray");
                const uint32_t w = m.u32(), h = m.u32();
                if (lot.maskWidth && (w != lot.maskWidth || h != lot.maskHeight)) {
                    lot.warnings.push_back("paint mask " + p.texture + " has a different size");
                    continue;
                }
                lot.maskWidth = w, lot.maskHeight = h;
                const auto values = m.bytes(size_t(w) * h);
                p.mask.assign(values.begin(), values.end());
            } else {
                lot.warnings.push_back("no mask for terrain paint " + p.texture);
                continue;
            }
            lot.paints.push_back(std::move(p));
        }
    }

    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != kTypeObject)
            continue;
        try {
            Object o = readObject(pkg.read(e));
            o.instance = e.tgi.instance;
            lot.objects.push_back(std::move(o));
        } catch (const std::exception& ex) {
            lot.warnings.push_back("object " + hexString(e.tgi.instance) + ": " + ex.what());
        }
    }
    std::sort(lot.objects.begin(), lot.objects.end(),
              [](const Object& a, const Object& b) { return a.instance < b.instance; });
    return lot;
}

} // namespace os2
