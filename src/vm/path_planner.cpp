#include "path_planner.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
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
                const float c = world.cost(t);
                for (int i = 0; i < K; ++i)
                    for (int j = 0; j < K; ++j)
                        cost[cellIndex(x * K + i, y * K + j)] = c;
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
    // Stairs: from the bottom landing to the top one.
    const auto& stairs = world.stairs();
    for (size_t i = 0; i < stairs.size(); ++i) {
        const Stair& s = stairs[i];
        if (!world.contains(s.bottom) || !world.contains(s.top))
            continue;
        Portal p;
        p.levelA = s.bottom.level, p.levelB = s.top.level;
        p.a = s.at(-kStairLanding);
        p.b = s.at(s.length() + kStairLanding);
        p.cost = kPortalCost;
        p.stair = int(i);
        portals_.push_back(p);
    }
    cells_ = static_;
    costs_ = staticCosts_;
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
    stamp(cells_[size_t(level - world_->firstLevel())], x0 - kInflation, y0 - kInflation, x1 + kInflation,
          y1 + kInflation, kind);
}

void PathPlanner::clearObstacles() {
    cells_ = static_;
    costs_ = staticCosts_;
}

void PathPlanner::addCost(int level, float x0, float y0, float x1, float y1, float cost) {
    if (level < world_->firstLevel() || level >= world_->firstLevel() + world_->levelCount())
        return;
    auto& grid = costs_[size_t(level - world_->firstLevel())];
    const int cx0 = std::max(0, int(std::floor(x0 * K))), cy0 = std::max(0, int(std::floor(y0 * K)));
    const int cx1 = std::min(cw_, int(std::ceil(x1 * K))), cy1 = std::min(ch_, int(std::ceil(y1 * K)));
    for (int cx = cx0; cx < cx1; ++cx)
        for (int cy = cy0; cy < cy1; ++cy)
            grid[cellIndex(cx, cy)] = std::max(grid[cellIndex(cx, cy)], cost);
}

void PathPlanner::addRouter(int level, float x, float y, int direction, bool walking, bool small) {
    if (walking) {
        const float tx = std::floor(x), ty = std::floor(y);
        addCost(level, tx - 0.375f, ty - 0.375f, tx + 1.375f, ty + 1.375f, 5.0f);
        return;
    }
    const auto [sx, sy] = directionStep(direction);
    const float n = std::max(1.0f, std::hypot(float(sx), float(sy)));
    const float fx = float(sx) / n, fy = float(sy) / n;
    auto square = [&](float half, float forward, float cost) {
        const float cx = x + fx * forward, cy = y + fy * forward;
        addCost(level, cx - half, cy - half, cx + half, cy + half, cost);
    };
    square(0.375f, 0.125f, 5.0f);
    square(0.375f, 0.09375f, 5.0f);
    square(small ? 0.1875f : 0.25f, 0, 7.5f);
    square(0.09375f, 0, 10.0f);
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
    return cellAt(level, int(std::floor(p[0] * K)), int(std::floor(p[1] * K)));
}

float PathPlanner::costAt(int level, Point2 p) const {
    const int cx = int(std::floor(p[0] * K)), cy = int(std::floor(p[1] * K));
    if (level < world_->firstLevel() || level >= world_->firstLevel() + world_->levelCount() || cx < 0 || cy < 0 ||
        cx >= cw_ || cy >= ch_)
        return kInf;
    return costs_[size_t(level - world_->firstLevel())][cellIndex(cx, cy)];
}

bool PathPlanner::clear(int level, Point2 a, Point2 b, uint8_t mask, float maxCost) const {
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
    while (!open.empty()) {
        const auto [cx, cy] = open.front();
        open.pop_front();
        const uint8_t bits = cellAt(level, cx, cy);
        if (bits == 0) {
            out = {(float(cx) + 0.5f) / K, (float(cy) + 0.5f) / K};
            return true;
        }
        static constexpr int kD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : kD) {
            const int nx = cx + d[0], ny = cy + d[1];
            if (std::abs(nx - sx) > r || std::abs(ny - sy) > r)
                continue;
            uint8_t& s = seen[size_t(nx - sx + r) * side + size_t(ny - sy + r)];
            if (s || (cellAt(level, nx, ny) & (kNoFloor | kDiagonal)))
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

namespace {

// A free square of a level's quadtree.
struct Quad {
    int level;
    int x, y, size; // cells
    float cost;
    Point2 centre() const { return {(float(x) + float(size) / 2) / K, (float(y) + float(size) / 2) / K}; }
};

struct Tree {
    int n = 0; // padded side, a power of two
    std::vector<int32_t> leafOf; // [x * n + y]: index into the quads, -1 blocked
};

} // namespace

bool PathPlanner::plan(Point2 start, int startLevel, int goalLevel, const std::vector<PlanGoal>& goals,
                       PlannedPath& out, RouteFail& why) {
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
            why = RouteFail::PathNotFound;
            return false;
        }
        out.escaped = true;
    }
    // The goals: free points; one only crowded by a wall or a floor's edge
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
        if (bits & kObject)
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
    // the goal is on it, else all (stairs).
    std::vector<Quad> quads;
    std::vector<Tree> trees(static_cast<size_t>(levels));
    const bool allLevels = startLevel != goalLevel;
    for (int li = 0; li < levels; ++li) {
        const int level = first + li;
        if (!allLevels && level != startLevel)
            continue;
        Tree& t = trees[size_t(li)];
        t.n = 1;
        while (t.n < std::max(cw_, ch_))
            t.n *= 2;
        const int n = t.n;
        t.leafOf.assign(size_t(n) * n, -1);
        // Summed area tables of the blocked cells and of the costs.
        const auto& grid = cells_[size_t(li)];
        const auto& cost = costs_[size_t(li)];
        std::vector<int32_t> blocked(size_t(n + 1) * (n + 1), 0);
        std::vector<double> csum(size_t(n + 1) * (n + 1), 0);
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
        // Squares split until each is all free at one cost or all blocked.
        std::vector<std::array<int, 3>> stack{{0, 0, n}};
        while (!stack.empty()) {
            const auto [x, y, s] = stack.back();
            stack.pop_back();
            const int b = area(blocked, x, y, s);
            if (b == s * s)
                continue;
            if (b == 0) {
                const float c0 = cost[cellIndex(x, y)];
                if (s == 1 || std::fabs(area(csum, x, y, s) - double(c0) * s * s) < 1e-3) {
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
    }
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
            if (s.other >= 0 && !locked(size_t(s.portal), s.atA))
                relax(node, nq + size_t(s.other), portals_[size_t(s.portal)].cost);
        }
    }
    if (found < 0) {
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
    std::vector<WP> pulled;
    size_t i = 0;
    pulled.push_back(wps[0]);
    while (i + 1 < wps.size()) {
        size_t j = i + 1;
        if (wps[j].portal < 0 && wps[j].level == wps[i].level)
            for (size_t k = j + 1; k < wps.size(); ++k) {
                if (wps[k].portal >= 0 || wps[k].level != wps[i].level)
                    break;
                if (smoothClear(wps[i].level, wps[i].p, wps[k].p))
                    j = k;
                if (wps[k].fixed)
                    break;
            }
        // Never skip over a fixed point.
        for (size_t k = i + 1; k < j; ++k)
            if (wps[k].fixed) {
                j = k;
                break;
            }
        pulled.push_back(wps[j]);
        i = j;
    }
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
