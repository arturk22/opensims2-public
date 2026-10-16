#include "lot_edit.hpp"

#include <algorithm>
#include <map>
#include <cmath>
#include <numeric>
#include <set>

namespace os2 {

namespace {

constexpr uint32_t kGraphAll = 5, kGraphAll2 = 0x18, kGraphLoadBearing = 6, kGraphLight = 7, kGraphDeck = 8,
                   kGraphStandard = 0x16, kGraphAttic = 0x17;
constexpr uint32_t kRoomsLoadBearing = 0x0A, kRoomsLight = 0x0B;

// The graphs a partition type goes into: cPartitionManager::AddWallToGraph
// (BV 0xc2e7cc) puts every partition but fence arches in one graph (0x18
// here), the ones that block placement in another (5: every type but the
// deck skirt 4, blocksPlacement false in walls.txt; fences too), the
// load-bearing ones in 6, the light-blocking ones in 7, and by vertical
// span floor-to-floor (0x16), floor-to-floor-based-ceiling (8: deck skirt
// 16), floor-to-roof (0x17: attic 3); fence arches have their own (0x13,
// none in the shipped lots). The instance of each graph comes from the
// saved lots (82 lots of the base game: 5 = every type but 4; 0x18 every
// type; 6 = 1, 23 (foundation), 29 (pool), 16 and 4; 7 = 1, 3 and 23; 0x16
// = 1, 23, 29; 0x17 = 3; 8 = 16).
bool inGraph(uint32_t graph, uint32_t type) {
    switch (graph) {
    case kGraphAll: return type != 4;
    case kGraphAll2: return true;
    case kGraphLoadBearing: return type == 1 || type == 23 || type == 29 || type == 16 || type == 4;
    case kGraphLight: return type == 1 || type == 3 || type == 23;
    case kGraphStandard: return type == 1 || type == 23 || type == 29;
    case kGraphAttic: return type == 3;
    case kGraphDeck: return type == 16;
    default: return false;
    }
}

bool primaryGraph(uint32_t instance) { return instance == kGraphAll || instance == kGraphAll2; }

bool samePoint(float x, float y, int level, const Lot::WallGraph::Vertex& v) {
    return v.level == level && std::fabs(v.x - x) < 1e-3f && std::fabs(v.y - y) < 1e-3f;
}

const Lot::WallGraph::Vertex* vertexById(const Lot::WallGraph& g, uint32_t id) {
    for (const auto& v : g.vertices)
        if (v.id == id)
            return &v;
    return nullptr;
}

uint32_t vertexAt(Lot::WallGraph& g, float x, float y, int level) {
    for (const auto& v : g.vertices)
        if (samePoint(x, y, level, v))
            return v.id;
    Lot::WallGraph::Vertex v;
    v.id = uint32_t(g.header[6]++);
    v.x = x, v.y = y, v.level = level;
    g.vertices.push_back(v);
    return v.id;
}

bool sameSegment(const Lot::WallVertex& a, const Lot::WallVertex& b, int level, int ax, int ay, int bx, int by) {
    auto at = [](const Lot::WallVertex& v, int x, int y) {
        return std::lround(v.x) == x && std::lround(v.y) == y && std::fabs(v.x - float(x)) < 1e-3f &&
               std::fabs(v.y - float(y)) < 1e-3f;
    };
    return a.level == level && ((at(a, ax, ay) && at(b, bx, by)) || (at(a, bx, by) && at(b, ax, ay)));
}

size_t tileIndex(const Lot& lot, int li, int x, int y) {
    return (size_t(li) * lot.width + size_t(x)) * lot.height + size_t(y);
}

// Union-find over quarter tiles.
struct Sets {
    std::vector<uint32_t> parent;
    explicit Sets(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
    uint32_t find(uint32_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    }
    void join(uint32_t a, uint32_t b) {
        a = find(a), b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    }
};

std::vector<std::array<uint16_t, 4>>* roomArray(Lot& lot, uint32_t graph) {
    if (graph == kGraphAll)
        return lot.rooms.empty() ? nullptr : &lot.rooms;
    const uint32_t a = graph == kGraphLoadBearing ? kRoomsLoadBearing : graph == kGraphLight ? kRoomsLight : 0;
    if (!a)
        return nullptr;
    auto it = lot.roomLayers.find(a);
    return it == lot.roomLayers.end() || it->second.empty() ? nullptr : &it->second;
}

// The room of the quarter on the side `n` (a unit normal) of the segment
// from (ax, ay) to (bx, by), from a room array.
int32_t roomBesideSegment(const Lot& lot, const std::vector<std::array<uint16_t, 4>>& rooms, int level, float ax,
                          float ay, float bx, float by, float nx, float ny) {
    if (!lot.hasLevel(level))
        return 0;
    const int li = level - lot.minLevel;
    const float mx = (ax + bx) / 2, my = (ay + by) / 2;
    if (std::fabs(bx - ax) > 0.5f && std::fabs(by - ay) > 0.5f) {
        // A diagonal: the tile it crosses, the quarters on that side.
        const int x = int(std::floor(std::min(ax, bx))), y = int(std::floor(std::min(ay, by)));
        if (x < 0 || y < 0 || x >= int(lot.width) || y >= int(lot.height))
            return 0;
        const auto& q = rooms[tileIndex(lot, li, x, y)];
        return nx > 0 ? q[QEast] : q[QWest];
    }
    const float px = mx + nx * 0.25f, py = my + ny * 0.25f;
    if (px < 0 || py < 0 || px >= float(lot.width) || py >= float(lot.height))
        return 0;
    const int x = int(std::floor(px)), y = int(std::floor(py));
    // The quarter facing back at the wall (-n).
    const int q = nx > 0.5f ? QWest : nx < -0.5f ? QEast : ny > 0.5f ? QSouth : QNorth;
    return rooms[tileIndex(lot, li, x, y)][size_t(q)];
}

// The left face's cycle of the half-edge (ax, ay) -> (bx, by) of the graph's
// lines on `level` (cWallGraph's LNext: at each point the next line clockwise
// from the way back), as the points it passes; `bridge` set when the walk
// comes back along (bx, by) -> (ax, ay): the line closes no cycle.
std::vector<std::array<int, 2>> leftCycle(const Lot::WallGraph& g, int level, int ax, int ay, int bx, int by,
                                          bool& bridge) {
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> next;
    for (const auto& l : g.lines) {
        const auto* a = vertexById(g, l.a);
        const auto* b = vertexById(g, l.b);
        if (!a || !b || a->level != level || b->level != level)
            continue;
        const std::pair<int, int> pa{int(std::lround(a->x)), int(std::lround(a->y))},
            pb{int(std::lround(b->x)), int(std::lround(b->y))};
        next[pa].push_back(pb), next[pb].push_back(pa);
    }
    auto angle = [](std::pair<int, int> from, std::pair<int, int> to) {
        return std::atan2(float(to.second - from.second), float(to.first - from.first));
    };
    std::vector<std::array<int, 2>> cycle;
    bridge = false;
    std::pair<int, int> u{ax, ay}, v{bx, by};
    for (size_t step = 0; step < 100000; ++step) {
        cycle.push_back({u.first, u.second});
        if (u == std::pair<int, int>{bx, by} && v == std::pair<int, int>{ax, ay})
            bridge = true;
        // The next line out of v: the first clockwise from v -> u.
        const float back = angle(v, u);
        std::pair<int, int> best = u;
        float bestDelta = 10.0f;
        for (const auto& w : next[v]) {
            float d = back - angle(v, w);
            while (d <= 1e-4f)
                d += 2.0f * float(M_PI);
            if (d < bestDelta)
                bestDelta = d, best = w;
        }
        u = v, v = best;
        if (u == std::pair<int, int>{ax, ay} && v == std::pair<int, int>{bx, by})
            break;
    }
    return cycle;
}

} // namespace

void postAttachRooms(Lot& lot, Lot::WallGraph& g, int level, int ax, int ay, int bx, int by) {
    auto* rooms = roomArray(lot, g.instance);
    const int W = int(lot.width), H = int(lot.height);
    if (!rooms || rooms->size() != size_t(W) * H * size_t(lot.levelCount) || !lot.hasLevel(level))
        return;
    const int li = level - lot.minLevel;
    // The quarter on each side of the line, left (seen from a to b, the
    // left normal (-dy, dx)) and right.
    const int dx = bx - ax, dy = by - ay;
    struct Side {
        int x, y, q;
    };
    auto sideOf = [&](int nx, int ny) -> Side {
        if (dx && dy) { // a diagonal: the half of its tile the normal points to
            const int tx = std::min(ax, bx), ty = std::min(ay, by);
            return {tx, ty, nx < 0 ? int(QWest) : int(QEast)};
        }
        if (dy == 0) { // along x: the tile above (+y) or below
            const int tx = std::min(ax, bx);
            return ny > 0 ? Side{tx, ay, QSouth} : Side{tx, ay - 1, QNorth};
        }
        const int ty = std::min(ay, by); // along y: the tile east (+x) or west
        return nx > 0 ? Side{ax, ty, QWest} : Side{ax - 1, ty, QEast};
    };
    // A diagonal's halves: "/" (dx == dy) parts west + north from east +
    // south, "\" north + east from south + west (roomComponents).
    auto diagonalSide = [&](int nx, int ny) -> Side {
        const int tx = std::min(ax, bx), ty = std::min(ay, by);
        if (dx == dy)
            return {tx, ty, ny > 0 || nx < 0 ? int(QNorth) : int(QSouth)};
        return {tx, ty, ny > 0 || nx > 0 ? int(QNorth) : int(QSouth)};
    };
    const Side left = dx && dy ? diagonalSide(-dy, dx) : sideOf(-dy, dx);
    const Side right = dx && dy ? diagonalSide(dy, -dx) : sideOf(dy, -dx);
    auto inLot = [&](const Side& s) { return s.x >= 0 && s.y >= 0 && s.x < W && s.y < H; };
    if (!inLot(left) && !inLot(right))
        return;
    // cWallGraph::SetRoomsPostAttach (Mac BV 0xc1f1bc, docs/buy-build-mac.md
    // §7.3, §7.25): a line that closes no cycle leaves the rooms as they are.
    const auto comps = roomComponents(lot, g);
    auto comp = [&](const Side& s) { return comps[tileIndex(lot, li, s.x, s.y)][size_t(s.q)]; };
    if (inLot(left) && inLot(right) && comp(left) == comp(right))
        return;
    bool bridge = false;
    const auto cycle = leftCycle(g, level, ax, ay, bx, by, bridge);
    if (bridge)
        return;
    // It closes one: CreateRoom gives the first ID free in the graph's room
    // list. The new room is the left cycle's, unless the room split is the
    // outdoors (0) and the left cycle does not enclose the line (the parity
    // test FUN_00c1c480): then the right one's.
    const Side& any = inLot(left) ? left : right;
    const uint16_t old = (*rooms)[tileIndex(lot, li, any.x, any.y)][size_t(any.q)];
    bool encloses = false;
    {
        const float px = (float(ax) + float(bx)) / 2 - 0.05f * float(dy), py = (float(ay) + float(by)) / 2 + 0.05f * float(dx);
        for (size_t i = 0, j = cycle.size() - 1; i < cycle.size(); j = i++) {
            const float xi = float(cycle[i][0]), yi = float(cycle[i][1]), xj = float(cycle[j][0]), yj = float(cycle[j][1]);
            if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi)
                encloses = !encloses;
        }
    }
    const Side& chosen = old != 0 || encloses ? left : right;
    if (!inLot(chosen))
        return;
    // (The graph's list as the room array has it: updateRooms keeps it so.)
    std::set<int32_t> used{0};
    for (const auto& q : *rooms)
        for (uint16_t r : q)
            used.insert(r);
    uint16_t id = 1;
    while (used.count(id))
        ++id;
    // Each line of the cycle seeds its side and RefreshMap floods the old
    // room's quarters from there: the chosen side's region.
    const uint32_t region = comp(chosen);
    for (size_t i = 0; i < comps.size(); ++i)
        for (int q = 0; q < 4; ++q)
            if (comps[i][size_t(q)] == region && (*rooms)[i][size_t(q)] == old)
                (*rooms)[i][size_t(q)] = id;
    g.rooms.insert(std::upper_bound(g.rooms.begin(), g.rooms.end(), int32_t(id)), int32_t(id));
}

const Lot::Wall* findWall(const Lot& lot, int level, int ax, int ay, int bx, int by) {
    for (const auto& w : lot.walls)
        if (sameSegment(w.a, w.b, level, ax, ay, bx, by))
            return &w;
    return nullptr;
}

std::optional<uint32_t> addWall(Lot& lot, int level, int ax, int ay, int bx, int by, uint32_t type,
                                std::array<uint16_t, 2> patterns) {
    const int dx = bx - ax, dy = by - ay;
    const bool edge = std::abs(dx) + std::abs(dy) == 1, diagonal = std::abs(dx) == 1 && std::abs(dy) == 1;
    if ((!edge && !diagonal) || findWall(lot, level, ax, ay, bx, by))
        return std::nullopt;
    // A new wall ID after every one the primary graphs have used.
    uint32_t id = 0;
    for (const auto& w : lot.walls)
        id = std::max(id, w.id + 1);
    for (const auto& g : lot.wallGraphs)
        if (primaryGraph(g.instance))
            id = std::max(id, uint32_t(std::max(g.header[7], 0)));
    if (lot.wallGraphs.empty()) { // a lot without graphs: the game's two primary ones
        for (uint32_t instance : {kGraphAll, kGraphLoadBearing, kGraphLight, kGraphDeck, 0x13u, kGraphStandard,
                                  kGraphAttic, kGraphAll2}) {
            Lot::WallGraph g;
            g.instance = instance;
            g.header = {0, 0, lot.minLevel, int32_t(lot.width), int32_t(lot.height), lot.levelCount, 0, 0};
            g.rooms = {0};
            lot.wallGraphs.push_back(g);
        }
    }
    for (auto& g : lot.wallGraphs) {
        if (!inGraph(g.instance, type))
            continue;
        Lot::WallGraph::Line l;
        l.a = vertexAt(g, float(ax), float(ay), level);
        l.b = vertexAt(g, float(bx), float(by), level);
        if (primaryGraph(g.instance)) {
            l.id = id;
            g.header[7] = int32_t(id + 1);
        } else {
            l.id = uint32_t(g.header[7]++);
        }
        g.lines.push_back(l);
        postAttachRooms(lot, g, level, ax, ay, bx, by);
    }
    Lot::Wall w;
    w.id = id;
    w.a = {float(ax), float(ay), level};
    w.b = {float(bx), float(by), level};
    w.type = type;
    w.patterns = patterns;
    lot.walls.push_back(w);
    return id;
}

std::optional<Lot::Wall> removeWall(Lot& lot, int level, int ax, int ay, int bx, int by,
                                    std::vector<RoomMerge>* merges) {
    auto it = std::find_if(lot.walls.begin(), lot.walls.end(),
                           [&](const Lot::Wall& w) { return sameSegment(w.a, w.b, level, ax, ay, bx, by); });
    if (it == lot.walls.end())
        return std::nullopt;
    const Lot::Wall was = *it;
    lot.walls.erase(it);
    for (auto& g : lot.wallGraphs) {
        // A fence arch (graph 0x13) is a partition of its own on the edge,
        // which the binary places without a fence (UserMayPlacePartition):
        // taking the fence away leaves it.
        if (g.instance == 0x13u)
            continue;
        const size_t before = g.lines.size();
        const auto* rooms = merges ? roomArray(lot, g.instance) : nullptr;
        std::erase_if(g.lines, [&](const Lot::WallGraph::Line& l) {
            const auto* a = vertexById(g, l.a);
            const auto* b = vertexById(g, l.b);
            const bool hit =
                a && b && sameSegment({a->x, a->y, a->level}, {b->x, b->y, b->level}, level, ax, ay, bx, by);
            if (hit && rooms) {
                // The rooms on the line's two sides as the room array has them now.
                float dx = b->x - a->x, dy = b->y - a->y;
                const float len = std::hypot(dx, dy);
                if (len > 1e-4f) {
                    dx /= len, dy /= len;
                    float rx = dy, ry = -dx;
                    if (!(std::fabs(dx) > 0.3f && std::fabs(dy) > 0.3f))
                        rx = std::round(rx), ry = std::round(ry);
                    RoomMerge m;
                    m.graph = g.instance;
                    m.right = roomBesideSegment(lot, *rooms, a->level, a->x, a->y, b->x, b->y, rx, ry);
                    m.left = roomBesideSegment(lot, *rooms, a->level, a->x, a->y, b->x, b->y, -rx, -ry);
                    if (m.right != m.left)
                        merges->push_back(m);
                }
            }
            return hit;
        });
        if (g.lines.size() == before)
            continue;
        // Vertices no line uses any more go too.
        std::set<uint32_t> used;
        for (const auto& l : g.lines)
            used.insert(l.a), used.insert(l.b);
        std::erase_if(g.vertices, [&](const Lot::WallGraph::Vertex& v) { return !used.count(v.id); });
    }
    return was;
}

uint16_t patternRef(std::map<uint16_t, std::string>& map, const std::string& name, uint16_t first) {
    uint16_t next = first;
    for (const auto& [ref, n] : map) {
        if (n == name)
            return ref;
        next = std::max<uint16_t>(next, uint16_t(ref + 1));
    }
    map[next] = name;
    return next;
}

bool setFloor(Lot& lot, int level, int x, int y, int q, uint16_t pattern) {
    if (!lot.hasLevel(level) || x < 0 || y < 0 || x >= int(lot.width) || y >= int(lot.height) || q < 0 || q > 3)
        return false;
    if (lot.floors.empty())
        lot.floors.assign(size_t(lot.width) * lot.height * size_t(lot.levelCount), {});
    lot.floors[tileIndex(lot, level - lot.minLevel, x, y)][size_t(q)] = pattern;
    if (lot.floorRotations.empty())
        lot.floorRotations.assign(lot.floors.size(), {1, 1, 1, 1});
    // A quarter left without a floor above the ground is in no room.
    if (!pattern && level != 0) {
        if (!lot.rooms.empty())
            lot.rooms[tileIndex(lot, level - lot.minLevel, x, y)][size_t(q)] = 0;
        for (auto& [k, layer] : lot.roomLayers)
            if (!layer.empty())
                layer[tileIndex(lot, level - lot.minLevel, x, y)][size_t(q)] = 0;
    }
    return true;
}

bool addLowerLevel(Lot& lot) {
    if (lot.minLevel < 0 || lot.elevations.empty())
        return false;
    const size_t plane = size_t(lot.width + 1) * size_t(lot.height + 1), tiles = size_t(lot.width) * lot.height;
    std::vector<float> below(plane);
    for (size_t i = 0; i < plane; ++i)
        below[i] = lot.elevations[size_t(0 - lot.minLevel) * plane + i] - 3.0f;
    lot.elevations.insert(lot.elevations.begin(), below.begin(), below.end());
    if (!lot.floors.empty())
        lot.floors.insert(lot.floors.begin(), tiles, std::array<uint16_t, 4>{});
    if (!lot.floorRotations.empty())
        lot.floorRotations.insert(lot.floorRotations.begin(), tiles, std::array<uint8_t, 4>{1, 1, 1, 1});
    // The locked tiles of the new level: the ground's (as lot_write copies
    // the lowest plane of the arrays it does not read; inferred).
    if (lot.locked.size() >= tiles) {
        const std::vector<uint8_t> ground(lot.locked.begin(), lot.locked.begin() + long(tiles));
        lot.locked.insert(lot.locked.begin(), ground.begin(), ground.end());
    }
    if (!lot.rooms.empty())
        lot.rooms.insert(lot.rooms.begin(), tiles, std::array<uint16_t, 4>{});
    for (auto& [k, layer] : lot.roomLayers)
        if (!layer.empty())
            layer.insert(layer.begin(), tiles, std::array<uint16_t, 4>{});
    --lot.minLevel;
    ++lot.levelCount;
    for (auto& g : lot.wallGraphs)
        g.header[2] = lot.minLevel, g.header[5] = lot.levelCount;
    fitDeckFoundationMarkers(lot);
    return true;
}

int32_t setDeckFoundationMarker(Lot& lot, int level, int x, int y, int q, int32_t style) {
    const size_t tiles = size_t(lot.width) * lot.height;
    if (!tiles || x < 0 || y < 0 || x >= int(lot.width) || y >= int(lot.height) || q < 0 || q > 3 ||
        level < lot.minLevel || level >= lot.minLevel + lot.levelCount - 1)
        return kNoMarkerPlane;
    if (lot.deckFoundationStyles.empty()) {
        lot.deckFoundationBase = lot.minLevel;
        lot.deckFoundationStyles.assign(tiles * size_t(lot.levelCount - 1), {-1, -1, -1, -1});
    }
    fitDeckFoundationMarkers(lot);
    const int32_t old = lot.deckFoundationStyle(level, x, y, q);
    lot.deckFoundationStyles[(size_t(level - lot.deckFoundationBase) * lot.width + size_t(x)) * lot.height +
                             size_t(y)][size_t(q)] = style;
    return old;
}

void fitDeckFoundationMarkers(Lot& lot) {
    const size_t tiles = size_t(lot.width) * lot.height;
    if (lot.deckFoundationStyles.empty() || !tiles)
        return;
    const int planes = std::max(lot.levelCount - 1, 0);
    const size_t oldPlanes = lot.deckFoundationStyles.size() / tiles;
    if (lot.deckFoundationBase == lot.minLevel && oldPlanes == size_t(planes))
        return;
    std::vector<std::array<int32_t, 4>> fitted(tiles * size_t(planes), {-1, -1, -1, -1});
    for (int k = 0; k < planes; ++k) {
        const int from = lot.minLevel + k - lot.deckFoundationBase;
        if (from >= 0 && size_t(from) < oldPlanes)
            std::copy_n(lot.deckFoundationStyles.begin() + long(size_t(from) * tiles), tiles,
                        fitted.begin() + long(size_t(k) * tiles));
    }
    lot.deckFoundationStyles = std::move(fitted);
    lot.deckFoundationBase = lot.minLevel;
}

namespace {
constexpr uint32_t kGraphArches = 0x13;
Lot::WallGraph* archGraph(Lot& lot) {
    for (auto& g : lot.wallGraphs)
        if (g.instance == kGraphArches)
            return &g;
    return nullptr;
}
} // namespace

std::vector<FenceArch> fenceArches(const Lot& lot) {
    std::vector<FenceArch> out;
    for (const auto& g : lot.wallGraphs) {
        if (g.instance != kGraphArches)
            continue;
        for (const auto& l : g.lines) {
            const auto* a = vertexById(g, l.a);
            const auto* b = vertexById(g, l.b);
            if (a && b && a->level == b->level)
                out.push_back({a->level, int(std::lround(a->x)), int(std::lround(a->y)), int(std::lround(b->x)),
                               int(std::lround(b->y))});
        }
    }
    return out;
}

bool hasFenceArch(const Lot& lot, int level, int ax, int ay, int bx, int by) {
    for (const auto& f : fenceArches(lot))
        if (f.level == level && ((f.ax == ax && f.ay == ay && f.bx == bx && f.by == by) ||
                                 (f.ax == bx && f.ay == by && f.bx == ax && f.by == ay)))
            return true;
    return false;
}

bool addFenceArch(Lot& lot, int level, int ax, int ay, int bx, int by) {
    Lot::WallGraph* g = archGraph(lot);
    if (!g || std::abs(bx - ax) + std::abs(by - ay) != 1 || hasFenceArch(lot, level, ax, ay, bx, by))
        return false;
    Lot::WallGraph::Line l;
    l.a = vertexAt(*g, float(ax), float(ay), level);
    l.b = vertexAt(*g, float(bx), float(by), level);
    l.id = uint32_t(g->header[7]++);
    g->lines.push_back(l);
    return true;
}

bool removeFenceArch(Lot& lot, int level, int ax, int ay, int bx, int by) {
    Lot::WallGraph* g = archGraph(lot);
    if (!g)
        return false;
    const size_t before = g->lines.size();
    std::erase_if(g->lines, [&](const Lot::WallGraph::Line& l) {
        const auto* a = vertexById(*g, l.a);
        const auto* b = vertexById(*g, l.b);
        return a && b && sameSegment({a->x, a->y, a->level}, {b->x, b->y, b->level}, level, ax, ay, bx, by);
    });
    return g->lines.size() != before;
}

void addUpperLevel(Lot& lot) {
    const size_t plane = size_t(lot.width + 1) * size_t(lot.height + 1), tiles = size_t(lot.width) * lot.height;
    if (lot.elevations.size() >= plane) {
        std::vector<float> above(lot.elevations.end() - long(plane), lot.elevations.end());
        for (float& z : above)
            z += 3.0f;
        lot.elevations.insert(lot.elevations.end(), above.begin(), above.end());
    }
    if (!lot.floors.empty())
        lot.floors.insert(lot.floors.end(), tiles, std::array<uint16_t, 4>{});
    if (!lot.floorRotations.empty())
        lot.floorRotations.insert(lot.floorRotations.end(), tiles, std::array<uint8_t, 4>{1, 1, 1, 1});
    if (lot.locked.size() >= tiles) {
        const std::vector<uint8_t> top(lot.locked.end() - long(tiles), lot.locked.end());
        lot.locked.insert(lot.locked.end(), top.begin(), top.end());
    }
    if (!lot.rooms.empty())
        lot.rooms.insert(lot.rooms.end(), tiles, std::array<uint16_t, 4>{});
    for (auto& [k, layer] : lot.roomLayers)
        if (!layer.empty())
            layer.insert(layer.end(), tiles, std::array<uint16_t, 4>{});
    ++lot.levelCount;
    for (auto& g : lot.wallGraphs)
        g.header[5] = lot.levelCount;
    fitDeckFoundationMarkers(lot);
}

bool setFloorRotation(Lot& lot, int level, int x, int y, int q, uint8_t rotation) {
    if (!lot.hasLevel(level) || x < 0 || y < 0 || x >= int(lot.width) || y >= int(lot.height) || q < 0 || q > 3)
        return false;
    if (lot.floorRotations.empty())
        lot.floorRotations.assign(size_t(lot.width) * lot.height * size_t(lot.levelCount), {1, 1, 1, 1});
    lot.floorRotations[tileIndex(lot, level - lot.minLevel, x, y)][size_t(q)] = rotation;
    return true;
}

uint8_t floorRotation(const Lot& lot, int level, int x, int y, int q) {
    if (lot.floorRotations.empty() || !lot.hasLevel(level) || x < 0 || y < 0 || x >= int(lot.width) ||
        y >= int(lot.height) || q < 0 || q > 3)
        return 1;
    return lot.floorRotations[tileIndex(lot, level - lot.minLevel, x, y)][size_t(q)];
}

int32_t wallSideRoom(const Lot& lot, const Lot::Wall& wall, int side, uint32_t which) {
    const std::vector<std::array<uint16_t, 4>>* rooms = &lot.rooms;
    if (which != 0x09) {
        auto it = lot.roomLayers.find(which);
        rooms = it == lot.roomLayers.end() ? nullptr : &it->second;
    }
    if (!rooms || rooms->empty())
        return 0;
    float dx = wall.b.x - wall.a.x, dy = wall.b.y - wall.a.y;
    const float len = std::hypot(dx, dy);
    if (len < 1e-4f)
        return 0;
    dx /= len, dy /= len;
    float rx = dy, ry = -dx;
    if (!(std::fabs(dx) > 0.3f && std::fabs(dy) > 0.3f))
        rx = std::round(rx), ry = std::round(ry);
    if (side)
        rx = -rx, ry = -ry;
    return roomBesideSegment(lot, *rooms, wall.a.level, wall.a.x, wall.a.y, wall.b.x, wall.b.y, rx, ry);
}

std::vector<std::array<uint32_t, 4>> roomComponents(const Lot& lot, const Lot::WallGraph& graph) {
    const int W = int(lot.width), H = int(lot.height), L = lot.levelCount;
    const size_t tiles = size_t(W) * H * size_t(L);
    // Blocked edges per level: h(x, y) the edge (x, y)-(x + 1, y), v(x, y)
    // the edge (x, y)-(x, y + 1); diagonal walls per tile: 1 "/", 2 "\".
    const size_t gw = size_t(W + 1), gh = size_t(H + 1);
    std::vector<uint8_t> hBlock(gw * gh * size_t(L)), vBlock(gw * gh * size_t(L)), diag(tiles);
    std::map<uint32_t, const Lot::WallGraph::Vertex*> byId;
    for (const auto& v : graph.vertices)
        byId[v.id] = &v;
    for (const auto& l : graph.lines) {
        auto ia = byId.find(l.a), ib = byId.find(l.b);
        if (ia == byId.end() || ib == byId.end())
            continue;
        const auto& a = *ia->second;
        const auto& b = *ib->second;
        if (!lot.hasLevel(a.level))
            continue;
        const size_t li = size_t(a.level - lot.minLevel);
        int x0 = int(std::lround(a.x)), y0 = int(std::lround(a.y));
        const int x1 = int(std::lround(b.x)), y1 = int(std::lround(b.y));
        const int sx = x1 > x0 ? 1 : x1 < x0 ? -1 : 0, sy = y1 > y0 ? 1 : y1 < y0 ? -1 : 0;
        if (sx && sy && std::abs(x1 - x0) != std::abs(y1 - y0))
            continue;
        while (x0 != x1 || y0 != y1) {
            const int nx = x0 + sx, ny = y0 + sy;
            if (sx && sy) {
                const int tx = std::min(x0, nx), ty = std::min(y0, ny);
                if (tx >= 0 && ty >= 0 && tx < W && ty < H)
                    diag[tileIndex(lot, int(li), tx, ty)] |= (sx == sy) ? 1 : 2;
            } else if (sx) {
                const int tx = std::min(x0, nx);
                if (tx >= 0 && tx < W && y0 >= 0 && y0 <= H)
                    hBlock[(li * gw + size_t(tx)) * gh + size_t(y0)] = 1;
            } else {
                const int ty = std::min(y0, ny);
                if (ty >= 0 && ty < H && x0 >= 0 && x0 <= W)
                    vBlock[(li * gw + size_t(x0)) * gh + size_t(ty)] = 1;
            }
            x0 = nx, y0 = ny;
        }
    }
    Sets sets(tiles * 4);
    auto node = [&](size_t li, int x, int y, int q) { return uint32_t(tileIndex(lot, int(li), x, y) * 4 + size_t(q)); };
    for (size_t li = 0; li < size_t(L); ++li)
        for (int x = 0; x < W; ++x)
            for (int y = 0; y < H; ++y) {
                const uint8_t d = diag[tileIndex(lot, int(li), x, y)];
                // "/" separates west+north from east+south, "\" north+east from south+west.
                if (!(d & 2))
                    sets.join(node(li, x, y, QWest), node(li, x, y, QNorth)),
                        sets.join(node(li, x, y, QEast), node(li, x, y, QSouth));
                if (!(d & 1))
                    sets.join(node(li, x, y, QNorth), node(li, x, y, QEast)),
                        sets.join(node(li, x, y, QSouth), node(li, x, y, QWest));
                if (x + 1 < W && !vBlock[(li * gw + size_t(x + 1)) * gh + size_t(y)])
                    sets.join(node(li, x, y, QEast), node(li, x + 1, y, QWest));
                if (y + 1 < H && !hBlock[(li * gw + size_t(x)) * gh + size_t(y + 1)])
                    sets.join(node(li, x, y, QNorth), node(li, x, y + 1, QSouth));
            }
    std::vector<std::array<uint32_t, 4>> out(tiles);
    for (size_t i = 0; i < tiles; ++i)
        for (int q = 0; q < 4; ++q)
            out[i][size_t(q)] = sets.find(uint32_t(i * 4 + size_t(q)));
    return out;
}

void updateRooms(Lot& lot, const std::vector<RoomMerge>& merges) {
    const int W = int(lot.width), H = int(lot.height);
    for (auto& g : lot.wallGraphs) {
        auto* rooms = roomArray(lot, g.instance);
        if (!rooms)
            continue;
        // The rooms the removed walls joined, in order: the right-hand room
        // survives, room 0 always does.
        std::map<int32_t, int32_t> merged;
        auto survivor = [&](int32_t r) {
            for (auto it = merged.find(r); it != merged.end(); it = merged.find(r))
                r = it->second;
            return r;
        };
        for (const RoomMerge& m : merges) {
            if (m.graph != g.instance)
                continue;
            const int32_t r = survivor(m.right), l = survivor(m.left);
            if (r == l)
                continue;
            if (r == 0 || l == 0)
                merged[r ? r : l] = 0;
            else
                merged[l] = r;
        }
        const auto comps = roomComponents(lot, g);
        auto floorAt = [&](size_t i, int q) { return !lot.floors.empty() && lot.floors[i][size_t(q)] != 0; };
        // Per component: its quarters' old rooms, size, and whether it
        // reaches the edge of the lot.
        struct Comp {
            size_t size = 0;
            std::map<uint16_t, size_t> old;
            bool edge = false, outside = false; // reaches the lot's edge; holds quarters of room 0 with terrain
            bool zero = false;                  // joined into room 0 by a removed wall
            bool hasZero = false;               // holds quarters of room 0
            uint16_t id = 0;
        };
        std::map<uint32_t, Comp> cs;
        for (size_t i = 0; i < comps.size(); ++i) {
            const int li = int(i / (size_t(W) * size_t(H)));
            const int x = int(i / size_t(H) % size_t(W)), y = int(i % size_t(H));
            for (int q = 0; q < 4; ++q) {
                Comp& c = cs[comps[i][size_t(q)]];
                ++c.size;
                if (const uint16_t r = (*rooms)[i][size_t(q)])
                    ++c.old[r];
                else if (c.hasZero = true; li == -lot.minLevel)
                    c.outside = true;
                c.edge |= (x == 0 && q == QWest) || (y == 0 && q == QSouth) || (x == W - 1 && q == QEast) ||
                          (y == H - 1 && q == QNorth);
            }
        }
        // Larger rooms pick first; each keeps the old ID most of its
        // quarters had, if no larger room took it. Room 0 always survives
        // (cWallGraph::SetRoomsPreDetach): a region joined to the outdoors
        // of the ground (room 0 reaching the lot's edge) is outdoors.
        std::vector<std::pair<uint32_t, Comp*>> order;
        for (auto& [k, c] : cs)
            order.push_back({k, &c});
        std::stable_sort(order.begin(), order.end(),
                         [](const auto& a, const auto& b) { return a.second->size > b.second->size; });
        std::set<uint16_t> taken;
        // A region whose old rooms were all joined into one keeps that one.
        for (auto& [k, c] : order) {
            if (merged.empty() || c->old.size() + size_t(c->hasZero) < 2 || (c->edge && c->outside))
                continue;
            std::set<int32_t> to;
            for (const auto& [r, n] : c->old)
                to.insert(survivor(r));
            if (c->hasZero)
                to.insert(0);
            if (to.size() != 1)
                continue;
            const int32_t s = *to.begin();
            if (s == 0) {
                c->zero = true;
            } else if (!taken.count(uint16_t(s))) {
                c->id = uint16_t(s);
                taken.insert(uint16_t(s));
            }
        }
        for (auto& [k, c] : order) {
            if (c->id || c->zero)
                continue;
            if (c->edge && c->outside) {
                c->old.clear();
                continue;
            }
            std::vector<std::pair<size_t, uint16_t>> cand;
            for (const auto& [r, n] : c->old)
                cand.push_back({n, r});
            std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first : a.second < b.second;
            });
            for (const auto& [n, r] : cand)
                if (!taken.count(r)) {
                    c->id = r;
                    taken.insert(r);
                    break;
                }
        }
        for (size_t i = 0; i < comps.size(); ++i) {
            for (int q = 0; q < 4; ++q) {
                Comp& c = cs[comps[i][size_t(q)]];
                uint16_t& r = (*rooms)[i][size_t(q)];
                // A region reaching the edge of the lot that had no room
                // stays out (the street beyond the lot's boundary, the
                // outdoors of the upper levels).
                // The ground's terrain counts as a floor (a room drawn on
                // bare ground is a room).
                const bool ground = int(i / (size_t(W) * size_t(H))) == -lot.minLevel;
                const bool keep =
                    (r != 0 || floorAt(i, q) || ground) && !(c.edge && c.old.empty()) && !c.zero;
                if (!keep) {
                    r = 0;
                    continue;
                }
                // A new room takes the smallest free ID (CreateRoom: the
                // first hole in the graph's sorted room list).
                if (!c.id) {
                    uint16_t id = 1;
                    while (taken.count(id))
                        ++id;
                    c.id = id;
                    taken.insert(id);
                }
                r = c.id;
            }
        }
        // The graph's lines and room list.
        std::set<int32_t> ids;
        for (const auto& q : *rooms)
            for (uint16_t r : q)
                ids.insert(r);
        g.rooms.assign(ids.begin(), ids.end());
        std::map<uint32_t, const Lot::WallGraph::Vertex*> byId;
        for (const auto& v : g.vertices)
            byId[v.id] = &v;
        for (auto& l : g.lines) {
            auto ia = byId.find(l.a), ib = byId.find(l.b);
            if (ia == byId.end() || ib == byId.end())
                continue;
            const auto& a = *ia->second;
            const auto& b = *ib->second;
            float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::hypot(dx, dy);
            if (len < 1e-4f)
                continue;
            dx /= len, dy /= len;
            const bool diagonal = std::fabs(dx) > 0.3f && std::fabs(dy) > 0.3f;
            // Right of a -> b is (dy, -dx).
            float rx = dy, ry = -dx;
            if (!diagonal)
                rx = std::round(rx), ry = std::round(ry);
            l.roomRight = roomBesideSegment(lot, *rooms, a.level, a.x, a.y, b.x, b.y, rx, ry);
            l.roomLeft = roomBesideSegment(lot, *rooms, a.level, a.x, a.y, b.x, b.y, -rx, -ry);
        }
    }
}

} // namespace os2
