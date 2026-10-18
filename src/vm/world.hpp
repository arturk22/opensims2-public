#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace os2 {
struct Lot;
}

namespace os2::vm {

// Directions as object data "direction" (1) holds them: 0-7, a step of 45
// degrees each. Read off the saved lots (N001_Lot11): objects that stand
// with their back to a wall (fridges, toilets, doors) face away from it, and
// the model's rotation is 90 - 45 * direction degrees about Z, so direction
// 0 faces -X, 2 faces +Y, 4 faces +X and 6 faces -Y (inferred).
//
// The tile step (dx, dy) that faces direction `dir` (taken modulo 8).
std::array<int, 2> directionStep(int dir);
// The 10-tile road strip of a lot that has no locked tile array (3ARY
// 0x03), as [x0, x1) x [y0, y1) tiles of the rest: each edge whose strip is
// mostly "road_*" floor is left out. Inferred from the data, only a fallback:
// the game locks the strip in the lot template's 3ARY 0x03, which every
// saved lot keeps (Lot::locked, docs/buy-build-mac.md §7.20).
std::array<int, 4> inferredRoadStripArea(const Lot& lot);
// Whether a tile of the lot (lot level, x, y) is locked (cWorldDB::IsLocked,
// cIWorldDB slot 0xc4): Lot::locked, or out of inferredRoadStripArea when the
// lot has no array. `area` is that fallback, computed once.
bool lotTileLocked(const Lot& lot, const std::array<int, 4>& area, int level, int x, int y);
// The direction (0-7) nearest to the vector (dx, dy); -1 for a zero vector.
int directionOf(float dx, float dy);

// A tile of the lot, on a VM level: 1 is the ground, as the saved objects
// number them (Lot level + 1).
struct TilePos {
    int x = 0, y = 0, level = 0;
    bool operator==(const TilePos&) const = default;
};

// A staircase between two levels (the game's modular stairs: one object per
// step, a quarter tile long and 3/16 of a tile high, from the "End" at the
// bottom to the "Portal - A" at the top, all facing the way down). Sims
// walk onto it from the landing tile in front of the bottom step or behind
// the top one.
struct Stair {
    TilePos bottom, top;           // landing tiles; top.level == bottom.level + 1
    std::array<float, 2> bottomEdge{}; // where the stair meets the bottom landing
    std::array<float, 2> up{};     // unit vector up the stair (opposite to the steps' facing)
    int steps = 0;                 // a quarter tile each
    float bottomZ = 0, topZ = 0;   // heights of the landings' floors
    int direction = 0;             // 0-7, up the stair (world.hpp directions)
    // The point `u` tiles up the stair from its bottom edge (negative: onto
    // the bottom landing; past steps / 4: onto the top one).
    std::array<float, 2> at(float u) const { return {bottomEdge[0] + up[0] * u, bottomEdge[1] + up[1] * u}; }
    float length() const { return float(steps) * 0.25f; }
};

// What does not move on a lot: its bounds, which tiles have something to
// walk on, the walls (and fences) between tiles, and the rooms. Objects are
// added on top of it per query (they move): see `blocked` grids below.
//
// Walls are kept as the edges between tiles: a wall from grid point (x, y)
// to (x + 1, y) separates tiles (x, y - 1) and (x, y); one from (x, y) to
// (x, y + 1) separates (x - 1, y) and (x, y). A diagonal wall cuts the
// tiles it crosses in half, which a tile grid cannot hold: those tiles are
// not walkable (walkers go around them, and canStep's corner rule keeps them
// from slipping between the halves' neighbours), unless a door stands in
// them.
class World {
public:
    World() = default;
    // A flat, open world: every tile of every level has a floor.
    World(uint32_t width, uint32_t height, int firstLevel = 1, int levelCount = 1);
    // The lot's walls (all types: walls, fences, foundation and pool edges),
    // floors (the ground level is the terrain, walkable everywhere; upper
    // levels need a floor tile), rooms and stairs (lotStairs).
    static World fromLot(const Lot& lot);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    int firstLevel() const { return firstLevel_; }
    int levelCount() const { return levelCount_; }
    bool empty() const { return width_ == 0; }
    // Bumped by every change (walls, doors, floors, stairs, costs): what
    // was built from the world (the path planner) is stale when it differs.
    uint32_t revision() const { return revision_; }

    bool contains(TilePos t) const;
    // A locked tile (cWorldDB::IsLocked: the lot's 3ARY 0x03, lotTileLocked):
    // Buy mode refuses it with 0x02 (IsOnLockedTile).
    bool locked(TilePos t) const {
        if (!contains(t) || locked_.empty())
            return false;
        return locked_[size_t(t.level - firstLevel_) * width_ * height_ + index(t.x, t.y)] != 0;
    }
    // A tile has something to stand on.
    bool walkable(TilePos t) const;
    void setWalkable(TilePos t, bool walkable);
    // Room ID of a tile (the first of its quarters that has one), 0 if none.
    int16_t room(TilePos t) const;
    void setRoom(TilePos t, int16_t room);

    // Wall between grid points (ax, ay) and (bx, by) on a VM level: an
    // axis-aligned one is split into tile edges, a diagonal one (|dx| ==
    // |dy|) marks the tiles it crosses; other slopes are ignored.
    // `walkOver`: a low fence Sims step over (cTSFenceEntry::MayWalkOver,
    // the fence's "maywalkover"): it bounds placement but not routing.
    void addWall(int level, int ax, int ay, int bx, int by, bool walkOver = false);
    // A door: opens the wall edge behind a door at tile `t` facing `dir` (a
    // door stands in front of the wall it cuts, its back to it); a door in a
    // diagonal wall opens the tile the wall crosses.
    void openDoor(TilePos t, int dir);
    // A wall between `t` and its neighbour (t.x + dx, t.y + dy); only for
    // orthogonal steps (|dx| + |dy| == 1).
    // `placement` also counts the walk-over fences (they block placement).
    bool wallBetween(TilePos t, int dx, int dy, bool placement = false) const;
    // A diagonal wall crosses the tile.
    bool diagonalWall(TilePos t) const;
    // Which diagonal walls cross the tile, as the binary's wall sides
    // (MovePointAwayFromWalls): 0x10 the one along fx + fy = 1 (grid slope
    // -1), 0x20 the one along fx = fy (slope +1); fx, fy in the tile.
    int diagonalSides(TilePos t) const;
    // No wall crosses the straight line between the two tile centres (same
    // level); tiles in between with a diagonal wall stop it.
    bool lineOfSight(TilePos a, TilePos b) const;

    // Stairs join a landing on one level with one on the level above; the
    // tiles under the steps are not walkable on the lower level.
    void addStair(const Stair& s);
    const std::vector<Stair>& stairs() const { return stairs_; }
    // The stair whose steps stand on a tile that has a floor (addStair's
    // tiles), -1 for none. The binary has no rule for these tiles: its
    // floor is there and the stair's pieces block by their footprints
    // (docs/routing-mac.md §9.4), which the path planner follows when the
    // stair's objects make its portals; walkable() and hasFloor() still
    // leave them out (Go Here, placement).
    int stairOver(TilePos t) const;

    // A portal an object's model makes (cTSObject::PortalAddToPathPlannerImpl,
    // BV 0xe22a44: a pair of its portal routing slots, docs/routing-mac.md
    // §8): the pool ladder's, from the pool's bottom (a level down) to the
    // deck. `a`/`b` are the two slots' points on their parts' levels; the
    // parts and their routing slots go to the portal tree (OBJf entry 15).
    struct ObjectPortal {
        int levelA = 0, levelB = 0;
        std::array<float, 2> a{}, b{};
        int16_t partA = 0, partB = 0; // object IDs
        int16_t slotA = 0, slotB = 0; // routing slot numbers in each part's SLOT
        bool oneWay = false;          // from a to b only (cPortal::IsOneWay)
        int stair = -1;               // the World::stairs entry a stair's portal climbs
        bool operator==(const ObjectPortal&) const = default;
    };
    // (The world changes, its revision with it, only when the portals do.)
    void setObjectPortals(std::vector<ObjectPortal> p) {
        if (p == objectPortals_)
            return;
        ++revision_;
        objectPortals_ = std::move(p);
    }
    const std::vector<ObjectPortal>& objectPortals() const { return objectPortals_; }

    // The doors openDoor opened, in order: the tile, the facing and whether
    // the door stands in a diagonal wall (the path planner makes each a
    // portal, path_planner.hpp).
    struct Door {
        TilePos tile;
        int dir = 0;
        bool diagonal = false;
    };
    const std::vector<Door>& doors() const { return doors_; }
    // The diagonal walls as added (grid points, a VM level each): the path
    // planner cuts their tiles in half along them.
    struct Segment {
        int level = 0;
        int ax = 0, ay = 0, bx = 0, by = 0;
    };
    const std::vector<Segment>& diagonalWalls() const { return diagonalWalls_; }
    // A tile has a floor (or terrain), whatever crosses it.
    bool hasFloor(TilePos t) const;
    // A tile of a pool (the lot's POOL entries): the ground tile over it
    // and the tile of its bottom a level down.
    bool pool(TilePos t) const { return contains(t) && pools_.count(cell(t)) != 0; }
    void setPool(TilePos t) {
        if (contains(t))
            pools_.insert(cell(t));
    }
    // What walking over a tile costs per tile walked (1 by default; from a
    // lot, World::fromLot: the floor's routing cost, the terrain's 2, the
    // slope), the most of its four quarter-tile triangles; and the cost of
    // the quarter under a point of it (west, north, east, south: the
    // nearest edge, as the lots' floors are laid).
    float cost(TilePos t) const;
    float cost(TilePos t, float x, float y) const;
    void setCost(TilePos t, float cost);
    void setQuarterCosts(TilePos t, const std::array<float, 4>& costs);
    // The quarter tiles the path planner closes by their floor, a bit each
    // (1 west, 2 north, 4 east, 8 south, as the costs): base 1.0's
    // WorkOutTileCostAndPassability state 5 (BV 7; FUN_01038900, BV
    // FUN_00165c88), the terrain under a floor on the level above that
    // stands less than StandardLevelHeight (3.0) over it at a corner
    // (World::fromLot). They are passed through no further: the planner
    // marks the cells whose centre is in such a quarter (docs/routing-mac.md
    // §9.5).
    uint8_t blockedQuarters(TilePos t) const { return contains(t) && !blocked_.empty() ? blocked_[cell(t)] : 0; }
    void setBlockedQuarters(TilePos t, uint8_t quarters);
    // Whether the quarter of `t` under the point (x, y) is closed
    // (blockedQuarters; the quarter as cost(t, x, y) picks it).
    bool quarterBlocked(TilePos t, float x, float y) const;

    // Index of a tile within one level's grid (x * height + y).
    size_t index(int x, int y) const { return size_t(x) * height_ + size_t(y); }

private:
    std::vector<uint8_t> locked_;     // [level][x][y], empty = none
    uint32_t width_ = 0, height_ = 0;
    int firstLevel_ = 1, levelCount_ = 0;
    uint32_t revision_ = 0;
    std::vector<uint8_t> walkable_;   // [level][x][y]
    std::vector<int16_t> underSteps_; // [level][x][y]: 1 + the stair whose steps stand on it, 0 none
    std::set<size_t> pools_;          // cells over pools
    std::vector<ObjectPortal> objectPortals_;
    std::vector<int16_t> rooms_;      // [level][x][y]
    std::vector<uint8_t> xWalls_;     // [level][x][y]: wall (x, y)-(x + 1, y), x < width, y <= height
    std::vector<uint8_t> yWalls_;     // [level][x][y]: wall (x, y)-(x, y + 1), x <= width, y < height
    std::vector<Stair> stairs_;
    std::vector<uint8_t> diagonals_;  // [level][x][y]: 1 a diagonal wall crosses it, 2 a door opens it
    std::vector<std::array<float, 4>> costs_; // [level][x][y], per quarter (west, north, east, south)
    std::vector<uint8_t> blocked_;    // [level][x][y], the closed quarters (blockedQuarters); empty = none
    std::vector<Door> doors_;
    std::vector<Segment> diagonalWalls_;
    size_t cell(TilePos t) const;
    size_t xEdge(int level, int x, int y) const;
    size_t yEdge(int level, int x, int y) const;
    // 0 none, 1 a wall, 2 a walk-over fence (a wall is kept over a fence).
    void setEdge(int level, int ax, int ay, int bx, int by, uint8_t wall);
};

// The stairs of a lot, from the steps' cObjects: each "Stair Modular - ... -
// Portal - A" (the top step) is followed down, a quarter tile and 3/16 of a
// tile at a time along its facing, through the "Bridge" steps to the "End"
// (the bottom step), bridging up to three missing steps in a row (saved lots
// lose some); stairs that do not reach an End are left out, and
// a stair saved twice (the lots keep stale copies) is kept once. VM levels
// (lot level + 1).
std::vector<Stair> lotStairs(const Lot& lot);

// The fences (catalog GUIDs, the walls' type) Sims walk over: World::fromLot
// leaves them out of routing. Set once from the build catalog
// (formats/build_catalog.hpp walkOverFences).
void setWalkOverFences(std::set<uint32_t> guids);
const std::set<uint32_t>& walkOverFences();

// The floors' routing costs by pattern name (formats/build_catalog.hpp
// floorRoutingCosts): World::fromLot gives each quarter tile its floor's.
// Set once from the build catalog; empty: every floor costs 1.
void setFloorRoutingCosts(std::map<std::string, float> costs);

// Tiles of one level that something stands in the way on (1 = blocked),
// indexed by World::index. Routing builds one per route from the objects.
using BlockedGrid = std::vector<uint8_t>;

// Whether a walker can step from `from` by (dx, dy) (-1..1 each) on its
// level: the tile is in bounds, walkable and not blocked, no wall is in the
// way, and a diagonal step does not cut a corner (both orthogonal steps
// around it must be open, as Sims do not squeeze past wall ends or objects).
bool canStep(const World& world, const BlockedGrid& blocked, TilePos from, int dx, int dy);

// A goal of a route: a tile and what reaching it costs on top of the walk
// (slot positions further from the optimal distance cost more).
struct PathGoal {
    int x = 0, y = 0;
    float cost = 0;
};

struct Path {
    std::vector<TilePos> tiles; // from the first step to the goal; empty when already on it
    size_t goal = 0;            // index of the goal reached
    float cost = 0;             // walk + goal cost
};

// A* over the tiles of `start`'s level, 8-connected (straight steps cost 1,
// diagonal ones sqrt 2, as canStep allows them), to the goal with the lowest
// walk + goal cost. The start tile may be blocked (a walker leaves the tile
// it is on); goal tiles must not be. False if no goal can be reached.
bool findPath(const World& world, const BlockedGrid& blocked, TilePos start, const std::vector<PathGoal>& goals,
              Path& out);

// The same across levels, by the world's stairs: `blocked` has a grid per
// level (index level - firstLevel), the goals are on `goalLevel`, and
// `stairs` gets, for each tile of the path, the stair taken to reach it (-1
// for a step on a level). A stair costs 1.5 x the walk between its landings.
bool findPath(const World& world, std::span<const BlockedGrid> blocked, TilePos start, int goalLevel,
              const std::vector<PathGoal>& goals, Path& out, std::vector<int>& stairs);

} // namespace os2::vm
