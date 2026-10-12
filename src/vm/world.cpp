#include "world.hpp"

#include "formats/lot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <queue>

namespace os2::vm {

namespace {
// Steps of directions 0-7: 0 faces -X, 2 +Y, 4 +X, 6 -Y (see world.hpp).
constexpr std::array<std::array<int, 2>, 8> kSteps{{{-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}}};
} // namespace

std::array<int, 2> directionStep(int dir) { return kSteps[size_t(((dir % 8) + 8) % 8)]; }

int directionOf(float dx, float dy) {
    if (dx == 0 && dy == 0)
        return -1;
    // Direction 0 is at 180 degrees and 2 at 90 (+Y), so direction = (180 - angle) / 45.
    const double angle = std::atan2(double(dy), double(dx)) * 180.0 / M_PI;
    const int d = int(std::lround((180.0 - angle) / 45.0));
    return ((d % 8) + 8) % 8;
}

World::World(uint32_t width, uint32_t height, int firstLevel, int levelCount)
    : width_(width), height_(height), firstLevel_(firstLevel), levelCount_(levelCount) {
    const size_t levels = size_t(std::max(levelCount, 0));
    walkable_.assign(levels * width * height, 1);
    rooms_.assign(levels * width * height, 0);
    diagonals_.assign(levels * width * height, 0);
    costs_.assign(levels * width * height, 1.0f);
    xWalls_.assign(levels * width * (height + 1), 0);
    yWalls_.assign(levels * (width + 1) * height, 0);
}

World World::fromLot(const Lot& lot) {
    // VM levels count from 1 = the lot's ground level (0).
    World w(lot.width, lot.height, lot.minLevel + 1, lot.levelCount);
    for (int li = 0; li < lot.levelCount; ++li) {
        const int lotLevel = lot.minLevel + li;
        for (uint32_t x = 0; x < lot.width; ++x)
            for (uint32_t y = 0; y < lot.height; ++y) {
                const TilePos t{int(x), int(y), lotLevel + 1};
                const size_t i = (size_t(li) * lot.width + x) * lot.height + y;
                if (lotLevel != 0) {
                    bool floor = false;
                    if (!lot.floors.empty())
                        for (uint16_t q : lot.floors[i])
                            floor |= q != 0;
                    w.setWalkable(t, floor);
                } else if (!lot.elevations.empty()) {
                    // The terrain's slope and water, as the Mac binary's
                    // WorkOutTileCostAndPassability has them (docs/routing-mac.md
                    // §2): a rise of 1 or more across the tile is impassable,
                    // 0.5-1 costs (rise - 0.5) * 20 if that is more; water
                    // over the terrain is impassable.
                    float lo = std::numeric_limits<float>::infinity(), hi = -lo;
                    bool water = false;
                    for (int cx = 0; cx < 2; ++cx)
                        for (int cy = 0; cy < 2; ++cy) {
                            const float h = lot.elevation(lotLevel, int(x) + cx, int(y) + cy);
                            lo = std::min(lo, h), hi = std::max(hi, h);
                            water |= lot.waterLevel(int(x) + cx, int(y) + cy) > h + 0.01f;
                        }
                    // A pool's tiles are water too (inferred: swimmers go in
                    // by the ladders' own routes).
                    for (const auto& p : lot.pools)
                        if (p.level == 0 && float(x) >= p.x - 0.01f && float(x) < p.x + float(p.width) - 0.01f &&
                            float(y) >= p.y - 0.01f && float(y) < p.y + float(p.height) - 0.01f)
                            water = true, w.setPool(t), w.setPool({t.x, t.y, t.level - 1});
                    const float rise = hi - lo;
                    if (rise >= 1.0f || water)
                        w.setWalkable(t, false);
                    else if (rise > 0.5f)
                        w.setCost(t, std::max(1.0f, (rise - 0.5f) * 20.0f));
                }
                if (!lot.rooms.empty())
                    for (uint16_t r : lot.rooms[i])
                        if (r) {
                            w.setRoom(t, int16_t(r));
                            break;
                        }
            }
    }
    for (const auto& wall : lot.walls) {
        auto snap = [](float v) { return int(std::lround(v)); };
        w.addWall(wall.a.level + 1, snap(wall.a.x), snap(wall.a.y), snap(wall.b.x), snap(wall.b.y),
                  Lot::isFence(wall.type) && walkOverFences().count(wall.type));
    }
    for (const Stair& s : lotStairs(lot))
        w.addStair(s);
    return w;
}

namespace {
std::set<uint32_t>& walkOverSet() {
    static std::set<uint32_t> s;
    return s;
}
} // namespace

void setWalkOverFences(std::set<uint32_t> guids) { walkOverSet() = std::move(guids); }
const std::set<uint32_t>& walkOverFences() { return walkOverSet(); }

std::vector<Stair> lotStairs(const Lot& lot) {
    constexpr float kRise = 0.1875f, kRun = 0.25f, kTolerance = 0.03f;
    struct Step {
        float x, y, z;
        bool end;
    };
    std::vector<Step> steps;
    std::vector<const Lot::Object*> tops;
    for (const auto& o : lot.objects) {
        const std::string& d = o.definition;
        if (d.rfind("Stair Modular - ", 0) != 0)
            continue;
        if (d.find(" - Portal - A_objt") != std::string::npos)
            tops.push_back(&o);
        else if (d.find(" - Bridge_objt") != std::string::npos || d.find(" - End_objt") != std::string::npos)
            steps.push_back({o.position[0], o.position[1], o.position[2], d.find(" - End_objt") != std::string::npos});
    }
    auto stepAt = [&](float x, float y, float z) -> const Step* {
        for (const auto& s : steps)
            if (std::fabs(s.x - x) < kTolerance && std::fabs(s.y - y) < kTolerance && std::fabs(s.z - z) < kTolerance)
                return &s;
        return nullptr;
    };
    // The VM level whose floor is at height z at a point, if any.
    auto levelAt = [&](float x, float y, float z) -> std::optional<int> {
        for (int l = lot.minLevel; l < lot.minLevel + lot.levelCount; ++l)
            if (std::fabs(lot.elevationAt(l, x, y) - z) < 0.1f)
                return l + 1;
        return std::nullopt;
    };
    std::vector<Stair> out;
    for (const Lot::Object* top : tops) {
        // The steps face the way down; the model's forward axis is +Y, turned
        // by the rotation about Z (a unit quaternion).
        const auto& q = top->rotation;
        const float yaw = 2 * std::atan2(q[2], q[3]);
        const float downX = -std::sin(yaw), downY = std::cos(yaw);
        const float dx = std::fabs(downX) < 0.5f ? 0 : (downX > 0 ? 1.0f : -1.0f);
        const float dy = std::fabs(downY) < 0.5f ? 0 : (downY > 0 ? 1.0f : -1.0f);
        if ((dx != 0) == (dy != 0))
            continue; // only straight stairs along an axis
        float x = top->position[0], y = top->position[1], z = top->position[2];
        // Saved lots miss some steps (stale copies of the stair in another
        // style fill in for each other, and some are gone from both), so a
        // few missing steps in a row are bridged.
        int count = 1, missing = 0;
        bool complete = false;
        for (int i = 0; i < 64 && missing <= 3; ++i) {
            x += dx * kRun, y += dy * kRun, z -= kRise;
            ++count;
            const Step* s = stepAt(x, y, z);
            if (!s) {
                ++missing;
                continue;
            }
            missing = 0;
            if (s->end) {
                complete = true;
                break;
            }
        }
        if (!complete)
            continue;
        Stair st;
        st.steps = count;
        st.up = {-dx, -dy};
        st.bottomEdge = {x, y}; // the bottom step's position is the stair's bottom edge
        st.direction = directionOf(-dx, -dy);
        const auto below = st.at(-0.5f), above = st.at(st.length() + 0.5f);
        const auto lower = levelAt(below[0], below[1], z);
        const auto upper = levelAt(above[0], above[1], top->position[2] + kRise);
        if (!lower || !upper || *upper != *lower + 1)
            continue;
        st.bottom = {int(std::floor(below[0])), int(std::floor(below[1])), *lower};
        st.top = {int(std::floor(above[0])), int(std::floor(above[1])), *upper};
        st.bottomZ = z;
        st.topZ = top->position[2] + kRise;
        const bool seen = std::any_of(out.begin(), out.end(), [&](const Stair& o) {
            return o.bottom == st.bottom && o.top == st.top;
        });
        if (!seen)
            out.push_back(st);
    }
    return out;
}

void World::addStair(const Stair& s) {
    ++revision_;
    // The tiles the steps stand on, on the lower level.
    for (int i = 0; i < s.steps; ++i) {
        const auto p = s.at(0.125f + 0.25f * float(i));
        setWalkable({int(std::floor(p[0])), int(std::floor(p[1])), s.bottom.level}, false);
    }
    stairs_.push_back(s);
}

bool World::contains(TilePos t) const {
    return t.x >= 0 && t.y >= 0 && uint32_t(t.x) < width_ && uint32_t(t.y) < height_ && t.level >= firstLevel_ &&
           t.level < firstLevel_ + levelCount_;
}

size_t World::cell(TilePos t) const { return size_t(t.level - firstLevel_) * width_ * height_ + index(t.x, t.y); }

size_t World::xEdge(int level, int x, int y) const {
    return (size_t(level - firstLevel_) * width_ + size_t(x)) * (height_ + 1) + size_t(y);
}

size_t World::yEdge(int level, int x, int y) const {
    return (size_t(level - firstLevel_) * (width_ + 1) + size_t(x)) * height_ + size_t(y);
}

bool World::walkable(TilePos t) const {
    return contains(t) && walkable_[cell(t)] && diagonals_[cell(t)] != 1;
}

bool World::hasFloor(TilePos t) const { return contains(t) && walkable_[cell(t)]; }

float World::cost(TilePos t) const { return contains(t) ? costs_[cell(t)] : 1.0f; }

void World::setCost(TilePos t, float cost) {
    ++revision_;
    if (contains(t))
        costs_[cell(t)] = cost;
}

bool World::diagonalWall(TilePos t) const { return contains(t) && diagonals_[cell(t)] != 0; }

void World::setWalkable(TilePos t, bool walkable) {
    ++revision_;
    if (contains(t))
        walkable_[cell(t)] = walkable;
}

int16_t World::room(TilePos t) const { return contains(t) ? rooms_[cell(t)] : 0; }

void World::setRoom(TilePos t, int16_t room) {
    ++revision_;
    if (contains(t))
        rooms_[cell(t)] = room;
}

void World::setEdge(int level, int ax, int ay, int bx, int by, uint8_t wall) {
    ++revision_;
    if (level < firstLevel_ || level >= firstLevel_ + levelCount_)
        return;
    if (ay == by && ax != bx) {
        for (int x = std::min(ax, bx); x < std::max(ax, bx); ++x)
            if (x >= 0 && uint32_t(x) < width_ && ay >= 0 && uint32_t(ay) <= height_)
                if (uint8_t& e = xWalls_[xEdge(level, x, ay)]; wall != 2 || e != 1)
                    e = wall;
    } else if (ax == bx && ay != by) {
        for (int y = std::min(ay, by); y < std::max(ay, by); ++y)
            if (ax >= 0 && uint32_t(ax) <= width_ && y >= 0 && uint32_t(y) < height_)
                if (uint8_t& e = yWalls_[yEdge(level, ax, y)]; wall != 2 || e != 1)
                    e = wall;
    }
}

void World::addWall(int level, int ax, int ay, int bx, int by, bool walkOver) {
    ++revision_;
    const int dx = bx - ax, dy = by - ay;
    if (dx && std::abs(dx) == std::abs(dy)) {
        if (walkOver) // a low fence across a tile: nothing for routing (inferred)
            return;
        const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
        if (level >= firstLevel_ && level < firstLevel_ + levelCount_)
            diagonalWalls_.push_back({level, ax, ay, bx, by});
        for (int i = 0; i < std::abs(dx); ++i) {
            // Each unit step crosses the tile between its two grid points.
            const int x0 = ax + i * sx, y0 = ay + i * sy;
            const TilePos t{std::min(x0, x0 + sx), std::min(y0, y0 + sy), level};
            if (contains(t) && diagonals_[cell(t)] == 0)
                diagonals_[cell(t)] = 1;
        }
        return;
    }
    setEdge(level, ax, ay, bx, by, walkOver ? 2 : 1);
}

void World::openDoor(TilePos t, int dir) {
    ++revision_;
    // The edge between the door's tile and the tile behind it.
    if (contains(t) && diagonals_[cell(t)]) {
        diagonals_[cell(t)] = 2; // a door in the diagonal wall crossing its tile
        doors_.push_back({t, ((dir % 8) + 8) % 8, true});
        return;
    }
    const auto [dx, dy] = directionStep(dir + 4);
    if (std::abs(dx) + std::abs(dy) != 1)
        return;
    if (contains(t))
        doors_.push_back({t, ((dir % 8) + 8) % 8, false});
    if (dx)
        setEdge(t.level, t.x + (dx > 0 ? 1 : 0), t.y, t.x + (dx > 0 ? 1 : 0), t.y + 1, 0);
    else
        setEdge(t.level, t.x, t.y + (dy > 0 ? 1 : 0), t.x + 1, t.y + (dy > 0 ? 1 : 0), 0);
}

bool World::wallBetween(TilePos t, int dx, int dy, bool placement) const {
    if (t.level < firstLevel_ || t.level >= firstLevel_ + levelCount_)
        return false;
    if (dx && !dy) {
        const int x = t.x + (dx > 0 ? 1 : 0); // the vertical edge at grid x
        if (x < 0 || uint32_t(x) > width_ || t.y < 0 || uint32_t(t.y) >= height_)
            return false;
        const uint8_t e = yWalls_[yEdge(t.level, x, t.y)];
        return e == 1 || (placement && e);
    }
    if (dy && !dx) {
        const int y = t.y + (dy > 0 ? 1 : 0);
        if (y < 0 || uint32_t(y) > height_ || t.x < 0 || uint32_t(t.x) >= width_)
            return false;
        const uint8_t e = xWalls_[xEdge(t.level, t.x, y)];
        return e == 1 || (placement && e);
    }
    return false;
}

bool World::lineOfSight(TilePos a, TilePos b) const {
    if (a.level != b.level)
        return false;
    auto crossesDiagonal = [&](int x, int y) {
        const TilePos t{x, y, a.level};
        return !(t == a) && !(t == b) && contains(t) && diagonals_[cell(t)] == 1;
    };
    // Walk the tiles the segment between the centres crosses (a supercover
    // line), checking the edge of each step; a step through a corner checks
    // both ways around it and passes if either is open.
    int x = a.x, y = a.y;
    const int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y);
    const int sx = b.x > a.x ? 1 : -1, sy = b.y > a.y ? 1 : -1;
    int ix = 0, iy = 0;
    while (ix < dx || iy < dy) {
        const long lhs = long(1 + 2 * ix) * dy, rhs = long(1 + 2 * iy) * dx;
        if (lhs == rhs) { // through a grid corner
            const TilePos t{x, y, a.level};
            const bool viaX = !wallBetween(t, sx, 0) && !wallBetween({x + sx, y, a.level}, 0, sy);
            const bool viaY = !wallBetween(t, 0, sy) && !wallBetween({x, y + sy, a.level}, sx, 0);
            if (!viaX && !viaY)
                return false;
            x += sx, y += sy, ++ix, ++iy;
            if (crossesDiagonal(x, y))
                return false;
        } else if (lhs < rhs) {
            if (wallBetween({x, y, a.level}, sx, 0))
                return false;
            x += sx, ++ix;
            if (crossesDiagonal(x, y))
                return false;
        } else {
            if (wallBetween({x, y, a.level}, 0, sy))
                return false;
            y += sy, ++iy;
            if (crossesDiagonal(x, y))
                return false;
        }
    }
    return true;
}

bool canStep(const World& world, const BlockedGrid& blocked, TilePos from, int dx, int dy) {
    auto open = [&](TilePos t) { return world.walkable(t) && !blocked[world.index(t.x, t.y)]; };
    const TilePos to{from.x + dx, from.y + dy, from.level};
    if ((dx == 0 && dy == 0) || !open(to))
        return false;
    if (dx == 0 || dy == 0)
        return !world.wallBetween(from, dx, dy);
    // Diagonal: both ways around the corner must be open.
    const TilePos sideX{from.x + dx, from.y, from.level}, sideY{from.x, from.y + dy, from.level};
    return open(sideX) && open(sideY) && !world.wallBetween(from, dx, 0) && !world.wallBetween(sideX, 0, dy) &&
           !world.wallBetween(from, 0, dy) && !world.wallBetween(sideY, dx, 0);
}

bool findPath(const World& world, const BlockedGrid& blocked, TilePos start, const std::vector<PathGoal>& goals,
              Path& out) {
    out = {};
    if (!world.contains(start) || goals.empty())
        return false;
    const size_t n = size_t(world.width()) * world.height();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    // Goal cost by tile (the cheapest goal on a tile wins).
    std::vector<float> goalCost(n, kInf);
    std::vector<int> goalOf(n, -1);
    bool anyGoal = false;
    for (size_t i = 0; i < goals.size(); ++i) {
        const TilePos g{goals[i].x, goals[i].y, start.level};
        if (!world.contains(g))
            continue;
        const size_t k = world.index(g.x, g.y);
        const bool reachable = (g == start) || (world.walkable(g) && !blocked[k]);
        if (reachable && goals[i].cost < goalCost[k]) {
            goalCost[k] = goals[i].cost;
            goalOf[k] = int(i);
            anyGoal = true;
        }
    }
    if (!anyGoal)
        return false;
    // Heuristic: octile distance plus goal cost to the nearest goal (admissible).
    std::vector<std::pair<TilePos, float>> goalTiles;
    for (size_t k = 0; k < n; ++k)
        if (goalOf[k] >= 0)
            goalTiles.push_back({{int(k / world.height()), int(k % world.height()), start.level}, goalCost[k]});
    auto heuristic = [&](int x, int y) {
        float best = kInf;
        for (const auto& [g, c] : goalTiles) {
            const float dx = float(std::abs(g.x - x)), dy = float(std::abs(g.y - y));
            best = std::min(best, std::max(dx, dy) + (float(M_SQRT2) - 1) * std::min(dx, dy) + c);
        }
        return best;
    };

    std::vector<float> dist(n, kInf);
    std::vector<int> parent(n, -1);
    std::vector<uint8_t> closed(n, 0);
    using Entry = std::pair<float, size_t>; // f, tile
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    const size_t s0 = world.index(start.x, start.y);
    dist[s0] = 0;
    open.push({heuristic(start.x, start.y), s0});
    // A popped node whose f includes the goal cost: the search ends when
    // the cheapest finished goal is not worse than any open node.
    float bestTotal = kInf;
    size_t bestTile = 0;
    while (!open.empty()) {
        const auto [f, k] = open.top();
        open.pop();
        if (f >= bestTotal)
            break;
        if (closed[k])
            continue;
        closed[k] = 1;
        if (goalOf[k] >= 0 && dist[k] + goalCost[k] < bestTotal) {
            bestTotal = dist[k] + goalCost[k];
            bestTile = k;
        }
        const TilePos t{int(k / world.height()), int(k % world.height()), start.level};
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                if ((!dx && !dy) || !canStep(world, blocked, t, dx, dy))
                    continue;
                const size_t m = world.index(t.x + dx, t.y + dy);
                const float d = dist[k] + (dx && dy ? float(M_SQRT2) : 1.0f);
                if (d < dist[m]) {
                    dist[m] = d;
                    parent[m] = int(k);
                    open.push({d + heuristic(t.x + dx, t.y + dy), m});
                }
            }
    }
    if (bestTotal == kInf)
        return false;
    for (size_t k = bestTile; k != s0; k = size_t(parent[k]))
        out.tiles.push_back({int(k / world.height()), int(k % world.height()), start.level});
    std::reverse(out.tiles.begin(), out.tiles.end());
    out.goal = size_t(goalOf[bestTile]);
    out.cost = bestTotal;
    return true;
}

bool findPath(const World& world, std::span<const BlockedGrid> blocked, TilePos start, int goalLevel,
              const std::vector<PathGoal>& goals, Path& out, std::vector<int>& stairs) {
    out = {};
    stairs.clear();
    if (!world.contains(start) || goals.empty() || blocked.size() < size_t(world.levelCount()))
        return false;
    const size_t n = size_t(world.width()) * world.height();
    const size_t levels = size_t(world.levelCount());
    const int first = world.firstLevel();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    auto node = [&](TilePos t) { return size_t(t.level - first) * n + world.index(t.x, t.y); };
    auto tileOf = [&](size_t k) {
        const size_t i = k % n;
        return TilePos{int(i / world.height()), int(i % world.height()), first + int(k / n)};
    };
    const size_t s0 = node(start);
    std::vector<float> goalCost(n * levels, kInf);
    std::vector<int> goalOf(n * levels, -1);
    std::vector<std::pair<TilePos, float>> goalTiles;
    for (size_t i = 0; i < goals.size(); ++i) {
        const TilePos g{goals[i].x, goals[i].y, goalLevel};
        if (!world.contains(g))
            continue;
        const size_t k = node(g);
        const bool reachable =
            (g == start) || (world.walkable(g) && !blocked[size_t(goalLevel - first)][world.index(g.x, g.y)]);
        if (reachable && goals[i].cost < goalCost[k]) {
            goalCost[k] = goals[i].cost;
            goalOf[k] = int(i);
        }
    }
    for (size_t k = 0; k < goalOf.size(); ++k)
        if (goalOf[k] >= 0)
            goalTiles.push_back({tileOf(k), goalCost[k]});
    if (goalTiles.empty())
        return false;
    // Stair landings by node, both ways.
    struct Link {
        size_t to;
        float cost;
        int stair;
    };
    std::vector<std::vector<Link>> links(n * levels);
    const auto& all = world.stairs();
    for (size_t i = 0; i < all.size(); ++i) {
        const Stair& st = all[i];
        if (!world.contains(st.bottom) || !world.contains(st.top))
            continue;
        const float cost = 1.5f * (st.length() + 1.0f);
        const size_t a = node(st.bottom), b = node(st.top);
        links[a].push_back({b, cost, int(i)});
        links[b].push_back({a, cost, int(i)});
    }
    // Octile distance to the nearest goal, ignoring levels (stairs cost more
    // than the distance they cover, so this stays admissible).
    auto heuristic = [&](TilePos t) {
        float best = kInf;
        for (const auto& [g, c] : goalTiles) {
            const float dx = float(std::abs(g.x - t.x)), dy = float(std::abs(g.y - t.y));
            best = std::min(best, std::max(dx, dy) + (float(M_SQRT2) - 1) * std::min(dx, dy) + c);
        }
        return best;
    };
    std::vector<float> dist(n * levels, kInf);
    std::vector<int64_t> parent(n * levels, -1);
    std::vector<int> via(n * levels, -1);
    std::vector<uint8_t> closed(n * levels, 0);
    using Entry = std::pair<float, size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    dist[s0] = 0;
    open.push({heuristic(start), s0});
    float bestTotal = kInf;
    size_t bestNode = 0;
    auto relax = [&](size_t from, size_t to, float d, int stair) {
        if (d < dist[to]) {
            dist[to] = d;
            parent[to] = int64_t(from);
            via[to] = stair;
            open.push({d + heuristic(tileOf(to)), to});
        }
    };
    while (!open.empty()) {
        const auto [f, k] = open.top();
        open.pop();
        if (f >= bestTotal)
            break;
        if (closed[k])
            continue;
        closed[k] = 1;
        if (goalOf[k] >= 0 && dist[k] + goalCost[k] < bestTotal) {
            bestTotal = dist[k] + goalCost[k];
            bestNode = k;
        }
        const TilePos t = tileOf(k);
        const BlockedGrid& grid = blocked[size_t(t.level - first)];
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if ((dx || dy) && canStep(world, grid, t, dx, dy))
                    relax(k, node({t.x + dx, t.y + dy, t.level}), dist[k] + (dx && dy ? float(M_SQRT2) : 1.0f), -1);
        for (const Link& l : links[k]) {
            const TilePos to = tileOf(l.to);
            if (world.walkable(to) && !blocked[size_t(to.level - first)][world.index(to.x, to.y)])
                relax(k, l.to, dist[k] + l.cost, l.stair);
        }
    }
    if (bestTotal == kInf)
        return false;
    for (size_t k = bestNode; k != s0; k = size_t(parent[k])) {
        out.tiles.push_back(tileOf(k));
        stairs.push_back(via[k]);
    }
    std::reverse(out.tiles.begin(), out.tiles.end());
    std::reverse(stairs.begin(), stairs.end());
    out.goal = size_t(goalOf[bestNode]);
    out.cost = bestTotal;
    return true;
}

} // namespace os2::vm
