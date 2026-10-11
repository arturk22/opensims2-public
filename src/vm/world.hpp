#pragma once

#include <array>
#include <cstdint>
#include <span>
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
// (x, y + 1) separates (x - 1, y) and (x, y). Diagonal walls are not kept
// (they cut tiles in half, which a tile grid cannot hold).
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

    bool contains(TilePos t) const;
    // A tile has something to stand on.
    bool walkable(TilePos t) const;
    void setWalkable(TilePos t, bool walkable);
    // Room ID of a tile (the first of its quarters that has one), 0 if none.
    int16_t room(TilePos t) const;
    void setRoom(TilePos t, int16_t room);

    // Axis-aligned wall between grid points (ax, ay) and (bx, by) on a VM
    // level; longer walls are split into tile edges, diagonal ones ignored.
    void addWall(int level, int ax, int ay, int bx, int by);
    // A door: opens the wall edge behind a door at tile `t` facing `dir` (a
    // door stands in front of the wall it cuts, its back to it).
    void openDoor(TilePos t, int dir);
    // A wall between `t` and its neighbour (t.x + dx, t.y + dy); only for
    // orthogonal steps (|dx| + |dy| == 1).
    bool wallBetween(TilePos t, int dx, int dy) const;
    // No wall crosses the straight line between the two tile centres (same level).
    bool lineOfSight(TilePos a, TilePos b) const;

    // Stairs join a landing on one level with one on the level above; the
    // tiles under the steps are not walkable on the lower level.
    void addStair(const Stair& s);
    const std::vector<Stair>& stairs() const { return stairs_; }

    // Index of a tile within one level's grid (x * height + y).
    size_t index(int x, int y) const { return size_t(x) * height_ + size_t(y); }

private:
    uint32_t width_ = 0, height_ = 0;
    int firstLevel_ = 1, levelCount_ = 0;
    std::vector<uint8_t> walkable_;   // [level][x][y]
    std::vector<int16_t> rooms_;      // [level][x][y]
    std::vector<uint8_t> xWalls_;     // [level][x][y]: wall (x, y)-(x + 1, y), x < width, y <= height
    std::vector<uint8_t> yWalls_;     // [level][x][y]: wall (x, y)-(x, y + 1), x <= width, y < height
    std::vector<Stair> stairs_;
    size_t cell(TilePos t) const;
    size_t xEdge(int level, int x, int y) const;
    size_t yEdge(int level, int x, int y) const;
    void setEdge(int level, int ax, int ay, int bx, int by, bool wall);
};

// The stairs of a lot, from the steps' cObjects: each "Stair Modular - ... -
// Portal - A" (the top step) is followed down, a quarter tile and 3/16 of a
// tile at a time along its facing, through the "Bridge" steps to the "End"
// (the bottom step), bridging up to three missing steps in a row (saved lots
// lose some); stairs that do not reach an End are left out, and
// a stair saved twice (the lots keep stale copies) is kept once. VM levels
// (lot level + 1).
std::vector<Stair> lotStairs(const Lot& lot);

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
