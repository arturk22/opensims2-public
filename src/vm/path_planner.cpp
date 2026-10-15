#include "path_planner.hpp"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <queue>

namespace os2::vm {

namespace {

constexpr int K = PathPlanner::kCellsPerTile;
constexpr float kInf = std::numeric_limits<float>::infinity();

float dist(Point2 a, Point2 b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

// A door's portal reaches half a tile (a diagonal door's 0.4 tile) out from
// the door's middle on both sides (approximation). Crossing any portal
// costs 1: the binary's cost is 1 + OBJD field 0x35 x a scale that nothing
// writes (DAT_02d86430, in __bss: 0; docs/routing-mac.md §8).
constexpr float kPortalCost = 1.0f;
// A stair's portal ends this far out on each landing, clear of the
// inflated edge of the stairwell and of the steps.
constexpr float kStairLanding = 0.3f;

} // namespace

PathPlanner::PathPlanner(const World& world) : world_(&world), signature_(signatureOf(world)) {
    cw_ = int(world.width()) * K, ch_ = int(world.height()) * K;
    const int levels = world.levelCount();
    static_.assign(size_t(std::max(levels, 0)), std::vector<uint8_t>(size_t(cw_) * size_t(ch_), 0));
    staticCosts_.assign(size_t(std::max(levels, 0)), std::vector<float>(size_t(cw_) * size_t(ch_), 1.0f));
    const float e = kInflation;
    for (int li = 0; li < levels; ++li) {
        const int level = world.firstLevel() + li;
        auto& grid = static_[size_t(li)];
        auto& cost = staticCosts_[size_t(li)];
        for (int x = 0; x < int(world.width()); ++x)
            for (int y = 0; y < int(world.height()); ++y) {
                const TilePos t{x, y, level};
                // Each cell the cost of the quarter tile its centre is in
                // (the floor rectangles' shapes: a cell takes a shape's cost
                // when its centre is inside it, UpdateTraversalCostAndPassability).
                for (int i = 0; i < K; ++i)
                    for (int j = 0; j < K; ++j)
                        cost[cellIndex(x * K + i, y * K + j)] =
                            world.cost(t, float(x) + (float(i) + 0.5f) / K, float(y) + (float(j) + 0.5f) / K);
                if (!world.hasFloor(t))
                    stamp(grid, float(x) - e, float(y) - e, float(x + 1) + e, float(y + 1) + e, kNoFloor);
            }
        // The lot's edge.
        const float w = float(world.width()), h = float(world.height());
        stamp(grid, -1, -1, w + 1, e, kNoFloor);
        stamp(grid, -1, h - e, w + 1, h + 1, kNoFloor);
        stamp(grid, -1, -1, e, h + 1, kNoFloor);
        stamp(grid, w - e, -1, w + 1, h + 1, kNoFloor);
        // Walls as thin rectangles on the tile edges, inflated.
        for (int x = 0; x <= int(world.width()); ++x)
            for (int y = 0; y < int(world.height()); ++y)
                if ((x < int(world.width()) && world.wallBetween({x, y, level}, -1, 0)) ||
                    (x > 0 && world.wallBetween({x - 1, y, level}, 1, 0)))
                    stamp(grid, float(x) - e, float(y) - e, float(x) + e, float(y + 1) + e, kWall);
        for (int x = 0; x < int(world.width()); ++x)
            for (int y = 0; y <= int(world.height()); ++y)
                if ((y < int(world.height()) && world.wallBetween({x, y, level}, 0, -1)) ||
                    (y > 0 && world.wallBetween({x, y - 1, level}, 0, 1)))
                    stamp(grid, float(x) - e, float(y) - e, float(x + 1) + e, float(y) + e, kWall);
    }
    for (const World::Segment& s : world.diagonalWalls())
        if (s.level >= world.firstLevel() && s.level < world.firstLevel() + levels)
            stampDiagonal(static_[size_t(s.level - world.firstLevel())], s);

    // Doors: the opening is closed (AddDoorBlockerRect) and a portal joins
    // the two sides.
    const auto& doors = world.doors();
    for (size_t i = 0; i < doors.size(); ++i) {
        const World::Door& d = doors[i];
        if (d.tile.level < world.firstLevel() || d.tile.level >= world.firstLevel() + levels)
            continue;
        auto& grid = static_[size_t(d.tile.level - world.firstLevel())];
        const float cx = float(d.tile.x) + 0.5f, cy = float(d.tile.y) + 0.5f;
        Portal p;
        p.levelA = p.levelB = d.tile.level;
        p.door = int(i);
        if (d.diagonal) {
            const auto [sx, sy] = directionStep(d.dir);
            const float n = std::hypot(float(sx), float(sy));
            const float ux = float(sx) / n, uy = float(sy) / n;
            p.a = {cx + ux * 0.4f, cy + uy * 0.4f};
            p.b = {cx - ux * 0.4f, cy - uy * 0.4f};
        } else {
            const auto [bx, by] = directionStep(d.dir + 4); // towards the wall behind the door
            const float ex = cx + 0.5f * float(bx), ey = cy + 0.5f * float(by); // middle of the opening
            if (bx)
                stamp(grid, ex - e, ey - 0.5f - e, ex + e, ey + 0.5f + e, kDoor);
            else
                stamp(grid, ex - 0.5f - e, ey - e, ex + 0.5f + e, ey + e, kDoor);
            p.a = {cx, cy};
            p.b = {cx + float(bx), cy + float(by)};
        }
        p.cost = kPortalCost;
        portals_.push_back(p);
    }
    // Stairs: from the bottom landing to the top one, unless the stair's
    // objects make its portals of their routing slots (World::objectPortals,
    // the binary's way: two one-way lanes, docs/routing-mac.md §8.6). This
    // one stays for stairs without them (worlds built without objects).
    const auto& stairs = world.stairs();
    const auto& objectPortals = world.objectPortals();
    for (size_t i = 0; i < stairs.size(); ++i) {
        const Stair& s = stairs[i];
        if (!world.contains(s.bottom) || !world.contains(s.top))
            continue;
        if (std::any_of(objectPortals.begin(), objectPortals.end(),
                        [&](const World::ObjectPortal& o) { return o.stair == int(i); }))
            continue;
        Portal p;
        p.levelA = s.bottom.level, p.levelB = s.top.level;
        // The portal's ends are on the landing tiles, kStairLanding in from
        // the edge each shares with the stair (a stair whose steps start or
        // end mid-tile, as the 6-step deck stairs of N003_Lot192, has its
        // ends there too, not over the steps' tile).
        auto edgeU = [&](const TilePos& t, bool bottom) {
            const int axis = std::fabs(s.up[0]) > 0.5f ? 0 : 1;
            const float lo = float(axis == 0 ? t.x : t.y), dir = s.up[size_t(axis)];
            const float edge = (dir > 0) == bottom ? lo + 1 : lo;
            return (edge - s.bottomEdge[size_t(axis)]) / dir;
        };
        p.a = s.at(std::min(edgeU(s.bottom, true), 0.0f) - kStairLanding);
        p.b = s.at(std::max(edgeU(s.top, false), s.length()) + kStairLanding);
        p.cost = kPortalCost;
        p.stair = int(i);
        portals_.push_back(p);
    }
    // Objects' portals (stairs, the pool ladder): between their two slots'
    // points.
    for (size_t i = 0; i < objectPortals.size(); ++i) {
        const World::ObjectPortal& o = objectPortals[i];
        if (o.levelA < world.firstLevel() || o.levelA >= world.firstLevel() + levels ||
            o.levelB < world.firstLevel() || o.levelB >= world.firstLevel() + levels)
            continue;
        Portal p;
        p.levelA = o.levelA, p.levelB = o.levelB;
        p.a = o.a, p.b = o.b;
        p.cost = kPortalCost;
        p.object = int(i);
        p.stair = o.stair;
        p.oneWay = o.oneWay;
        portals_.push_back(p);
    }
    cells_ = static_;
    costs_ = staticCosts_;
    for (const auto& level : staticCosts_)
        for (const float c : level)
            staticCostsExact_ = staticCostsExact_ && exactCost(c);
    pyramids_.resize(static_.size());
}

void PathPlanner::stamp(std::vector<uint8_t>& grid, float x0, float y0, float x1, float y1, uint8_t bit) const {
    // Cells whose inside overlaps the rectangle.
    const int cx0 = std::max(0, int(std::floor(x0 * K))), cy0 = std::max(0, int(std::floor(y0 * K)));
    const int cx1 = std::min(cw_, int(std::ceil(x1 * K))), cy1 = std::min(ch_, int(std::ceil(y1 * K)));
    for (int cx = cx0; cx < cx1; ++cx)
        for (int cy = cy0; cy < cy1; ++cy)
            grid[cellIndex(cx, cy)] |= bit;
}

void PathPlanner::stampDiagonal(std::vector<uint8_t>& grid, const World::Segment& s) const {
    // The cells a band of half-width kInflation around the wall's line
    // touches (a square cell meets it when its centre is within the
    // inflation plus its half-extent along the normal).
    const float ax = float(s.ax), ay = float(s.ay), bx = float(s.bx), by = float(s.by);
    const float len = std::hypot(bx - ax, by - ay);
    if (len <= 0)
        return;
    const float ux = (bx - ax) / len, uy = (by - ay) / len, nx = -uy, ny = ux;
    const float half = 0.5f / K;
    const float reach = kInflation + (std::fabs(nx) + std::fabs(ny)) * half;
    const int cx0 = std::max(0, int(std::floor((std::min(ax, bx) - 1) * K)));
    const int cx1 = std::min(cw_, int(std::ceil((std::max(ax, bx) + 1) * K)));
    const int cy0 = std::max(0, int(std::floor((std::min(ay, by) - 1) * K)));
    const int cy1 = std::min(ch_, int(std::ceil((std::max(ay, by) + 1) * K)));
    for (int cx = cx0; cx < cx1; ++cx)
        for (int cy = cy0; cy < cy1; ++cy) {
            const float px = (float(cx) + 0.5f) / K - ax, py = (float(cy) + 0.5f) / K - ay;
            const float along = px * ux + py * uy, across = px * nx + py * ny;
            if (std::fabs(across) <= reach && along >= -kInflation - half && along <= len + kInflation + half)
                grid[cellIndex(cx, cy)] |= kDiagonal;
        }
}

void PathPlanner::addObstacle(int level, float x0, float y0, float x1, float y1, Cell kind) {
    if (level < world_->firstLevel() || level >= world_->firstLevel() + world_->levelCount())
        return;
    ops_.push_back({level, x0, y0, x1, y1, float(kind)});
    const int li = level - world_->firstLevel();
    record(li, x0 - kInflation, y0 - kInflation, x1 + kInflation, y1 + kInflation, kind, 0);
    if (kind == kObject)
        record(li, x0, y0, x1, y1, kObjectCore, 0);
}

void PathPlanner::clearObstacles() {
    ops_.clear();
    stamps_.clear();
    inexactStamps_ = 0;
    gridsDirty_ = true;
}

void PathPlanner::addCost(int level, float x0, float y0, float x1, float y1, float cost) {
    if (level < world_->firstLevel() || level >= world_->firstLevel() + world_->levelCount())
        return;
    ops_.push_back({level, x0, y0, x1, y1, -1.0f - cost}); // cost ops are negative, obstacle kinds are not
    record(level - world_->firstLevel(), x0, y0, x1, y1, 0, cost);
}

void PathPlanner::record(int li, float x0, float y0, float x1, float y1, uint8_t bits, float cost) {
    // The cells whose inside overlaps the rectangle (as stamp()).
    const int cx0 = std::max(0, int(std::floor(x0 * K))), cy0 = std::max(0, int(std::floor(y0 * K)));
    const int cx1 = std::min(cw_, int(std::ceil(x1 * K))), cy1 = std::min(ch_, int(std::ceil(y1 * K)));
    if (cx0 >= cx1 || cy0 >= cy1)
        return;
    if (!bits && !exactCost(cost))
        ++inexactStamps_;
    stamps_.push_back({li, cx0, cy0, cx1, cy1, bits, bits ? 0u : std::bit_cast<uint32_t>(cost)});
    gridsDirty_ = true;
}

void PathPlanner::updateGrids() const {
    gridsDirty_ = false;
    if (stamps_ == applied_)
        return;
    // The stamps of one list and not of the other (as multisets), past the
    // head and tail both share (the obstacles come in the objects' order,
    // so mostly a walking Sim's rectangles differ).
    size_t head = 0, tail = 0;
    while (head < stamps_.size() && head < applied_.size() && stamps_[head] == applied_[head])
        ++head;
    while (tail < stamps_.size() - head && tail < applied_.size() - head &&
           stamps_[stamps_.size() - 1 - tail] == applied_[applied_.size() - 1 - tail])
        ++tail;
    std::vector<Stamp> was(applied_.begin() + ptrdiff_t(head), applied_.end() - ptrdiff_t(tail));
    std::vector<Stamp> now(stamps_.begin() + ptrdiff_t(head), stamps_.end() - ptrdiff_t(tail));
    std::sort(was.begin(), was.end());
    std::sort(now.begin(), now.end());
    std::vector<Stamp> changed;
    std::set_symmetric_difference(was.begin(), was.end(), now.begin(), now.end(), std::back_inserter(changed));
    applied_ = stamps_;
    if (changed.empty())
        return; // the same stamps in another order: the same grids
    auto apply = [&](const Stamp& s, int x0, int y0, int x1, int y1) {
        x0 = std::max(x0, s.x0), y0 = std::max(y0, s.y0), x1 = std::min(x1, s.x1), y1 = std::min(y1, s.y1);
        if (s.bits) {
            auto& grid = cells_[size_t(s.li)];
            for (int cx = x0; cx < x1; ++cx)
                for (int cy = y0; cy < y1; ++cy)
                    grid[cellIndex(cx, cy)] |= uint8_t(s.bits);
        } else {
            auto& grid = costs_[size_t(s.li)];
            const float cost = std::bit_cast<float>(s.cost);
            for (int cx = x0; cx < x1; ++cx)
                for (int cy = y0; cy < y1; ++cy)
                    grid[cellIndex(cx, cy)] = std::max(grid[cellIndex(cx, cy)], cost);
        }
    };
    for (size_t li = 0; li < cells_.size(); ++li) {
        int64_t area = 0;
        bool any = false;
        for (const Stamp& s : changed)
            if (size_t(s.li) == li)
                any = true, area += int64_t(s.x1 - s.x0) * (s.y1 - s.y0);
        if (!any)
            continue;
        Pyramid& p = pyramids_[li];
        if (area * 4 > int64_t(cw_) * ch_) { // most of the level: all of it again
            cells_[li] = static_[li];
            costs_[li] = staticCosts_[li];
            for (const Stamp& s : stamps_)
                if (size_t(s.li) == li)
                    apply(s, 0, 0, cw_, ch_);
            p.valid = false;
            p.dirty.clear();
            continue;
        }
        // Each changed rectangle: its static cells, then every stamp over it.
        for (const Stamp& r : changed) {
            if (size_t(r.li) != li)
                continue;
            for (int cx = r.x0; cx < r.x1; ++cx) {
                const size_t a = cellIndex(cx, r.y0), b = cellIndex(cx, r.y1);
                std::copy(static_[li].begin() + ptrdiff_t(a), static_[li].begin() + ptrdiff_t(b),
                          cells_[li].begin() + ptrdiff_t(a));
                std::copy(staticCosts_[li].begin() + ptrdiff_t(a), staticCosts_[li].begin() + ptrdiff_t(b),
                          costs_[li].begin() + ptrdiff_t(a));
            }
            for (const Stamp& s : stamps_)
                if (s.li == r.li && s.x0 < r.x1 && r.x0 < s.x1 && s.y0 < r.y1 && r.y0 < s.y1)
                    apply(s, r.x0, r.y0, r.x1, r.y1);
            if (p.valid) {
                if (p.dirty.size() >= 1024)
                    p.valid = false, p.dirty.clear(); // built again in full when needed
                else
                    p.dirty.push_back({r.x0, r.y0, r.x1, r.y1});
            }
        }
    }
}

void PathPlanner::addRouter(int level, float x, float y, int, bool walking, bool) {
    // A human's rectangles (docs/routing-mac.md §5, round 9): the router's
    // box grown by 0.375 (cost 5, standing only), 0.1875 (7.5) and 0.0625
    // (10), whatever its direction or age. The box: its tile while it walks
    // (the walking rectangle of §5), its position standing (inferred: the
    // box of a standing Sim taken as a point).
    float x0 = x, y0 = y, x1 = x, y1 = y;
    if (walking) {
        x0 = std::floor(x), y0 = std::floor(y);
        x1 = x0 + 1, y1 = y0 + 1;
    }
    auto grown = [&](float by, float cost) { addCost(level, x0 - by, y0 - by, x1 + by, y1 + by, cost); };
    if (!walking)
        grown(0.375f, 5.0f);
    grown(0.1875f, 7.5f);
    grown(0.0625f, 10.0f);
}

float PathPlanner::uniform(float hi) {
    noise_ *= 0x278DDE6Du;
    return hi * float(noise_ >> 8) / float(1u << 24);
}

uint8_t PathPlanner::cellAt(int level, int cx, int cy) const {
    if (level < world_->firstLevel() || level >= world_->firstLevel() + world_->levelCount() || cx < 0 || cy < 0 ||
        cx >= cw_ || cy >= ch_)
        return kNoFloor;
    return cells_[size_t(level - world_->firstLevel())][cellIndex(cx, cy)];
}

uint8_t PathPlanner::at(int level, Point2 p) const {
    syncGrids();
    return cellAt(level, int(std::floor(p[0] * K)), int(std::floor(p[1] * K)));
}

float PathPlanner::costAt(int level, Point2 p) const {
    syncGrids();
    const int cx = int(std::floor(p[0] * K)), cy = int(std::floor(p[1] * K));
    if (level < world_->firstLevel() || level >= world_->firstLevel() + world_->levelCount() || cx < 0 || cy < 0 ||
        cx >= cw_ || cy >= ch_)
        return kInf;
    return costs_[size_t(level - world_->firstLevel())][cellIndex(cx, cy)];
}

bool PathPlanner::clear(int level, Point2 a, Point2 b, uint8_t mask, float maxCost) const {
    syncGrids();
    // The cells the segment crosses (Amanatides & Woo); through a corner,
    // both cells beside it count.
    const float ax = a[0] * K, ay = a[1] * K, bx = b[0] * K, by = b[1] * K;
    int cx = int(std::floor(ax)), cy = int(std::floor(ay));
    const bool costs = maxCost < kInf;
    auto blocked = [&](int x, int y) {
        if (cellAt(level, x, y) & mask)
            return true;
        return costs && x >= 0 && y >= 0 && x < cw_ && y < ch_ &&
               costs_[size_t(level - world_->firstLevel())][cellIndex(x, y)] > maxCost + 1e-3f;
    };
    if (blocked(cx, cy))
        return false;
    const float dx = bx - ax, dy = by - ay;
    const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    const float tdx = dx != 0 ? std::fabs(1.0f / dx) : kInf, tdy = dy != 0 ? std::fabs(1.0f / dy) : kInf;
    float tx = dx != 0 ? (dx > 0 ? (float(cx + 1) - ax) : (ax - float(cx))) * tdx : kInf;
    float ty = dy != 0 ? (dy > 0 ? (float(cy + 1) - ay) : (ay - float(cy))) * tdy : kInf;
    // t runs from 0 at a to 1 at b; each step crosses into the next cell.
    for (int guard = 0; std::min(tx, ty) < 1.0f && guard < 4 * (cw_ + ch_); ++guard) {
        if (std::fabs(tx - ty) < 1e-6f) {
            if (blocked(cx + sx, cy) || blocked(cx, cy + sy))
                return false;
            cx += sx, cy += sy, tx += tdx, ty += tdy;
        } else if (tx < ty) {
            cx += sx, tx += tdx;
        } else {
            cy += sy, ty += tdy;
        }
        if (blocked(cx, cy))
            return false;
    }
    return true;
}

float PathPlanner::costAlong(int level, Point2 a, Point2 b, uint8_t mask) const {
    syncGrids();
    if (level < world_->firstLevel() || level >= world_->firstLevel() + world_->levelCount())
        return kInf;
    const auto& costs = costs_[size_t(level - world_->firstLevel())];
    const float ax = a[0] * K, ay = a[1] * K, bx = b[0] * K, by = b[1] * K;
    const float length = dist(a, b);
    int cx = int(std::floor(ax)), cy = int(std::floor(ay));
    auto cellCost = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= cw_ || y >= ch_ || (cellAt(level, x, y) & mask))
            return kInf;
        return costs[cellIndex(x, y)];
    };
    const float dx = bx - ax, dy = by - ay;
    const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    const float tdx = dx != 0 ? std::fabs(1.0f / dx) : kInf, tdy = dy != 0 ? std::fabs(1.0f / dy) : kInf;
    float tx = dx != 0 ? (dx > 0 ? (float(cx + 1) - ax) : (ax - float(cx))) * tdx : kInf;
    float ty = dy != 0 ? (dy > 0 ? (float(cy + 1) - ay) : (ay - float(cy))) * tdy : kInf;
    float t = 0, total = 0;
    for (int guard = 0; guard < 4 * (cw_ + ch_); ++guard) {
        const float c = cellCost(cx, cy);
        if (c >= kInf)
            return kInf;
        const float next = std::min(1.0f, std::min(tx, ty));
        total += (next - t) * length * c;
        t = next;
        if (t >= 1.0f)
            break;
        if (std::fabs(tx - ty) < 1e-6f) {
            if (cellCost(cx + sx, cy) >= kInf || cellCost(cx, cy + sy) >= kInf)
                return kInf;
            cx += sx, cy += sy, tx += tdx, ty += tdy;
        } else if (tx < ty) {
            cx += sx, tx += tdx;
        } else {
            cy += sy, ty += tdy;
        }
    }
    return total;
}

bool PathPlanner::escape(int level, Point2 start, Point2& out) const {
    // Breadth first over the cells around the start, through objects and
    // Sims but not across walls or off the floor, to the nearest free cell
    // (approximation: how the binary gets a Sim out of a footprint is not
    // decoded). Two tiles at most.
    const int sx = int(std::floor(start[0] * K)), sy = int(std::floor(start[1] * K));
    if (sx < 0 || sy < 0 || sx >= cw_ || sy >= ch_)
        return false;
    const int r = 2 * K;
    const int side = 2 * r + 1;
    std::vector<uint8_t> seen(size_t(side) * size_t(side), 0);
    std::deque<std::array<int, 2>> open{{sx, sy}};
    seen[size_t(r) * side + r] = 1;
    // A walker left where there is no floor (on a stair's steps, after its
    // climb was cut short) steps off over the floorless cells too.
    const bool stranded = cellAt(level, sx, sy) & kNoFloor;
    // One standing in a diagonal wall's cells (stopped in a diagonal doorway:
    // N002_Lot4's Vidcund, every route failed until he starved) steps out
    // through them too (inferred, as the rest of the escape).
    const bool inDiagonal = cellAt(level, sx, sy) & kDiagonal;
    while (!open.empty()) {
        const auto [cx, cy] = open.front();
        open.pop_front();
        const uint8_t bits = cellAt(level, cx, cy);
        if (bits == 0) {
            // The nearest point of that cell, a little inside it (inferred):
            // its middle could lie toward a Sim beside the walker, a step
            // the walk's collision check then refuses for good (N001_Lot68:
            // Lilith asked to step aside in a gap between two cabinets
            // stepped first toward Angela, who waited for her, ~1400 ticks).
            constexpr float kIn = 0.125f / K;
            out = {std::clamp(start[0], float(cx) / K + kIn, float(cx + 1) / K - kIn),
                   std::clamp(start[1], float(cy) / K + kIn, float(cy + 1) / K - kIn)};
            return true;
        }
        static constexpr int kD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : kD) {
            const int nx = cx + d[0], ny = cy + d[1];
            if (std::abs(nx - sx) > r || std::abs(ny - sy) > r)
                continue;
            uint8_t& s = seen[size_t(nx - sx + r) * side + size_t(ny - sy + r)];
            if (s || (cellAt(level, nx, ny) & ((stranded ? 0 : kNoFloor) | (inDiagonal ? 0 : kDiagonal))))
                continue;
            // Crossing a tile edge: not through a wall (a door's edge is open).
            const int tx = (d[0] > 0 ? cx : nx), ty = (d[1] > 0 ? cy : ny);
            if (d[0] && (tx + 1) % K == 0 && world_->wallBetween({tx / K, cy / K, level}, 1, 0))
                continue;
            if (d[1] && (ty + 1) % K == 0 && world_->wallBetween({cx / K, ty / K, level}, 0, 1))
                continue;
            s = 1;
            open.push_back({nx, ny});
        }
    }
    return false;
}

void PathPlanner::buildRoomGroups() const {
    roomGroupsBuilt_ = true;
    const int levels = world_->levelCount();
    constexpr uint8_t kBlocks = kWall | kNoFloor | kDiagonal | kDoor;
    // Label the free cells of each level (4-connected).
    std::vector<std::vector<int32_t>> label(size_t(std::max(levels, 0)));
    int32_t groups = 0;
    std::vector<int32_t> stack;
    for (int li = 0; li < levels; ++li) {
        const auto& grid = static_[size_t(li)];
        auto& lab = label[size_t(li)];
        lab.assign(grid.size(), -1);
        for (int cx = 0; cx < cw_; ++cx)
            for (int cy = 0; cy < ch_; ++cy) {
                const size_t i0 = cellIndex(cx, cy);
                if (lab[i0] >= 0 || (grid[i0] & kBlocks))
                    continue;
                const int32_t g = groups++;
                lab[i0] = g;
                stack.assign(1, int32_t(i0));
                while (!stack.empty()) {
                    const int32_t i = stack.back();
                    stack.pop_back();
                    const int x = i / ch_, y = i % ch_;
                    const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
                    for (int k = 0; k < 4; ++k) {
                        if (nx[k] < 0 || ny[k] < 0 || nx[k] >= cw_ || ny[k] >= ch_)
                            continue;
                        const size_t j = cellIndex(nx[k], ny[k]);
                        if (lab[j] < 0 && !(grid[j] & kBlocks)) {
                            lab[j] = g;
                            stack.push_back(int32_t(j));
                        }
                    }
                }
            }
    }
    // Join the groups the portals link (union-find).
    std::vector<int32_t> parent(static_cast<size_t>(groups));
    for (int32_t g = 0; g < groups; ++g)
        parent[size_t(g)] = g;
    auto find = [&](int32_t g) {
        while (parent[size_t(g)] != g)
            g = parent[size_t(g)] = parent[size_t(parent[size_t(g)])];
        return g;
    };
    auto groupAt = [&](int level, Point2 p) -> int32_t {
        if (level < world_->firstLevel() || level >= world_->firstLevel() + levels)
            return -1;
        const auto& lab = label[size_t(level - world_->firstLevel())];
        const int px = int(std::floor(p[0] * K)), py = int(std::floor(p[1] * K));
        int32_t best = -1;
        float bestD = kInf;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                const int x = px + dx, y = py + dy;
                if (x < 0 || y < 0 || x >= cw_ || y >= ch_ || lab[cellIndex(x, y)] < 0)
                    continue;
                const float d = float(dx * dx + dy * dy);
                if (d < bestD)
                    bestD = d, best = lab[cellIndex(x, y)];
            }
        return best;
    };
    for (const Portal& p : portals_) {
        const int32_t a = groupAt(p.levelA, p.a), b = groupAt(p.levelB, p.b);
        if (a >= 0 && b >= 0)
            parent[size_t(find(a))] = find(b);
    }
    // The rooms of each joined group, by tile (the cells of a tile share its room).
    std::map<int32_t, std::vector<int16_t>> rooms;
    for (int li = 0; li < levels; ++li) {
        const int level = world_->firstLevel() + li;
        const auto& lab = label[size_t(li)];
        for (int x = 0; x < int(world_->width()); ++x)
            for (int y = 0; y < int(world_->height()); ++y) {
                const int16_t room = world_->room({x, y, level});
                for (int i = 0; i < K; ++i)
                    for (int j = 0; j < K; ++j)
                        if (const int32_t g = lab[cellIndex(x * K + i, y * K + j)]; g >= 0) {
                            rooms[find(g)].push_back(room);
                            i = j = K; // one free cell is enough for this tile
                        }
            }
    }
    for (auto& [g, list] : rooms) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        roomGroups_.push_back(std::move(list));
    }
}

const std::vector<int16_t>& PathPlanner::accessibleRooms(int16_t room) const {
    if (!roomGroupsBuilt_)
        buildRoomGroups();
    auto it = reach_.find(room);
    if (it != reach_.end())
        return it->second;
    std::vector<int16_t> out;
    for (const auto& list : roomGroups_)
        if (std::binary_search(list.begin(), list.end(), room))
            out.insert(out.end(), list.begin(), list.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return reach_.emplace(room, std::move(out)).first->second;
}

bool PathPlanner::nearFree(int level, Point2 p, uint8_t mask, Point2& out) const {
    const int px = int(std::floor(p[0] * K)), py = int(std::floor(p[1] * K));
    float best = kInf;
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy)
            if (!(cellAt(level, px + dx, py + dy) & mask)) {
                const Point2 c{(float(px + dx) + 0.5f) / K, (float(py + dy) + 0.5f) / K};
                if (dist(c, p) < best)
                    best = dist(c, p), out = c;
            }
    return best < kInf;
}

int PathPlanner::treeSide() const {
    int n = 1;
    while (n < std::max(cw_, ch_))
        n *= 2;
    return n;
}

void PathPlanner::updatePyramid(int li) {
    Pyramid& p = pyramids_[size_t(li)];
    const int n = treeSide();
    int top = 0; // squares of 2^1 .. 2^top cells
    while ((2 << top) <= n)
        ++top;
    if (!p.valid) {
        p.blocked.resize(size_t(top));
        p.sum.resize(size_t(top));
        for (int k = 1; k <= top; ++k) {
            const size_t m = size_t(n >> k);
            p.blocked[size_t(k - 1)].assign(m * m, 0);
            p.sum[size_t(k - 1)].assign(m * m, 0.0);
        }
        p.dirty.assign(1, {0, 0, n, n});
        p.valid = true;
    }
    const auto& grid = cells_[size_t(li)];
    const auto& cost = costs_[size_t(li)];
    for (const auto& r : p.dirty)
        for (int k = 1; k <= top; ++k) {
            // The squares of 2^k cells over the rectangle, from their four
            // quarters (cells past the grid count as blocked at cost 0, as
            // in the summed-area tables).
            const int m = n >> k, mc = n >> (k - 1);
            auto& B = p.blocked[size_t(k - 1)];
            auto& S = p.sum[size_t(k - 1)];
            for (int i = r[0] >> k; i <= (r[2] - 1) >> k; ++i)
                for (int j = r[1] >> k; j <= (r[3] - 1) >> k; ++j) {
                    int32_t b = 0;
                    double sum = 0;
                    if (k == 1) {
                        for (const int x : {2 * i, 2 * i + 1})
                            for (const int y : {2 * j, 2 * j + 1}) {
                                const bool inside = x < cw_ && y < ch_;
                                b += !inside || grid[cellIndex(x, y)] != 0;
                                sum += inside ? double(cost[cellIndex(x, y)]) : 0.0;
                            }
                    } else {
                        const auto& Bc = p.blocked[size_t(k - 2)];
                        const auto& Sc = p.sum[size_t(k - 2)];
                        for (const int x : {2 * i, 2 * i + 1})
                            for (const int y : {2 * j, 2 * j + 1}) {
                                b += Bc[size_t(x) * size_t(mc) + size_t(y)];
                                sum += Sc[size_t(x) * size_t(mc) + size_t(y)];
                            }
                    }
                    B[size_t(i) * size_t(m) + size_t(j)] = b;
                    S[size_t(i) * size_t(m) + size_t(j)] = sum;
                }
        }
    p.dirty.clear();
}

std::shared_ptr<const PathPlanner::QuadTrees> PathPlanner::quadTrees(bool allLevels, int startLevel) {
    syncGrids();
    const int first = world_->firstLevel(), levels = world_->levelCount();
    const int key = allLevels ? -1 : startLevel;
    for (auto it = treeCache_.begin(); it != treeCache_.end(); ++it)
        if (it->level == key && it->ops == ops_) {
            auto hit = it->trees;
            if (it != treeCache_.begin())
                std::rotate(treeCache_.begin(), it, it + 1); // most recent first
            return hit;
        }
    auto out = std::make_shared<QuadTrees>();
    std::vector<Quad>& quads = out->quads;
    std::vector<Tree>& trees = out->trees;
    trees.resize(static_cast<size_t>(levels));
    // The sums are exact either way (integers of 2^-24 under 2^53) when the
    // costs are; the pyramid's are those of the tables then, bit for bit.
    const bool exact = staticCostsExact_ && inexactStamps_ == 0;
    for (int li = 0; li < levels; ++li) {
        const int level = first + li;
        if (!allLevels && level != startLevel)
            continue;
        Tree& t = trees[size_t(li)];
        t.n = treeSide();
        const int n = t.n;
        if (t.leafOf.size() != size_t(n) * n)
            t.leafOf.assign(size_t(n) * n, -1);
        const auto& grid = cells_[size_t(li)];
        const auto& cost = costs_[size_t(li)];
        // Squares split until each is all free at one cost or all blocked,
        // by the blocked cells and the cost in each (`blockedIn`, `costIn`).
        auto split = [&](auto blockedIn, auto costIn) {
            std::vector<std::array<int, 3>> stack{{0, 0, n}};
            while (!stack.empty()) {
                const auto [x, y, s] = stack.back();
                stack.pop_back();
                const int b = blockedIn(x, y, s);
                if (b == s * s)
                    continue;
                if (b == 0) {
                    const float c0 = cost[cellIndex(x, y)];
                    if (s == 1 || std::fabs(costIn(x, y, s) - double(c0) * s * s) < 1e-3) {
                        const int id = int(quads.size());
                        quads.push_back({level, x, y, s, c0});
                        for (int i = x; i < x + s; ++i)
                            std::fill_n(t.leafOf.begin() + ptrdiff_t(size_t(i) * n + size_t(y)), s, id);
                        continue;
                    }
                }
                const int h = s / 2;
                stack.push_back({x, y, h});
                stack.push_back({x + h, y, h});
                stack.push_back({x, y + h, h});
                stack.push_back({x + h, y + h, h});
            }
        };
        if (exact) {
            updatePyramid(li);
            const Pyramid& p = pyramids_[size_t(li)];
            split(
                [&](int x, int y, int s) -> int {
                    if (s == 1)
                        return !(x < cw_ && y < ch_) || grid[cellIndex(x, y)] != 0;
                    const int k = std::countr_zero(unsigned(s));
                    return p.blocked[size_t(k - 1)][size_t(x >> k) * size_t(n >> k) + size_t(y >> k)];
                },
                [&](int x, int y, int s) -> double {
                    const int k = std::countr_zero(unsigned(s));
                    return p.sum[size_t(k - 1)][size_t(x >> k) * size_t(n >> k) + size_t(y >> k)];
                });
            continue;
        }
        // Summed area tables of the blocked cells and of the costs.
        // Row 0 and column 0 stay zero; every other entry is written below.
        if (satBlocked_.size() != size_t(n + 1) * (n + 1)) {
            satBlocked_.assign(size_t(n + 1) * (n + 1), 0);
            satCost_.assign(size_t(n + 1) * (n + 1), 0);
        }
        std::vector<int32_t>& blocked = satBlocked_;
        std::vector<double>& csum = satCost_;
        auto sat = [&](auto& v, int x, int y) -> auto& { return v[size_t(x) * (n + 1) + size_t(y)]; };
        for (int x = 0; x < n; ++x)
            for (int y = 0; y < n; ++y) {
                const bool inside = x < cw_ && y < ch_;
                const int b = !inside || grid[cellIndex(x, y)] != 0;
                const double c = inside ? cost[cellIndex(x, y)] : 0.0;
                sat(blocked, x + 1, y + 1) = b + sat(blocked, x, y + 1) + sat(blocked, x + 1, y) - sat(blocked, x, y);
                sat(csum, x + 1, y + 1) = c + sat(csum, x, y + 1) + sat(csum, x + 1, y) - sat(csum, x, y);
            }
        auto area = [&](auto& v, int x, int y, int s) {
            return sat(v, x + s, y + s) - sat(v, x, y + s) - sat(v, x + s, y) + sat(v, x, y);
        };
        split([&](int x, int y, int s) -> int { return area(blocked, x, y, s); },
              [&](int x, int y, int s) -> double { return area(csum, x, y, s); });
    }
    treeCache_.insert(treeCache_.begin(), {key, ops_, out});
    if (treeCache_.size() > kTreeCacheSize)
        treeCache_.pop_back();
    return out;
}

bool PathPlanner::plan(Point2 start, int startLevel, int goalLevel, const std::vector<PlanGoal>& goals,
                       PlannedPath& out, RouteFail& why) {
    // A goal on the start's level is looked for on that level first; when
    // none is reachable there, through the other levels too (from a deck
    // down a stair and up another one into the house: N003_Lot192).
    if (plan(start, startLevel, goalLevel, goals, out, why, startLevel != goalLevel) || startLevel != goalLevel ||
        why != RouteFail::PathNotFound || world_->levelCount() < 2 || portals_.empty())
        return why == RouteFail::Success;
    return plan(start, startLevel, goalLevel, goals, out, why, true);
}

bool PathPlanner::plan(Point2 start, int startLevel, int goalLevel, const std::vector<PlanGoal>& goals,
                       PlannedPath& out, RouteFail& why, bool allLevels) {
    syncGrids();
    out = {};
    why = RouteFail::Success;
    const int first = world_->firstLevel(), levels = world_->levelCount();
    if (goals.empty()) {
        why = RouteFail::NoValidGoals;
        return false;
    }
    if (startLevel < first || startLevel >= first + levels || goalLevel < first || goalLevel >= first + levels) {
        why = RouteFail::PathNotFound;
        return false;
    }
    // The start: a walker inside an object or a Sim steps out first.
    Point2 from = start;
    if (!free(startLevel, start)) {
        if (!escape(startLevel, start, from)) {
            if (std::getenv("OS2_PLAN_DEBUG"))
                std::fprintf(stderr, "plan: start (%.2f, %.2f, %d) bits %02X, no escape\n", start[0], start[1], startLevel, at(startLevel, start));
            why = RouteFail::PathNotFound;
            return false;
        }
        out.escaped = true;
    }
    // The goals: free points; one only crowded by a wall, a floor's edge or
    // an object's inflation (a bone slot's place right against a table:
    // EvalTileForGoal already turned down the goals on an object's tile)
    // moves to a free point next to it, and the walker steps onto the exact
    // goal at the end.
    struct Goal {
        Point2 p, exact;
        float cost;
        size_t index;
    };
    std::vector<Goal> usable;
    bool object = false;
    for (size_t i = 0; i < goals.size(); ++i) {
        const Point2 g{goals[i].x, goals[i].y};
        const uint8_t bits = at(goalLevel, g);
        if (bits == 0) {
            usable.push_back({g, g, goals[i].cost, i});
            continue;
        }
        if (bits & kObjectCore)
            object = true;
        else if (!(bits & kNoFloor)) {
            Point2 near{};
            if (nearFree(goalLevel, g, 0xFF, near))
                usable.push_back({near, g, goals[i].cost, i});
        }
    }
    if (usable.empty()) {
        why = object ? RouteFail::DestinationOccupied : RouteFail::NoValidGoals;
        return false;
    }

    // Quadtrees of the levels the search may use: the start's alone when
    // the goal is on it, else all (stairs). Built from the grids alone, so
    // they are kept while the obstacles stay the same (the same calls since
    // clearObstacles(): compared exactly, not by hash) and reused by the
    // next plans; the A* reads them and never changes them. New ones are
    // split from the pyramid of sums, which only the cells that changed
    // since the last ones update (a walking Sim's rectangles).
    const std::shared_ptr<const QuadTrees> built = quadTrees(allLevels, startLevel);
    const std::vector<Quad>& quads = built->quads;
    const std::vector<Tree>& trees = built->trees;
    auto leafAt = [&](int level, Point2 p) -> int {
        const Tree& t = trees[size_t(level - first)];
        if (t.n == 0)
            return -1;
        const int cx = int(std::floor(p[0] * K)), cy = int(std::floor(p[1] * K));
        if (cx < 0 || cy < 0 || cx >= t.n || cy >= t.n)
            return -1;
        return t.leafOf[size_t(cx) * t.n + size_t(cy)];
    };

    // Special nodes after the squares: the start, the goals, the portals' ends.
    struct Special {
        Point2 p;
        int level;
        int leaf;
        int goal = -1;   // index into usable
        int portal = -1; // its portal, and the node at its other end
        int other = -1;
        bool atA = false; // the portal's end a
    };
    std::vector<Special> specials;
    specials.push_back({from, startLevel, leafAt(startLevel, from)});
    for (size_t i = 0; i < usable.size(); ++i)
        specials.push_back({usable[i].p, goalLevel, leafAt(goalLevel, usable[i].p), int(i)});
    for (size_t i = 0; i < portals_.size(); ++i) {
        const Portal& p = portals_[i];
        if (trees[size_t(p.levelA - first)].n == 0 || trees[size_t(p.levelB - first)].n == 0)
            continue;
        const int la = leafAt(p.levelA, p.a), lb = leafAt(p.levelB, p.b);
        if (la < 0 || lb < 0)
            continue;
        const int ia = int(specials.size());
        specials.push_back({p.a, p.levelA, la, -1, int(i), ia + 1, true});
        specials.push_back({p.b, p.levelB, lb, -1, int(i), ia, false});
    }
    if (specials[0].leaf < 0) {
        if (std::getenv("OS2_PLAN_DEBUG"))
            std::fprintf(stderr, "plan: start (%.2f, %.2f, %d) in no square\n", from[0], from[1], startLevel);
        why = RouteFail::PathNotFound;
        return false;
    }
    std::vector<std::vector<int>> inLeaf(quads.size());
    for (size_t i = 0; i < specials.size(); ++i)
        if (specials[i].leaf >= 0)
            inLeaf[size_t(specials[i].leaf)].push_back(int(i));

    const size_t nq = quads.size(), total = nq + specials.size();
    auto pointOf = [&](size_t node) { return node < nq ? quads[node].centre() : specials[node - nq].p; };
    float minGoal = kInf;
    for (const Goal& g : usable)
        minGoal = std::min(minGoal, g.cost);
    std::vector<float> g(total, kInf);
    // cNoisyDistancePathHeuristic: the distance d to the (nearest) goal,
    // plus noise up to a = min(0.1 g, 3) past 3 tiles, ramping in from 3 to
    // 6; plus the cheapest goal cost (goals here carry costs).
    auto heuristic = [&](size_t node) {
        const Point2 p = pointOf(node);
        float d = kInf;
        for (const Goal& gl : usable)
            d = std::min(d, dist(p, gl.p));
        const float a = std::min(0.1f * g[node], 3.0f);
        if (d >= 6)
            d += uniform(a);
        else if (d >= 3)
            d += uniform(a) * (d - 3) / 3;
        return d + minGoal;
    };
    std::vector<int32_t> parent(total, -1);
    std::vector<uint8_t> closed(total, 0);
    using Entry = std::pair<float, size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    const size_t s0 = nq;
    g[s0] = 0;
    open.push({heuristic(s0), s0});
    auto relax = [&](size_t a, size_t b, float c) {
        c += uniform(0.01f); // AddNodeToOpenList's noise
        if (g[a] + c < g[b]) {
            g[b] = g[a] + c;
            parent[b] = int32_t(a);
            open.push({g[b] + heuristic(b), b});
        }
    };
    // The portals' locks, asked once per way and plan.
    std::vector<int8_t> lockCache(portals_.size() * 2, -1);
    auto locked = [&](size_t portal, bool forward) {
        if (!portalLocked)
            return false;
        int8_t& v = lockCache[portal * 2 + (forward ? 0 : 1)];
        if (v < 0)
            v = portalLocked(portal, forward) ? 1 : 0;
        return v == 1;
    };
    int found = -1;
    while (!open.empty()) {
        const auto [f, node] = open.top();
        open.pop();
        if (closed[node])
            continue;
        closed[node] = 1;
        if (node >= nq && specials[node - nq].goal >= 0) {
            found = int(node);
            break;
        }
        if (node < nq) {
            const Quad& q = quads[node];
            const Tree& t = trees[size_t(q.level - first)];
            const Point2 c = q.centre();
            // Neighbouring squares, through the middle of the shared edge.
            auto visit = [&](int cx, int cy, bool vertical) {
                if (cx < 0 || cy < 0 || cx >= t.n || cy >= t.n)
                    return -1;
                const int id = t.leafOf[size_t(cx) * t.n + size_t(cy)];
                if (id < 0)
                    return -1;
                const Quad& r = quads[size_t(id)];
                Point2 m;
                if (vertical) { // a shared vertical edge
                    const int lo = std::max(q.y, r.y), hi = std::min(q.y + q.size, r.y + r.size);
                    m = {float(cx < q.x ? q.x : q.x + q.size) / K, (float(lo) + float(hi)) / 2 / K};
                } else {
                    const int lo = std::max(q.x, r.x), hi = std::min(q.x + q.size, r.x + r.size);
                    m = {(float(lo) + float(hi)) / 2 / K, float(cy < q.y ? q.y : q.y + q.size) / K};
                }
                relax(node, size_t(id), dist(c, m) * q.cost + dist(m, r.centre()) * r.cost);
                return id;
            };
            for (int side = 0; side < 4; ++side) {
                int last = -2;
                for (int k = 0; k < q.size; ++k) {
                    int cx, cy;
                    const bool vertical = side < 2;
                    if (side == 0) cx = q.x - 1, cy = q.y + k;
                    else if (side == 1) cx = q.x + q.size, cy = q.y + k;
                    else if (side == 2) cx = q.x + k, cy = q.y - 1;
                    else cx = q.x + k, cy = q.y + q.size;
                    const int id = (cx >= 0 && cy >= 0 && cx < t.n && cy < t.n) ? t.leafOf[size_t(cx) * t.n + size_t(cy)] : -1;
                    if (id >= 0 && id != last)
                        visit(cx, cy, vertical);
                    last = id;
                }
            }
            for (int sp : inLeaf[node]) {
                const Special& s = specials[size_t(sp)];
                float extra = s.goal >= 0 ? usable[size_t(s.goal)].cost : 0;
                relax(node, nq + size_t(sp), dist(c, s.p) * q.cost + extra);
            }
        } else {
            const Special& s = specials[node - nq];
            const Quad& q = quads[size_t(s.leaf)];
            relax(node, size_t(s.leaf), dist(s.p, q.centre()) * q.cost);
            for (int sp : inLeaf[size_t(s.leaf)]) {
                if (nq + size_t(sp) == node)
                    continue;
                const Special& o = specials[size_t(sp)];
                const float extra = o.goal >= 0 ? usable[size_t(o.goal)].cost : 0;
                relax(node, nq + size_t(sp), dist(s.p, o.p) * q.cost + extra);
            }
            if (s.other >= 0 && (s.atA || !portals_[size_t(s.portal)].oneWay) && !locked(size_t(s.portal), s.atA))
                relax(node, nq + size_t(s.other), portals_[size_t(s.portal)].cost);
        }
    }
    if (found < 0) {
        if (std::getenv("OS2_PLAN_DEBUG"))
            std::fprintf(stderr, "plan: from (%.2f, %.2f, %d) bits %02X to %zu goals on %d (first (%.2f, %.2f)): searched, none reached%s\n",
                         from[0], from[1], startLevel, at(startLevel, start), usable.size(), goalLevel, usable[0].p[0], usable[0].p[1],
                         allLevels ? " (all levels)" : "");
        if (const char* d = std::getenv("OS2_PLAN_DEBUG"); d && std::atoi(d) >= 2) // the cells, 1/8 tile each, 4 tiles round
            for (int cy = int(from[1] * K) + 32; cy >= int(from[1] * K) - 32; --cy) {
                std::fprintf(stderr, "  %6.3f ", float(cy) / K);
                for (int cx = int(from[0] * K) - 32; cx <= int(from[0] * K) + 32; ++cx) {
                    const uint8_t b = at(startLevel, {(float(cx) + 0.5f) / K, (float(cy) + 0.5f) / K});
                    std::fputc(cx == int(from[0] * K) && cy == int(from[1] * K) ? 'S' : b & kWall ? '#' : b & kDoor ? 'D' : b & kNoFloor ? ' ' : b & kObjectCore ? 'O' : b & kObject ? 'o' : b ? '?' : '.', stderr);
                }
                std::fputc('\n', stderr);
            }
        why = RouteFail::PathNotFound;
        return false;
    }

    // The nodes walked, start to goal.
    std::vector<size_t> nodes;
    for (int32_t k = found; k >= 0; k = parent[size_t(k)])
        nodes.push_back(size_t(k));
    std::reverse(nodes.begin(), nodes.end());
    // Waypoints: the specials' points and the middles of the edges between
    // squares; `fixed` ones (start, portal ends, goal) stay where they are.
    struct WP {
        Point2 p;
        int level;
        int portal;
        bool fixed;
    };
    std::vector<WP> wps;
    wps.push_back({start, startLevel, -1, true});
    if (out.escaped)
        wps.push_back({from, startLevel, -1, true});
    for (size_t i = 1; i < nodes.size(); ++i) {
        const size_t a = nodes[i - 1], b = nodes[i];
        if (b >= nq) {
            const Special& s = specials[b - nq];
            const bool viaPortal = a >= nq && specials[a - nq].other == int(b - nq);
            wps.push_back({s.p, s.level, viaPortal ? s.portal : -1, true});
        } else if (a < nq) {
            const Quad& q = quads[a];
            const Quad& r = quads[b];
            Point2 m;
            if (r.x + r.size == q.x || q.x + q.size == r.x) {
                const int lo = std::max(q.y, r.y), hi = std::min(q.y + q.size, r.y + r.size);
                m = {float(r.x + r.size == q.x ? q.x : r.x) / K, (float(lo) + float(hi)) / 2 / K};
            } else {
                const int lo = std::max(q.x, r.x), hi = std::min(q.x + q.size, r.x + r.size);
                m = {(float(lo) + float(hi)) / 2 / K, float(r.y + r.size == q.y ? q.y : r.y) / K};
            }
            wps.push_back({m, q.level, -1, false});
        }
    }
    const Goal& goal = usable[size_t(specials[size_t(found) - nq].goal)];
    if (dist(goal.exact, goal.p) > 1e-4f)
        wps.push_back({goal.exact, goalLevel, -1, true});

    // Filters (CreateResultPath has PointSpacing, CornerCutting,
    // ImpassableRegionBoundary, Laplacian 0.45, NewPortalDirection,
    // LaplacianWeighted 0.325; approximation of each): corner cutting =
    // string pulling between fixed points, then points every half tile on
    // the long legs, Laplacian smoothing that keeps the line clear, and
    // the points left on a straight line dropped.
    // A shortcut may not cross anything, nor cells dearer than its ends
    // (another Sim, a costly floor the A* went round).
    auto smoothClear = [&](int level, Point2 a, Point2 b) {
        return clear(level, a, b, 0xFF, std::max(costAt(level, a), costAt(level, b)));
    };
    // CornerCuttingFilter (BV 0x60372c, read in round n18): along the
    // path, from the last point kept, a point is dropped when the straight
    // line from that point to the one after it is passable and costs less
    // than the path between them plus 0.5 (cQuadTree::GetCostAlongLine
    // against the path's own cost), and kept otherwise (or when it is
    // within 0.1 of the last one kept, dropped); the ends, portals and
    // level changes are kept. A detour the A* took for a cheaper floor (the
    // sidewalk round the grass) stays.
    std::vector<float> along(wps.size(), 0.0f); // the path's cost from its start
    for (size_t k = 1; k < wps.size(); ++k) {
        const bool leg = wps[k].portal < 0 && wps[k].level == wps[k - 1].level;
        const float c = leg ? costAlong(wps[k].level, wps[k - 1].p, wps[k].p, kStatic | kObject) : 0.0f;
        along[k] = along[k - 1] + (c < kInf ? c : dist(wps[k - 1].p, wps[k].p) * costAt(wps[k].level, wps[k].p));
    }
    std::vector<WP> pulled;
    pulled.push_back(wps[0]);
    size_t anchor = 0;
    for (size_t k = 1; k + 1 < wps.size(); ++k) {
        const WP& w = wps[k];
        const WP& next = wps[k + 1];
        bool keep = w.fixed || w.portal >= 0 || next.portal >= 0 || next.level != w.level ||
                    wps[anchor].level != w.level;
        if (!keep) {
            const float direct = costAlong(w.level, wps[anchor].p, next.p, kStatic | kObject);
            keep = direct >= kInf ||
                   (along[k + 1] - along[anchor] <= direct - 0.5f && dist(wps[anchor].p, w.p) >= 0.1f);
        }
        if (keep) {
            pulled.push_back(w);
            anchor = k;
        }
    }
    if (wps.size() > 1)
        pulled.push_back(wps.back());
    std::vector<WP> spaced;
    spaced.push_back(pulled[0]);
    for (size_t k = 1; k < pulled.size(); ++k) {
        const WP& a = pulled[k - 1];
        const WP& b = pulled[k];
        if (b.portal < 0 && a.level == b.level) {
            const float d = dist(a.p, b.p);
            const int pieces = int(d / 0.5f);
            for (int s = 1; s < pieces; ++s) {
                const float u = float(s) / float(pieces);
                spaced.push_back({{a.p[0] + (b.p[0] - a.p[0]) * u, a.p[1] + (b.p[1] - a.p[1]) * u}, a.level, -1, false});
            }
        }
        spaced.push_back(b);
    }
    for (const float weight : {0.45f, 0.325f})
        for (size_t k = 1; k + 1 < spaced.size(); ++k) {
            WP& w = spaced[k];
            const WP& a = spaced[k - 1];
            const WP& b = spaced[k + 1];
            if (w.fixed || b.portal >= 0 || a.level != w.level || b.level != w.level)
                continue;
            const Point2 moved{w.p[0] + weight * ((a.p[0] + b.p[0]) / 2 - w.p[0]),
                               w.p[1] + weight * ((a.p[1] + b.p[1]) / 2 - w.p[1])};
            if (smoothClear(w.level, a.p, moved) && smoothClear(w.level, moved, b.p))
                w.p = moved;
        }
    std::vector<WP> simple;
    simple.push_back(spaced[0]);
    for (size_t k = 1; k < spaced.size(); ++k) {
        const WP& w = spaced[k];
        if (k + 1 < spaced.size() && !w.fixed && spaced[k + 1].portal < 0 && spaced[k + 1].level == w.level) {
            const WP& a = simple.back();
            const WP& b = spaced[k + 1];
            const float ux = w.p[0] - a.p[0], uy = w.p[1] - a.p[1], vx = b.p[0] - w.p[0], vy = b.p[1] - w.p[1];
            const float cross = std::fabs(ux * vy - uy * vx), dot = ux * vx + uy * vy;
            if (dot > 0 && cross < 0.035f * std::hypot(ux, uy) * std::hypot(vx, vy))
                continue; // within 2 degrees of straight
        }
        simple.push_back(w);
    }
    for (size_t k = 1; k < simple.size(); ++k)
        out.points.push_back({simple[k].p[0], simple[k].p[1], simple[k].level, simple[k].portal});
    out.goal = goal.index;
    out.cost = g[size_t(found)];
    return true;
}

} // namespace os2::vm
