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
// rectangles and the order of the path filters (CreateResultPath).
// Inferred / approximated (marked "approximation" below): the smallest
// square (1/8 tile, the inflation; the binary's is not found), the A*'s
// nodes (centres of the squares, joined through the middle of the edge
// they share), the portals' costs, what each filter does exactly and how a
// walker leaves a blocked start.

#include "world.hpp"

#include <array>
#include <cstdint>
#include <limits>
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
    static constexpr int kCellsPerTile = 8;          // smallest square: 1/8 tile (approximation)
    static constexpr float kInflation = 0.12499f;    // binary: AddObjectRectangle / AddWallRectangle

    // What blocks a cell (bits).
    enum Cell : uint8_t {
        kWall = 0x01,     // an axis-aligned wall or fence
        kNoFloor = 0x02,  // out of the lot, nothing to stand on, steep or under water
        kObject = 0x04,
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
    // The cost rectangles of a router (a Sim) at (x, y) facing `direction`
    // (0-7), standing or walking, as AddBlockerToPathPlanner has them:
    // standing, half-widths 0.375 (cost 5, twice, pushed 0.125 / 0.09375
    // forward), 0.25 (7.5; 0.1875 for a small one) and 0.09375 (10); walking,
    // its tile +- 0.375 at cost 5.
    void addRouter(int level, float x, float y, int direction, bool walking, bool small = false);
    // Drops the objects and Sims added (the walls and floors stay).
    void clearObstacles();
    // Seeds the A*'s noise (the binary's generator: seed * 0x278DDE6D).
    void seed(uint32_t s) { noise_ = s ? s : 1; }

    // The cells' bits at a point (kNoFloor out of the lot).
    uint8_t at(int level, Point2 p) const;
    bool free(int level, Point2 p, uint8_t mask = 0xFF) const { return (at(level, p) & mask) == 0; }
    // No cell with any of `mask`'s bits along the segment.
    // With `maxCost`, no cell along it may cost more either.
    bool clear(int level, Point2 a, Point2 b, uint8_t mask = 0xFF,
               float maxCost = std::numeric_limits<float>::infinity()) const;
    // What walking a tile at a point costs (the floor and the cost rectangles).
    float costAt(int level, Point2 p) const;

    // Plans from `start` on `startLevel` to the cheapest of `goals` (on
    // `goalLevel`): false and `why` when none can be reached. A start
    // inside an object or a Sim (a walker getting up, a Sim standing in a
    // doorway) first steps out to the nearest free point without crossing
    // a wall.
    bool plan(Point2 start, int startLevel, int goalLevel, const std::vector<PlanGoal>& goals, PlannedPath& out,
              RouteFail& why);

private:
    const World* world_;
    size_t signature_ = 0;
    int cw_ = 0, ch_ = 0; // cells per level
    std::vector<std::vector<uint8_t>> static_; // [level - first][cx * ch + cy]
    std::vector<std::vector<uint8_t>> cells_;  // static + obstacles
    std::vector<std::vector<float>> staticCosts_; // per cell: the floor
    std::vector<std::vector<float>> costs_;       // floor and cost rectangles
    uint32_t noise_ = 1;
    float uniform(float hi); // U(0; hi)
    std::vector<Portal> portals_;

    size_t cellIndex(int cx, int cy) const { return size_t(cx) * size_t(ch_) + size_t(cy); }
    uint8_t cellAt(int level, int cx, int cy) const;
    void stamp(std::vector<uint8_t>& grid, float x0, float y0, float x1, float y1, uint8_t bit) const;
    void stampDiagonal(std::vector<uint8_t>& grid, const World::Segment& s) const;
    bool escape(int level, Point2 start, Point2& out) const;
    bool nearFree(int level, Point2 p, uint8_t mask, Point2& out) const;
};

} // namespace os2::vm
