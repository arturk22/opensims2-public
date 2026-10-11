#include "lot_edit.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

namespace os2 {

namespace {

constexpr uint32_t kGraphAll = 5, kGraphAll2 = 0x18, kGraphLoadBearing = 6, kGraphLight = 7, kGraphDeck = 8,
                   kGraphStandard = 0x16, kGraphAttic = 0x17;
constexpr uint32_t kRoomsLoadBearing = 0x0A, kRoomsLight = 0x0B;

// The graphs a partition type goes into, from what the saved lots hold
// (82 lots of the base game: every type in 5 and 0x18; walls 1, 23
// (foundation), 29 (pool), 16 and 4 (deck skirts) in 6; 1, 3 (attic) and 23
// in 7; 1, 23 and 29 in 0x16; 3 in 0x17; 16 in 8). Their meaning follows
// walls.txt (bearsLoad, blocksLight, verticalSpan floorToFloor with a
// standard thickness, floorToRoof); fences are in 5 and 0x18 only.
bool inGraph(uint32_t graph, uint32_t type) {
    switch (graph) {
    case kGraphAll:
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

} // namespace

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

std::optional<Lot::Wall> removeWall(Lot& lot, int level, int ax, int ay, int bx, int by) {
    auto it = std::find_if(lot.walls.begin(), lot.walls.end(),
                           [&](const Lot::Wall& w) { return sameSegment(w.a, w.b, level, ax, ay, bx, by); });
    if (it == lot.walls.end())
        return std::nullopt;
    const Lot::Wall was = *it;
    lot.walls.erase(it);
    for (auto& g : lot.wallGraphs) {
        const size_t before = g.lines.size();
        std::erase_if(g.lines, [&](const Lot::WallGraph::Line& l) {
            const auto* a = vertexById(g, l.a);
            const auto* b = vertexById(g, l.b);
            return a && b && sameSegment({a->x, a->y, a->level}, {b->x, b->y, b->level}, level, ax, ay, bx, by);
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

void updateRooms(Lot& lot) {
    const int W = int(lot.width), H = int(lot.height);
    for (auto& g : lot.wallGraphs) {
        auto* rooms = roomArray(lot, g.instance);
        if (!rooms)
            continue;
        const auto comps = roomComponents(lot, g);
        auto floorAt = [&](size_t i, int q) { return !lot.floors.empty() && lot.floors[i][size_t(q)] != 0; };
        // Per component: its quarters' old rooms, size, and whether it
        // reaches the edge of the lot.
        struct Comp {
            size_t size = 0;
            std::map<uint16_t, size_t> old;
            bool edge = false;
            uint16_t id = 0;
        };
        std::map<uint32_t, Comp> cs;
        uint16_t next = 1;
        for (size_t i = 0; i < comps.size(); ++i) {
            const int li = int(i / (size_t(W) * size_t(H)));
            const int x = int(i / size_t(H) % size_t(W)), y = int(i % size_t(H));
            for (int q = 0; q < 4; ++q) {
                Comp& c = cs[comps[i][size_t(q)]];
                ++c.size;
                if (const uint16_t r = (*rooms)[i][size_t(q)]) {
                    ++c.old[r];
                    next = std::max<uint16_t>(next, uint16_t(r + 1));
                }
                c.edge |= (x == 0 && q == QWest) || (y == 0 && q == QSouth) || (x == W - 1 && q == QEast) ||
                          (y == H - 1 && q == QNorth);
                (void)li;
            }
        }
        // Larger rooms pick first; each keeps the old ID most of its
        // quarters had, if no larger room took it.
        std::vector<std::pair<uint32_t, Comp*>> order;
        for (auto& [k, c] : cs)
            order.push_back({k, &c});
        std::stable_sort(order.begin(), order.end(),
                         [](const auto& a, const auto& b) { return a.second->size > b.second->size; });
        std::set<uint16_t> taken;
        for (auto& [k, c] : order) {
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
                const bool keep = (r != 0 || floorAt(i, q)) && !(c.edge && c.old.empty());
                if (!keep) {
                    r = 0;
                    continue;
                }
                if (!c.id)
                    c.id = next++;
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
