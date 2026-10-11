#include "world.hpp"

#include "formats/lot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
        w.addWall(wall.a.level + 1, snap(wall.a.x), snap(wall.a.y), snap(wall.b.x), snap(wall.b.y));
    }
    return w;
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

bool World::walkable(TilePos t) const { return contains(t) && walkable_[cell(t)]; }

void World::setWalkable(TilePos t, bool walkable) {
    if (contains(t))
        walkable_[cell(t)] = walkable;
}

int16_t World::room(TilePos t) const { return contains(t) ? rooms_[cell(t)] : 0; }

void World::setRoom(TilePos t, int16_t room) {
    if (contains(t))
        rooms_[cell(t)] = room;
}

void World::setEdge(int level, int ax, int ay, int bx, int by, bool wall) {
    if (level < firstLevel_ || level >= firstLevel_ + levelCount_)
        return;
    if (ay == by && ax != bx) {
        for (int x = std::min(ax, bx); x < std::max(ax, bx); ++x)
            if (x >= 0 && uint32_t(x) < width_ && ay >= 0 && uint32_t(ay) <= height_)
                xWalls_[xEdge(level, x, ay)] = wall;
    } else if (ax == bx && ay != by) {
        for (int y = std::min(ay, by); y < std::max(ay, by); ++y)
            if (ax >= 0 && uint32_t(ax) <= width_ && y >= 0 && uint32_t(y) < height_)
                yWalls_[yEdge(level, ax, y)] = wall;
    }
}

void World::addWall(int level, int ax, int ay, int bx, int by) { setEdge(level, ax, ay, bx, by, true); }

void World::openDoor(TilePos t, int dir) {
    // The edge between the door's tile and the tile behind it.
    const auto [dx, dy] = directionStep(dir + 4);
    if (std::abs(dx) + std::abs(dy) != 1)
        return; // a diagonal door would be in a diagonal wall, which is not kept
    if (dx)
        setEdge(t.level, t.x + (dx > 0 ? 1 : 0), t.y, t.x + (dx > 0 ? 1 : 0), t.y + 1, false);
    else
        setEdge(t.level, t.x, t.y + (dy > 0 ? 1 : 0), t.x + 1, t.y + (dy > 0 ? 1 : 0), false);
}

bool World::wallBetween(TilePos t, int dx, int dy) const {
    if (t.level < firstLevel_ || t.level >= firstLevel_ + levelCount_)
        return false;
    if (dx && !dy) {
        const int x = t.x + (dx > 0 ? 1 : 0); // the vertical edge at grid x
        if (x < 0 || uint32_t(x) > width_ || t.y < 0 || uint32_t(t.y) >= height_)
            return false;
        return yWalls_[yEdge(t.level, x, t.y)];
    }
    if (dy && !dx) {
        const int y = t.y + (dy > 0 ? 1 : 0);
        if (y < 0 || uint32_t(y) > height_ || t.x < 0 || uint32_t(t.x) >= width_)
            return false;
        return xWalls_[xEdge(t.level, t.x, y)];
    }
    return false;
}

bool World::lineOfSight(TilePos a, TilePos b) const {
    if (a.level != b.level)
        return false;
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
        } else if (lhs < rhs) {
            if (wallBetween({x, y, a.level}, sx, 0))
                return false;
            x += sx, ++ix;
        } else {
            if (wallBetween({x, y, a.level}, 0, sy))
                return false;
            y += sy, ++iy;
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

} // namespace os2::vm
