// Placing, moving and binning lots (hood_lots.hpp).

#include "hood_lots.hpp"
#include "formats/log_once.hpp"

#include "dbpf.hpp"
#include "hood_moves.hpp"
#include "lot_objects.hpp"
#include "property_set.hpp"
#include "resindex.hpp"
#include "strings.hpp"

#include <algorithm>
#include <cfloat>
#include <bit>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>
#include "port/files.hpp"

namespace os2::hood {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTypeBaseLotInfo = 0x6C589723, kTypeStr = 0x53545223;
// cTSUserToolPlaceNewLot::Init's size tables (BV 0x2733010 / 0x2732fc0),
// the first ten (the panel's buttons; 10-19 are expansion sizes).
constexpr int kSizeW[kEmptyLotSizes] = {2, 3, 4, 3, 4, 5, 3, 5, 4, 5};
constexpr int kSizeD[kEmptyLotSizes] = {3, 3, 3, 4, 4, 3, 5, 4, 5, 5};
const char* const kSkirt = "lottexture-test-01"; // every shipped lot's

bool under(const fs::path& path, const fs::path& dir) {
    if (dir.empty())
        return false;
    const std::string d = fs::weakly_canonical(fs::absolute(dir)).string();
    const std::string p = fs::weakly_canonical(fs::absolute(path)).string();
    if (p == d)
        return true;
    if (p.size() <= d.size() || p.compare(0, d.size(), d) != 0)
        return false;
    const char sep = p[d.size()];
    return sep == '/' || sep == '\\';
}

void checkFolder(const fs::path& folder, const fs::path& forbidden) {
    if (under(folder, forbidden))
        throw std::runtime_error("refusing to write into the game's data (" + folder.string() + ")");
}

using Key = std::pair<uint32_t, uint32_t>;
void rewritePackage(const fs::path& file, const std::map<Key, std::vector<uint8_t>>& replace, const std::set<Key>& drop,
                    uint32_t addGroup = 0xFFFFFFFF) {
    const fs::path tmp = file.parent_path() / (file.filename().string() + ".saving");
    {
        DbpfPackage pkg(file);
        DbpfWriter w;
        std::set<Key> done;
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type == type::DIR)
                continue;
            const Key key{e.tgi.type, e.tgi.instance};
            if (drop.count(key))
                continue;
            if (auto it = replace.find(key); it != replace.end())
                w.add(e.tgi, it->second, e.compressed()), done.insert(key);
            else
                w.addStored(e.tgi, pkg.readRaw(e), e.rawSize);
        }
        for (const auto& [key, bytes] : replace)
            if (!done.count(key))
                w.add(Tgi{key.first, addGroup, key.second, 0}, bytes, true);
        pkg.closeFile();
        w.save(tmp);
    }
    os2::replaceFile(tmp, file);
}

struct RoadTile {
    int x = 0, y = 0;
};

std::vector<RoadTile> roadTiles(const Occupants& o) {
    std::vector<RoadTile> out;
    auto add = [&](const Road& r) {
        float x = INFINITY, y = INFINITY;
        for (const auto& c : r.corners)
            x = std::min(x, c[0]), y = std::min(y, c[1]);
        out.push_back({int(std::floor(x / kHoodCellSize + 0.01f)), int(std::floor(y / kHoodCellSize + 0.01f))});
    };
    for (const auto& r : o.roads)
        add(r);
    for (const auto& b : o.bridges)
        add(b.road);
    return out;
}

// cTSNHoodOccupant's tile rectangle (vtable 0x20, BV 0xb1ae28): its bounds
// over 10, the low corner floored unless less than 0.07 of a tile is left
// in the first tile, the high one ceiled likewise (FLOAT_0274bbf4 = 0.93).
TileRect occupantTiles(const OccupantPosition& at) {
    auto lo = [](float v) {
        int n = int(std::floor(v));
        return v - float(n) > 0.93f ? n + 1 : n;
    };
    auto hi = [](float v) {
        int n = int(std::ceil(v));
        return float(n) - v > 0.93f ? n - 1 : n;
    };
    return {lo(at.boundsMin[0] / kHoodCellSize), lo(at.boundsMin[1] / kHoodCellSize),
            hi(at.boundsMax[0] / kHoodCellSize), hi(at.boundsMax[1] / kHoodCellSize)};
}

constexpr float kRoadOverTerrain = 0.4f; // cTSNHoodRoadOccupant::FlattenToTerrain

bool overlaps(const TileRect& a, const TileRect& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

} // namespace

TileRect lotRect(const LotInfo& info) {
    return {int(info.x), int(info.y), int(info.x + info.currentWidth()), int(info.y + info.currentDepth())};
}

EmptyLotSize emptyLotSize(int index) {
    index = std::clamp(index, 0, kEmptyLotSizes - 1);
    return {kSizeW[index], kSizeD[index]};
}

TileRect emptyLotRect(EmptyLotSize size, int x, int y, int rotation) {
    const bool odd = rotation & 1;
    TileRect r{x, y, x + (odd ? size.depth : size.width), y + (odd ? size.width : size.depth)};
    switch (rotation & 3) {
    case 0: --r.x0; break;
    case 1: ++r.y1; break;
    case 2: ++r.x1; break;
    default: --r.y0; break;
    }
    return r;
}

Placement checkPlacement(const PlacementWorld& world, const TileRect& R, uint32_t self, const LotInfo* beach) {
    Placement out;
    const Terrain& t = *world.terrain;
    auto fail = [&](const char* why) {
        out.why = why;
        return out;
    };
    // 1. The corners on the terrain.
    if (R.x0 < 0 || R.y0 < 0 || R.x1 > int(t.width) || R.y1 > int(t.height) || R.width() < 1 || R.depth() < 1)
        return fail("off the terrain");
    const auto roads = roadTiles(*world.occupants);
    std::set<std::pair<int, int>> roadSet;
    for (const auto& r : roads)
        roadSet.insert({r.x, r.y});
    // 2. Other lots: none over the rectangle less a tile all round, and no
    // tile of another lot without a road on it.
    const TileRect inner{R.x0 + 1, R.y0 + 1, R.x1 - 1, R.y1 - 1};
    for (const auto& l : world.hood->lots) {
        if (l.id == self)
            continue;
        const TileRect o = lotRect(l.info);
        if (inner.width() > 0 && inner.depth() > 0 && overlaps(o, inner))
            return fail("another lot");
        for (int y = std::max(R.y0, o.y0); y < std::min(R.y1, o.y1); ++y)
            for (int x = std::max(R.x0, o.x0); x < std::min(R.x1, o.x1); ++x)
                if (!roadSet.count({x, y}))
                    return fail("another lot");
    }
    // 3. AreBlockingObjectsPresent (BV 0xa271cc): the occupants
    // GetOccupants returns (their tile rectangle overlaps the lot's,
    // strictly) whose bounds touch the lot's world rectangle (vtable 0x24,
    // inclusive) and that may not be in a lot (vtable 0x34 IsAllowedInLot:
    // the product's allowedinlot).
    const float wx0 = float(R.x0) * kHoodCellSize, wy0 = float(R.y0) * kHoodCellSize;
    const float wx1 = float(R.x1) * kHoodCellSize, wy1 = float(R.y1) * kHoodCellSize;
    auto blocks = [&](const OccupantPosition& at, uint32_t guid) {
        if (!overlaps(R, occupantTiles(at)))
            return false;
        if (at.boundsMax[0] < wx0 || at.boundsMin[0] > wx1 || at.boundsMax[1] < wy0 || at.boundsMin[1] > wy1)
            return false;
        auto it = world.objects.find(guid);
        return it == world.objects.end() || !it->second.allowedInLot;
    };
    auto culprit = [&](const OccupantPosition& at, uint32_t guid, const char* what) {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "objects in the way (%s 0x%08x at (%.1f, %.1f), bounds (%.1f, %.1f)-(%.1f, %.1f), lot (%.0f, "
                      "%.0f)-(%.0f, %.0f))",
                      what, guid, at.position[0], at.position[1], at.boundsMin[0], at.boundsMin[1], at.boundsMax[0],
                      at.boundsMax[1], wx0, wy0, wx1, wy1);
        out.why = buf;
        return out;
    };
    for (const auto& f : world.occupants->flora)
        if (blocks(f.at, f.guid))
            return culprit(f.at, f.guid, "flora");
    for (const auto& p : world.occupants->props)
        if (blocks(p.at, p.guid))
            return culprit(p.at, p.guid, "prop");
    // 4. Water (not a beach lot).
    for (int y = R.y0; y <= R.y1 && !beach; ++y)
        for (int x = R.x0; x <= R.x1; ++x)
            if (t.vertex(x, y) < t.seaLevel)
                return fail("water");
    // 5. A road there (GetOccupants of type 2), and not too steep.
    std::vector<const Road*> roadOccs;
    for (const auto& r : world.occupants->roads)
        if (overlaps(R, occupantTiles(r.at)))
            roadOccs.push_back(&r);
    for (const auto& br : world.occupants->bridges)
        if (overlaps(R, occupantTiles(br.road.at)))
            roadOccs.push_back(&br.road);
    if (roadOccs.empty())
        return fail("no road");
    {
        float lo = INFINITY, hi = -INFINITY;
        int lx = 0, ly = 0, hx = 0, hy = 0;
        for (int y = R.y0; y <= R.y1; ++y)
            for (int x = R.x0; x <= R.x1; ++x) {
                const float h = t.vertex(x, y);
                if (h < lo)
                    lo = h, lx = x, ly = y;
                if (h > hi)
                    hi = h, hx = x, hy = y;
            }
        const int dx = hx - lx, dy = hy - ly;
        if ((dx || dy) && (hi - lo) / (std::sqrt(float(dx * dx + dy * dy)) * 10.0f) > world.maxSlope)
            return fail("too steep");
    }
    // 6. The roads, clipped to the rectangle: each must touch its ring of
    // edge tiles and have two connections at least (the road's connection
    // bits, vtable 0x88: byte +0x86), else the lot may not go; each marks
    // the edge tiles it covers.
    std::array<std::vector<bool>, 4> edge;
    edge[0].assign(size_t(R.depth()), false), edge[2].assign(size_t(R.depth()), false);
    edge[1].assign(size_t(R.width()), false), edge[3].assign(size_t(R.width()), false);
    for (const Road* road : roadOccs) {
        const TileRect o = occupantTiles(road->at);
        const TileRect c{std::max(o.x0, R.x0), std::max(o.y0, R.y0), std::min(o.x1, R.x1), std::min(o.y1, R.y1)};
        if (c.width() <= 0 || c.depth() <= 0)
            continue;
        const bool ring = c.x0 >= R.x1 - 1 || c.x1 <= R.x0 + 1 || c.y0 >= R.y1 - 1 || c.y1 <= R.y0 + 1;
        if (!ring)
            return fail("a road through the lot");
        if (std::popcount(unsigned(road->connections & 0xF)) < 2)
            return fail("a road end on the lot");
        if (c.x0 == R.x0)
            for (int k = c.y0 - R.y0; k < c.y1 - R.y0; ++k)
                edge[0][size_t(k)] = true;
        if (c.y1 == R.y1)
            for (int k = c.x0 - R.x0; k < c.x1 - R.x0; ++k)
                edge[1][size_t(k)] = true;
        if (c.x1 == R.x1)
            for (int k = R.y1 - c.y1; k < R.y1 - c.y0; ++k)
                edge[2][size_t(k)] = true;
        if (c.y0 == R.y0)
            for (int k = R.x1 - c.x1; k < R.x1 - c.x0; ++k)
                edge[3][size_t(k)] = true;
    }
    bool anyFull = false;
    std::array<size_t, 4> count{};
    for (int i = 0; i < 4; ++i) {
        count[size_t(i)] = size_t(std::count(edge[size_t(i)].begin(), edge[size_t(i)].end(), true));
        out.roads[size_t(i)] = count[size_t(i)] == edge[size_t(i)].size() && !edge[size_t(i)].empty();
        anyFull = anyFull || out.roads[size_t(i)];
        if (!out.roads[size_t(i)])
            for (size_t k = 1; k + 1 < edge[size_t(i)].size(); ++k)
                if (edge[size_t(i)][k])
                    return fail("roads beside the lot");
    }
    if (!anyFull)
        return fail("no road along an edge");
    for (int i = 0; i < 4; ++i) {
        const auto& e = edge[size_t(i)];
        if (out.roads[size_t(i)] || !count[size_t(i)])
            continue;
        if ((e.front() && !out.roads[size_t((i + 3) & 3)]) || (e.back() && !out.roads[size_t((i + 1) & 3)]))
            return fail("roads beside the lot");
    }
    for (int i = 0; i < 4; ++i)
        if (out.roads[size_t(i)]) {
            out.front = i;
            break;
        }
    // Each road edge's height (this+0x6c, DetermineFlattenHeightForRoadOccs):
    // here the terrain under its road tiles, their corners' mean height less
    // the 0.4 roads float over the terrain (FlattenToTerrain,
    // FLOAT_0274b4b0); the shipped lots' elevations are exactly that.
    for (int e = 0; e < 4; ++e) {
        if (!out.roads[size_t(e)])
            continue;
        float sum = 0;
        int n = 0;
        for (const Road* r : roadOccs) {
            const TileRect o = occupantTiles(r->at);
            bool on = false;
            switch (e) {
            case 0: on = o.x0 == R.x0 && o.y0 >= R.y0 && o.y1 <= R.y1; break;
            case 1: on = o.y1 == R.y1 && o.x0 >= R.x0 && o.x1 <= R.x1; break;
            case 2: on = o.x1 == R.x1 && o.y0 >= R.y0 && o.y1 <= R.y1; break;
            default: on = o.y0 == R.y0 && o.x0 >= R.x0 && o.x1 <= R.x1; break;
            }
            if (on)
                for (const auto& c : r->corners)
                    sum += c[2], ++n;
        }
        out.edgeHeight[size_t(e)] = n ? sum / float(n) - kRoadOverTerrain : t.heightAt((wx0 + wx1) / 2, (wy0 + wy1) / 2);
    }
    out.roadHeight = out.edgeHeight[size_t(out.front)];
    // 7. IsOKToPlaceBeachLot (BV 0xa2687c): the last road edge's height at
    // most 10 over the sea, within 1.0 of the base lot's road height, and a
    // vertex at or under the sea on the edge across from that road.
    if (beach) {
        int e = -1;
        for (int i = 0; i < 4; ++i)
            if (out.roads[size_t(i)])
                e = i;
        const float h = out.edgeHeight[size_t(e)];
        if (h - t.seaLevel > 10.0f || std::fabs(h - beach->roadHeight) > 1.0f)
            return fail("not a beach");
        bool wet = false;
        for (int k = (e == 1 || e == 3) ? R.x0 : R.y0; k <= ((e == 1 || e == 3) ? R.x1 : R.y1) && !wet; ++k) {
            const float v = e == 1 ? t.vertex(k, R.y0) : e == 3 ? t.vertex(k, R.y1) : e == 0 ? t.vertex(R.x1, k)
                                                                                              : t.vertex(R.x0, k);
            wet = v <= t.seaLevel;
        }
        if (!wet)
            return fail("not a beach");
    }
    out.ok = true;
    return out;
}

void applyTunables(PlacementWorld& world, const TerrainSettings& settings) {
    world.maxSlope = std::tan(settings.maxLotSlope * 3.14f / 180.0f);
    world.talusAngle = settings.smoothingTalusAngle;
    world.smoothingPasses = settings.smoothingPasses;
}

namespace {

// The first lot over `r` (cTSLotManager vtable 0xec; inferred: sharing a tile).
const Lot* lotOver(const PlacementWorld& world, const TileRect& r, uint32_t skip = 0) {
    for (const auto& l : world.hood->lots)
        if (l.id != skip && overlaps(lotRect(l.info), r))
            return &l;
    return nullptr;
}

std::vector<const Road*> roadsOver(const Occupants& o, const TileRect& r) {
    std::vector<const Road*> out;
    for (const auto& road : o.roads)
        if (overlaps(r, occupantTiles(road.at)))
            out.push_back(&road);
    for (const auto& b : o.bridges)
        if (overlaps(r, occupantTiles(b.road.at)))
            out.push_back(&b.road);
    return out;
}

// cTSNHoodRoadOccupant::GetSameHeightVertices (BV 0xb1ac10): of the road's
// corners (x0,y0), (x0,y1), (x1,y1), (x1,y0), the ones `v` must stay level
// with: the other three on an intersection or bend (connections on both
// axes), the one across the road on a straight piece.
std::vector<std::pair<int, int>> sameHeightVertices(const Road& road, std::pair<int, int> v) {
    const TileRect t = occupantTiles(road.at);
    const std::array<std::pair<int, int>, 4> c{{{t.x0, t.y0}, {t.x0, t.y1}, {t.x1, t.y1}, {t.x1, t.y0}}};
    int i = 0;
    while (i < 4 && c[size_t(i)] != v)
        ++i;
    if (i == 4)
        return {};
    const bool along02 = road.connections & 0x5, along13 = road.connections & 0xA;
    std::vector<std::pair<int, int>> out;
    if (along02 && along13) {
        for (int k = 0; k < 4; ++k)
            if (k != i)
                out.push_back(c[size_t(k)]);
    } else if (along02) {
        constexpr int kAcross[4] = {1, 0, 3, 2}; // DAT_02738d90
        out.push_back(c[size_t(kAcross[i])]);
    } else if (along13) {
        constexpr int kAcross[4] = {3, 2, 1, 0}; // DAT_02738d80
        out.push_back(c[size_t(kAcross[i])]);
    }
    return out;
}

} // namespace

TerrainEdit conformTerrainToLot(const PlacementWorld& world, const LotInfo& base, uint32_t self, int x, int y,
                                int front, const Placement& placement) {
    TerrainEdit out;
    const Terrain& t = *world.terrain;
    const int W = int(base.width), D = int(base.depth);
    if (base.heights.size() != size_t(W + 1) * size_t(D + 1)) {
        out.why = "the lot has no height per vertex";
        return out;
    }
    // The road strip along the creation front edge (base vertices).
    int su0 = 0, su1 = W, sv0 = 0, sv1 = D;
    switch (base.creationFrontEdge & 3) {
    case 0: su1 = 1; break;
    case 1: sv0 = D - 1; break;
    case 2: su0 = W - 1; break;
    default: sv1 = 1; break;
    }
    const float offset = placement.heightAt(front);
    const int turn = (front - int(base.creationFrontEdge) + 4) & 3;
    auto put = [&](std::pair<int, int> v, float h) {
        auto [it, fresh] = out.heights.emplace(v, h);
        return fresh || it->second == h;
    };
    for (int v = 0; v <= D; ++v)
        for (int u = 0; u <= W; ++u) {
            const float h = offset + base.heights[size_t(v) * size_t(W + 1) + size_t(u)];
            int X = u, Y = v;
            switch (turn) {
            case 1: X = v, Y = W - u; break;
            case 2: X = W - u, Y = D - v; break;
            case 3: X = D - v, Y = u; break;
            default: break;
            }
            X += x, Y += y;
            if (!put({X, Y}, h)) {
                out.why = "the roads cannot follow the lot's ground";
                return out;
            }
            const bool strip = u >= su0 && u <= su1 && v >= sv0 && v <= sv1;
            if (strip || std::fabs(t.vertex(X, Y) - h) <= 0.01f)
                continue;
            const TileRect around{X - 1, Y - 1, X + 1, Y + 1};
            if (const Lot* other = lotOver(world, around); other && other->id != self) {
                out.why = "the ground would change under another lot";
                return out;
            }
            for (const Road* road : roadsOver(*world.occupants, around))
                for (auto sv : sameHeightVertices(*road, {X, Y}))
                    if (!put(sv, h)) {
                        out.why = "the roads cannot follow the lot's ground";
                        return out;
                    }
        }
    out.ok = true;
    return out;
}

size_t smoothTerrainUnderLot(Terrain& terrain, const PlacementWorld& world, const TileRect& R, uint32_t self) {
    const int gw = int(terrain.gridWidth), gh = int(terrain.gridHeight);
    if (gw <= 0 || gh <= 0 || terrain.heights.size() != size_t(gw) * size_t(gh))
        return 0;
    const std::vector<float> before = terrain.heights;
    auto at = [&](int x, int y) -> float& { return terrain.heights[size_t(y) * size_t(gw) + size_t(x)]; };
    auto inside = [&](int x, int y) { return x >= R.x0 && x <= R.x1 && y >= R.y0 && y <= R.y1 && x >= 0 && y >= 0 && x < gw && y < gh; };
    // FUN_004d359c: the locked vertices (the 3x3 vertex rectangle clamped to the tiles).
    const int w = R.x1 - R.x0 + 1;
    std::vector<bool> locked(size_t(w) * size_t(R.y1 - R.y0 + 1), false);
    const int tw = int(terrain.width), th = int(terrain.height);
    auto clampT = [](int v, int n) { return std::clamp(v, 0, n - 1); };
    for (int y = R.y0; y <= R.y1; ++y)
        for (int x = R.x0; x <= R.x1; ++x) {
            const TileRect around{clampT(x - 1, tw), clampT(y - 1, th), clampT(x + 1, tw), clampT(y + 1, th)};
            bool lock = lotOver(world, around, self) != nullptr || !roadsOver(*world.occupants, around).empty();
            for (const auto& f : world.occupants->flora)
                lock = lock || overlaps(around, occupantTiles(f.at));
            for (const auto& p : world.occupants->props)
                lock = lock || overlaps(around, occupantTiles(p.at));
            locked[size_t(y - R.y0) * size_t(w) + size_t(x - R.x0)] = lock;
        }
    auto isLocked = [&](int x, int y) { return locked[size_t(y - R.y0) * size_t(w) + size_t(x - R.x0)]; };
    const float straight = std::tan(world.talusAngle * 3.14f / 180.0f) * 10.0f, diagonal = straight * 1.414f;
    constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, kDy[8] = {0, -1, -1, -1, 0, 1, 1, 1};
    for (int pass = 0; pass < world.smoothingPasses; ++pass)
        for (int y = R.y0; y <= R.y1; ++y)
            for (int x = R.x0; x <= R.x1; ++x) {
                if (!inside(x, y))
                    continue;
                float h = at(x, y);
                for (int k = 0; k < 8; ++k) {
                    const int nx = x + kDx[k], ny = y + kDy[k];
                    if (!inside(nx, ny))
                        continue;
                    const float excess = h - at(nx, ny) - ((k & 1) ? diagonal : straight);
                    if (excess > 0) {
                        const float d = excess * 0.2f;
                        if (!isLocked(x, y))
                            h -= d, at(x, y) = h;
                        if (!isLocked(nx, ny))
                            at(nx, ny) += d;
                    }
                }
            }
    // FUN_00a28af0: multiples of 0.1875.
    for (int y = R.y0; y <= R.y1; ++y)
        for (int x = R.x0; x <= R.x1; ++x)
            if (inside(x, y))
                at(x, y) = std::nearbyint(at(x, y) / 0.1875f) * 0.1875f;
    size_t changed = 0;
    for (size_t i = 0; i < before.size(); ++i)
        changed += before[i] != terrain.heights[i];
    return changed;
}

bool roadsMatch(const LotInfo& base, int front, const std::array<bool, 4>& place) {
    for (int i = 0; i < 4; ++i) {
        const bool baseRoad = (base.roads >> i) & 1;
        if (baseRoad != place[size_t((i + front - int(base.creationFrontEdge) + 8) & 3)])
            return false;
    }
    return true;
}

std::optional<int> frontFromDrag(float fromX, float fromY, float toX, float toY) {
    const float dx = toX - fromX, dy = toY - fromY;
    if (std::fabs(dx) + std::fabs(dy) < 16.0f) // FLOAT_0274b69c
        return std::nullopt;
    const double a = std::atan2(double(dy), double(dx));
    const float f = float((a + 3.1415927410125732) * -2.0 / 3.1415927410125732 + 4.5);
    int n = int(std::floor(f)) + 4;
    n %= 4; // C truncation; never negative here, the binary clamps to 0..3
    return std::clamp(n, 0, 3);
}

std::optional<std::pair<int, Placement>> findFront(const PlacementWorld& world, const LotInfo& base, int x, int y,
                                                    uint32_t self, int preferred) {
    std::vector<int> order;
    if (preferred >= 0)
        order.push_back(preferred & 3);
    for (int k = 0; k < 4; ++k)
        if (std::find(order.begin(), order.end(), (int(base.creationFrontEdge) + k) & 3) == order.end())
            order.push_back((int(base.creationFrontEdge) + k) & 3);
    for (int front : order) {
        LotInfo turned = base;
        turned.frontEdge = uint8_t(front), turned.x = uint32_t(std::max(0, x)), turned.y = uint32_t(std::max(0, y));
        if (x < 0 || y < 0)
            continue;
        const Placement p = checkLot(world, base, self, x, y, front);
        if (p.ok)
            return std::pair{front, p};
    }
    return std::nullopt;
}

Placement checkLot(const PlacementWorld& world, const LotInfo& base, uint32_t self, int x, int y, int front) {
    LotInfo turned = base;
    turned.frontEdge = uint8_t(front & 3), turned.x = uint32_t(std::max(0, x)), turned.y = uint32_t(std::max(0, y));
    Placement p = checkPlacement(world, lotRect(turned), self, (base.flags & LotInfo::kFlagBeach) ? &base : nullptr);
    if (!p.ok)
        return p;
    if (x < 0 || y < 0 || !roadsMatch(base, front, p.roads)) {
        p.ok = false, p.why = "its roads are not on its road edges";
        return p;
    }
    // cTSUserToolMoveLot / PlaceLotFromInventory::IsOKToPlaceLot: the
    // terrain must be able to take the lot's ground (a dry run).
    if (const TerrainEdit e = conformTerrainToLot(world, base, self, x, y, front, p); !e.ok)
        p.ok = false, p.why = e.why;
    return p;
}

std::map<uint32_t, OccupantFlags> loadOccupantFlags(ResourceIndex& index) {
    std::map<uint32_t, OccupantFlags> out;
    for (const auto& loc : index.ofType(hoodtype::ObjectEntry)) {
        const auto bytes = index.read(loc);
        const auto props = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto g = props.find("guid");
        if (g == props.end())
            continue;
        OccupantFlags f;
        if (auto it = props.find("allowedinlot"); it != props.end())
            f.allowedInLot = it->second == "1" || it->second == "true";
        if (auto it = props.find("removeonlotplop"); it != props.end())
            f.removeOnLotPlop = it->second == "1" || it->second == "true";
        try {
            out[uint32_t(std::stoul(g->second))] = f;
        } catch (const std::logic_error& err) { // not a number (std::stoul): the entry is skipped
            os2::logOnce("occupant GUID", g->second, err);
        }
    }
    return out;
}

fs::path playerLotCatalog(const fs::path& hoodFolder, const fs::path& gameCatalog) {
    const fs::path abs = fs::absolute(hoodFolder);
    if (abs.parent_path().filename() != "Neighborhoods")
        return {};
    const fs::path dir = abs.parent_path().parent_path() / "LotCatalog";
    std::error_code ec;
    if (!fs::exists(dir)) {
        fs::create_directories(dir, ec);
        if (!gameCatalog.empty() && fs::is_directory(gameCatalog))
            for (const auto& e : fs::directory_iterator(gameCatalog, ec))
                if (e.path().extension() == ".package")
                    fs::copy_file(e.path(), dir / e.path().filename(), fs::copy_options::skip_existing, ec);
    }
    return dir;
}

std::vector<BinLot> loadLotBin(const fs::path& catalogDir) {
    std::vector<BinLot> out;
    std::error_code ec;
    if (catalogDir.empty() || !fs::is_directory(catalogDir))
        return out;
    for (const auto& e : fs::directory_iterator(catalogDir, ec)) {
        const std::string name = e.path().filename().string();
        if (e.path().extension() != ".package" || name.rfind("cx_", 0) != 0)
            continue;
        try {
            DbpfPackage pkg(e.path());
            for (const auto& entry : pkg.entries())
                if (entry.tgi.type == kTypeBaseLotInfo) {
                    BinLot b;
                    b.file = e.path();
                    b.catalogId = uint32_t(std::stoul(name.substr(3, 8)));
                    b.base = LotInfo::parseBase(pkg.read(entry));
                    for (const auto& f : pkg.entries())
                        if (f.tgi.type == hoodtype::FAMI)
                            b.familyId = uint32_t(f.tgi.instance);
                    if (b.familyId)
                        for (const auto& f : pkg.entries())
                            if (f.tgi.type == kTypeStr && f.tgi.instance == b.familyId) {
                                const StringTable t = StringTable::parse(pkg.read(f));
                                b.familyName = t.get(0);
                                if (b.familyName.empty())
                                    b.familyName = "?";
                            }
                    if (b.familyId && b.familyName.empty())
                        b.familyName = "?";
                    pkg.closeFile();
                    Lot l;
                    l.info = b.base;
                    l.package = b.file;
                    b.price = lotFileInfo(l).price;
                    out.push_back(std::move(b));
                    break;
                }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "lot bin: %s: %s\n", name.c_str(), ex.what());
        }
    }
    std::sort(out.begin(), out.end(), [](const BinLot& a, const BinLot& b) { return a.catalogId < b.catalogId; });
    return out;
}

bool deleteBinLot(const BinLot& lot, const fs::path& forbidden) {
    if (under(lot.file, forbidden))
        return false;
    std::error_code ec;
    return fs::remove(lot.file, ec);
}

uint32_t nextLotId(const Neighborhood& hood) {
    uint32_t id = 1;
    for (const auto& l : hood.lots)
        id = std::max(id, l.id + 1);
    const fs::path lots = hood.package.parent_path() / "Lots";
    while (fs::exists(lots / (hood.prefix + "_Lot" + std::to_string(id) + ".package")))
        ++id;
    return id;
}

namespace {

LotInfo newLotInfo(const LotInfo& base, uint32_t id, int x, int y, int front, float height) {
    LotInfo info = base;
    info.version = 13;
    if (!info.baseVersion)
        info.baseVersion = 6;
    info.x = uint32_t(x), info.y = uint32_t(y);
    info.heightOffset = height;
    info.lotId = int32_t(id);
    info.frontEdge = uint8_t(front & 3);
    info.skirtTexture = kSkirt;
    info.rest = {0};
    return info;
}

// FUN_00a284cc (cTSUserToolPlaceLotBase): the flora and props over the lot
// whose product says removeonlotplop (indices into the occupants).
std::pair<std::vector<size_t>, std::vector<size_t>> occupantsOnLot(const PlacementWorld& world, const TileRect& R) {
    const float x0 = float(R.x0) * kHoodCellSize, y0 = float(R.y0) * kHoodCellSize;
    const float x1 = float(R.x1) * kHoodCellSize, y1 = float(R.y1) * kHoodCellSize;
    auto over = [&](const OccupantPosition& at, uint32_t guid) {
        auto it = world.objects.find(guid);
        return it != world.objects.end() && it->second.removeOnLotPlop && at.boundsMax[0] > x0 &&
               at.boundsMin[0] < x1 && at.boundsMax[1] > y0 && at.boundsMin[1] < y1;
    };
    std::vector<size_t> flora, props;
    for (size_t i = 0; i < world.occupants->flora.size(); ++i)
        if (over(world.occupants->flora[i].at, world.occupants->flora[i].guid))
            flora.push_back(i);
    for (size_t i = 0; i < world.occupants->props.size(); ++i)
        if (over(world.occupants->props[i].at, world.occupants->props[i].guid))
            props.push_back(i);
    return {flora, props};
}

// What placing a lot does to the terrain.
struct TerrainChange {
    const std::map<std::pair<int, int>, float>* heights = nullptr; // TryModifyingTerrainToConformToLot
    std::optional<TileRect> smooth;                                // Smoothen under a new lot
    const std::map<size_t, std::array<float, 4>>* roadHeights = nullptr; // roads laid again on the terrain
};

// The lot info, the occupants without what the lot removes and the terrain.
void writeLot(const Neighborhood& hood, const PlacementWorld& world, const LotInfo& info, uint32_t id,
              std::vector<std::string>& lines, bool dropName, const TerrainChange& terrain = {}) {
    std::map<Key, std::vector<uint8_t>> replace{{{hoodtype::LotInfo, id}, info.write()}};
    const auto [flora, props] = occupantsOnLot(world, lotRect(info));
    Occupants left = *world.occupants;
    uint32_t terrainInstance = 0;
    std::optional<Terrain> ground;
    {
        DbpfPackage pkg(hood.package);
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type == hoodtype::Occupants &&
                (!flora.empty() || !props.empty() || (terrain.roadHeights && !terrain.roadHeights->empty()))) {
                auto bytes = pkg.read(e);
                if (terrain.roadHeights && !terrain.roadHeights->empty()) { // before the cut moves the roads
                    bytes = Occupants::setRoadHeights(bytes, Occupants::parse(bytes), *terrain.roadHeights);
                    lines.push_back(std::to_string(terrain.roadHeights->size()) + " roads laid on the flattened terrain");
                }
                const Occupants parsed = Occupants::parse(bytes);
                replace[{hoodtype::Occupants, e.tgi.instance}] = Occupants::removeOccupants(bytes, parsed, flora, props);
                if (!flora.empty() || !props.empty())
                    lines.push_back(std::to_string(flora.size() + props.size()) + " trees and props removed from the lot");
            } else if (e.tgi.type == hoodtype::Terrain && (terrain.heights || terrain.smooth)) {
                ground = Terrain::parse(pkg.read(e));
                terrainInstance = e.tgi.instance;
            }
        }
    }
    for (auto it = flora.rbegin(); it != flora.rend(); ++it)
        left.flora.erase(left.flora.begin() + std::ptrdiff_t(*it));
    for (auto it = props.rbegin(); it != props.rend(); ++it)
        left.props.erase(left.props.begin() + std::ptrdiff_t(*it));
    if (ground) {
        size_t changed = 0;
        if (terrain.heights)
            for (const auto& [v, h] : *terrain.heights)
                if (v.first >= 0 && v.second >= 0 && v.first < int(ground->gridWidth) && v.second < int(ground->gridHeight)) {
                    float& g = ground->heights[size_t(v.second) * ground->gridWidth + size_t(v.first)];
                    changed += g != h;
                    g = h;
                }
        if (terrain.heights)
            lines.push_back(std::to_string(changed) + " terrain vertices " +
                            (terrain.smooth ? "flattened under the abutting roads" : "conformed to the lot"));
        if (terrain.smooth) {
            PlacementWorld w = world;
            w.occupants = &left;
            lines.push_back(std::to_string(smoothTerrainUnderLot(*ground, w, *terrain.smooth)) +
                            " terrain vertices smoothed under the lot");
        }
        replace[{hoodtype::Terrain, terrainInstance}] = ground->write();
    }
    std::set<Key> drop;
    if (dropName)
        drop.insert({kTypeStr, 0x8000 | id});
    rewritePackage(hood.package, replace, drop);
}

} // namespace

namespace {

// The family that went to the bin with a lot comes back with it: its FAMI
// (living on the new lot), name strings, wardrobe and photo, under its old
// family number unless another family took it (then the next free one, and
// its Sims' person data 61 follow).
std::string restoreBinFamily(const Neighborhood& hood, const BinLot& bin, uint32_t lot) {
    std::map<Key, std::vector<uint8_t>> res;
    {
        DbpfPackage pkg(bin.file);
        for (const auto& e : pkg.entries())
            if (e.tgi.instance == bin.familyId &&
                (e.tgi.type == hoodtype::FAMI || e.tgi.type == kTypeStr || e.tgi.type == hoodtype::Wardrobe ||
                 e.tgi.type == 0x8C3CE95A))
                res[{e.tgi.type, e.tgi.instance}] = pkg.read(e);
    }
    auto fami = res.find({hoodtype::FAMI, bin.familyId});
    if (fami == res.end())
        return "the bin lot's family record is missing";
    uint32_t id = bin.familyId;
    if (hood.family(id)) {
        id = 1;
        for (const auto& f : hood.families)
            id = std::max(id, f.id + 1);
    }
    FamilyRecord r = FamilyRecord::parse(fami->second);
    r.lot = lot;
    std::map<Key, std::vector<uint8_t>> hoodAdds{{{hoodtype::FAMI, id}, r.write()}};
    for (auto [type, keep] : {std::pair{kTypeStr, true}, {hoodtype::Wardrobe, true}})
        if (auto it = res.find({type, bin.familyId}); it != res.end() && keep)
            hoodAdds[{type, id}] = it->second;
    if (id != bin.familyId)
        for (uint32_t guid : r.members)
            if (const Neighbor* n = hood.neighborByGuid(guid)) {
                SimDescription d = n->description;
                d.personData[sdsc::FamilyNumber] = int16_t(id);
                hoodAdds[{hoodtype::SDSC, n->id}] = d.write();
            }
    rewritePackage(hood.package, hoodAdds, {});
    if (auto it = res.find({0x8C3CE95A, bin.familyId}); it != res.end()) {
        const fs::path thumbs = hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package");
        if (fs::exists(thumbs))
            rewritePackage(thumbs, {{{0x8C3CE95A, id}, it->second}}, {}, 0x7FE65A3D);
    }
    return "family " + std::to_string(id) + " \"" + bin.familyName + "\" came back with the lot";
}

} // namespace

RoadFlatten flattenAbuttingRoads(const PlacementWorld& world, const TileRect& R, int front, float height) {
    RoadFlatten out;
    if (!world.terrain || !world.occupants || !world.hood)
        return out;
    const Terrain& t = *world.terrain;
    const int tilesX = int(t.gridWidth) - 1, tilesY = int(t.gridHeight) - 1; // this+0x88 / +0x8c
    // The front road strip (vertices, inclusive).
    TileRect fs = R;
    switch (front & 3) {
    case 0: fs.x1 = R.x0 + 1; break;
    case 1: fs.y0 = R.y1 - 1; break;
    case 2: fs.x0 = R.x1 - 1; break;
    default: fs.y1 = R.y0 + 1; break;
    }
    const std::array<TileRect, 4> strips = {TileRect{R.x0 - 1, R.y0 - 1, R.x0, R.y1 + 1},
                                            TileRect{R.x0 - 1, R.y1, R.x1 + 1, R.y1 + 1},
                                            TileRect{R.x1, R.y0 - 1, R.x1 + 1, R.y1 + 1},
                                            TileRect{R.x0 - 1, R.y0 - 1, R.x1 + 1, R.y0}};
    auto vertex = [&](int x, int y) {
        if (auto it = out.heights.find({x, y}); it != out.heights.end())
            return it->second; // an earlier strip set it
        return t.vertex(x, y);
    };
    for (int i = 0; i < 4; ++i) {
        const TileRect& s = strips[size_t(i)];
        if (s.x0 < 0 || s.y0 < 0 || s.x1 > tilesX || s.y1 > tilesY)
            continue;
        // GetOccupants(strip, 2): the plain roads (inferred: not the bridges).
        std::vector<size_t> roads;
        for (size_t k = 0; k < world.occupants->roads.size(); ++k)
            if (overlaps(s, occupantTiles(world.occupants->roads[k].at)))
                roads.push_back(k);
        if (roads.empty())
            continue;
        const bool fixed = s.x1 >= fs.x0 && s.x0 <= fs.x1 && s.y1 >= fs.y0 && s.y0 <= fs.y1;
        // DetermineFlattenHeightForRoadOccs (BV 0xa27714).
        float lo = fixed ? height : -FLT_MAX, hi = fixed ? height : FLT_MAX;
        bool ok = true;
        for (size_t k : roads) {
            const TileRect r = occupantTiles(world.occupants->roads[k].at);
            // The tool's vtable 0x50(rect, 1): inferred ExpandVertexRect by one; the
            // first lot over it (cTSLotManager vtable 0xec).
            const TileRect grown{r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1};
            const Lot* lot = lotOver(world, grown);
            if (!lot)
                continue;
            const TileRect l = lotRect(lot->info);
            if (l.x0 > r.x1 || r.x0 > l.x1 || l.y0 > r.y1 || r.y0 > l.y1)
                continue;
            for (int y = std::max(l.y0, r.y0); y <= std::min(l.y1, r.y1) && ok; ++y)
                for (int x = std::max(l.x0, r.x0); x <= std::min(l.x1, r.x1) && ok; ++x) {
                    const float h = vertex(x, y);
                    if (lo - h > 0.01f || h - hi > 0.01f)
                        ok = false;
                    lo = hi = h;
                }
        }
        float level = 0;
        if (!ok || lo > hi) {
            ok = false;
        } else if (lo == hi) {
            level = lo;
        } else { // free and no lot: the first road's (x0, y0) vertex
            const TileRect r = occupantTiles(world.occupants->roads[roads.front()].at);
            level = vertex(r.x0, r.y0);
        }
        if (!ok) {
            out.lines.push_back("abutting roads on edge " + std::to_string(i) + " left as they are");
            continue;
        }
        level = std::nearbyint(level / 0.1875f) * 0.1875f;
        // FlattenRoadsAndTerrain (BV 0xa27a30): every vertex of each road's rectangle.
        for (size_t k : roads) {
            const TileRect r = occupantTiles(world.occupants->roads[k].at);
            for (int y = r.y0; y <= r.y1; ++y)
                for (int x = r.x0; x <= r.x1; ++x)
                    out.heights[{x, y}] = level;
            out.roads.insert(k);
        }
        out.lines.push_back(std::to_string(roads.size()) + " abutting roads on edge " + std::to_string(i) +
                            " flattened to " + std::to_string(level));
    }
    // SmoothenRoadsConnectedToLot (BV 0xa26cf4) = the occupant manager's
    // SmoothenRoadsConnectedToRectangle (BV 0x557a18): the roads over the
    // lot are marked visited (road interface vtable 0x5c(1): a flag at
    // +0x6c, read by 0x64); the roads of the rectangle grown by one tile
    // (clamped to the neighborhood) that are not visited: a bend or an
    // intersection (0x4c IsAnIntersectionOrBend) is marked visited, a
    // straight one has its segment smoothed (SmoothenContainingRoadSegment).
    // Inferred: the visited flags start clear (the begin message 0x0C2BD5B7).
    {
        std::vector<uint8_t> visited(world.occupants->roads.size(), 0);
        auto bend = [](const Road& r) { return (r.connections & 0x5) && (r.connections & 0xA); };
        auto roadsIn = [&](const TileRect& q) {
            std::vector<size_t> v;
            for (size_t k = 0; k < world.occupants->roads.size(); ++k)
                if (overlaps(q, occupantTiles(world.occupants->roads[k].at)))
                    v.push_back(k);
            return v;
        };
        auto lotIn = [&](const TileRect& q) { return overlaps(q, R) || lotOver(world, q) != nullptr; };
        auto visitedIn = [&](const TileRect& q) {
            for (size_t k : roadsIn(q))
                if (visited[k])
                    return true;
            return false;
        };
        for (size_t k : roadsIn(R))
            visited[k] = 1;
        const TileRect ring{std::clamp(R.x0 - 1, 0, tilesX), std::clamp(R.y0 - 1, 0, tilesY),
                            std::clamp(R.x1 + 1, 0, tilesX), std::clamp(R.y1 + 1, 0, tilesY)};
        // DAT_02734bd0 / DAT_02734bc0: direction 0 -x, 1 +y, 2 +x, 3 -y.
        constexpr int kDx[4] = {-1, 0, 1, 0}, kDy[4] = {0, 1, 0, -1};
        size_t smoothed = 0;
        for (size_t k : roadsIn(ring)) {
            const Road& road = world.occupants->roads[k];
            if (visited[k])
                continue;
            if (bend(road)) {
                visited[k] = 1;
                continue;
            }
            // CollectRoadOccupantsInSegment (BV 0x556f40): from the road along
            // its axis both ways, cell by cell, while the road has a
            // connection that way and the next cell's road is no bend.
            TileRect seg = occupantTiles(road.at);
            const bool even = road.connections & 0x5, odd = road.connections & 0xA;
            if (even || odd) {
                int dir = even ? 0 : 3;
                for (int pass = 0; pass < 2; ++pass) {
                    if (pass == 1)
                        dir = (dir + 2) & 3;
                    const TileRect start = occupantTiles(road.at);
                    int x = start.x0, y = start.y0;
                    const Road* cur = &road;
                    while (cur && (cur->connections & (1 << dir))) {
                        x += kDx[dir], y += kDy[dir];
                        const TileRect cell{x, y, x + 1, y + 1};
                        const Road* next = nullptr;
                        for (const Road* r : roadsOver(*world.occupants, cell)) {
                            next = r;
                            break;
                        }
                        if (next && bend(*next))
                            next = nullptr;
                        cur = next;
                        if (cur)
                            seg = {std::min(seg.x0, x), std::min(seg.y0, y), std::max(seg.x1, x + 1), std::max(seg.y1, y + 1)};
                    }
                }
            }
            // SmoothenContainingRoadSegment (BV 0x5575c0): the segment's vertex
            // pairs across the road, one per step along it: fixed where a lot
            // or a visited road is round the vertex or the one across (taking
            // that one's height) and at both ends, else free within +/-10 of
            // its height; FUN_00553d90 then, 20 times over, eases every step
            // steeper than 1.1 by 0.3 of the excess from both sides (within
            // their ranges); both vertices of each pair get the value.
            const bool alongX = seg.y1 - seg.y0 == 1;
            const int len = alongX ? seg.x1 - seg.x0 : seg.y1 - seg.y0;
            struct Entry {
                float v, hi, lo;
            };
            std::vector<Entry> e(size_t(len + 1));
            for (int i = 0; i <= len; ++i) {
                const int x = alongX ? seg.x0 + i : seg.x0, y = alongX ? seg.y0 : seg.y0 + i;
                const int ox = alongX ? x : x + 1, oy = alongX ? y + 1 : y; // across the road
                const TileRect a{x - 1, y - 1, x + 1, y + 1};
                const TileRect b{ox - 1, oy - 1, ox + 1, oy + 1};
                float v;
                bool fixedHere = true;
                if (lotIn(a))
                    v = vertex(x, y);
                else if (lotIn(b))
                    v = vertex(ox, oy);
                else if (visitedIn(a))
                    v = vertex(x, y);
                else if (visitedIn(b))
                    v = vertex(ox, oy);
                else
                    v = vertex(x, y), fixedHere = i == 0 || i == len;
                e[size_t(i)] = {v, fixedHere ? v : v + 10.0f, fixedHere ? v : v - 10.0f};
            }
            for (int it = 0; it < 20; ++it)
                for (int i = 0; i <= len; ++i)
                    for (int j : {i - 1, i + 1}) {
                        if (j < 0 || j > len)
                            continue;
                        Entry& me = e[size_t(i)];
                        Entry& other = e[size_t(j)];
                        const float d = me.v - other.v;
                        if (d > 1.1f) {
                            const float ease = (d - 1.1f) * 0.3f;
                            me.v = std::max(me.v - ease, me.lo);
                            other.v = std::min(other.v + ease, other.hi);
                        }
                    }
            for (int i = 0; i <= len; ++i) {
                const int x = alongX ? seg.x0 + i : seg.x0, y = alongX ? seg.y0 : seg.y0 + i;
                const int ox = alongX ? x : x + 1, oy = alongX ? y + 1 : y;
                out.heights[{x, y}] = e[size_t(i)].v;
                out.heights[{ox, oy}] = e[size_t(i)].v;
            }
            for (size_t r2 : roadsIn(seg))
                out.roads.insert(r2);
            ++smoothed;
        }
        if (smoothed)
            out.lines.push_back(std::to_string(smoothed) + " road segments round the lot smoothed");
    }
    // Then the roads lie on the terrain again (each corner 0.4 over its
    // vertex, cTSNHoodRoadOccupant::FlattenToTerrain; inferred to follow the
    // end message 0x8C2BD74C).
    for (size_t k : out.roads) {
        const Road& road = world.occupants->roads[k];
        std::array<float, 4> z{};
        for (size_t c = 0; c < 4; ++c)
            z[c] = vertex(int(std::lround(road.corners[c][0] / kHoodCellSize)),
                          int(std::lround(road.corners[c][1] / kHoodCellSize))) + kRoadOverTerrain;
        out.roadHeights[k] = z;
    }
    return out;
}

Placement checkEmptyLot(const PlacementWorld& world, EmptyLotSize size, int x, int y, int rotation) {
    Placement p = checkPlacement(world, emptyLotRect(size, x, y, rotation));
    const int f = rotation & 3;
    if (p.ok && !p.roads[size_t(f)]) {
        p.ok = false;
        p.why = "no road along the lot's front";
    }
    if (p.ok)
        p.front = f, p.roadHeight = p.edgeHeight[size_t(f)];
    return p;
}

LotResult placeEmptyLot(const fs::path& folder, const PlacementWorld& world, EmptyLotSize size, int x, int y,
                        int rotation, const fs::path& forbidden, const std::string& name, uint8_t zone,
                        const fs::path& templatesDir) {
    LotResult out;
    try {
        checkFolder(folder, forbidden);
        const TileRect R = emptyLotRect(size, x, y, rotation);
        const Placement p = checkEmptyLot(world, size, x, y, rotation);
        if (!p.ok)
            throw std::runtime_error("cannot place the lot here: " + p.why);
        const Neighborhood& hood = *world.hood;
        LotInfo base;
        base.baseVersion = 6;
        base.width = uint32_t(R.width()), base.depth = uint32_t(R.depth());
        base.type = zone; // cTSWinProcPlaceLotDialog::GetLotZoneType: 0 residential, 1 community
        base.name = name;
        for (int i = 0; i < 4; ++i)
            if (p.roads[size_t(i)])
                base.roads |= uint8_t(1 << i);
        base.creationFrontEdge = uint8_t(p.front);
        base.flags = 0;
        base.heights.assign(size_t(R.width() + 1) * size_t(R.depth() + 1), 0.0f);
        out.lot = nextLotId(hood);
        const LotInfo info = newLotInfo(base, out.lot, R.x0, R.y0, p.front, p.roadHeight);
        // A name string left by an older lot of this ID goes: the lot info's
        // name is the lot's. The terrain under the lot is smoothed
        // (Smoothen with [SmoothingUnderNewLot], then rounded to 0.1875).
        TerrainChange change;
        change.smooth = R;
        // FlattenAbuttingRoadsIfPossible with the front and its road height.
        const RoadFlatten flat = flattenAbuttingRoads(world, R, p.front, p.heightAt(p.front));
        for (const auto& l : flat.lines)
            out.lines.push_back(l);
        if (!flat.heights.empty())
            change.heights = &flat.heights;
        if (!flat.roadHeights.empty())
            change.roadHeights = &flat.roadHeights;
        writeLot(hood, world, info, out.lot, out.lines, true, change);
        out.lines.push_back("empty lot " + std::to_string(out.lot) + " (" + std::to_string(R.width()) + "x" +
                            std::to_string(R.depth()) + " with the road) placed at (" + std::to_string(R.x0) + ", " +
                            std::to_string(R.y0) + "), front " + std::to_string(p.front));
        if (!templatesDir.empty()) {
            const LotResult file = makeLotFile(folder, out.lot, templatesDir, forbidden);
            for (const auto& l : file.lines)
                out.lines.push_back(l);
            if (!file.ok)
                throw std::runtime_error(file.error);
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

fs::path lotTemplate(const fs::path& templatesDir, uint8_t zone, int across, int along) {
    if (templatesDir.empty() || across < 1 || along < 1)
        return {};
    const fs::path f = templatesDir / ((zone == 1 ? "CommunityLotTemplate" : "LotTemplate") + std::to_string(across * 10) +
                                       "x" + std::to_string(along * 10) + ".package");
    return fs::exists(f) ? f : fs::path{};
}

LotResult makeLotFile(const fs::path& folder, uint32_t lotId, const fs::path& templatesDir, const fs::path& forbidden) {
    LotResult out;
    out.lot = lotId;
    try {
        checkFolder(folder, forbidden);
        const Neighborhood hood = Neighborhood::load(folder, false);
        const Lot* lot = hood.lot(lotId);
        if (!lot)
            throw std::runtime_error("no lot " + std::to_string(lotId));
        if (!lot->package.empty() && fs::exists(lot->package)) {
            out.ok = true;
            return out;
        }
        LotInfo info = lot->info;
        const int f = info.frontEdge & 3;
        // The template that turns to the lot's front: across the road is
        // the current width for fronts 0/2 (x edges), the depth for 1/3.
        const int across = int((f & 1) ? info.currentDepth() : info.currentWidth());
        const int along = int((f & 1) ? info.currentWidth() : info.currentDepth());
        bool flat = std::all_of(info.heights.begin(), info.heights.end(), [](float h) { return h == 0.0f; });
        fs::path tmpl = flat ? lotTemplate(templatesDir, info.type, across, along) : fs::path{};
        bool rewriteInfo = false;
        if (!tmpl.empty()) {
            if (info.creationFrontEdge != 0 || int(info.width) != across || int(info.depth) != along) {
                // As made facing edge 0 and turned to `f`: base edge i is
                // the neighborhood's edge (i + f) mod 4.
                std::array<bool, 4> place{};
                for (int i = 0; i < 4; ++i)
                    if (info.roads & (1 << i))
                        place[size_t((i + f - int(info.creationFrontEdge) + 8) & 3)] = true;
                info.roads = 0;
                for (int i = 0; i < 4; ++i)
                    if (place[size_t((i + f) & 3)])
                        info.roads |= uint8_t(1 << i);
                info.creationFrontEdge = 0;
                info.width = uint32_t(across), info.depth = uint32_t(along);
                info.heights.assign(size_t(across + 1) * size_t(along + 1), 0.0f);
                rewriteInfo = true;
            }
        } else {
            // The road strip faces no road (or a template is missing): the
            // template of the lot's own base size, as it is.
            tmpl = lotTemplate(templatesDir, info.type, int(info.width), int(info.depth));
            if (tmpl.empty())
                throw std::runtime_error("no lot template for lot " + std::to_string(lotId) + " (" +
                                         std::to_string(info.width) + "x" + std::to_string(info.depth) + ", front " +
                                         std::to_string(f) + ") in " + templatesDir.string());
            out.lines.push_back("lot " + std::to_string(lotId) + ": its road strip faces no road; " +
                                tmpl.filename().string() + " unturned");
        }
        const fs::path file = folder / "Lots" / (hood.prefix + "_Lot" + std::to_string(lotId) + ".package");
        fs::create_directories(file.parent_path());
        const std::string saving = file.string() + ".saving";
        {
            DbpfPackage src(tmpl);
            LotObjects objects = LotObjects::load(src);
            // SetCommonSimulatorGlobals: 10 Current House = the lot, 41 Lot
            // Zoning = its type (the lot load sets them again).
            if (objects.globals.size() > 41)
                objects.globals[10] = int16_t(lotId), objects.globals[41] = int16_t(info.type);
            // The lot value (SIMI +0xA4) the neighborhood shows: the world
            // value of the size off the road, what GetLotFileInfo gave
            // without the file (the templates already hold it).
            Lot noFile = *lot;
            noFile.package.clear();
            noFile.info = info;
            objects.simulator.fA4 = uint32_t(lotFileInfo(noFile).price);
            objects.save(src, saving);
        }
        // The base lot info the lot file keeps (SaveLot writes it; the bin reads it).
        std::vector<uint8_t> base(64, 0);
        const auto b = info.writeBase(true);
        base.insert(base.end(), b.begin(), b.end());
        rewritePackage(saving, {{{kTypeBaseLotInfo, 0}, base}}, {});
        if (rewriteInfo) {
            rewritePackage(hood.package, {{{hoodtype::LotInfo, lotId}, info.write()}}, {});
            out.lines.push_back("lot " + std::to_string(lotId) + " info: made facing edge 0, " + std::to_string(across) +
                                "x" + std::to_string(along) + ", turned to front " + std::to_string(f));
        }
        os2::replaceFile(saving, file);
        out.lines.push_back("lot " + std::to_string(lotId) + " file " + file.filename().string() + " from " +
                            tmpl.filename().string());
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

LotResult placeBinLot(const fs::path& folder, const PlacementWorld& world, const BinLot& bin, int x, int y, int front,
                      const fs::path& forbidden) {
    LotResult out;
    try {
        checkFolder(folder, forbidden);
        const Placement p = checkLot(world, bin.base, 0, x, y, front);
        if (!p.ok)
            throw std::runtime_error("cannot place the lot here: " + p.why);
        const Neighborhood& hood = *world.hood;
        out.lot = nextLotId(hood);
        const TerrainEdit ground = conformTerrainToLot(world, bin.base, out.lot, x, y, front, p);
        const fs::path file = folder / "Lots" / (hood.prefix + "_Lot" + std::to_string(out.lot) + ".package");
        fs::create_directories(file.parent_path());
        fs::copy_file(bin.file, file.string() + ".saving", fs::copy_options::overwrite_existing);
        if (bin.familyId) // the family's records go back to the neighborhood, not the lot file
            rewritePackage(file.string() + ".saving", {},
                           {{hoodtype::FAMI, bin.familyId}, {kTypeStr, bin.familyId}, {hoodtype::Wardrobe, bin.familyId},
                            {0x8C3CE95A, bin.familyId}});
        os2::replaceFile(file.string() + ".saving", file);
        const LotInfo info = newLotInfo(bin.base, out.lot, x, y, front, p.heightAt(front));
        TerrainChange change;
        change.heights = &ground.heights;
        writeLot(hood, world, info, out.lot, out.lines, true, change);
        if (bin.familyId) {
            out.lines.push_back(restoreBinFamily(hood, bin, out.lot));
            // A family's lot leaves the bin with it (inferred: the family
            // cannot be placed twice).
            if (!under(bin.file, forbidden)) {
                std::error_code ec;
                fs::remove(bin.file, ec);
            }
        }
        out.lines.push_back("lot " + std::to_string(out.lot) + " \"" + bin.base.name + "\" from the bin (" +
                            bin.file.filename().string() + ") placed at (" + std::to_string(x) + ", " +
                            std::to_string(y) + "), front " + std::to_string(front));
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

LotResult moveLot(const fs::path& folder, const PlacementWorld& world, uint32_t lotId, int x, int y, int front,
                  const fs::path& forbidden) {
    LotResult out;
    try {
        checkFolder(folder, forbidden);
        const Lot* lot = world.hood->lot(lotId);
        if (!lot)
            throw std::runtime_error("no lot " + std::to_string(lotId));
        LotInfo info = lot->info;
        info.frontEdge = uint8_t(front & 3), info.x = uint32_t(x), info.y = uint32_t(y);
        const Placement p = checkLot(world, lot->info, lotId, x, y, front);
        if (!p.ok)
            throw std::runtime_error("cannot move the lot here: " + p.why);
        info.heightOffset = p.heightAt(front);
        const TerrainEdit ground = conformTerrainToLot(world, lot->info, lotId, x, y, front, p);
        TerrainChange change;
        change.heights = &ground.heights;
        writeLot(*world.hood, world, info, lotId, out.lines, false, change);
        out.lot = lotId;
        out.lines.push_back("lot " + std::to_string(lotId) + " moved to (" + std::to_string(x) + ", " +
                            std::to_string(y) + "), front " + std::to_string(front));
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

namespace {

constexpr uint32_t kTypeFamilyThumb = 0x8C3CE95A;

fs::path familyThumbnails(const Neighborhood& hood) {
    return hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package");
}

// The resources of `pkg` of these (type, instance) keys, raw.
std::map<Key, std::vector<uint8_t>> readResources(const fs::path& file, const std::set<Key>& keys) {
    std::map<Key, std::vector<uint8_t>> out;
    if (file.empty() || !fs::exists(file))
        return out;
    DbpfPackage pkg(file);
    for (const auto& e : pkg.entries())
        if (keys.count({e.tgi.type, e.tgi.instance}))
            out[{e.tgi.type, e.tgi.instance}] = pkg.read(e);
    return out;
}

} // namespace

LotResult moveLotToBin(const fs::path& folder, uint32_t lotId, const fs::path& catalogDir, const fs::path& forbidden) {
    LotResult out;
    try {
        checkFolder(folder, forbidden);
        if (catalogDir.empty())
            throw std::runtime_error("no Lots & Houses bin for this neighborhood");
        checkFolder(catalogDir, forbidden);
        const Neighborhood hood = Neighborhood::load(folder, false);
        const Lot* lot = hood.lot(lotId);
        if (!lot)
            throw std::runtime_error("no lot " + std::to_string(lotId));
        if (lot->package.empty() || !fs::exists(lot->package) || !lotFileInfo(*lot).hasHouse)
            throw std::runtime_error("empty lot");
        const uint32_t famId = familyOnLot(hood, lotId);
        const Family* fam = famId ? hood.family(famId) : nullptr;
        uint32_t id = 1;
        for (const auto& b : loadLotBin(catalogDir))
            id = std::max(id, b.catalogId + 1);
        char name[32];
        std::snprintf(name, sizeof name, "cx_%08u.package", id);
        const fs::path to = catalogDir / name;
        fs::create_directories(catalogDir);
        fs::copy_file(lot->package, to.string() + ".saving", fs::copy_options::overwrite_existing);
        // The base lot info the bin reads (SaveLot writes it into the lot file).
        std::vector<uint8_t> base(64, 0);
        const auto b = lot->info.writeBase(true);
        base.insert(base.end(), b.begin(), b.end());
        std::map<Key, std::vector<uint8_t>> binAdds{{{kTypeBaseLotInfo, 0}, base}};
        std::set<Key> hoodDrops{{hoodtype::LotInfo, lotId}, {kTypeStr, 0x8000 | lotId}};
        std::map<Key, std::vector<uint8_t>> hoodAdds;
        if (fam) {
            // The family goes with the lot (cTSUserToolMoveLotToCatalog: STR#
            // 0x83 #31): its record, name strings and clothing bin travel in
            // the bin's lot file, its photo too; it leaves the neighborhood
            // and loses its relationships with the Sims outside it. Which
            // resources the game writes into the catalog file is inferred.
            auto fromHood = readResources(hood.package, {{hoodtype::FAMI, famId}, {kTypeStr, famId},
                                                         {hoodtype::Wardrobe, famId}});
            for (auto& [k, bytes] : fromHood) {
                binAdds[k] = std::move(bytes);
                hoodDrops.insert(k);
            }
            auto photo = readResources(familyThumbnails(hood), {{kTypeFamilyThumb, famId}});
            for (auto& [k, bytes] : photo)
                binAdds[k] = std::move(bytes);
            const std::set<uint16_t> members(fam->members.begin(), fam->members.end());
            size_t dropped = 0;
            {
                DbpfPackage pkg(hood.package);
                for (const auto& e : pkg.entries())
                    if (e.tgi.type == hoodtype::SREL) {
                        const bool a = members.count(uint16_t(e.tgi.instance >> 16)) != 0;
                        const bool c = members.count(uint16_t(e.tgi.instance)) != 0;
                        if (a != c)
                            hoodDrops.insert({e.tgi.type, e.tgi.instance}), ++dropped;
                    }
            }
            // Each Sim's list of the Sims it has a relationship with (SDSC):
            // the family's keep each other, the others drop them.
            for (const auto& n : hood.neighbors) {
                SimDescription d = n.description;
                const bool in = members.count(n.id) != 0;
                std::erase_if(d.relations, [&](uint32_t r) { return (members.count(uint16_t(r)) != 0) != in; });
                if (d.relations.size() != n.description.relations.size())
                    hoodAdds[{hoodtype::SDSC, n.id}] = d.write();
            }
            out.lines.push_back("family " + std::to_string(famId) + " \"" + fam->name + "\" goes with the lot (" +
                                std::to_string(dropped) + " relationships with other Sims dropped)");
        }
        rewritePackage(to.string() + ".saving", binAdds, {});
        os2::replaceFile(to.string() + ".saving", to);
        rewritePackage(hood.package, hoodAdds, hoodDrops);
        if (fam && fs::exists(familyThumbnails(hood)))
            rewritePackage(familyThumbnails(hood), {}, {{kTypeFamilyThumb, famId}});
        fs::remove(lot->package);
        out.lot = lotId;
        out.lines.push_back("lot " + std::to_string(lotId) + " moved to the bin as " + to.filename().string());
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

std::vector<DecorProduct> loadDecorCatalog(ResourceIndex& index) {
    std::vector<DecorProduct> out;
    for (const auto& loc : index.ofType(hoodtype::ObjectEntry)) {
        const auto bytes = index.read(loc);
        const auto props = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto get = [&](const char* k) -> std::string {
            auto it = props.find(k);
            return it == props.end() ? std::string() : it->second;
        };
        auto num = [&](const char* k) -> uint32_t {
            const std::string v = get(k);
            return v.empty() ? 0 : uint32_t(std::stoul(v));
        };
        if (get("guid").empty() || get("modelname").empty() || get("sort").empty() || num("showincatalog") == 0 ||
            num("deprecated") != 0)
            continue;
        DecorProduct p;
        p.guid = num("guid"), p.name = get("name"), p.model = get("modelname"), p.sort = get("sort");
        p.inLot = num("allowedinlot") != 0, p.onRoad = num("allowedonroad") != 0, p.surface = int(num("placementsurface"));
        p.removeOnLotPlop = num("removeonlotplop") != 0;
        p.thumbGroup = num("thumbnailgroupid"), p.thumbInstance = num("thumbnailinstanceid");
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const DecorProduct& a, const DecorProduct& b) {
        return a.sort != b.sort ? a.sort < b.sort : a.guid < b.guid;
    });
    return out;
}

int decorationError(const PlacementWorld& world, const DecorProduct& product, const OccupantPosition& at) {
    const Terrain& t = *world.terrain;
    const float x = at.position[0], y = at.position[1];
    if (x < 0 || y < 0 || x > t.worldWidth() || y > t.worldHeight())
        return DecorOffTerrain;
    const TileRect tiles = occupantTiles(at); // the occupant's vtable 0x20
    // A lot (cTSNeighborhood vtable 0xec) when the product may not stand in one.
    if ((product.removeOnLotPlop || !product.inLot) && world.hood && lotOver(world, tiles))
        return DecorInLot;
    // cTSNHoodTerrain::IsCompletelyLandTile / IsCompletelyWaterTile over the tiles: a
    // vertex is water when it is under the sea level.
    auto water = [&](int i, int j) { return t.vertex(i, j) < t.seaLevel; };
    for (int j = tiles.y0; j < tiles.y1; ++j)
        for (int i = tiles.x0; i < tiles.x1; ++i) {
            const int wet = water(i, j) + water(i, j + 1) + water(i + 1, j + 1) + water(i + 1, j);
            const bool inside = i >= 0 && j >= 0 && i < int(t.width) && j < int(t.height);
            if (product.surface == 0 && (!inside || wet != 0))
                return DecorNeedsLand;
            if (product.surface == 1 && (!inside || wet != 4))
                return DecorNeedsWater;
        }
    // The roads GetOccupants(tiles, 2) gives whose bounds touch the object's (vtable 0x24, inclusive).
    if (!product.onRoad && world.occupants)
        for (const Road* r : roadsOver(*world.occupants, tiles))
            if (r->at.boundsMin[0] <= at.boundsMax[0] && at.boundsMin[0] <= r->at.boundsMax[0] &&
                r->at.boundsMin[1] <= at.boundsMax[1] && at.boundsMin[1] <= r->at.boundsMax[1])
                return DecorOnRoad;
    return DecorOk;
}

std::string decorationBlocked(const PlacementWorld& world, const DecorProduct& product, const OccupantPosition& at) {
    switch (decorationError(world, product, at)) {
    case DecorInLot: return "may not stand in a lot";
    case DecorOnRoad: return "may not stand on a road";
    case DecorNeedsLand: return "needs land";
    case DecorNeedsWater: return "needs water";
    case DecorOffTerrain: return "outside the neighborhood";
    default: return "";
    }
}

DecorResult addDecoration(const fs::path& folder, const PlacementWorld& world, const DecorProduct& product,
                          const OccupantPosition& at, float rotation, const fs::path& forbidden) {
    DecorResult out;
    try {
        checkFolder(folder, forbidden);
        if (const int code = decorationError(world, product, at); code != DecorOk) {
            out.code = code;
            out.error = decorationBlocked(world, product, at);
            return out;
        }
        DbpfPackage pkg(world.hood->package);
        for (const auto& e : pkg.entries())
            if (e.tgi.type == hoodtype::Occupants) {
                const auto bytes = pkg.read(e);
                const Occupants parsed = Occupants::parse(bytes);
                const std::map<Key, std::vector<uint8_t>> replace{
                    {{hoodtype::Occupants, e.tgi.instance},
                     Occupants::addOccupant(bytes, parsed, product.flora(), at, product.guid, rotation)}};
                pkg.closeFile();
                rewritePackage(world.hood->package, replace, {});
                out.lines.push_back(product.name + " (" + std::to_string(product.guid) + ") placed at " +
                                    std::to_string(int(at.position[0])) + "," + std::to_string(int(at.position[1])));
                out.ok = true;
                return out;
            }
        out.error = "the neighborhood has no occupants resource";
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

DecorResult removeDecoration(const fs::path& folder, const PlacementWorld& world, bool flora, size_t index,
                             const fs::path& forbidden) {
    DecorResult out;
    try {
        checkFolder(folder, forbidden);
        DbpfPackage pkg(world.hood->package);
        for (const auto& e : pkg.entries())
            if (e.tgi.type == hoodtype::Occupants) {
                const auto bytes = pkg.read(e);
                const Occupants parsed = Occupants::parse(bytes);
                if (index >= (flora ? parsed.flora.size() : parsed.props.size())) {
                    out.error = "no such decoration";
                    return out;
                }
                const std::map<Key, std::vector<uint8_t>> replace{
                    {{hoodtype::Occupants, e.tgi.instance},
                     Occupants::removeOccupants(bytes, parsed, flora ? std::vector<size_t>{index} : std::vector<size_t>{},
                                                flora ? std::vector<size_t>{} : std::vector<size_t>{index})}};
                pkg.closeFile();
                rewritePackage(world.hood->package, replace, {});
                out.lines.push_back(std::string(flora ? "flora " : "prop ") + std::to_string(index) + " removed");
                out.ok = true;
                return out;
            }
        out.error = "the neighborhood has no occupants resource";
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

} // namespace os2::hood
