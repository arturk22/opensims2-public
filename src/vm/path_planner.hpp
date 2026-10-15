#pragma once

// Path planning in continuous space, as the Mac binary's nPathPlanner does it
// (docs/routing-mac.md): each level is a quadtree of free and blocked
// squares over the lot, every obstacle (wall, object, Sim, the edge of a
// floor) is inflated by 1/8 tile (the room a Sim's body takes), doors and
// stairs are portals between free squares, and the A* path through the
// squares is filtered into a smooth line (corner cutting, then Laplacian
// smoothing). Sims walk to points, not tile centres, so the halves of a tile
// a diagonal wall cuts are places of their own.
//
// From the binary (docs/routing-mac.md): the inflation (0.12499 tile,
// AddObjectRectangle / AddWallRectangle), the portals (AddDoorPortal,
// AddPortal), the door blockers (AddDoorBlockerRect: the opening itself is
// closed, a Sim goes through the door's portal), the floor costs
// (World::cost), the A*'s costs (segment length x the cells' cost, uniform
// noise of up to 0.01 per node, the noisy distance heuristic), Sims as cost
// rectangles, the order of the path filters (CreateResultPath), the
// smallest square (1/8 tile), the portals' cost (1) and their locks (the
// portal object's TEST tree, PathPlanner::portalLocked).
// Inferred / approximated (marked "approximation" below): the A*'s nodes
// (centres of the squares, joined through the middle of the edge they
// share), where the portals' ends are, what each filter does exactly and
// how a walker leaves a blocked start.

#include "world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace os2::vm {

using Point2 = std::array<float, 2>;

// Why a route failed: the route result the Mac binary writes to person
// data 62 (DoPostFailedOrSucceeded; texts in STR# 0xF1 "route results",
// docs/routing-mac.md §7). Codes 4-5 and 8-10 are not traced to who sets
// them in the binary.
enum class RouteFail : int16_t {
    Success = 0,
    Unknown = 1,
    PathNotFound = 2,
    Interrupted = 3,
    CouldntSit = 4,
    CouldntStand = 5,
    NoValidGoals = 6,
    DestinationOccupied = 7,
    NoValidChair = 8,
    WallInTheWay = 9,
    AltitudesDontMatch = 10,
    PreRouteFailed = 11,
    PostRouteFailed = 12,
    GoalNotAvailable = 13,
    BlockedByPerson = 14,
    BlockedByObject = 15,
    InterruptedByCallback = 16,
};

// A goal of a plan: a point on the goal level and what reaching it costs on
// top of the walk (slot positions further from the optimal distance cost more).
struct PlanGoal {
    float x = 0, y = 0;
    float cost = 0;
};

struct PlannedPath {
    struct Waypoint {
        float x = 0, y = 0;
        int level = 0;
        // The walker reaches this point through a portal from the previous
        // one: index into PathPlanner::portals(), -1 for a walk.
        int portal = -1;
    };
    std::vector<Waypoint> points; // after the start, to the goal's point
    size_t goal = 0;              // index of the goal reached
    float cost = 0;               // walk + goal cost
    bool escaped = false;         // the first point only leaves a blocked start
};

class PathPlanner {
public:
    static constexpr int kCellsPerTile = 8;          // smallest square: 1/8 tile (binary: cPathPlanner::Init)
    static constexpr float kInflation = 0.12499f;    // binary: AddObjectRectangle / AddWallRectangle

    // What blocks a cell (bits).
    enum Cell : uint8_t {
        kWall = 0x01,     // an axis-aligned wall or fence
        kNoFloor = 0x02,  // out of the lot, nothing to stand on, steep or under water
        kObject = 0x04,
        kObjectCore = 0x08, // inside an object's footprint itself, not only its inflation
        kDiagonal = 0x10, // a diagonal wall
        kDoor = 0x20,     // a door's opening (the door is a portal)
        kStatic = kWall | kNoFloor | kDiagonal | kDoor,
    };

    // A door or a stair: the walker goes from `a` (on `levelA`) to `b` (on
    // `levelB`) or back, for `cost`.
    struct Portal {
        int levelA = 0, levelB = 0;
        Point2 a{}, b{};
        float cost = 0;
        int stair = -1; // index into World::stairs()
        int door = -1;  // index into World::doors()
        int object = -1; // index into World::objectPortals()
        bool oneWay = false; // from a to b only (cPortal::IsOneWay)
    };

    explicit PathPlanner(const World& world);

    const World& world() const { return *world_; }
    // Whether this planner was built from `w` as it is now (the same world,
    // with the same doors, stairs and diagonal walls).
    bool builtFrom(const World& w) const { return &w == world_ && signature_ == signatureOf(w); }
    static size_t signatureOf(const World& w) { return w.revision(); }
    const std::vector<Portal>& portals() const { return portals_; }

    // An obstacle's footprint (not inflated: it is inflated here), with the
    // cell bit it sets (kObject).
    void addObstacle(int level, float x0, float y0, float x1, float y1, Cell kind);
    // A rectangle that costs `cost` per tile walked (another Sim: §5 of
    // docs/routing-mac.md): a cell costs the most of its floor and the
    // rectangles over it.
    void addCost(int level, float x0, float y0, float x1, float y1, float cost);
    // The cost rectangles of a router (a Sim) at (x, y), standing or
    // walking, as AddBlockerToPathPlanner has them for a human (the binary
    // looks at neither direction nor age; the arguments stay for pets):
    // its box grown by 0.375 (cost 5, standing), 0.1875 (7.5), 0.0625 (10).
    void addRouter(int level, float x, float y, int direction, bool walking, bool small = false);
    // Drops the objects and Sims added (the walls and floors stay).
    void clearObstacles();
    // Whether a portal is locked one way (from its end `a` to `b` when
    // `forward`): asked the first time a plan crosses it that way, as the
    // binary's TSObjectPortalLockedCB runs the portal object's TEST tree;
    // none: every portal is open. A locked portal is not crossed.
    std::function<bool(size_t portal, bool forward)> portalLocked;
    // Seeds the A*'s noise (the binary's generator: seed * 0x278DDE6D).
    void seed(uint32_t s) { noise_ = s ? s : 1; }
    uint32_t noise() const { return noise_; }

    // The cells' bits at a point (kNoFloor out of the lot).
    uint8_t at(int level, Point2 p) const;
    bool free(int level, Point2 p, uint8_t mask = 0xFF) const { return (at(level, p) & mask) == 0; }
    // No cell with any of `mask`'s bits along the segment.
    // With `maxCost`, no cell along it may cost more either.
    bool clear(int level, Point2 a, Point2 b, uint8_t mask = 0xFF,
               float maxCost = std::numeric_limits<float>::infinity()) const;
    // What walking a tile at a point costs (the floor and the cost rectangles).
    float costAt(int level, Point2 p) const;
    // What walking the segment costs (each cell's cost x the length in it,
    // cQuadTree::GetCostAlongLine), infinity when a cell of `mask` is on it.
    float costAlong(int level, Point2 a, Point2 b, uint8_t mask = 0xFF) const;

    // Plans from `start` on `startLevel` to the cheapest of `goals` (on
    // `goalLevel`): false and `why` when none can be reached. A start
    // inside an object or a Sim (a walker getting up, a Sim standing in a
    // doorway) first steps out to the nearest free point without crossing
    // a wall.
    bool plan(Point2 start, int startLevel, int goalLevel, const std::vector<PlanGoal>& goals, PlannedPath& out,
              RouteFail& why);

    // The rooms a walker in `room` can reach (cTSPerson::GetAccessibleRooms:
    // the router fills the set from the Sim's room), sorted; `room` itself
    // included. From the static grid (walls, floors, door openings) and the
    // portals, built once per planner; objects, Sims and portal locks are
    // not looked at (inferred). Empty when `room` has no free cell (the
    // caller then filters nothing).
    const std::vector<int16_t>& accessibleRooms(int16_t room) const;
    // The rooms of each walkable group (for looking into accessibleRooms).
    const std::vector<std::vector<int16_t>>& roomGroups() const {
        if (!roomGroupsBuilt_)
            buildRoomGroups();
        return roomGroups_;
    }

private:
    // plan() on the start's level alone, or on all of them.
    bool plan(Point2 start, int startLevel, int goalLevel, const std::vector<PlanGoal>& goals, PlannedPath& out,
              RouteFail& why, bool allLevels);
    const World* world_;
    size_t signature_ = 0;
    int cw_ = 0, ch_ = 0; // cells per level
    std::vector<std::vector<uint8_t>> static_; // [level - first][cx * ch + cy]
    std::vector<std::vector<float>> staticCosts_; // per cell: the floor
    // The grids with the obstacles and cost rectangles: brought up to date
    // with the calls since clearObstacles() when they are read (syncGrids),
    // only where they changed since the last time.
    mutable std::vector<std::vector<uint8_t>> cells_;  // static + obstacles
    mutable std::vector<std::vector<float>> costs_;    // floor and cost rectangles
    uint32_t noise_ = 1;
    float uniform(float hi); // U(0; hi)
    std::vector<Portal> portals_;
    // A rectangle of cells one obstacle or cost call writes (clipped to the
    // grid, not empty): obstacle bits OR'ed into the cells, or a cost the
    // cells take when it is more than theirs. A cell is its static value
    // with every stamp over it, in whatever order (OR and max), so the same
    // stamps give the same grids.
    struct Stamp {
        int32_t li;             // level - first
        int32_t x0, y0, x1, y1; // cells [x0, x1) x [y0, y1)
        uint32_t bits;          // obstacle bits; 0: a cost
        uint32_t cost;          // the cost's float, bit for bit (0 for an obstacle)
        bool operator==(const Stamp&) const = default;
        auto operator<=>(const Stamp&) const = default;
    };
    std::vector<Stamp> stamps_;          // the calls since clearObstacles()
    mutable std::vector<Stamp> applied_; // what cells_ and costs_ hold
    mutable bool gridsDirty_ = false;
    void record(int li, float x0, float y0, float x1, float y1, uint8_t bits, float cost);
    void syncGrids() const {
        if (gridsDirty_)
            updateGrids();
    }
    void updateGrids() const;
    // The quadtree's sums by square (a pyramid: blocked cells and cost per
    // aligned square of 2^k cells, k >= 1) of each level, brought up to date
    // over the rectangles of cells that changed; it replaces summed-area
    // tables rebuilt from the whole grid at every new set of obstacles. Its
    // sums are exact (as those of the tables were) when every cost is 0 or
    // in [0.5, 16] (exactCosts); else the tables are used.
    struct Pyramid {
        bool valid = false;
        std::vector<std::vector<int32_t>> blocked; // [k - 1][(x >> k) * (n >> k) + (y >> k)]
        std::vector<std::vector<double>> sum;
        std::vector<std::array<int32_t, 4>> dirty; // cell rectangles changed since
    };
    mutable std::vector<Pyramid> pyramids_; // per level
    bool staticCostsExact_ = true;
    int inexactStamps_ = 0; // cost stamps outside exactCosts
    static bool exactCost(float c) { return c == 0.0f || (c >= 0.5f && c <= 16.0f); }
    int treeSide() const; // the quadtree's padded side, a power of two
    void updatePyramid(int li);
    // Scratch of the summed area tables (costs outside exactCosts), kept
    // between plans so each plan does not allocate them.
    std::vector<int32_t> satBlocked_;
    std::vector<double> satCost_;
    // A free square of a level's quadtree, and a level's squares by cell.
    struct Quad {
        int level;
        int x, y, size; // cells
        float cost;
        Point2 centre() const {
            return {(float(x) + float(size) / 2) / kCellsPerTile, (float(y) + float(size) / 2) / kCellsPerTile};
        }
    };
    struct Tree {
        int n = 0; // padded side, a power of two
        std::vector<int32_t> leafOf; // [x * n + y]: index into the quads, -1 blocked
    };
    struct QuadTrees {
        std::vector<Quad> quads;
        std::vector<Tree> trees; // per level
    };
    // The obstacle and cost calls since clearObstacles(), in order: what the
    // grids were built from (compared exactly to reuse a quadtree).
    struct Op {
        int level;
        float x0, y0, x1, y1, kind; // kind >= 0: obstacle bits; < 0: a cost of -1 - kind
        bool operator==(const Op&) const = default;
    };
    struct CachedTrees {
        int level; // -1: all levels, else the start's level alone
        std::vector<Op> ops;
        std::shared_ptr<const QuadTrees> trees;
    };
    static constexpr size_t kTreeCacheSize = 4;
    std::vector<Op> ops_;
    std::vector<CachedTrees> treeCache_; // most recently used first
    std::shared_ptr<const QuadTrees> quadTrees(bool allLevels, int startLevel);
    // accessibleRooms(): rooms by connected group of free static cells
    // (joined through the portals), built on first use.
    mutable bool roomGroupsBuilt_ = false;
    mutable std::vector<std::vector<int16_t>> roomGroups_;   // per group: its rooms, sorted
    mutable std::map<int16_t, std::vector<int16_t>> reach_;  // room -> rooms reachable
    void buildRoomGroups() const;

    size_t cellIndex(int cx, int cy) const { return size_t(cx) * size_t(ch_) + size_t(cy); }
    uint8_t cellAt(int level, int cx, int cy) const; // the grids must be synced (syncGrids)
    void stamp(std::vector<uint8_t>& grid, float x0, float y0, float x1, float y1, uint8_t bit) const;
    void stampDiagonal(std::vector<uint8_t>& grid, const World::Segment& s) const;
    bool escape(int level, Point2 start, Point2& out) const;
    bool nearFree(int level, Point2 p, uint8_t mask, Point2& out) const;
};

} // namespace os2::vm
