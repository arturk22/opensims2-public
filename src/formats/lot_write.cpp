#include "lot_write.hpp"

#include "binreader.hpp"
#include "binwriter.hpp"
#include "dbpf.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace os2 {

namespace {

constexpr uint32_t kTypeRoof = 0xAB9406AA, kTypePool = 0x0C900FDB;
constexpr uint32_t kTypeFencePosts = 0xAB4BA572, kFencePostLayer = 0x11, kTypeLotTexture = 0x4B58975B,
                   kType2DArray = 0x6B943B43, kPaintMaskFirst = 0x5CBC, kWaterLevels = 0x3B76;
constexpr uint32_t kType3DArray = 0x2A51171B, kTypeStringMap = 0xCAC4FC40, kTypeWallGraph = 0x0A284D0B,
                   kTypeWallLayer = 0x8A84D7B0;
constexpr uint32_t kArrayFloors = 0x00, kArrayElevation = 0x01, kArrayRooms = 0x09, kArrayFloorRotations = 0x15, kArrayDeckFoundations = 0x14, kSmapWalls = 0x0D, kSmapFloors = 0x0E,
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

LotGeometryRecord elevationArray(DbpfPackage& pkg, const Lot& lot) {
    const Head h = headOf(pkg, kType3DArray, kArrayElevation, 1);
    BinWriter w = begin(h, kType3DArray, "c3DArray");
    w.u32(lot.width + 1), w.u32(lot.height + 1), w.u32(uint32_t(lot.levelCount));
    for (float v : lot.elevations)
        w.f32(v);
    return {kType3DArray, h.group, kArrayElevation, std::move(w.bytes())};
}

LotGeometryRecord rotationArray(DbpfPackage& pkg, const Lot& lot) {
    const Head h = headOf(pkg, kType3DArray, kArrayFloorRotations, 1);
    BinWriter w = begin(h, kType3DArray, "c3DArray");
    w.u32(lot.width), w.u32(lot.height), w.u32(uint32_t(lot.levelCount));
    for (const auto& q : lot.floorRotations)
        for (uint8_t v : q)
            w.u8(v);
    return {kType3DArray, h.group, kArrayFloorRotations, std::move(w.bytes())};
}

// 3ARY 0x14: the deck/foundation/pool markers, four i32 a tile, one plane a
// level from the lowest to the one under the top (Lot::deckFoundationStyles).
LotGeometryRecord markerArray(DbpfPackage& pkg, const Lot& lot) {
    const Head h = headOf(pkg, kType3DArray, kArrayDeckFoundations, 1);
    BinWriter w = begin(h, kType3DArray, "c3DArray");
    const size_t tiles = size_t(lot.width) * lot.height;
    w.u32(lot.width), w.u32(lot.height), w.u32(uint32_t(tiles ? lot.deckFoundationStyles.size() / tiles : 0));
    for (const auto& q : lot.deckFoundationStyles)
        for (int32_t v : q)
            w.i32(v);
    return {kType3DArray, h.group, kArrayDeckFoundations, std::move(w.bytes())};
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
    {
        // FPL 0x11 (cFencePostLayer): the posts, only when the lot has one
        // or has posts now.
        const Head h = headOf(source, kTypeFencePosts, kFencePostLayer, 1);
        bool had = false;
        for (const auto& e : source.entries())
            had |= e.tgi.type == kTypeFencePosts && e.tgi.instance == kFencePostLayer;
        if (had || !lot.fencePosts.empty()) {
            BinWriter w = begin(h, kTypeFencePosts, "cFencePostLayer");
            w.u32(uint32_t(lot.fencePosts.size()));
            for (const auto& p : lot.fencePosts) {
                w.i32(p.level);
                w.f32(p.x), w.f32(p.y);
                w.u32(p.fence);
            }
            out.push_back({kTypeFencePosts, h.group, kFencePostLayer, std::move(w.bytes())});
        }
    }
    // LTTX 0 (the terrain's base and paints) and a 2ARY mask per paint
    // (0x5CBC + i), when the lot has them.
    if (!lot.terrainBase.empty() || !lot.paints.empty()) {
        Head h = headOf(source, kTypeLotTexture, 0, 7);
        BinWriter w;
        w.raw(h.name);
        w.u32(kTypeLotTexture);
        w.u32(h.version);
        w.u32(lot.width), w.u32(lot.height);
        w.str7(lot.terrainBase);
        w.u32(uint32_t(lot.paints.size()));
        for (const auto& p : lot.paints)
            w.str7(p.texture);
        out.push_back({kTypeLotTexture, h.group, 0, std::move(w.bytes())});
        for (size_t i = 0; i < lot.paints.size(); ++i) {
            const uint32_t instance = kPaintMaskFirst + uint32_t(i);
            const Head m = headOf(source, kType2DArray, instance, 1);
            BinWriter b = begin(m, kType2DArray, "c2DArray");
            b.u32(lot.maskWidth), b.u32(lot.maskHeight);
            for (uint8_t v : lot.paints[i].mask)
                b.u8(v);
            out.push_back({kType2DArray, m.group, instance, std::move(b.bytes())});
        }
    }
    // 2ARY 0x3B76: the terrain's water level at every grid point ([y][x]),
    // when the lot has it (ponds: the water tool sets it).
    if (lot.water.size() == size_t(lot.width + 1) * size_t(lot.height + 1)) {
        const Head m = headOf(source, kType2DArray, kWaterLevels, 1);
        BinWriter b = begin(m, kType2DArray, "c2DArray");
        b.u32(lot.width + 1), b.u32(lot.height + 1);
        for (float v : lot.water)
            b.f32(v);
        out.push_back({kType2DArray, m.group, kWaterLevels, std::move(b.bytes())});
    }
    {
        // ROOF 0 (OpenTS2's RoofCodec layout, no block name), when the lot
        // has one or has roofs now.
        bool had = false;
        for (const auto& e : source.entries())
            had |= e.tgi.type == kTypeRoof && e.tgi.instance == 0;
        if (had || !lot.roofs.empty()) {
            const Head h = headOf(source, kTypeRoof, 0, 1);
            BinWriter w;
            w.raw(h.name);
            w.u32(kTypeRoof);
            w.u32(h.version);
            w.u32(uint32_t(lot.roofs.size()));
            for (const auto& f : lot.roofs) {
                w.u32(f.id);
                w.f32(f.x0), w.f32(f.y0), w.i32(f.level);
                w.f32(f.x1), w.f32(f.y1), w.i32(f.levelTo);
                w.u32(f.type);
                w.u32(f.pattern);
                if (h.version >= 2)
                    w.f32(f.angle);
                if (h.version >= 3)
                    w.u32(f.style);
            }
            out.push_back({kTypeRoof, h.group, 0, std::move(w.bytes())});
        }
    }
    {
        // POOL 0 (OpenTS2's PoolCodec layout, no block name).
        bool had = false;
        for (const auto& e : source.entries())
            had |= e.tgi.type == kTypePool && e.tgi.instance == 0;
        if (had || !lot.pools.empty()) {
            const Head h = headOf(source, kTypePool, 0, 1);
            BinWriter w;
            w.raw(h.name);
            w.u32(kTypePool);
            w.u32(h.version);
            w.u32(uint32_t(lot.pools.size()));
            for (const auto& p : lot.pools) {
                w.u32(p.id);
                w.f32(p.x), w.f32(p.y);
                w.i32(p.level);
                w.u32(p.width), w.u32(p.height);
                w.u32(p.unknown1);
                w.f32(p.depth);
                w.u32(p.unknown2);
            }
            out.push_back({kTypePool, h.group, 0, std::move(w.bytes())});
        }
    }
    // The lot's other 3ARYs (0x03, 0x0C, ...: not read; 0x14 when the lot
    // has no markers read) get the
    // levels added below (a pool's): a copy of their lowest plane each
    // (inferred), so that every array keeps the lot's level count.
    {
        uint32_t sourceLevels = 0;
        for (const auto& e : source.entries())
            if (e.tgi.type == kType3DArray && e.tgi.instance == kArrayElevation) {
                const auto b = source.read(e);
                BinReader r(b);
                r.skip(kFileNameSize + 8);
                r.str7();
                r.u32(), r.u32();
                sourceLevels = r.u32();
            }
        const int added = sourceLevels ? lot.levelCount - int(sourceLevels) : 0;
        if (added > 0)
            for (const auto& e : source.entries()) {
                if (e.tgi.type != kType3DArray || e.tgi.instance == kArrayElevation || e.tgi.instance == kArrayFloors ||
                    e.tgi.instance == kArrayRooms || e.tgi.instance == kArrayFloorRotations ||
                    (e.tgi.instance == kArrayDeckFoundations && !lot.deckFoundationStyles.empty()) ||
                    lot.roomLayers.count(e.tgi.instance))
                    continue;
                const auto b = source.read(e);
                BinReader r(b);
                r.skip(kFileNameSize + 8);
                r.str7();
                const uint32_t x = r.u32(), y = r.u32(), z = r.u32();
                const size_t head = r.pos(), plane = b.size() > head && z ? (b.size() - head) / z : 0;
                if (!z || plane == 0 || head + plane * z != b.size())
                    continue;
                std::vector<uint8_t> bytes(b.begin(), b.begin() + long(head));
                const uint32_t nz = z + uint32_t(added);
                std::memcpy(&bytes[head - 4], &nz, 4); // little-endian, as the source
                for (int k = 0; k < added; ++k)
                    bytes.insert(bytes.end(), b.begin() + long(head), b.begin() + long(head + plane));
                bytes.insert(bytes.end(), b.begin() + long(head), b.end());
                (void)x, (void)y;
                out.push_back({kType3DArray, e.tgi.group, e.tgi.instance, std::move(bytes)});
            }
    }
    if (!lot.elevations.empty())
        out.push_back(elevationArray(source, lot));
    if (!lot.floors.empty())
        out.push_back(quarterArray(source, kArrayFloors, lot, lot.floors));
    if (!lot.floorRotations.empty())
        out.push_back(rotationArray(source, lot));
    if (!lot.deckFoundationStyles.empty())
        out.push_back(markerArray(source, lot));
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
