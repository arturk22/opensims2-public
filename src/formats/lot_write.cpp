#include "lot_write.hpp"

#include "binreader.hpp"
#include "binwriter.hpp"
#include "dbpf.hpp"

#include <algorithm>
#include <map>

namespace os2 {

namespace {

constexpr uint32_t kType3DArray = 0x2A51171B, kTypeStringMap = 0xCAC4FC40, kTypeWallGraph = 0x0A284D0B,
                   kTypeWallLayer = 0x8A84D7B0;
constexpr uint32_t kArrayFloors = 0x00, kArrayRooms = 0x09, kSmapWalls = 0x0D, kSmapFloors = 0x0E,
                   kWallLayerPartitions = 0x04;
constexpr size_t kFileNameSize = 64;

// The key, file name and version of the source's record, or the game's
// defaults for a new one.
struct Head {
    uint32_t group = 0xFFFFFFFF;
    std::vector<uint8_t> name = std::vector<uint8_t>(kFileNameSize, 0);
    uint32_t version = 1;
};

Head headOf(DbpfPackage& pkg, uint32_t type, uint32_t instance, uint32_t defaultVersion) {
    Head h;
    h.version = defaultVersion;
    for (const auto& e : pkg.entries())
        if (e.tgi.type == type && e.tgi.instance == instance) {
            const auto bytes = pkg.read(e);
            if (bytes.size() >= kFileNameSize + 8) {
                h.group = e.tgi.group;
                h.name.assign(bytes.begin(), bytes.begin() + kFileNameSize);
                h.version = le32(&bytes[kFileNameSize + 4]);
            }
            break;
        }
    return h;
}

BinWriter begin(const Head& h, uint32_t blockId, const char* blockName) {
    BinWriter w;
    w.raw(h.name);
    w.u32(blockId);
    w.u32(h.version);
    w.str7(blockName);
    return w;
}

LotGeometryRecord quarterArray(DbpfPackage& pkg, uint32_t instance, const Lot& lot,
                               const std::vector<std::array<uint16_t, 4>>& values) {
    const Head h = headOf(pkg, kType3DArray, instance, 1);
    BinWriter w = begin(h, kType3DArray, "c3DArray");
    w.u32(lot.width), w.u32(lot.height), w.u32(uint32_t(lot.levelCount));
    for (const auto& q : values)
        for (uint16_t v : q)
            w.u16(v);
    return {kType3DArray, h.group, instance, std::move(w.bytes())};
}

LotGeometryRecord stringMap(DbpfPackage& pkg, uint32_t instance, const std::map<uint16_t, std::string>& names,
                            const std::map<uint16_t, uint32_t>& uses) {
    const Head h = headOf(pkg, kTypeStringMap, instance, 1);
    BinWriter w = begin(h, kTypeStringMap, "cStringMap");
    // In name order (a map by name in the game); patterns no longer used
    // are left out.
    std::vector<std::pair<std::string, uint16_t>> entries;
    for (const auto& [ref, name] : names)
        if (auto it = uses.find(ref); it != uses.end() && it->second)
            entries.emplace_back(name, ref);
    std::sort(entries.begin(), entries.end());
    w.u32(uint32_t(entries.size()));
    for (const auto& [name, ref] : entries) {
        w.str7(name);
        w.u16(ref);
        w.u32(uses.at(ref));
    }
    return {kTypeStringMap, h.group, instance, std::move(w.bytes())};
}

} // namespace

std::vector<LotGeometryRecord> writeLotGeometry(const Lot& lot, DbpfPackage& source) {
    std::vector<LotGeometryRecord> out;
    for (const auto& g : lot.wallGraphs) {
        Head h = headOf(source, kTypeWallGraph, g.instance, g.version);
        h.version = g.version;
        BinWriter w = begin(h, kTypeWallGraph, "cWallGraph");
        for (int32_t v : g.header)
            w.i32(v);
        w.u32(uint32_t(g.vertices.size()));
        for (const auto& v : g.vertices) {
            w.u32(v.id);
            w.f32(v.x), w.f32(v.y);
            w.i32(v.level);
        }
        w.u32(uint32_t(g.rooms.size()));
        for (int32_t r : g.rooms)
            w.i32(r);
        w.u32(uint32_t(g.lines.size()));
        for (const auto& l : g.lines) {
            w.u32(l.id), w.u32(l.a);
            w.i32(l.roomRight);
            w.u32(l.b);
            w.i32(l.roomLeft);
        }
        out.push_back({kTypeWallGraph, h.group, g.instance, std::move(w.bytes())});
    }
    {
        const Head h = headOf(source, kTypeWallLayer, kWallLayerPartitions, 1);
        BinWriter w = begin(h, kTypeWallLayer, "cWallLayer");
        std::map<uint32_t, Lot::LayerEntry> layer = lot.staleWallLayer;
        for (const auto& wall : lot.walls)
            if (wall.layered)
                layer[wall.id] = {wall.type, wall.patterns};
        w.u32(uint32_t(layer.size()));
        for (const auto& [id, e] : layer) {
            w.u32(id), w.u32(e.type);
            w.u16(e.patterns[0]), w.u16(e.patterns[1]);
        }
        out.push_back({kTypeWallLayer, h.group, kWallLayerPartitions, std::move(w.bytes())});
    }
    if (!lot.floors.empty())
        out.push_back(quarterArray(source, kArrayFloors, lot, lot.floors));
    if (!lot.rooms.empty())
        out.push_back(quarterArray(source, kArrayRooms, lot, lot.rooms));
    for (const auto& [instance, values] : lot.roomLayers)
        out.push_back(quarterArray(source, instance, lot, values));
    std::map<uint16_t, uint32_t> floorUses, wallUses;
    for (const auto& q : lot.floors)
        for (uint16_t v : q)
            if (v)
                ++floorUses[v];
    for (const auto& wall : lot.walls)
        if (wall.layered)
            for (uint16_t p : wall.patterns)
                if (p != 0xFFFF)
                    ++wallUses[p];
    for (const auto& [id, e] : lot.staleWallLayer)
        for (uint16_t p : e.patterns)
            if (p != 0xFFFF)
                ++wallUses[p];
    out.push_back(stringMap(source, kSmapWalls, lot.wallPatterns, wallUses));
    out.push_back(stringMap(source, kSmapFloors, lot.floorPatterns, floorUses));
    return out;
}

} // namespace os2
