// Build mode's tools over a lot in the VM (build_edit.hpp).

#include "build_edit.hpp"

#include "formats/lot_edit.hpp"
#include "lot.hpp"
#include "placement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace os2::vm {

namespace {

constexpr uint32_t kWallType = Lot::Normal; // walls.txt "wall 1", the wall tool's
const char* const kDefaultWallPattern = "blank"; // its defaultPattern
// Object data 0x3E: an object in use (UserMayEraseWallOrFence: > 0, or
// IsOccupied).
constexpr int16_t kInUseData = 0x3E;

std::string money(int v) { return "§" + std::to_string(v); }

std::string point(int x, int y) { return "(" + std::to_string(x) + "," + std::to_string(y) + ")"; }

// The streets the lot packages carry (named floor patterns of
// floorPatterns.txt): nothing is built over them (inferred: the game keeps
// the lot's buildable area off them).
bool street(const std::string& pattern) { return pattern.rfind("road_", 0) == 0 || pattern == "sidewalk"; }

// The grid points a section stands on: its ends, or the four corners of
// the tile a diagonal crosses (TryAdjustFloorElevationForWall levels the
// bounding box of a diagonal).
std::vector<std::array<int, 2>> sectionPoints(const std::array<int, 4>& s) {
    if (s[0] != s[2] && s[1] != s[3]) {
        const int x = std::min(s[0], s[2]), y = std::min(s[1], s[3]);
        return {{x, y}, {x + 1, y}, {x, y + 1}, {x + 1, y + 1}};
    }
    return {{s[0], s[1]}, {s[2], s[3]}};
}

bool sameSection(const std::array<int, 4>& a, const Lot::Wall& w) {
    auto at = [](const Lot::WallVertex& v, int x, int y) { return std::lround(v.x) == x && std::lround(v.y) == y; };
    return (at(w.a, a[0], a[1]) && at(w.b, a[2], a[3])) || (at(w.a, a[2], a[3]) && at(w.b, a[0], a[1]));
}

// The partitions round the raised tiles: foundation 23, lattice deck 16,
// column deck skirt 4 (deckfoundations.txt wallStyle).
bool raisedEdge(uint32_t type) { return type == Lot::Foundation || type == 16 || type == Lot::DeckSkirt; }

// deckfoundations.txt products 0-2: the edge style, its pattern, the floor.
struct DeckProduct {
    uint32_t style;
    const char* wallPattern;
    const char* floor;
    int price;
};
DeckProduct deckProduct(int product) {
    switch (product) {
    case 1: return {16, "deckredwood", "deckDarkRedwood", kLatticeDeckTilePrice};
    case 2: return {Lot::DeckSkirt, "deckSkirtMinimal", "deckDarkRedwood", kColumnDeckTilePrice};
    default: return {Lot::Foundation, "foundationbrick", "floor_plywoodFoundation", kFoundationTilePrice};
    }
}
// deck 2's supportObject (the column under a deck on the terrain).
constexpr uint32_t kDeckColumnGuid = 0xAA75A097;

} // namespace

std::vector<Stair> objectStairs(const VM& vm, const Lot& lot) {
    // A staircase object (OBJD "Stairs - Straight - <style>", build type 1
    // subsort 0x20): its "Lo" parts stand on the lower level, from the "Lo
    // ... Pad" (the bottom landing) along the steps, and its "Hi" parts on
    // the level above (OBJD level offset 1), the "Hi ... Pad" being the top
    // landing.
    std::map<ObjectId, std::pair<std::optional<TilePos>, std::optional<TilePos>>> pads; // group -> (bottom, top)
    for (const auto& [id, o] : vm.objects()) {
        if (!o.definition || !o.position.inWorld())
            continue;
        const std::string& n = o.definition->fileName;
        if (n.rfind("Stairs - ", 0) != 0 || n.find(" Pad") == std::string::npos)
            continue;
        auto& p = pads[multiTileGroup(o)];
        if (n.find(" - Lo ") != std::string::npos)
            p.first = VM::tileOf(o);
        else if (n.find(" - Hi ") != std::string::npos)
            p.second = VM::tileOf(o);
    }
    std::vector<Stair> out;
    for (const auto& [group, p] : pads) {
        if (!p.first || !p.second || p.second->level != p.first->level + 1)
            continue;
        const int dx = p.second->x - p.first->x, dy = p.second->y - p.first->y;
        if ((dx != 0) == (dy != 0) || std::abs(dx + dy) < 2)
            continue;
        Stair st;
        const float ux = float(dx > 0) - float(dx < 0), uy = float(dy > 0) - float(dy < 0);
        st.bottom = *p.first, st.top = *p.second;
        st.up = {ux, uy};
        st.steps = 4 * (std::abs(dx + dy) - 1);
        st.bottomEdge = {float(p.first->x) + 0.5f + ux * 0.5f, float(p.first->y) + 0.5f + uy * 0.5f};
        st.direction = directionOf(ux, uy);
        st.bottomZ = lot.elevationAt(st.bottom.level - 1, float(st.bottom.x) + 0.5f, float(st.bottom.y) + 0.5f);
        st.topZ = lot.elevationAt(st.top.level - 1, float(st.top.x) + 0.5f, float(st.top.y) + 0.5f);
        out.push_back(st);
        (void)group;
    }
    return out;
}

std::vector<std::array<int, 4>> autoRoofRects(const std::vector<uint8_t>& grid, int W, int H, float aspectLimit) {
    // cRoofRect: [x0, x1) x [y0, y1) and the row it last grew on.
    struct Rect {
        int x0, y0, x1, y1, row;
        bool operator<(const Rect& o) const {
            return std::tie(x0, y0, x1, y1, row) < std::tie(o.x0, o.y0, o.x1, o.y1, o.row);
        }
    };
    std::set<Rect> active;
    std::vector<Rect> done;
    auto marked = [&](int x, int y) { return grid[size_t(y) * size_t(W) + size_t(x)] != 0; };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W;) {
            if (!marked(x, y)) {
                ++x;
                continue;
            }
            int e = x;
            while (e < W && marked(e, y))
                ++e;
            // AddToActiveRects: an active rectangle inside the run grows down
            // a row; one that overlaps (or touches) it gives a new one, the
            // overlap from its top down to this row; the run itself starts a
            // rectangle when some of its cells are in neither.
            const Rect run{x, y, e, y + 1, y};
            std::vector<uint8_t> uncovered(size_t(e - x), 1);
            std::vector<std::pair<Rect, Rect>> grown;
            std::vector<Rect> split;
            for (const Rect& a : active) {
                if (a.x0 >= run.x0 && a.x1 <= run.x1) {
                    grown.push_back({a, {a.x0, a.y0, a.x1, a.y1 + 1, run.row}});
                    std::fill(uncovered.begin() + (a.x0 - x), uncovered.begin() + (a.x1 - x), 0);
                } else if (run.x0 <= a.x1 && a.x0 <= run.x1) {
                    const int lo = std::max(a.x0, run.x0), hi = std::min(a.x1, run.x1);
                    split.push_back({lo, a.y0, hi, a.y1 + 1, run.row});
                    if (hi > lo)
                        std::fill(uncovered.begin() + (lo - x), uncovered.begin() + (hi - x), 0);
                }
            }
            for (const auto& [from, to] : grown) {
                active.erase(from);
                active.insert(to);
            }
            for (const Rect& r : split)
                if (std::none_of(grown.begin(), grown.end(),
                                 [&](const auto& g) { return g.second.x0 == r.x0 && g.second.x1 == r.x1; }))
                    active.insert(r);
            if (std::find(uncovered.begin(), uncovered.end(), 1) != uncovered.end())
                active.insert(run);
            x = e;
        }
        // MoveInactiveRects: what did not grow on this row is finished.
        for (auto it = active.begin(); it != active.end();)
            if (it->row != y)
                done.push_back(*it), it = active.erase(it);
            else
                ++it;
    }
    done.insert(done.end(), active.begin(), active.end());
    // BV 0x9a0a08 (with 2.0): the cells' cover counted over every
    // rectangle; then, most elongated first (cAspectRatioGreaterThan), one
    // longer than `aspectLimit` to one goes when every cell of it stays
    // covered by the others (its cover is taken off either way).
    std::erase_if(done, [](const Rect& r) { return r.x1 <= r.x0 || r.y1 <= r.y0; });
    auto aspect = [](const Rect& r) {
        const float w = float(r.x1 - r.x0), h = float(r.y1 - r.y0);
        return std::max(w, h) / std::min(w, h);
    };
    std::stable_sort(done.begin(), done.end(), [&](const Rect& a, const Rect& b) { return aspect(a) > aspect(b); });
    std::vector<int> cover(size_t(W) * size_t(H), 0);
    for (const Rect& r : done)
        for (int yy = r.y0; yy < r.y1; ++yy)
            for (int xx = r.x0; xx < r.x1; ++xx)
                ++cover[size_t(yy) * size_t(W) + size_t(xx)];
    std::vector<std::array<int, 4>> out;
    for (const Rect& r : done) {
        bool needed = !(aspect(r) > aspectLimit);
        if (!needed)
            for (int yy = r.y0; yy < r.y1; ++yy)
                for (int xx = r.x0; xx < r.x1; ++xx)
                    if (--cover[size_t(yy) * size_t(W) + size_t(xx)] < 1)
                        needed = true;
        if (needed)
            out.push_back({r.x0, r.y0, r.x1, r.y1});
    }
    return out;
}

std::vector<std::array<int, 4>> wallSections(int x0, int y0, int x1, int y1) {
    std::vector<std::array<int, 4>> out;
    const int dx = x1 - x0, dy = y1 - y0;
    if (dx && dy && std::abs(dx) != std::abs(dy))
        return out;
    const int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
    for (int x = x0, y = y0; x != x1 || y != y1; x += sx, y += sy)
        out.push_back({x, y, x + sx, y + sy});
    return out;
}

BuildEditor::BuildEditor(VM& vm, Lot& lot, const BuildCatalog* catalog) : vm_(vm), lot_(lot), catalog_(catalog) {
    // The stairs the world was loaded with (the lot's modular stair
    // cObjects), less the staircase objects that are among them.
    if (vm_.world) {
        const auto objects = objectStairs(vm_, lot_);
        for (const Stair& s : vm_.world->stairs())
            if (std::none_of(objects.begin(), objects.end(),
                             [&](const Stair& o) { return o.bottom == s.bottom && o.top == s.top; }))
                loadedStairs_.push_back(s);
    }
}

BuildEditor::Geometry BuildEditor::capture() const {
    Geometry g;
    g.elevations = lot_.elevations;
    g.floorRotations = lot_.floorRotations;
    g.objects = lot_.objects;
    g.fencePosts = lot_.fencePosts;
    g.paints = lot_.paints;
    g.roofs = lot_.roofs;
    g.pools = lot_.pools;
    g.water = lot_.water;
    g.minLevel = lot_.minLevel, g.levelCount = lot_.levelCount;
    g.walls = lot_.walls;
    g.wallGraphs = lot_.wallGraphs;
    g.floors = lot_.floors;
    g.rooms = lot_.rooms;
    g.roomLayers = lot_.roomLayers;
    g.floorPatterns = lot_.floorPatterns;
    g.wallPatterns = lot_.wallPatterns;
    g.staleWallLayer = lot_.staleWallLayer;
    return g;
}

void BuildEditor::restore(const Geometry& g) {
    merges_.clear();
    lot_.elevations = g.elevations;
    lot_.floorRotations = g.floorRotations;
    lot_.objects = g.objects;
    lot_.fencePosts = g.fencePosts;
    lot_.paints = g.paints;
    lot_.roofs = g.roofs;
    lot_.pools = g.pools;
    lot_.water = g.water;
    lot_.minLevel = g.minLevel, lot_.levelCount = g.levelCount;
    lot_.walls = g.walls;
    lot_.wallGraphs = g.wallGraphs;
    lot_.floors = g.floors;
    lot_.rooms = g.rooms;
    lot_.roomLayers = g.roomLayers;
    lot_.floorPatterns = g.floorPatterns;
    lot_.wallPatterns = g.wallPatterns;
    lot_.staleWallLayer = g.staleWallLayer;
}

void BuildEditor::syncWorld() {
    // The world from the lot's walls, floors and rooms; the stairs (the
    // lot's own and the staircase objects now in the world) and doors as
    // the VM has them.
    Lot shape = lot_;
    shape.objects.clear();
    World w = World::fromLot(shape);
    // The lot's modular stairs while their top step is still in the world
    // (the sledgehammer sells flights), then the ones Build mode laid and
    // the staircase objects.
    std::vector<Stair> stairs;
    auto known = [&](const Stair& s) {
        return std::any_of(stairs.begin(), stairs.end(), [&](const Stair& o) { return o.bottom == s.bottom && o.top == s.top; });
    };
    for (const Stair& s : loadedStairs_) {
        const auto top = s.at(0.25f * float(s.steps - 1));
        const bool there = std::any_of(vm_.objects().begin(), vm_.objects().end(), [&](const auto& kv) {
            const Object& o = kv.second;
            return o.position.inWorld() && o.definition && o.definition->fileName.rfind("Stair Modular - ", 0) == 0 &&
                   o.definition->fileName.find(" - Portal - A") != std::string::npos &&
                   std::fabs(o.position.x - top[0]) < 0.05f && std::fabs(o.position.y - top[1]) < 0.05f;
        });
        if (there && !known(s))
            stairs.push_back(s);
    }
    for (const Stair& s : modularStairs(vm_, lot_))
        if (!known(s))
            stairs.push_back(s);
    for (const Stair& s : objectStairs(vm_, lot_))
        if (!known(s))
            stairs.push_back(s);
    for (const Stair& s : stairs)
        w.addStair(s);
    // Each part of a door opens the wall behind it when the tile behind is
    // another part of the same door (vm::loadLot).
    for (const auto& [id, o] : vm_.objects()) {
        if (!o.definition || o.definition->objectType() != kObjectTypeDoor || !o.position.inWorld())
            continue;
        const int dir = o.data[objdata::Direction];
        const auto [dx, dy] = directionStep(dir + 4);
        const TilePos t = VM::tileOf(o);
        for (const auto& [pid, p] : vm_.objects())
            if (pid != id && p.position.inWorld() && multiTileGroup(p) == multiTileGroup(o) &&
                VM::tileOf(p) == TilePos{t.x + dx, t.y + dy, t.level}) {
                w.openDoor(t, dir);
                break;
            }
    }
    // The objects whose tile changed room get the new room (object data 29).
    std::vector<std::pair<ObjectId, int16_t>> moved;
    if (vm_.world)
        for (const auto& [id, o] : vm_.objects()) {
            if (!o.position.inWorld() || o.data[objdata::ContainerId])
                continue;
            const TilePos t = VM::tileOf(o);
            if (vm_.world->contains(t) && w.contains(t) && vm_.world->room(t) != w.room(t))
                moved.push_back({id, w.room(t)});
        }
    vm_.world = std::move(w);
    vm_.planner.reset();
    setLotRooms(vm_, lot_);
    for (const auto& [id, room] : moved)
        if (Object* o = vm_.object(id))
            o->data[objdata::Room] = room;
    ++revision_;
}

void BuildEditor::changed() {
    updateRooms(lot_, merges_);
    merges_.clear();
    syncWorld();
}

void BuildEditor::commit(Geometry before, int cost, std::vector<Sold> sold, std::vector<Sold> created) {
    Op op;
    op.before = std::move(before);
    op.after = capture();
    op.cost = cost;
    op.sold = std::move(sold);
    op.created = std::move(created);
    undo_.push_back(std::move(op));
    redo_.clear();
}

std::string BuildEditor::wallBlocked(int level, int ax, int ay, int bx, int by, int* code, bool fence,
                                     bool levelled) const {
    auto fail = [&](int c, const char* why) {
        if (code)
            *code = c;
        return std::string(why);
    };
    const int li = level - 1;
    if (!lot_.hasLevel(li))
        return fail(0x40, "no such level");
    if (fence && li < 0)
        return fail(0x1C, "no fences below the ground");
    const int W = int(lot_.width), H = int(lot_.height);
    if (std::min({ax, bx, ay, by}) < 0 || std::max(ax, bx) > W || std::max(ay, by) > H)
        return fail(0x40, "out of the lot");
    // The tiles on both sides (an edge) or the tile crossed (a diagonal).
    std::vector<TilePos> tiles;
    if (ax != bx && ay != by) {
        tiles.push_back({std::min(ax, bx), std::min(ay, by), level});
    } else if (ay == by) {
        tiles.push_back({std::min(ax, bx), ay - 1, level});
        tiles.push_back({std::min(ax, bx), ay, level});
    } else {
        tiles.push_back({ax - 1, std::min(ay, by), level});
        tiles.push_back({ax, std::min(ay, by), level});
    }
    std::erase_if(tiles, [&](const TilePos& t) { return t.x < 0 || t.y < 0 || t.x >= W || t.y >= H; });
    // A wall needs one side in the lot, a fence both (0x40); here the lot
    // edge counts as out for a fence.
    if (fence && tiles.size() < (ax != bx && ay != by ? 1u : 2u))
        return fail(0x40, "out of the lot");
    for (const TilePos& t : tiles)
        for (uint16_t q : lot_.floor(li, uint32_t(t.x), uint32_t(t.y)))
            if (q)
                if (auto it = lot_.floorPatterns.find(q); it != lot_.floorPatterns.end() && street(it->second))
                    return fail(0x45, "not on the street");
    const float dz = std::fabs(lot_.elevation(li, ax, ay) - lot_.elevation(li, bx, by));
    if (fence) {
        // UserMayPlaceFence: |dz| / horizontal length above sqrt(3) (60°).
        if (dz / std::hypot(float(bx - ax), float(by - ay)) > std::sqrt(3.0f))
            return fail(0x31, "the terrain is too steep");
    } else if (li == 0 && !levelled && dz > 0.01f) {
        // requiresFlatBottom (walls.txt): on the ground the terrain under
        // the wall must be level.
        return fail(0x49, "the ground must be flat");
    }
    // Above the ground a wall stands on a wall or next to a floor (inferred
    // from requiresSupportUnderneath; the game's exact support test is not read).
    if (li > 0 && !findWall(lot_, li - 1, ax, ay, bx, by)) {
        bool floor = false;
        for (const TilePos& t : tiles)
            for (uint16_t q : lot_.floor(li, uint32_t(t.x), uint32_t(t.y)))
                floor |= q != 0;
        if (!floor)
            return fail(0x48, "nothing holds it up");
    }
    // Objects: a wall does not cut a multi-tile object in two, and a
    // diagonal does not cross a tile something stands on (the walls'
    // blocksPlacement; IntersectsWalls of the placement tests, §3.3).
    // A Sim on either side (MayPlaceWallWithRespectToObjects: 0x4B).
    for (const auto& [id, o] : vm_.objects())
        if (o.person && o.position.inWorld() &&
            std::find(tiles.begin(), tiles.end(), VM::tileOf(o)) != tiles.end())
            return fail(0x4B, "a Sim is in the way");
    if (tiles.size() == 1) {
        for (ObjectId id : vm_.objectsOnTile(tiles[0]))
            if (const Object* o = vm_.object(id); o && !o->person)
                return fail(0x0C, "an object is in the way");
    } else if (tiles.size() == 2) {
        for (ObjectId a : vm_.objectsOnTile(tiles[0]))
            for (ObjectId b : vm_.objectsOnTile(tiles[1])) {
                const Object* oa = vm_.object(a);
                const Object* ob = vm_.object(b);
                if (oa && ob && multiTileGroup(*oa) == multiTileGroup(*ob))
                    return fail(0x0C, "an object is in the way");
            }
    }
    return "";
}

bool BuildEditor::levelPoints(const std::vector<std::array<int, 2>>& points, float height,
                              const std::vector<std::array<int, 4>>& free) {
    if (!lot_.hasLevel(0) || lot_.elevations.empty())
        return true;
    const int W = int(lot_.width), H = int(lot_.height);
    const size_t base = size_t(0 - lot_.minLevel) * size_t(W + 1) * size_t(H + 1);
    std::vector<size_t> change;
    for (const auto& [x, y] : points) {
        if (x < 0 || y < 0 || x > W || y > H)
            continue;
        const size_t i = base + size_t(x) * size_t(H + 1) + size_t(y);
        if (std::fabs(lot_.elevations[i] - height) < 1e-4f)
            continue;
        // A ground floor on a tile around the point, or a wall (or fence)
        // ending there, holds it (inferred: the binary's leveller,
        // FUN_009a5a68, is not read).
        for (int tx = x - 1; tx <= x; ++tx)
            for (int ty = y - 1; ty <= y; ++ty)
                if (tx >= 0 && ty >= 0 && tx < W && ty < H)
                    for (uint16_t q : lot_.floor(0, uint32_t(tx), uint32_t(ty)))
                        if (q)
                            return false;
        for (const auto& w : lot_.walls) {
            if (w.a.level != 0)
                continue;
            const bool touches = (std::lround(w.a.x) == x && std::lround(w.a.y) == y) ||
                                 (std::lround(w.b.x) == x && std::lround(w.b.y) == y);
            if (touches && std::none_of(free.begin(), free.end(), [&](const auto& f) { return sameSection(f, w); }))
                return false;
        }
        change.push_back(i);
    }
    for (size_t i : change)
        lot_.elevations[i] = height;
    return true;
}

BuildEditor::Result BuildEditor::drawWalls(int level, int x0, int y0, int x1, int y1) {
    Result r;
    const auto sections = wallSections(x0, y0, x1, y1);
    if (sections.empty()) {
        r.message = "walls go along the grid or at 45 degrees";
        return r;
    }
    Geometry before = capture();
    // PlaceWallLine: section by section from the start, the ground levelled
    // under each to the start's height; the first refusal stops the run.
    const float height = lot_.elevation(0, x0, y0);
    std::vector<std::array<int, 4>> todo;
    for (const auto& s : sections) {
        if (findWall(lot_, level - 1, s[0], s[1], s[2], s[3]))
            continue;
        const std::vector<float> ground = lot_.elevations;
        if (level == 1)
            levelPoints(sectionPoints(s), height, todo);
        if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3], &r.error); !why.empty()) {
            lot_.elevations = ground;
            r.message = "cannot build a wall from " + point(s[0], s[1]) + " to " + point(s[2], s[3]) + ": " + why;
            break;
        }
        todo.push_back(s);
    }
    r.cost = int(todo.size()) * kWallSectionPrice;
    if (todo.empty() || r.cost > funds(vm_)) {
        restore(before);
        if (!todo.empty())
            r.message = "not enough money (" + money(r.cost) + ")";
        else if (r.message.empty())
            r.ok = true, r.message = "the walls are there already";
        r.cost = 0;
        return r;
    }
    const uint16_t blank = patternRef(lot_.wallPatterns, kDefaultWallPattern, 0);
    for (const auto& s : todo)
        if (addWall(lot_, level - 1, s[0], s[1], s[2], s[3], kWallType, {blank, blank}))
            ++r.changed;
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " wall sections for " + money(r.cost) +
                (r.message.empty() ? "" : " (stopped: " + r.message + ")");
    return r;
}

BuildEditor::Result BuildEditor::drawRoom(int level, int x0, int y0, int x1, int y1) {
    Result r;
    if (x0 == x1 || y0 == y1) {
        r.message = "a room needs an area";
        return r;
    }
    // The four sides, as one change; the room tool's "L"s must both pass
    // (UserAddPartitions type 1), so any refusal builds nothing.
    const std::array<std::array<int, 4>, 4> sides{{{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}};
    Geometry before = capture();
    const float height = lot_.elevation(0, x0, y0);
    std::vector<std::array<int, 4>> todo;
    for (const auto& side : sides)
        for (const auto& s : wallSections(side[0], side[1], side[2], side[3])) {
            if (findWall(lot_, level - 1, s[0], s[1], s[2], s[3]))
                continue;
            if (level == 1)
                levelPoints(sectionPoints(s), height, todo);
            if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3], &r.error); !why.empty()) {
                restore(before);
                r.message = "cannot build the room: " + why;
                return r;
            }
            todo.push_back(s);
        }
    if (int(todo.size()) * kWallSectionPrice > funds(vm_)) {
        restore(before);
        r.message = "not enough money (" + money(int(todo.size()) * kWallSectionPrice) + ")";
        return r;
    }
    const uint16_t blank = patternRef(lot_.wallPatterns, kDefaultWallPattern, 0);
    for (const auto& s : todo)
        if (addWall(lot_, level - 1, s[0], s[1], s[2], s[3], kWallType, {blank, blank}))
            ++r.changed;
    r.cost = r.changed * kWallSectionPrice;
    r.ok = true;
    if (!r.changed) {
        restore(before);
        return r.message = "the walls are there already", r;
    }
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.message = "a room of " + std::to_string(r.changed) + " wall sections for " + money(r.cost);
    return r;
}

std::vector<ObjectId> BuildEditor::attachedObjects(int level, int ax, int ay, int bx, int by, bool* inUse) const {
    std::vector<ObjectId> out;
    if (inUse)
        *inUse = false;
    if (ax != bx && ay != by)
        return out; // nothing stands in a tile a diagonal crosses
    // The tiles on both sides and the step from each to the wall.
    std::vector<std::pair<TilePos, std::array<int, 2>>> sides;
    if (ay == by)
        sides = {{{std::min(ax, bx), ay - 1, level}, {0, 1}}, {{std::min(ax, bx), ay, level}, {0, -1}}};
    else
        sides = {{{ax - 1, std::min(ay, by), level}, {1, 0}}, {{ax, std::min(ay, by), level}, {-1, 0}}};
    for (const auto& [t, step] : sides) {
        if (!vm_.world || !vm_.world->contains(t))
            continue;
        for (ObjectId id : vm_.objectsOnTile(t)) {
            const Object* o = vm_.object(id);
            if (!o || o->person || o->data[objdata::ContainerId] || !o->position.inWorld())
                continue;
            const int dir = o->data[objdata::Direction];
            bool attached = false;
            // Doors and windows stand in the wall in front of or behind
            // them; other objects attach where their wall flags (od 13)
            // need a wall.
            const bool portal = o->definition && (o->definition->objectType() == kObjectTypeDoor ||
                                                  o->definition->objectType() == kObjectTypeWindow);
            const int16_t wp = o->data[objdata::WallPlacementFlags];
            for (int side = 0; side < 4 && !attached; ++side)
                if (sideStep(dir, side) == step)
                    attached = portal ? (side == 0 || side == 2) : (wp & (1 << side)) != 0;
            if (!attached)
                continue;
            const ObjectId lead = multiTileGroup(*o);
            if (std::find(out.begin(), out.end(), lead) != out.end())
                continue;
            out.push_back(lead);
            // In use: object data 0x3E above 0, or a Sim in one of its
            // slots (IsOccupied), in any part of the group.
            for (const auto& [pid, p] : vm_.objects()) {
                if (multiTileGroup(p) != lead || p.person)
                    continue;
                bool used = p.data[kInUseData] > 0;
                for (const auto& [sid, sim] : vm_.objects())
                    used |= sim.person && sim.data[objdata::ContainerId] == pid;
                if (used && inUse)
                    *inUse = true;
            }
        }
    }
    return out;
}

BuildEditor::Result BuildEditor::deleteWalls(int level, int x0, int y0, int x1, int y1) {
    return eraseRun(level, x0, y0, x1, y1, false);
}

BuildEditor::Result BuildEditor::deleteFences(int level, int x0, int y0, int x1, int y1) {
    return eraseRun(level, x0, y0, x1, y1, true);
}

BuildEditor::Result BuildEditor::eraseRun(int level, int x0, int y0, int x1, int y1, bool fences) {
    Result r;
    const auto sections = wallSections(x0, y0, x1, y1);
    if (sections.empty()) {
        r.message = "walls go along the grid or at 45 degrees";
        return r;
    }
    Geometry before = capture();
    int refund = 0, objectRefund = 0; // the objects' is paid by sell()
    std::vector<Sold> sold;
    std::vector<std::array<int, 2>> ends;
    std::string stopped;
    for (const auto& s : sections) {
        const Lot::Wall* w = findWall(lot_, level - 1, s[0], s[1], s[2], s[3]);
        // Only the partitions the wall tool deletes (walls.txt deleteTool
        // partition: walls and fences; foundations, deck skirts and pools go
        // with their own tools). UserMayEraseWallOrFence: no wall there is
        // skipped (0x42), a Sim beside it (0x4B) or an attached object in use
        // (0x1B) stops the run there.
        if (!w || !(w->type == Lot::Normal || Lot::isFence(w->type)) || (fences && !Lot::isFence(w->type)))
            continue;
        const uint32_t type = w->type;
        int code = 0;
        if (wallBlocked(level, s[0], s[1], s[2], s[3], &code, true) == "a Sim is in the way") {
            r.error = 0x4B, stopped = "a Sim is in the way";
            break;
        }
        bool inUse = false;
        const auto attached = attachedObjects(level, s[0], s[1], s[2], s[3], &inUse);
        if (inUse) {
            r.error = 0x1B, stopped = "an object on the wall is in use";
            break;
        }
        // TryRemoveWallOrFence: the attached objects are sold first
        // (EraseWallAttachedObjects, their current value back), then the
        // section for int(price x RefundRate); the full price comes back
        // only through undo.
        for (ObjectId id : attached) {
            Sold x;
            x.id = id;
            x.where = currentPlacement(vm_, id);
            objectRefund += sell(vm_, id);
            for (ObjectId part : groupOf(vm_, id))
                std::erase_if(lot_.objects, [&](const Lot::Object& c) { return c.instance == uint32_t(uint16_t(part)); });
            sold.push_back(std::move(x));
        }
        int price = kWallSectionPrice;
        if (Lot::isFence(type)) {
            const BuildFence* f = catalog_ ? catalog_->fenceByGuid(type) : nullptr;
            price = f ? f->price : 0;
            ends.push_back({s[0], s[1]}), ends.push_back({s[2], s[3]});
        }
        refund += int(float(price) * kBuildRefundRate);
        removeWall(lot_, level - 1, s[0], s[1], s[2], s[3], &merges_);
        ++r.changed;
    }
    // Posts at the ends left with no fence go (inferred from
    // TryRemoveWallOrFence's post pass: the binary's count limit is not read).
    for (const auto& [x, y] : ends) {
        bool fenced = false;
        for (const auto& w : lot_.walls)
            fenced |= Lot::isFence(w.type) && w.a.level == level - 1 &&
                      ((std::lround(w.a.x) == x && std::lround(w.a.y) == y) ||
                       (std::lround(w.b.x) == x && std::lround(w.b.y) == y));
        if (!fenced)
            std::erase_if(lot_.fencePosts, [&](const Lot::FencePost& p) {
                return p.level == level - 1 && std::lround(p.x) == x && std::lround(p.y) == y;
            });
    }
    if (!r.changed) {
        restore(before);
        r.ok = stopped.empty();
        r.message = stopped.empty() ? "no walls there" : "cannot delete the wall: " + stopped;
        return r;
    }
    r.ok = true;
    r.cost = -(refund + objectRefund);
    addFunds(vm_, refund);
    changed();
    r.message = std::to_string(r.changed) + (fences ? " fence" : " wall") + " sections deleted for " + money(refund) +
                (sold.empty() ? "" : ", " + std::to_string(sold.size()) + " attached objects sold for " +
                                         money(objectRefund)) +
                (stopped.empty() ? "" : " (stopped: " + stopped + ")");
    commit(std::move(before), r.cost, std::move(sold));
    return r;
}

BuildEditor::Result BuildEditor::drawFences(int level, int x0, int y0, int x1, int y1, const BuildFence& fence) {
    Result r;
    const auto sections = wallSections(x0, y0, x1, y1);
    if (sections.empty()) {
        r.message = "fences go along the grid or at 45 degrees";
        return r;
    }
    std::vector<std::array<int, 4>> todo;
    for (const auto& s : sections) {
        if (findWall(lot_, level - 1, s[0], s[1], s[2], s[3]))
            continue;
        if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3], &r.error, true); !why.empty()) {
            r.message = "cannot put a fence from " + point(s[0], s[1]) + " to " + point(s[2], s[3]) + ": " + why;
            break;
        }
        todo.push_back(s);
    }
    r.cost = int(todo.size()) * fence.price;
    if (todo.empty() || r.cost > funds(vm_)) {
        if (!todo.empty())
            r.message = "not enough money (" + money(r.cost) + ")";
        else if (r.message.empty())
            r.ok = true, r.message = "the fence is there already";
        r.cost = 0;
        return r;
    }
    Geometry before = capture();
    for (const auto& s : todo)
        if (addWall(lot_, level - 1, s[0], s[1], s[2], s[3], fence.guid, {0xFFFF, 0xFFFF}))
            ++r.changed;
    // A post at every grid point of the run that has none (PlaceWallLine).
    std::vector<std::array<int, 2>> points;
    for (const auto& s : sections)
        points.push_back({s[0], s[1]});
    points.push_back({sections.back()[2], sections.back()[3]});
    for (const auto& [x, y] : points) {
        const bool has = std::any_of(lot_.fencePosts.begin(), lot_.fencePosts.end(), [&](const Lot::FencePost& p) {
            return p.level == level - 1 && std::lround(p.x) == x && std::lround(p.y) == y;
        });
        bool fenced = false;
        for (const auto& w : lot_.walls)
            fenced |= Lot::isFence(w.type) && w.a.level == level - 1 &&
                      ((std::lround(w.a.x) == x && std::lround(w.a.y) == y) ||
                       (std::lround(w.b.x) == x && std::lround(w.b.y) == y));
        if (!has && fenced)
            lot_.fencePosts.push_back({level - 1, float(x), float(y), fence.guid});
    }
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " sections of " + fence.name + " for " + money(r.cost) +
                (r.message.empty() ? "" : " (stopped: " + r.message + ")");
    return r;
}

bool BuildEditor::foundationTile(int x, int y) const {
    if (!lot_.hasLevel(0) || !lot_.hasLevel(1) || x < 0 || y < 0 || x >= int(lot_.width) || y >= int(lot_.height))
        return false;
    for (int cx = x; cx <= x + 1; ++cx)
        for (int cy = y; cy <= y + 1; ++cy)
            if (const float d = lot_.elevation(1, cx, cy) - lot_.elevation(0, cx, cy);
                d > kFoundationHeight + 0.5f || d < kFoundationHeight - 0.5f)
                return false; // (a story over terrain that rose is no foundation either)
    return true;
}

void BuildEditor::foundationWalls(int newStyle, const std::set<std::pair<int, int>>* changedTiles, bool laid) {
    // Only the edges of the tiles that changed (and their neighbours'):
    // the lot's other raised edges stay as they are (a house on a taller
    // foundation keeps its walls).
    if (!changedTiles || changedTiles->empty())
        return;
    std::set<std::pair<int, int>> near;
    for (const auto& [x, y] : *changedTiles)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                near.insert({x + dx, y + dy});
    auto touches = [&](const std::array<int, 4>& s) {
        // The tiles on both sides of an axis edge.
        const int x = std::min(s[0], s[2]), y = std::min(s[1], s[3]);
        if (s[1] == s[3])
            return changedTiles->count({x, y}) || changedTiles->count({x, y - 1});
        return changedTiles->count({x, y}) || changedTiles->count({x - 1, y});
    };
    struct Want {
        std::array<int, 4> s;
        bool fresh; // the edge of a tile just laid
    };
    std::vector<Want> want;
    for (const auto& [x, y] : near) {
        if (!foundationTile(x, y))
            continue;
        const bool fresh = laid && changedTiles->count({x, y});
        if (!foundationTile(x - 1, y))
            want.push_back({{x, y, x, y + 1}, fresh});
        if (!foundationTile(x + 1, y))
            want.push_back({{x + 1, y, x + 1, y + 1}, fresh});
        if (!foundationTile(x, y - 1))
            want.push_back({{x, y, x + 1, y}, fresh});
        if (!foundationTile(x, y + 1))
            want.push_back({{x, y + 1, x + 1, y + 1}, fresh});
    }
    std::erase_if(want, [&](const Want& w) { return !touches(w.s); });
    std::vector<std::array<int, 4>> gone;
    for (const auto& w : lot_.walls) {
        if (!raisedEdge(w.type) || w.a.level != 0)
            continue;
        const std::array<int, 4> s{int(std::lround(w.a.x)), int(std::lround(w.a.y)), int(std::lround(w.b.x)),
                                   int(std::lround(w.b.y))};
        if (touches(s) && std::none_of(want.begin(), want.end(), [&](const Want& x) { return sameSection(x.s, w); }))
            gone.push_back(s);
    }
    for (const auto& s : gone)
        removeWall(lot_, 0, s[0], s[1], s[2], s[3], &merges_);
    // An edge keeps its style; a new one takes the tool's (a foundation's
    // when its tile was not just laid: the edge a deletion opens).
    for (const auto& [s, fresh] : want)
        if (!findWall(lot_, 0, s[0], s[1], s[2], s[3])) {
            const DeckProduct d = deckProduct(fresh ? newStyle : 0);
            const uint16_t ref = patternRef(lot_.wallPatterns, d.wallPattern, 0);
            addWall(lot_, 0, s[0], s[1], s[2], s[3], d.style, {ref, ref});
        }
}

BuildEditor::Result BuildEditor::drawFoundation(int x0, int y0, int x1, int y1, int product) {
    Result r;
    const DeckProduct deck = deckProduct(product);
    const char* what = product ? "deck" : "foundation";
    auto fail = [&](int code, const std::string& why) {
        r.error = code;
        r.message = std::string("cannot lay a ") + what + ": " + why;
        return r;
    };
    if (!lot_.hasLevel(0) || !lot_.hasLevel(1) || lot_.elevations.empty())
        return fail(0x1A, "the lot has no story above the ground");
    const int W = int(lot_.width), H = int(lot_.height);
    const int ax = std::max(0, std::min(x0, x1)), bx = std::min(W - 1, std::max(x0, x1));
    const int ay = std::max(0, std::min(y0, y1)), by = std::min(H - 1, std::max(y0, y1));
    // CanPlaceTile / MayPlaceQuadrilateral (BV 0x34f5a8, 0x352a30): water
    // (0x1E), objects that are not allowed in a foundation (0x0C, od 42
    // without 0x200); walls and floors inside are refused too (inferred).
    std::vector<std::array<int, 2>> tiles;
    for (int x = ax; x <= bx; ++x)
        for (int y = ay; y <= by; ++y) {
            if (foundationTile(x, y))
                continue;
            for (uint16_t q : lot_.floor(0, uint32_t(x), uint32_t(y)))
                if (q) {
                    auto it = lot_.floorPatterns.find(q);
                    return fail(it != lot_.floorPatterns.end() && street(it->second) ? 0x45 : 0x0C,
                                "a floor is in the way at " + point(x, y));
                }
            for (int cx = x; cx <= x + 1; ++cx)
                for (int cy = y; cy <= y + 1; ++cy)
                    if (lot_.waterLevel(cx, cy) > lot_.elevation(0, cx, cy))
                        return fail(0x1E, "water at " + point(x, y));
            for (ObjectId id : vm_.objectsOnTile({x, y, 1}))
                if (const Object* o = vm_.object(id); o && (o->person || !(o->data[objdata::PlacementFlags] & 0x200)))
                    return fail(o->person ? 0x4B : 0x0C, "something is in the way at " + point(x, y));
            tiles.push_back({x, y});
        }
    for (const auto& w : lot_.walls) {
        if (w.a.level != 0 || raisedEdge(w.type))
            continue;
        const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
        if (mx >= float(ax) && mx <= float(bx + 1) && my >= float(ay) && my <= float(by + 1))
            return fail(0x0C, "a wall is in the way");
    }
    if (tiles.empty()) {
        r.ok = true;
        r.message = std::string("the ") + what + " is there already";
        return r;
    }
    r.cost = int(tiles.size()) * deck.price;
    if (r.cost > funds(vm_)) {
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    Geometry before = capture();
    // The terrain levelled to the first corner (MayPlaceQuadrilateral:
    // each point not at that height must be movable, else 0x81).
    const float height = lot_.elevation(0, std::clamp(x0, 0, W), std::clamp(y0, 0, H));
    std::vector<std::array<int, 2>> points;
    for (const auto& [x, y] : tiles)
        for (int cx = x; cx <= x + 1; ++cx)
            for (int cy = y; cy <= y + 1; ++cy)
                points.push_back({cx, cy});
    std::vector<std::array<int, 4>> free;
    for (const auto& w : lot_.walls)
        if (raisedEdge(w.type) && w.a.level == 0)
            free.push_back({int(std::lround(w.a.x)), int(std::lround(w.a.y)), int(std::lround(w.b.x)),
                            int(std::lround(w.b.y))});
    if (!levelPoints(points, height, free)) {
        restore(before);
        return fail(0x81, "the ground cannot be levelled");
    }
    // The stories above sit a foundation higher there.
    const size_t plane = size_t(W + 1) * size_t(H + 1);
    for (const auto& [cx, cy] : points)
        for (int k = 1; lot_.hasLevel(k); ++k)
            lot_.elevations[size_t(k - lot_.minLevel) * plane + size_t(cx) * size_t(H + 1) + size_t(cy)] =
                height + kFoundationHeight + 3.0f * float(k - 1);
    const uint16_t top = patternRef(lot_.floorPatterns, deck.floor, 1);
    std::set<std::pair<int, int>> laid;
    for (const auto& [x, y] : tiles) {
        laid.insert({x, y});
        for (int q = 0; q < 4; ++q)
            setFloor(lot_, 1, x, y, q, top), setFloorRotation(lot_, 1, x, y, q, 1);
    }
    foundationWalls(product, &laid, true);
    // A deck on columns: its support object at each corner of the rectangle
    // (supportLayout -corners), on the terrain under it (inferred: the
    // column stands on the corner tile).
    std::vector<Sold> created;
    if (product == 2)
        if (const auto def = vm_.code().definitionByGuid(kDeckColumnGuid))
            for (const auto& [cx, cy] : std::set<std::pair<int, int>>{{ax, ay}, {bx, ay}, {ax, by}, {bx, by}}) {
                try {
                    const ObjectId id = createGhost(vm_, def->first, def->second);
                    const Placement p = checkPlacement(vm_, id, {cx, cy, 1}, 0);
                    if (!p.ok) {
                        deleteGroup(vm_, id);
                        continue;
                    }
                    // Put down as the object tool does (the trees run), part of the deck's price.
                    if (!buy(vm_, id, p, 0)) {
                        deleteGroup(vm_, id);
                        continue;
                    }
                    created.push_back({id, p});
                } catch (const std::exception&) {
                }
            }
    r.changed = int(tiles.size());
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost, {}, std::move(created));
    r.ok = true;
    r.message = std::to_string(r.changed) + " " + what + " tiles for " + money(r.cost) +
                (product == 2 ? " (" + std::to_string(undo_.back().created.size()) + " columns)" : "");
    return r;
}

BuildEditor::Result BuildEditor::deleteFoundation(int x0, int y0, int x1, int y1) {
    Result r;
    if (!lot_.hasLevel(1)) {
        r.message = "no foundation there";
        return r;
    }
    const int W = int(lot_.width), H = int(lot_.height);
    const int ax = std::max(0, std::min(x0, x1)), bx = std::min(W - 1, std::max(x0, x1));
    const int ay = std::max(0, std::min(y0, y1)), by = std::min(H - 1, std::max(y0, y1));
    std::vector<std::array<int, 2>> tiles;
    for (int x = ax; x <= bx; ++x)
        for (int y = ay; y <= by; ++y)
            if (foundationTile(x, y)) {
                // Not with something standing on it, or walls on it (inferred).
                for (ObjectId id : vm_.objectsOnTile({x, y, 2}))
                    if (vm_.object(id)) {
                        r.error = 0x0C, r.message = "cannot delete the foundation: something stands on " + point(x, y);
                        return r;
                    }
                tiles.push_back({x, y});
            }
    for (const auto& w : lot_.walls) {
        if (w.a.level != 1)
            continue;
        // A wall on the tiles (one along their edge with the rest stays).
        const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
        if (mx > float(ax) + 0.01f && mx < float(bx + 1) - 0.01f && my > float(ay) + 0.01f &&
            my < float(by + 1) - 0.01f) {
            r.error = 0x0C, r.message = "cannot delete the foundation: a wall stands on it";
            return r;
        }
    }
    if (tiles.empty()) {
        r.ok = true;
        r.message = "no foundation there";
        return r;
    }
    Geometry before = capture();
    const size_t plane = size_t(W + 1) * size_t(H + 1);
    std::set<std::pair<int, int>> removed;
    for (const auto& [x, y] : tiles)
        removed.insert({x, y});
    auto stays = [&](int x, int y) { return !removed.count({x, y}) && foundationTile(x, y); };
    std::vector<std::array<int, 2>> corners;
    // Each tile gives back its product's price: a deck's floor marks a deck
    // (the lattice one's price; inferred: the game keeps the product per tile).
    int refund = 0;
    std::vector<Sold> columns;
    for (const auto& [x, y] : tiles) {
        bool deck = false;
        for (uint16_t q : lot_.floor(1, uint32_t(x), uint32_t(y)))
            if (auto it = lot_.floorPatterns.find(q); it != lot_.floorPatterns.end() && it->second.rfind("deck", 0) == 0)
                deck = true;
        refund += int(float(deck ? kLatticeDeckTilePrice : kFoundationTilePrice) * kBuildRefundRate);
        // The deck's columns on it go with it (back by undo).
        for (ObjectId id : vm_.objectsOnTile({x, y, 1}))
            if (const Object* o = vm_.object(id); o && o->guid() == kDeckColumnGuid) {
                columns.push_back({id, currentPlacement(vm_, id)});
                pickUp(vm_, id);
            }
        for (int q = 0; q < 4; ++q)
            setFloor(lot_, 1, x, y, q, 0);
        for (int cx = x; cx <= x + 1; ++cx)
            for (int cy = y; cy <= y + 1; ++cy)
                if (!stays(cx - 1, cy - 1) && !stays(cx, cy - 1) && !stays(cx - 1, cy) && !stays(cx, cy))
                    corners.push_back({cx, cy});
    }
    for (const auto& [cx, cy] : corners) {
        const float ground = lot_.elevation(0, cx, cy);
        for (int k = 1; lot_.hasLevel(k); ++k)
            lot_.elevations[size_t(k - lot_.minLevel) * plane + size_t(cx) * size_t(H + 1) + size_t(cy)] =
                ground + 3.0f * float(k);
    }
    foundationWalls(0, &removed, false);
    r.changed = int(tiles.size());
    r.cost = -refund;
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost, std::move(columns));
    r.ok = true;
    r.message = std::to_string(r.changed) + " foundation tiles deleted for " + money(-r.cost);
    return r;
}

std::vector<std::array<int, 4>> BuildEditor::atticWalls(int lotLevel) const {
    std::vector<std::array<int, 4>> out;
    auto covered = [&](const Lot::Roof* self, float mx, float my) {
        // Inside another roof of the level (not on its edge).
        for (const auto& r : lot_.roofs) {
            if (&r == self || r.level != lotLevel)
                continue;
            const float ax = std::min(r.x0, r.x1), bx = std::max(r.x0, r.x1), ay = std::min(r.y0, r.y1),
                        by = std::max(r.y0, r.y1);
            if (mx > ax + 0.01f && mx < bx - 0.01f && my > ay + 0.01f && my < by - 0.01f)
                return true;
        }
        return false;
    };
    for (const auto& r : lot_.roofs) {
        if (r.level != lotLevel)
            continue;
        const int ax = int(std::lround(std::min(r.x0, r.x1))), bx = int(std::lround(std::max(r.x0, r.x1)));
        const int ay = int(std::lround(std::min(r.y0, r.y1))), by = int(std::lround(std::max(r.y0, r.y1)));
        std::vector<std::array<int, 4>> ends; // runs along the gable ends
        const bool xLonger = bx - ax >= by - ay;
        switch (r.type) {
        case Lot::LongGable: // the ridge along the longer side: gables on the short ends
            if (xLonger)
                ends = {{ax, ay, ax, by}, {bx, ay, bx, by}};
            else
                ends = {{ax, ay, bx, ay}, {ax, by, bx, by}};
            break;
        case Lot::ShortGable:
            if (xLonger)
                ends = {{ax, ay, bx, ay}, {ax, by, bx, by}};
            else
                ends = {{ax, ay, ax, by}, {bx, ay, bx, by}};
            break;
        case Lot::ShedGable: {
            // The slope rises the way of the drag: the sides and the high edge.
            const float dx = r.x1 - r.x0, dy = r.y1 - r.y0;
            if (std::fabs(dx) >= std::fabs(dy))
                ends = {{ax, ay, bx, ay}, {ax, by, bx, by}, dx > 0 ? std::array<int, 4>{bx, ay, bx, by} : std::array<int, 4>{ax, ay, ax, by}};
            else
                ends = {{ax, ay, ax, by}, {bx, ay, bx, by}, dy > 0 ? std::array<int, 4>{ax, by, bx, by} : std::array<int, 4>{ax, ay, bx, ay}};
            break;
        }
        default: break;
        }
        for (const auto& e : ends)
            for (const auto& s : wallSections(e[0], e[1], e[2], e[3])) {
                if (covered(&r, (float(s[0]) + float(s[2])) / 2, (float(s[1]) + float(s[3])) / 2))
                    continue;
                if (std::none_of(out.begin(), out.end(), [&](const auto& o) {
                        return (o[0] == s[0] && o[1] == s[1] && o[2] == s[2] && o[3] == s[3]) ||
                               (o[0] == s[2] && o[1] == s[3] && o[2] == s[0] && o[3] == s[1]);
                    }))
                    out.push_back(s);
            }
    }
    return out;
}

void BuildEditor::syncAtticWalls(int lotLevel, const std::vector<std::array<int, 4>>& before) {
    // Only what the change of roofs changes: the lot's other attic walls
    // stay as they are (the saved lots do not always follow the rule).
    const auto after = atticWalls(lotLevel);
    auto in = [](const std::vector<std::array<int, 4>>& v, const std::array<int, 4>& s) {
        return std::any_of(v.begin(), v.end(), [&](const auto& o) {
            return (o[0] == s[0] && o[1] == s[1] && o[2] == s[2] && o[3] == s[3]) ||
                   (o[0] == s[2] && o[1] == s[3] && o[2] == s[0] && o[3] == s[1]);
        });
    };
    for (const auto& s : before)
        if (!in(after, s))
            if (const Lot::Wall* w = findWall(lot_, lotLevel, s[0], s[1], s[2], s[3]); w && w->type == Lot::Attic)
                removeWall(lot_, lotLevel, s[0], s[1], s[2], s[3], &merges_);
    const uint16_t blank = patternRef(lot_.wallPatterns, kDefaultWallPattern, 0);
    for (const auto& s : after)
        if (!in(before, s) && !findWall(lot_, lotLevel, s[0], s[1], s[2], s[3]))
            addWall(lot_, lotLevel, s[0], s[1], s[2], s[3], Lot::Attic, {blank, blank});
}

BuildEditor::Result BuildEditor::drawRoof(int level, int x0, int y0, int x1, int y1, uint32_t type) {
    Result r;
    const int li = level - 1 + 1; // the roof's base: the floor of the VM level above the walls
    if ((x0 == x1 || y0 == y1) && !(type >= Lot::DiagonalLongGable && type <= Lot::DiagonalShedHip)) {
        r.message = "a roof needs an area";
        return r;
    }
    if (!lot_.hasLevel(li)) {
        r.error = 0x03;
        r.message = "cannot put a roof there: no level above";
        return r;
    }
    const int W = int(lot_.width), H = int(lot_.height);
    if (std::min({x0, x1, y0, y1}) < 0 || std::max(x0, x1) > W || std::max(y0, y1) > H) {
        r.error = 0x02;
        r.message = "cannot put a roof there: out of the lot";
        return r;
    }
    // The diagonal roofs (10-14) are drawn from a drag across the diamond
    // (DiagonalRoofFromType); the base game's roofs.txt has no product for
    // them (the BV's diagonal roof styles 0x12-0x16 make them), so only
    // headless runs ask for one.
    const bool diagonal = type >= Lot::DiagonalLongGable && type <= Lot::DiagonalShedHip;
    if (!diagonal && (type < Lot::ShedGable || type > Lot::Mansard))
        type = Lot::Hip;
    if (diagonal) {
        // The diamond's corners (lot_roof's diagonalVector): all in the lot.
        const float vx = float(x1 - x0), vy = float(y1 - y0), yD = (vy - vx) / 2, dx = vy - yD;
        for (const auto& [cx, cy] : std::array<std::array<float, 2>, 4>{
                 {{float(x0), float(y0)}, {float(x0) + dx, float(y0) + dx}, {float(x1), float(y1)},
                  {float(x0) - yD, float(y0) + yD}}})
            if (cx < 0 || cy < 0 || cx > float(W) || cy > float(H)) {
                r.error = 0x02;
                r.message = "cannot put a roof there: out of the lot";
                return r;
            }
    }
    Geometry before = capture();
    Lot::Roof roof;
    for (const auto& o : lot_.roofs)
        roof.id = std::max(roof.id, o.id + 1);
    roof.id = std::max(roof.id, 1u);
    roof.x0 = float(x0), roof.y0 = float(y0), roof.x1 = float(x1), roof.y1 = float(y1);
    roof.level = roof.levelTo = li;
    roof.type = type;
    roof.pattern = lot_.roofs.empty() ? 0 : lot_.roofs.back().pattern;
    roof.angle = 0.5f;
    const auto attic = atticWalls(li);
    lot_.roofs.push_back(roof);
    syncAtticWalls(li, attic);
    r.changed = 1;
    changed();
    commit(std::move(before), 0);
    r.ok = true;
    r.message = "a roof of type " + std::to_string(type) + " over " + point(x0, y0) + "-" + point(x1, y1);
    return r;
}

BuildEditor::Result BuildEditor::placeDormer(int product, int level, int x, int y, int direction) {
    Result r;
    if (product < 6 || product > 11) {
        r.message = "not a dormer";
        return r;
    }
    const int width = (product - 6) % 3 + 1, deep = width + 2;
    // MakeDormer: the extents in x (iVar8) and y (iVar5) and the corner.
    int ex = deep, ey = width, x0 = x, y0 = y;
    switch (direction) {
    case 2: x0 -= deep; break;
    case 0: ex = width, ey = deep; break;
    case 4: ex = width, ey = deep, y0 -= deep; break;
    default: break;
    }
    const int li = level - 1;
    // The highest level a lot may have (FUN_00163808; lot level 4 = the
    // fifth story, as the tallest saved lots: inferred).
    constexpr int kTopLevel = 4;
    if (!lot_.hasLevel(li) || li + 1 > kTopLevel) {
        r.error = 0x06;
        r.message = "cannot place a dormer: no level above";
        return r;
    }
    const int W = int(lot_.width), H = int(lot_.height);
    if (x0 < 0 || y0 < 0 || x0 + ex > W || y0 + ey > H) {
        r.error = 0x02;
        r.message = "cannot place a dormer: out of the lot";
        return r;
    }
    // The four runs (FUN_001d5a5c, AreDormerWallsValid): no wall at their
    // grid points (0x08), and each section placeable (UserMayPlacePartition;
    // the support rule left out: a dormer stands on the roof).
    const std::array<std::array<int, 4>, 4> runs{
        {{x0, y0, x0, y0 + ey}, {x0 + ex, y0, x0 + ex, y0 + ey}, {x0, y0, x0 + ex, y0}, {x0, y0 + ey, x0 + ex, y0 + ey}}};
    auto wallAt = [&](int gx, int gy) {
        for (const auto& w : lot_.walls)
            if (w.a.level == li && !Lot::isFence(w.type) &&
                ((std::lround(w.a.x) == gx && std::lround(w.a.y) == gy) || (std::lround(w.b.x) == gx && std::lround(w.b.y) == gy)))
                return true;
        return false;
    };
    std::vector<std::array<int, 4>> todo;
    for (const auto& run : runs)
        for (const auto& s : wallSections(run[0], run[1], run[2], run[3])) {
            if (wallAt(s[0], s[1]) || wallAt(s[2], s[3])) {
                r.error = 0x08;
                r.message = "cannot place a dormer: a wall is in the way at " + point(s[0], s[1]);
                return r;
            }
            int code = 0;
            if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3], &code, false, true);
                !why.empty() && code != 0x48) {
                r.error = code;
                r.message = "cannot place a dormer: " + why;
                return r;
            }
            todo.push_back(s);
        }
    r.cost = int(todo.size()) * kWallSectionPrice;
    if (r.cost > funds(vm_)) {
        r.error = 0x16;
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    Geometry before = capture();
    if (!lot_.hasLevel(li + 1))
        addUpperLevel(lot_);
    const uint16_t blank = patternRef(lot_.wallPatterns, kDefaultWallPattern, 0);
    for (const auto& s : todo)
        if (addWall(lot_, li, s[0], s[1], s[2], s[3], kWallType, {blank, blank}))
            ++r.changed;
    // The roof (UserMayAddRoof type 4 or 5) with the gable ends' attic walls.
    Lot::Roof roof;
    for (const auto& o : lot_.roofs)
        roof.id = std::max(roof.id, o.id + 1);
    roof.id = std::max(roof.id, 1u);
    roof.x0 = float(x0), roof.y0 = float(y0), roof.x1 = float(x0 + ex), roof.y1 = float(y0 + ey);
    roof.level = roof.levelTo = li + 1;
    roof.type = product < 9 ? Lot::LongGable : Lot::Hip;
    roof.pattern = lot_.roofs.empty() ? 0 : lot_.roofs.back().pattern;
    roof.angle = lot_.roofs.empty() ? 0.5f : lot_.roofs.back().angle;
    const auto attic = atticWalls(li + 1);
    lot_.roofs.push_back(roof);
    syncAtticWalls(li + 1, attic);
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = "a " + std::to_string(width) + "-wide " + (product < 9 ? "gable" : "hip") + " dormer at " +
                point(x0, y0) + "-" + point(x0 + ex, y0 + ey) + " for " + money(r.cost);
    return r;
}

BuildEditor::Result BuildEditor::removeRoofs() {
    Result r;
    if (lot_.roofs.empty()) {
        r.ok = true;
        r.message = "no roofs";
        return r;
    }
    Geometry before = capture();
    std::set<int> levels;
    for (const auto& f : lot_.roofs)
        levels.insert(f.level);
    std::map<int, std::vector<std::array<int, 4>>> attic;
    for (int l : levels)
        attic[l] = atticWalls(l);
    r.changed = int(lot_.roofs.size());
    lot_.roofs.clear();
    autoRoofs_.clear();
    for (int l : levels)
        syncAtticWalls(l, attic[l]);
    changed();
    commit(std::move(before), 0);
    r.ok = true;
    r.message = std::to_string(r.changed) + " roofs removed";
    return r;
}

BuildEditor::Result BuildEditor::autoRoof(int tool, bool preserve) {
    Result r;
    const int W = int(lot_.width), H = int(lot_.height);
    auto it0 = lot_.roomLayers.find(0x0B);
    const auto& rooms = it0 != lot_.roomLayers.end() && it0->second.size() == lot_.rooms.size() ? it0->second : lot_.rooms;
    if (rooms.empty()) {
        r.message = "no rooms";
        return r;
    }
    Geometry before = capture();
    // The roofs the tool made before go (not with "preserve").
    std::set<int> touched;
    if (!preserve && !autoRoofs_.empty()) {
        std::map<int, std::vector<std::array<int, 4>>> attic;
        for (const auto& f : lot_.roofs)
            if (autoRoofs_.count(f.id) && !attic.count(f.level))
                attic[f.level] = atticWalls(f.level);
        std::erase_if(lot_.roofs, [&](const Lot::Roof& f) { return autoRoofs_.count(f.id) != 0; });
        for (auto& [l, a] : attic)
            syncAtticWalls(l, a);
        autoRoofs_.clear();
    }
    auto inside = [&](int lotLevel, int x, int y) {
        if (!lot_.hasLevel(lotLevel))
            return false;
        for (uint16_t q : rooms[(size_t(lotLevel - lot_.minLevel) * lot_.width + size_t(x)) * lot_.height + size_t(y)])
            if (q && q != 0xFFFF)
                return true;
        return false;
    };
    auto roofed = [&](int lotLevel, int x, int y) {
        const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
        for (const auto& f : lot_.roofs)
            if (f.level == lotLevel && cx > std::min(f.x0, f.x1) && cx < std::max(f.x0, f.x1) &&
                cy > std::min(f.y0, f.y1) && cy < std::max(f.y0, f.y1))
                return true;
        return false;
    };
    int roofsMade = 0, slabTiles = 0;
    const uint16_t slab = tool == 5 ? patternRef(lot_.floorPatterns, "ccc26098", 1) : 0;
    for (int below = std::max(0, lot_.minLevel); lot_.hasLevel(below + 1); ++below) {
        const int above = below + 1;
        // AppendRoomTilesIfValid: inside below, not inside above, no floor above, no roof.
        std::vector<uint8_t> grid(size_t(W) * size_t(H));
        bool any = false;
        for (int x = 0; x < W; ++x)
            for (int y = 0; y < H; ++y) {
                if (!inside(below, x, y) || inside(above, x, y) || roofed(above, x, y))
                    continue;
                bool floor = false;
                for (uint16_t q : lot_.floor(above, uint32_t(x), uint32_t(y)))
                    floor |= q != 0;
                if (floor)
                    continue;
                grid[size_t(y) * size_t(W) + size_t(x)] = 1, any = true;
            }
        if (!any)
            continue;
        if (tool == 5) {
            // The tiled slab: the roof floor pattern on the marked tiles.
            for (int x = 0; x < W; ++x)
                for (int y = 0; y < H; ++y)
                    if (grid[size_t(y) * size_t(W) + size_t(x)]) {
                        for (int q = 0; q < 4; ++q)
                            setFloor(lot_, above, x, y, q, slab);
                        ++slabTiles;
                    }
            continue;
        }
        const auto done = autoRoofRects(grid, W, H);
        const auto attic = atticWalls(above);
        touched.insert(above);
        for (const auto& q : done) {
            const int w = q[2] - q[0], h = q[3] - q[1];
            if (w <= 0 || h <= 0)
                continue;
            uint32_t type = Lot::Hip;
            switch (tool) {
            case 1: type = Lot::Hip; break;
            case 2: type = Lot::LongGable; break;
            case 3: type = h <= w ? Lot::Hip : Lot::LongGable; break;
            case 4: type = h < w ? Lot::ShortGable : Lot::LongGable; break;
            default: break;
            }
            Lot::Roof roof;
            for (const auto& o : lot_.roofs)
                roof.id = std::max(roof.id, o.id + 1);
            roof.id = std::max(roof.id, 1u);
            roof.x0 = float(q[0]), roof.y0 = float(q[1]), roof.x1 = float(q[2]), roof.y1 = float(q[3]);
            roof.level = roof.levelTo = above;
            roof.type = type;
            roof.pattern = lot_.roofs.empty() ? 0 : lot_.roofs.back().pattern;
            roof.angle = 0.5f;
            lot_.roofs.push_back(roof);
            autoRoofs_.insert(roof.id);
            ++roofsMade;
        }
        syncAtticWalls(above, attic);
    }
    r.changed = roofsMade + slabTiles;
    if (!r.changed && preserve) {
        restore(before);
        r.ok = true;
        r.message = "nothing to roof";
        return r;
    }
    changed();
    commit(std::move(before), 0);
    r.ok = true;
    r.message = tool == 5 ? std::to_string(slabTiles) + " tiles of roof slab"
                          : std::to_string(roofsMade) + " auto roofs of tool " + std::to_string(tool);
    return r;
}

BuildEditor::Result BuildEditor::setRoofPattern(uint32_t guid) {
    Result r;
    Geometry before = capture();
    for (auto& f : lot_.roofs)
        if (f.pattern != guid)
            f.pattern = guid, ++r.changed;
    r.ok = true;
    if (!r.changed) {
        r.message = lot_.roofs.empty() ? "no roofs" : "the roofs have that pattern";
        return r;
    }
    ++revision_;
    commit(std::move(before), 0);
    r.message = std::to_string(r.changed) + " roofs take the pattern";
    return r;
}

BuildEditor::Result BuildEditor::deleteRoof(int level, float x, float y) {
    Result r;
    const int li = level;
    // The last roof of the level over the point (the one drawn on top).
    for (size_t i = lot_.roofs.size(); i-- > 0;) {
        const Lot::Roof& f = lot_.roofs[i];
        if (f.level != li || x < std::min(f.x0, f.x1) || x > std::max(f.x0, f.x1) || y < std::min(f.y0, f.y1) ||
            y > std::max(f.y0, f.y1))
            continue;
        Geometry before = capture();
        const auto attic = atticWalls(li);
        lot_.roofs.erase(lot_.roofs.begin() + long(i));
        syncAtticWalls(li, attic);
        r.changed = 1;
        changed();
        commit(std::move(before), 0);
        r.ok = true;
        r.message = "roof deleted";
        return r;
    }
    r.ok = true;
    r.message = "no roof there";
    return r;
}

bool BuildEditor::terrainFree(int x, int y) const {
    const int W = int(lot_.width), H = int(lot_.height);
    if (x < 0 || y < 0 || x > W || y > H)
        return false;
    for (int tx = x - 1; tx <= x; ++tx)
        for (int ty = y - 1; ty <= y; ++ty)
            if (tx >= 0 && ty >= 0 && tx < W && ty < H) {
                for (uint16_t q : lot_.floor(0, uint32_t(tx), uint32_t(ty)))
                    if (q)
                        return false;
                if (foundationTile(tx, ty))
                    return false;
            }
    for (const auto& w : lot_.walls)
        if (w.a.level == 0 && ((std::lround(w.a.x) == x && std::lround(w.a.y) == y) ||
                               (std::lround(w.b.x) == x && std::lround(w.b.y) == y)))
            return false;
    return true;
}

void BuildEditor::beginTerrainStroke(TerrainOp op, float x, float y) {
    Stroke s;
    s.op = op;
    s.before = capture();
    s.height = lot_.elevationAt(0, std::clamp(x, 0.0f, float(lot_.width)), std::clamp(y, 0.0f, float(lot_.height)));
    stroke_ = std::move(s);
}

BuildEditor::Result BuildEditor::applyTerrainBrush(float cx, float cy, int size, const std::vector<uint8_t>& profile,
                                                   const std::string& paint) {
    Result r;
    if (!stroke_)
        beginTerrainStroke(TerrainOp::Raise, cx, cy);
    Stroke& st = *stroke_;
    const int W = int(lot_.width), H = int(lot_.height);
    if (!lot_.hasLevel(0) || lot_.elevations.empty() || profile.size() < 32 * 32) {
        r.message = "no terrain";
        return r;
    }
    // Only exposed terrain of the ground (0x8B).
    const int tx = int(std::floor(cx)), ty = int(std::floor(cy));
    if (tx < 0 || ty < 0 || tx >= W || ty >= H || !terrainFree(tx, ty) || !terrainFree(tx + 1, ty + 1)) {
        r.error = 0x8B;
        r.message = "only exposed terrain can be edited";
        return r;
    }
    const float width = 2.0f + (10.0f - 2.0f) * 0.5f * float(std::clamp(size, 0, 2));
    auto strength = [&](float px, float py) -> int {
        const float u = (px - cx) / width + 0.5f, v = (py - cy) / width + 0.5f;
        if (u < 0 || v < 0 || u >= 1 || v >= 1)
            return 0;
        return profile[size_t(int(v * 32)) * 32 + size_t(int(u * 32))];
    };
    if (st.op == TerrainOp::Paint || st.op == TerrainOp::Erase) {
        // The masks: 4 points a tile, (maskWidth x maskHeight), row by row.
        if (!lot_.maskWidth || !lot_.maskHeight) {
            lot_.maskWidth = uint32_t(W) * 4 + 1, lot_.maskHeight = uint32_t(H) * 4 + 1;
            for (auto& p : lot_.paints)
                p.mask.assign(size_t(lot_.maskWidth) * lot_.maskHeight, 0);
        }
        size_t target = lot_.paints.size();
        for (size_t i = 0; i < lot_.paints.size(); ++i)
            if (lot_.paints[i].texture == paint)
                target = i;
        if (st.op == TerrainOp::Paint && target == lot_.paints.size() && !paint.empty())
            lot_.paints.push_back({paint, std::vector<uint8_t>(size_t(lot_.maskWidth) * lot_.maskHeight, 0)});
        const float sx = float(W) / float(lot_.maskWidth - 1), sy = float(H) / float(lot_.maskHeight - 1);
        for (uint32_t my = 0; my < lot_.maskHeight; ++my)
            for (uint32_t mx = 0; mx < lot_.maskWidth; ++mx) {
                const int p = strength(float(mx) * sx, float(my) * sy) / 4; // a quarter a tick (inferred)
                if (!p)
                    continue;
                const size_t at = size_t(my) * lot_.maskWidth + mx;
                for (size_t i = 0; i < lot_.paints.size(); ++i) {
                    uint8_t& m = lot_.paints[i].mask[at];
                    const int v = st.op == TerrainOp::Paint && i == target ? std::min(255, int(m) + p) : std::max(0, int(m) - p);
                    if (v != m)
                        m = uint8_t(v), ++r.changed;
                }
            }
        st.changed += r.changed;
        r.ok = true;
        if (r.changed) {
            ++revision_;
        }
        r.message = std::to_string(r.changed) + " paint mask points";
        return r;
    }
    constexpr float kStep = 0.00625f;
    const size_t base = size_t(0 - lot_.minLevel) * size_t(W + 1) * size_t(H + 1);
    float moved = 0;
    std::vector<std::pair<size_t, float>> changes;
    for (int x = 0; x <= W; ++x)
        for (int y = 0; y <= H; ++y) {
            const int p = strength(float(x), float(y));
            if (!p || !terrainFree(x, y))
                continue;
            const size_t i = base + size_t(x) * size_t(H + 1) + size_t(y);
            const float h = lot_.elevations[i];
            float n = h;
            switch (st.op) {
            case TerrainOp::Raise: n = std::ceil((h + float(p) * 0.0002f) / kStep - 1e-4f) * kStep; break;
            case TerrainOp::Lower: n = std::floor((h - float(p) * 0.00002f) / kStep + 1e-4f) * kStep; break;
            case TerrainOp::Water: // WaterBrushFn: the context's strength 1e-5, threshold 0
                n = std::floor((h - float(p) * 0.00001f) / kStep + 1e-4f) * kStep;
                st.dug.insert({x, y});
                break;
            case TerrainOp::Flatten: n = st.height; break;
            case TerrainOp::Smooth: {
                float sum = 0;
                int count = 0;
                for (const auto& [dx, dy] : std::array<std::array<int, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}})
                    if (x + dx >= 0 && y + dy >= 0 && x + dx <= W && y + dy <= H)
                        sum += lot_.elevations[base + size_t(x + dx) * size_t(H + 1) + size_t(y + dy)], ++count;
                if (count)
                    n = std::round((h + (sum / float(count) - h) * float(p) / 255.0f) / kStep) * kStep;
                break;
            }
            default: break;
            }
            if (std::fabs(n - h) < 1e-6f)
                continue;
            changes.push_back({i, n});
            moved += std::fabs(n - h);
        }
    // UpdateFundsForHeightChange: the stroke's total change, rounded, x §1;
    // what it adds is paid now, refused when the funds are short.
    const int owed = int(std::lround((st.moved + moved) * 1.0f)) - st.paid;
    if (owed > funds(vm_)) {
        r.message = "not enough money (" + money(owed) + ")";
        return r;
    }
    for (const auto& [i, n] : changes) {
        // The stories above move with the terrain (ChangeFloorElevationCoupled).
        const float d = n - lot_.elevations[i];
        lot_.elevations[i] = n;
        for (int k = 1; lot_.hasLevel(k); ++k)
            lot_.elevations[i + size_t(k) * size_t(W + 1) * size_t(H + 1)] += d;
    }
    if (st.op == TerrainOp::Water)
        adjustWater(st.dug);
    st.moved += moved;
    st.paid += owed;
    st.changed += int(changes.size());
    addFunds(vm_, -owed);
    r.ok = true;
    r.cost = owed;
    r.changed = int(changes.size());
    if (r.changed)
        ++revision_;
    r.message = std::to_string(r.changed) + " terrain points moved" + (owed ? " for " + money(owed) : "");
    return r;
}

void BuildEditor::adjustWater(const std::set<std::pair<int, int>>& dug) {
    const int W = int(lot_.width), H = int(lot_.height);
    const size_t rowW = size_t(W + 1);
    auto ground = [&](int x, int y) { return lot_.elevation(0, x, y); };
    if (lot_.water.size() != rowW * size_t(H + 1)) {
        lot_.water.assign(rowW * size_t(H + 1), 0.0f);
        for (int y = 0; y <= H; ++y)
            for (int x = 0; x <= W; ++x)
                lot_.water[size_t(y) * rowW + size_t(x)] = ground(x, y) - 1.0f;
    }
    auto water = [&](int x, int y) -> float& { return lot_.water[size_t(y) * rowW + size_t(x)]; };
    auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x <= W && y <= H; };
    static const int nx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, ny[8] = {0, -1, -1, -1, 0, 1, 1, 1};
    // The change array: the dug points and the water connected to them
    // (ExpandWaterHtChangeArrayToIncludeConnectedPreExistingWater).
    std::set<std::pair<int, int>> change(dug.begin(), dug.end());
    std::vector<std::pair<int, int>> todo(dug.begin(), dug.end());
    while (!todo.empty()) {
        const auto [x, y] = todo.back();
        todo.pop_back();
        for (int k = 0; k < 8; ++k) {
            const int ax = x + nx[k], ay = y + ny[k];
            if (inside(ax, ay) && !change.count({ax, ay}) && water(ax, ay) > ground(ax, ay))
                change.insert({ax, ay}), todo.push_back({ax, ay});
        }
    }
    // ChooseWaterElevationsForPondWithVertex: each 8-connected group, its
    // water 0.25 under the lowest ground round it.
    std::set<std::pair<int, int>> seen, pond;
    for (const auto& start : change) {
        if (seen.count(start))
            continue;
        std::vector<std::pair<int, int>> group{start};
        seen.insert(start);
        float rim = std::numeric_limits<float>::max();
        for (size_t i = 0; i < group.size(); ++i)
            for (int k = 0; k < 8; ++k) {
                const int ax = group[i].first + nx[k], ay = group[i].second + ny[k];
                if (!inside(ax, ay))
                    continue;
                if (change.count({ax, ay})) {
                    if (seen.insert({ax, ay}).second)
                        group.push_back({ax, ay});
                } else {
                    rim = std::min(rim, ground(ax, ay));
                }
            }
        if (rim == std::numeric_limits<float>::max())
            continue;
        const float level = rim - 0.25f;
        for (const auto& [x, y] : group) {
            water(x, y) = ground(x, y) - level > 0.25f ? ground(x, y) - 1.0f : level;
            if (water(x, y) > ground(x, y))
                pond.insert({x, y});
        }
    }
    // AdjustWaterElevationsAtNonPondVertices.
    for (const auto& [x, y] : change) {
        if (pond.count({x, y}))
            continue;
        float w = ground(x, y) - 1.0f;
        for (int k = 0; k < 8; ++k) {
            const int ax = x + nx[k], ay = y + ny[k];
            if (inside(ax, ay) && pond.count({ax, ay})) {
                w = water(ax, ay) - 0.1f;
                break;
            }
        }
        water(x, y) = w;
    }
}

BuildEditor::Result BuildEditor::terrainStroke(TerrainOp op, float x, float y, int ticks, int size,
                                               const std::vector<uint8_t>& profile, const std::string& paint) {
    beginTerrainStroke(op, x, y);
    Result last;
    for (int i = 0; i < std::max(ticks, 1); ++i) {
        last = applyTerrainBrush(x, y, size, profile, paint);
        if (!last.ok)
            break;
    }
    Result r = endTerrainStroke();
    if (!last.ok && !r.changed) {
        last.ok = false;
        return last;
    }
    return r;
}

BuildEditor::Result BuildEditor::endTerrainStroke() {
    Result r;
    if (!stroke_) {
        r.message = "no stroke";
        return r;
    }
    Stroke st = std::move(*stroke_);
    stroke_.reset();
    r.ok = true;
    r.cost = st.paid;
    r.changed = st.changed;
    if (!st.changed) {
        r.message = "nothing changed";
        return r;
    }
    changed();
    commit(std::move(st.before), st.paid);
    r.message = std::to_string(st.changed) + " terrain changes for " + money(st.paid);
    return r;
}

bool BuildEditor::poolTile(int x, int y) const {
    for (const auto& p : lot_.pools)
        if (p.level == 0 && float(x) >= p.x - 0.01f && float(x) < p.x + float(p.width) - 0.01f &&
            float(y) >= p.y - 0.01f && float(y) < p.y + float(p.height) - 0.01f)
            return true;
    return false;
}

void BuildEditor::poolWalls() {
    if (!lot_.hasLevel(-1))
        return;
    const int W = int(lot_.width), H = int(lot_.height);
    std::vector<std::array<int, 4>> want;
    for (int x = 0; x < W; ++x)
        for (int y = 0; y < H; ++y) {
            if (!poolTile(x, y))
                continue;
            if (!poolTile(x - 1, y))
                want.push_back({x, y, x, y + 1});
            if (!poolTile(x + 1, y))
                want.push_back({x + 1, y, x + 1, y + 1});
            if (!poolTile(x, y - 1))
                want.push_back({x, y, x + 1, y});
            if (!poolTile(x, y + 1))
                want.push_back({x, y + 1, x + 1, y + 1});
        }
    std::vector<std::array<int, 4>> gone;
    for (const auto& w : lot_.walls)
        if (w.type == Lot::Pool && w.a.level == -1 &&
            std::none_of(want.begin(), want.end(), [&](const auto& s) { return sameSection(s, w); }))
            gone.push_back({int(std::lround(w.a.x)), int(std::lround(w.a.y)), int(std::lround(w.b.x)),
                            int(std::lround(w.b.y))});
    for (const auto& s : gone)
        removeWall(lot_, -1, s[0], s[1], s[2], s[3], &merges_);
    const uint16_t lip = patternRef(lot_.wallPatterns, "wall_poolroundedlipblue", 0);
    for (const auto& s : want)
        if (!findWall(lot_, -1, s[0], s[1], s[2], s[3]))
            addWall(lot_, -1, s[0], s[1], s[2], s[3], Lot::Pool, {lip, lip});
}

BuildEditor::Result BuildEditor::drawPool(int x0, int y0, int x1, int y1) {
    Result r;
    auto fail = [&](int code, const std::string& why) {
        r.error = code;
        r.message = "cannot dig a pool: " + why;
        return r;
    };
    if (!lot_.hasLevel(0) || lot_.elevations.empty())
        return fail(0x8A, "no terrain");
    const int W = int(lot_.width), H = int(lot_.height);
    const int ax = std::max(0, std::min(x0, x1)), bx = std::min(W - 1, std::max(x0, x1));
    const int ay = std::max(0, std::min(y0, y1)), by = std::min(H - 1, std::max(y0, y1));
    // Tiles of a pool already there are skipped: a rectangle over or next to
    // a pool extends it (the deck tool's CanPlaceTile lets a pool tile go
    // beside a pool), which gives pools that are not rectangles; the pool
    // keeps the ground height it has (inferred).
    std::optional<float> poolHeight;
    for (int x = ax - 1; x <= bx + 1 && !poolHeight; ++x)
        for (int y = ay - 1; y <= by + 1 && !poolHeight; ++y)
            if (poolTile(x, y))
                poolHeight = lot_.elevation(0, x, y);
    std::vector<std::array<int, 2>> fresh;
    for (int x = ax; x <= bx; ++x)
        for (int y = ay; y <= by; ++y) {
            if (poolTile(x, y))
                continue;
            fresh.push_back({x, y});
            if (foundationTile(x, y))
                return fail(0x8A, "pools only go in the terrain");
            for (uint16_t q : lot_.floor(0, uint32_t(x), uint32_t(y)))
                if (q)
                    return fail(0x8A, "pools only go in the terrain");
            for (int cx = x; cx <= x + 1; ++cx)
                for (int cy = y; cy <= y + 1; ++cy)
                    if (lot_.waterLevel(cx, cy) > lot_.elevation(0, cx, cy))
                        return fail(0x1E, "water at " + point(x, y));
            for (ObjectId id : vm_.objectsOnTile({x, y, 1}))
                if (const Object* o = vm_.object(id); o && (o->person || !(o->data[objdata::PlacementFlags] & 0x200)))
                    return fail(o->person ? 0x4B : 0x0C, "something is in the way at " + point(x, y));
        }
    for (const auto& w : lot_.walls) {
        if (w.a.level != 0)
            continue;
        const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
        if (mx > float(ax) + 0.01f && mx < float(bx + 1) - 0.01f && my > float(ay) + 0.01f && my < float(by + 1) - 0.01f)
            return fail(0x0C, "a wall is in the way");
    }
    const int tiles = int(fresh.size());
    if (!tiles) {
        r.ok = true;
        r.message = "the pool is there already";
        return r;
    }
    r.cost = tiles * kPoolTilePrice;
    if (r.cost > funds(vm_)) {
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    Geometry before = capture();
    const float height = poolHeight.value_or(lot_.elevation(0, std::clamp(x0, 0, W), std::clamp(y0, 0, H)));
    std::vector<std::array<int, 2>> points;
    for (const auto& [x, y] : fresh)
        for (int cx = x; cx <= x + 1; ++cx)
            for (int cy = y; cy <= y + 1; ++cy)
                points.push_back({cx, cy});
    std::vector<std::array<int, 4>> lips; // the pool's own edge walls do not hold the ground
    for (const auto& w : lot_.walls)
        if (w.type == Lot::Pool)
            lips.push_back({int(std::lround(w.a.x)), int(std::lround(w.a.y)), int(std::lround(w.b.x)), int(std::lround(w.b.y))});
    if (!levelPoints(points, height, lips)) {
        restore(before);
        return fail(0x81, "the ground cannot be levelled");
    }
    addLowerLevel(lot_);
    const size_t plane = size_t(W + 1) * size_t(H + 1);
    for (const auto& [cx, cy] : points)
        lot_.elevations[size_t(-1 - lot_.minLevel) * plane + size_t(cx) * size_t(H + 1) + size_t(cy)] = height - 3.0f;
    const uint16_t lip = patternRef(lot_.floorPatterns, "floor_poolroundedlipblue", 1);
    for (const auto& [x, y] : fresh)
        for (int q = 0; q < 4; ++q)
            setFloor(lot_, -1, x, y, q, lip), setFloorRotation(lot_, -1, x, y, q, 1);
    // The new tiles as POOL rectangles (rows of the same span merged), so
    // the water of a pool that is not a rectangle covers just its tiles.
    std::vector<uint8_t> grid(size_t(W) * size_t(H), 0);
    for (const auto& [x, y] : fresh)
        grid[size_t(y) * size_t(W) + size_t(x)] = 1;
    uint32_t id = 1;
    for (const auto& p : lot_.pools)
        id = std::max(id, p.id + 1);
    // Rows of runs; a run with the span of a rectangle open on the row
    // above extends it (no overlaps, unlike the auto-roof's).
    std::vector<std::array<int, 4>> rects, open;
    for (int y = 0; y <= H; ++y) {
        std::vector<std::array<int, 4>> next;
        for (int x = 0; y < H && x < W;) {
            if (!grid[size_t(y) * size_t(W) + size_t(x)]) {
                ++x;
                continue;
            }
            int e = x;
            while (e < W && grid[size_t(y) * size_t(W) + size_t(e)])
                ++e;
            auto m = std::find_if(open.begin(), open.end(), [&](const auto& o) { return o[0] == x && o[2] == e; });
            if (m != open.end()) {
                next.push_back({x, (*m)[1], e, y + 1});
                open.erase(m);
            } else {
                next.push_back({x, y, e, y + 1});
            }
            x = e;
        }
        rects.insert(rects.end(), open.begin(), open.end());
        open = std::move(next);
    }
    for (const auto& q : rects) {
        Lot::SwimmingPool pool;
        pool.id = id++;
        pool.x = float(q[0]), pool.y = float(q[1]);
        pool.level = 0;
        pool.width = uint32_t(q[2] - q[0]), pool.height = uint32_t(q[3] - q[1]);
        pool.depth = 0.25f;
        lot_.pools.push_back(pool);
    }
    poolWalls();
    r.changed = tiles;
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = "a pool of " + std::to_string(tiles) + " tiles for " + money(r.cost);
    return r;
}

BuildEditor::Result BuildEditor::deletePool(int x, int y) {
    Result r;
    for (size_t i = 0; i < lot_.pools.size(); ++i) {
        const Lot::SwimmingPool p = lot_.pools[i];
        if (p.level != 0 || float(x) < p.x || float(x) >= p.x + float(p.width) || float(y) < p.y ||
            float(y) >= p.y + float(p.height))
            continue;
        const int px = int(std::lround(p.x)), py = int(std::lround(p.y)), pw = int(p.width), ph = int(p.height);
        for (const auto& [id, o] : vm_.objects()) {
            if (!o.position.inWorld())
                continue;
            const TilePos t = VM::tileOf(o);
            const bool in = t.x >= px && t.x < px + pw && t.y >= py && t.y < py + ph;
            const bool edge = t.x >= px - 1 && t.x <= px + pw && t.y >= py - 1 && t.y <= py + ph;
            if (o.person && in && t.level <= 1) {
                r.error = 0x2D, r.message = "cannot delete the pool: a Sim is in it";
                return r;
            }
            if (!o.person && o.definition && edge &&
                (o.definition->fileName.find("Pool Ladder") != std::string::npos ||
                 o.definition->fileName.find("Diving Board") != std::string::npos)) {
                r.error = 0x2E, r.message = "cannot delete the pool: its ladder or diving board first";
                return r;
            }
        }
        Geometry before = capture();
        // The whole pool: every POOL rectangle joined to this one by an edge
        // (a pool laid in several drags).
        std::set<size_t> pool{i};
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t j = 0; j < lot_.pools.size(); ++j) {
                const auto& b = lot_.pools[j];
                if (pool.count(j) || b.level != 0)
                    continue;
                for (size_t k : pool) {
                    const auto& a = lot_.pools[k];
                    const bool xs = a.x <= b.x + float(b.width) && b.x <= a.x + float(a.width);
                    const bool ys = a.y <= b.y + float(b.height) && b.y <= a.y + float(a.height);
                    const bool xo = a.x < b.x + float(b.width) && b.x < a.x + float(a.width);
                    const bool yo = a.y < b.y + float(b.height) && b.y < a.y + float(a.height);
                    if ((xs && yo) || (ys && xo)) {
                        pool.insert(j), grew = true;
                        break;
                    }
                }
            }
        }
        std::vector<Lot::SwimmingPool> removed;
        for (auto k = pool.rbegin(); k != pool.rend(); ++k) {
            removed.push_back(lot_.pools[*k]);
            lot_.pools.erase(lot_.pools.begin() + long(*k));
        }
        r.changed = 0;
        for (const auto& q : removed) {
            const int qx = int(std::lround(q.x)), qy = int(std::lround(q.y));
            for (int tx = qx; tx < qx + int(q.width); ++tx)
                for (int ty = qy; ty < qy + int(q.height); ++ty)
                    if (!poolTile(tx, ty)) {
                        ++r.changed;
                        for (int qq = 0; qq < 4; ++qq)
                            setFloor(lot_, -1, tx, ty, qq, 0);
                    }
        }
        poolWalls();
        r.cost = -r.changed * int(float(kPoolTilePrice) * kBuildRefundRate);
        addFunds(vm_, -r.cost);
        changed();
        commit(std::move(before), r.cost);
        r.ok = true;
        r.message = "pool deleted for " + money(-r.cost) + " back";
        return r;
    }
    r.ok = true;
    r.message = "no pool there";
    return r;
}

bool BuildEditor::floorable(int level, int x, int y) const {
    const int li = level - 1;
    if (li <= 0)
        return true;
    const int W = int(lot_.width), H = int(lot_.height);
    // Inferred from IsFloorable (BV 0x16a990): HasSupport and
    // SupportedUnderneath are taken as a floor on the tile below or a
    // load-bearing wall under one of its edges.
    auto supported = [&](int tx, int ty) {
        if (tx < 0 || ty < 0 || tx >= W || ty >= H)
            return false;
        for (uint16_t q : lot_.floor(li - 1, uint32_t(tx), uint32_t(ty)))
            if (q)
                return true;
        for (const auto& w : lot_.walls) {
            if (w.a.level != li - 1 || Lot::isFence(w.type) || w.type == Lot::Attic)
                continue;
            const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
            if (mx >= float(tx) - 0.01f && mx <= float(tx + 1) + 0.01f && my >= float(ty) - 0.01f &&
                my <= float(ty + 1) + 0.01f)
                return true;
        }
        return false;
    };
    if (supported(x, y))
        return true;
    // A neighbour supported from below, or, next to a floor, a supported
    // tile within two (not on the far corners: |dx| + |dy| < 4).
    bool nextToFloor = false;
    for (const auto& [dx, dy] : std::array<std::array<int, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
        if (supported(x + dx, y + dy))
            return true;
        if (x + dx >= 0 && y + dy >= 0 && x + dx < W && y + dy < H)
            for (uint16_t q : lot_.floor(li, uint32_t(x + dx), uint32_t(y + dy)))
                nextToFloor |= q != 0;
    }
    if (!nextToFloor)
        return false;
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy)
            if (std::abs(dx) + std::abs(dy) < 4 && supported(x + dx, y + dy))
                return true;
    return false;
}

int BuildEditor::clickQuarters(int level, float px, float py) const {
    const int x = int(std::floor(px)), y = int(std::floor(py));
    const float fx = px - float(x), fy = py - float(y);
    // "/" separates west+north from east+south, "\" north+east from
    // south+west (lot_edit.hpp quarters: bit 1 west, 2 north, 4 east, 8 south).
    if (findWall(lot_, level - 1, x, y, x + 1, y + 1))
        return fy > fx ? 0x3 : 0xC;
    if (findWall(lot_, level - 1, x + 1, y, x, y + 1))
        return fx + fy > 1 ? 0x6 : 0x9;
    return 0xF;
}

BuildEditor::Result BuildEditor::paintFloor(int level, int x0, int y0, int x1, int y1, const BuildPattern* pattern,
                                            int quarters, int rotation) {
    Result r;
    const int li = level - 1;
    if (!lot_.hasLevel(li)) {
        r.message = "no such level";
        return r;
    }
    if (rotation != 1 && rotation != 2 && rotation != 4 && rotation != 8)
        rotation = 1;
    const int W = int(lot_.width), H = int(lot_.height);
    const int ax = std::max(0, std::min(x0, x1)), bx = std::min(W - 1, std::max(x0, x1));
    const int ay = std::max(0, std::min(y0, y1)), by = std::min(H - 1, std::max(y0, y1));
    Geometry before = capture();
    // Painting on the ground levels the terrain under the rectangle to the
    // first tile's height first (the floor tool's release: LevelRect;
    // inferred: the mean of that tile's corners). Points a wall holds stay.
    if (pattern && li == 0 && !lot_.elevations.empty()) {
        const int sx = std::clamp(x0, 0, W - 1), sy = std::clamp(y0, 0, H - 1);
        const float height = (lot_.elevation(0, sx, sy) + lot_.elevation(0, sx + 1, sy) +
                              lot_.elevation(0, sx, sy + 1) + lot_.elevation(0, sx + 1, sy + 1)) / 4;
        const size_t base = size_t(0 - lot_.minLevel) * size_t(W + 1) * size_t(H + 1);
        for (int x = ax; x <= bx + 1; ++x)
            for (int y = ay; y <= by + 1; ++y) {
                bool held = false;
                for (const auto& w : lot_.walls)
                    held |= w.a.level == 0 && ((std::lround(w.a.x) == x && std::lround(w.a.y) == y) ||
                                               (std::lround(w.b.x) == x && std::lround(w.b.y) == y));
                if (!held)
                    lot_.elevations[base + size_t(x) * size_t(H + 1) + size_t(y)] = height;
            }
    }
    std::map<uint16_t, std::string> names = lot_.floorPatterns;
    const uint16_t ref = pattern ? patternRef(names, pattern->lotName(), 1) : 0;
    // cTSUserToolUndoableFloorPattern::ChangeTile: per quarter whose pattern
    // or rotation changes, + new price (placing) or - refund of the old
    // (erasing), a quarter each.
    float cost = 0;
    std::vector<std::pair<std::array<int, 3>, uint16_t>> changes;
    int refused = 0;
    for (int x = ax; x <= bx; ++x)
        for (int y = ay; y <= by; ++y) {
            // UserMayPlaceFloorPattern: terrain not flat on the ground
            // (0x53), a tile above it that is not floorable (0x52).
            if (pattern && li == 0 &&
                (std::fabs(lot_.elevation(0, x, y) - lot_.elevation(0, x + 1, y + 1)) > 0.01f ||
                 std::fabs(lot_.elevation(0, x + 1, y) - lot_.elevation(0, x, y + 1)) > 0.01f ||
                 std::fabs(lot_.elevation(0, x, y) - lot_.elevation(0, x + 1, y)) > 0.01f)) {
                refused = 0x53;
                continue;
            }
            if (pattern && !floorable(level, x, y)) {
                refused = 0x52;
                continue;
            }
            const auto& q = lot_.floor(li, uint32_t(x), uint32_t(y));
            for (int k = 0; k < 4; ++k) {
                if (!(quarters & (1 << k)))
                    continue;
                const bool sameRotation = !pattern || floorRotation(lot_, li, x, y, k) == rotation;
                if (q[size_t(k)] == ref && sameRotation)
                    continue;
                const std::string old = q[size_t(k)] ? (names.count(q[size_t(k)]) ? names[q[size_t(k)]] : "") : "";
                if (li == 0 && street(old))
                    continue; // the street stays
                // FloorPatternCost (BV 0x9a3e50): floor(price x (refund ? 0.8 : 1)).
                if (pattern) {
                    cost += float(pattern->price) * 0.25f;
                } else if (q[size_t(k)]) {
                    cost -= std::floor(float(priceOf(old, true)) * kBuildRefundRate) * 0.25f;
                }
                changes.push_back({{x, y, k}, ref});
            }
        }
    if (changes.empty()) {
        restore(before);
        r.ok = !refused;
        r.error = refused;
        r.message = refused == 0x53 ? "the ground is not flat" : refused == 0x52 ? "nothing holds the floor up"
                                                                                : "nothing to change";
        return r;
    }
    r.cost = int(std::lround(cost));
    if (r.cost > funds(vm_)) {
        restore(before);
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    lot_.floorPatterns = names;
    if (pattern)
        prices_[pattern->lotName()] = pattern->price;
    for (const auto& [t, v] : changes) {
        setFloor(lot_, li, t[0], t[1], t[2], v);
        setFloorRotation(lot_, li, t[0], t[1], t[2], uint8_t(v ? rotation : 1));
    }
    r.changed = int(changes.size());
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " quarter tiles " + (pattern ? "floored with " + pattern->name : "cleared") +
                (r.cost >= 0 ? " for " + money(r.cost) : ", " + money(-r.cost) + " back");
    return r;
}

// ChangeWallPattern (BV 0x394914): + the new pattern's price a face;
// taking a covering off (back to the style's "blank") gives back
// int(old price x 0.8); covering over another gives nothing back.
BuildEditor::Result BuildEditor::cover(std::vector<std::pair<uint32_t, int>> faces, const BuildPattern* pattern,
                                       const std::string& what) {
    Result r;
    Geometry before = capture();
    std::map<uint16_t, std::string> names = lot_.wallPatterns;
    const uint16_t ref = patternRef(names, pattern ? pattern->lotName() : kDefaultWallPattern, 0);
    float cost = 0;
    std::erase_if(faces, [&](const std::pair<uint32_t, int>& f) {
        for (const auto& w : lot_.walls)
            if (w.id == f.first) {
                const uint16_t old = w.patterns[size_t(f.second)];
                if (old == ref)
                    return true;
                if (pattern)
                    cost += float(pattern->price);
                else if (auto it = names.find(old); it != names.end())
                    cost -= float(int(float(priceOf(it->second, false)) * kBuildRefundRate));
                return false;
            }
        return true;
    });
    if (faces.empty()) {
        r.ok = true;
        r.message = "no wall faces to change " + what;
        return r;
    }
    r.cost = int(std::lround(cost));
    if (r.cost > funds(vm_)) {
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    lot_.wallPatterns = names;
    if (pattern)
        prices_[pattern->lotName()] = pattern->price;
    for (const auto& [id, k] : faces)
        for (auto& w : lot_.walls)
            if (w.id == id)
                w.patterns[size_t(k)] = ref, w.layered = true;
    r.changed = int(faces.size());
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " wall sides " + what + " " +
                (pattern ? "covered with " + pattern->name : std::string("uncovered")) +
                (r.cost >= 0 ? " for " + money(r.cost) : ", " + money(-r.cost) + " back");
    return r;
}

BuildEditor::Result BuildEditor::paintWalls(int level, int x0, int y0, int x1, int y1, int side,
                                            const BuildPattern* pattern) {
    std::vector<std::pair<uint32_t, int>> faces; // wall ID, pattern index
    for (const auto& s : wallSections(x0, y0, x1, y1)) {
        const Lot::Wall* w = findWall(lot_, level - 1, s[0], s[1], s[2], s[3]);
        if (!w || Lot::isFence(w->type) || w->type == Lot::DeckSkirt)
            continue; // mayChangeSurface false
        // The wall may run the other way: its right is the run's left.
        const bool same = std::lround(w->a.x) == s[0] && std::lround(w->a.y) == s[1];
        for (int k = 0; k < 2; ++k) {
            const int runSide = (k == 0) == same ? 1 : -1;
            if (side == 0 || side == runSide)
                faces.push_back({w->id, k});
        }
    }
    return cover(std::move(faces), pattern, "along the run");
}

BuildEditor::Result BuildEditor::paintRoom(int level, int x, int y, const BuildPattern* pattern) {
    Result r;
    const int li = level - 1;
    if (lot_.rooms.empty() || !lot_.hasLevel(li) || x < 0 || y < 0 || x >= int(lot_.width) || y >= int(lot_.height)) {
        r.message = "no room there";
        return r;
    }
    uint16_t room = 0;
    for (uint16_t q : lot_.rooms[(size_t(li - lot_.minLevel) * lot_.width + size_t(x)) * lot_.height + size_t(y)])
        room = room ? room : q;
    if (!room) {
        r.message = "no room there";
        return r;
    }
    std::vector<std::pair<uint32_t, int>> faces;
    for (const auto& w : lot_.walls) {
        if (w.a.level != li || Lot::isFence(w.type) || w.type == Lot::DeckSkirt)
            continue;
        for (int k = 0; k < 2; ++k)
            if (wallSideRoom(lot_, w, k) == room)
                faces.push_back({w.id, k});
    }
    return cover(std::move(faces), pattern, "of room " + std::to_string(room));
}

BuildEditor::Result BuildEditor::paintFlood(int level, int ax, int ay, int bx, int by, int side,
                                            const BuildPattern* pattern) {
    Result r;
    const int li = level - 1;
    const Lot::Wall* start = findWall(lot_, li, ax, ay, bx, by);
    if (!start || Lot::isFence(start->type) || start->type == Lot::DeckSkirt) {
        r.message = "no wall face there";
        return r;
    }
    // The run's right (+1) is the wall's right (patterns[0]) when the wall
    // runs the same way.
    const bool same = std::lround(start->a.x) == ax && std::lround(start->a.y) == ay;
    const int k0 = (side >= 0) == same ? 0 : 1;
    // UserSetWallPatternFlood: from the clicked face round the ring of faces
    // of its room both ways, while a face has the pattern the clicked one
    // had. The ring here: faces of the same room (the light rooms, 0x0B,
    // which fences do not split) on the level that share an end point.
    const uint32_t which = lot_.roomLayers.count(0x0B) && !lot_.roomLayers.at(0x0B).empty() ? 0x0B : 0x09;
    const int32_t room = wallSideRoom(lot_, *start, k0, which);
    const uint16_t had = start->patterns[size_t(k0)];
    auto key = [](const Lot::Wall& w) {
        return std::array<long, 4>{std::lround(w.a.x), std::lround(w.a.y), std::lround(w.b.x), std::lround(w.b.y)};
    };
    std::vector<std::pair<uint32_t, int>> faces{{start->id, k0}};
    std::set<std::pair<uint32_t, int>> seen{{start->id, k0}};
    std::vector<const Lot::Wall*> stack{start};
    while (!stack.empty()) {
        const Lot::Wall* w = stack.back();
        stack.pop_back();
        const auto e = key(*w);
        for (const auto& o : lot_.walls) {
            if (o.a.level != li || Lot::isFence(o.type) || o.type == Lot::DeckSkirt)
                continue;
            const auto f = key(o);
            const bool touches = (f[0] == e[0] && f[1] == e[1]) || (f[0] == e[2] && f[1] == e[3]) ||
                                 (f[2] == e[0] && f[3] == e[1]) || (f[2] == e[2] && f[3] == e[3]);
            if (!touches)
                continue;
            for (int k = 0; k < 2; ++k) {
                if (seen.count({o.id, k}) || o.patterns[size_t(k)] != had || wallSideRoom(lot_, o, k, which) != room)
                    continue;
                seen.insert({o.id, k});
                faces.push_back({o.id, k});
                stack.push_back(&o);
            }
        }
    }
    return cover(std::move(faces), pattern, "round room " + std::to_string(room));
}

// --- Modular stairs (docs/buy-build-mac.md §7.12) ---------------------------

namespace {
// WorldDB vtable 0x158: the height of a step (a story of 3 in 16 steps:
// the saved flights drop 0.1875 a step; inferred from the data).
constexpr float kStepRise = 0.1875f, kStepRun = 0.25f;
// UserPlaceModularStairs' tolerances (BV floats 0x274b870, 0x274bbb8, 0x274b474).
constexpr float kReach = 0.09375f, kDig = -0.09375f, kDeckSlack = 0.001f;
// Object data 80 / 81: the next and the previous object of the flight
// (LinkObjects; -1 at the ends), as the saved flights hold them.
constexpr int16_t kNextLink = 80, kPrevLink = 81;

bool modularPart(const Object& o, const char* kind = nullptr) {
    if (!o.definition)
        return false;
    const std::string& n = o.definition->fileName;
    return n.rfind("Stair Modular - ", 0) == 0 && (!kind || n.find(kind) != std::string::npos);
}
float heightOf(const Lot& lot, const Object& o) {
    if (o.position.hasHeight())
        return o.position.height;
    return lot.hasLevel(o.position.level - 1) ? lot.elevationAt(o.position.level - 1, o.position.x, o.position.y) : 0.0f;
}
} // namespace

std::vector<Stair> modularStairs(const VM& vm, const Lot& lot) {
    std::vector<Stair> out;
    for (const auto& [id, o] : vm.objects()) {
        if (!o.position.inWorld() || !modularPart(o, " - Portal - A"))
            continue;
        const Object* last = &o;
        int count = 1;
        for (int guard = 0; guard < 64; ++guard) {
            const ObjectId next = last->data.size() > size_t(kNextLink) ? last->data[kNextLink] : -1;
            auto it = next > 0 ? vm.objects().find(next) : vm.objects().end();
            const Object* n = it != vm.objects().end() ? &it->second : nullptr;
            if (!n || !n->position.inWorld() || !modularPart(*n))
                break;
            last = n, ++count;
        }
        if (count < 2 || !modularPart(*last, " - End"))
            continue;
        const auto [dx, dy] = directionStep(o.data[objdata::Direction]);
        if ((dx != 0) == (dy != 0))
            continue;
        Stair st;
        st.steps = count;
        st.up = {float(-dx), float(-dy)};
        st.bottomEdge = {last->position.x, last->position.y};
        st.direction = directionOf(st.up[0], st.up[1]);
        const auto below = st.at(-0.5f), above = st.at(st.length() + 0.5f);
        st.bottom = {int(std::floor(below[0])), int(std::floor(below[1])), last->position.level};
        st.top = {int(std::floor(above[0])), int(std::floor(above[1])), o.position.level};
        if (st.top.level != st.bottom.level + 1)
            continue;
        st.bottomZ = heightOf(lot, *last);
        st.topZ = heightOf(lot, o) + kStepRise;
        out.push_back(st);
    }
    return out;
}

struct BuildEditor::StairsPlan {
    int steps = 0, top = 0, bottom = 0; // VM levels of the top tile and of the bottom landing
    TilePos topTile;
    int direction = 0;
    float topZ = 0;
    std::array<float, 2> u{}, edge{};
    std::vector<float> ground;                           // the terrain's grid points with the changes
    std::map<std::pair<int, int>, float> terrain;        // ground grid points the flight moves
    std::array<float, 2> step(int i) const {
        return {edge[0] + u[0] * kStepRun * float(i + 1), edge[1] + u[1] * kStepRun * float(i + 1)};
    }
    float z(int i) const { return topZ - kStepRise * float(i + 1); }
    // The tile a step stands on (its quarter tile's centre).
    TilePos tile(int i, int level) const {
        const auto p = step(i);
        return {int(std::floor(p[0] - u[0] * 0.125f)), int(std::floor(p[1] - u[1] * 0.125f)), level};
    }
};

int BuildEditor::planStairs(StairsPlan& plan, TilePos top, int direction, std::optional<std::array<float, 2>> end,
                            bool wholeTiles) const {
    const int W = int(lot_.width), H = int(lot_.height);
    auto inLot = [&](const TilePos& t) { return t.x >= 0 && t.y >= 0 && t.x < W && t.y < H; };
    auto hasFloor = [&](int li, int x, int y) {
        if (li == 0)
            return !poolTile(x, y);
        if (!lot_.hasLevel(li))
            return false;
        for (uint16_t q : lot_.floor(li, uint32_t(x), uint32_t(y)))
            if (q)
                return true;
        return false;
    };
    auto wet = [&](int x, int y) {
        for (int cx = x; cx <= x + 1; ++cx)
            for (int cy = y; cy <= y + 1; ++cy)
                if (lot_.waterLevel(cx, cy) > lot_.elevation(0, cx, cy))
                    return true;
        return false;
    };
    // A wall (or fence) on the edge between a tile and the next one down
    // the flight; the raised edges of foundations and decks the flight
    // comes down from do not count (inferred).
    auto wallAhead = [&](const TilePos& t, int li) {
        const int dx = int(plan.u[0]), dy = int(plan.u[1]);
        const Lot::Wall* w = nullptr;
        if (dx) {
            const int ex = t.x + (dx > 0 ? 1 : 0);
            w = findWall(lot_, li, ex, t.y, ex, t.y + 1);
        } else {
            const int ey = t.y + (dy > 0 ? 1 : 0);
            w = findWall(lot_, li, t.x, ey, t.x + 1, ey);
        }
        return w && !raisedEdge(w->type);
    };
    // Something in the way of a part of the flight: what stands on the
    // floor layer (od 63: 0 or bit 1; wall objects such as windows do not
    // count, inferred from TSObjectUserCanSetPositionAndOrientation's layers).
    auto objectOn = [&](const TilePos& t) {
        for (ObjectId id : vm_.objectsOnTile(t))
            if (const Object* o = vm_.object(id)) {
                const int16_t layers = o->data[objdata::ExclusivePlacementFlags];
                if (o->person || layers == 0 || (layers & 1))
                    return true;
            }
        return false;
    };
    const size_t rowH = size_t(H + 1);
    plan = StairsPlan{};
    if (lot_.hasLevel(0) && !lot_.elevations.empty()) {
        const size_t base = size_t(0 - lot_.minLevel) * size_t(W + 1) * rowH;
        plan.ground.assign(lot_.elevations.begin() + long(base), lot_.elevations.begin() + long(base + size_t(W + 1) * rowH));
    }
    auto groundAt = [&](float x, float y) {
        if (plan.ground.empty())
            return 0.0f;
        x = std::clamp(x, 0.0f, float(W)), y = std::clamp(y, 0.0f, float(H));
        const int x0 = std::min(int(x), W - 1), y0 = std::min(int(y), H - 1);
        const float fx = x - float(x0), fy = y - float(y0);
        auto g = [&](int gx, int gy) { return plan.ground[size_t(gx) * rowH + size_t(gy)]; };
        return (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy;
    };
    auto move = [&](int gx, int gy, float h) {
        if (gx < 0 || gy < 0 || gx > W || gy > H || plan.ground.empty())
            return true;
        float& g = plan.ground[size_t(gx) * rowH + size_t(gy)];
        if (std::fabs(g - h) < 1e-4f)
            return true;
        if (!terrainFree(gx, gy))
            return false;
        g = h;
        plan.terrain[{gx, gy}] = h;
        return true;
    };

    plan.topTile = top;
    plan.direction = direction;
    if (direction % 2)
        return 0x71;
    const auto [sx, sy] = directionStep(direction);
    plan.u = {float(sx), float(sy)};
    plan.top = top.level;
    const int li = top.level - 1;
    // The top tile: a floor tile of a story above the ground (WorldDB 0xB8, 0x6C).
    if (!inLot(top) || li < 1 || !hasFloor(li, top.x, top.y))
        return 0x6C;
    const std::array<float, 2> center{float(top.x) + 0.5f, float(top.y) + 0.5f};
    plan.edge = {center[0] + plan.u[0] * 0.5f, center[1] + plan.u[1] * 0.5f};
    plan.topZ = lot_.elevationAt(li, plan.edge[0], plan.edge[1]);
    // IsFlat (upper floors are not levelled here: 0x6E).
    const float z00 = lot_.elevation(li, top.x, top.y);
    for (int cx = top.x; cx <= top.x + 1; ++cx)
        for (int cy = top.y; cy <= top.y + 1; ++cy)
            if (std::fabs(lot_.elevation(li, cx, cy) - z00) > 0.001f)
                return 0x6E;
    // The number of steps.
    int n = 0;
    if (end) {
        const TilePos et{int(std::floor((*end)[0])), int(std::floor((*end)[1])), top.level};
        if (et.x == top.x && et.y == top.y)
            return 0x78;
        if (et.x != top.x && et.y != top.y)
            return 0x71;
        if (directionOf((*end)[0] - center[0], (*end)[1] - center[1]) != direction)
            return 0x71;
        const float d = std::hypot((*end)[0] - center[0], (*end)[1] - center[1]);
        n = int(std::ceil((d - 0.5f) * 4.0f - 1e-4f));
        if (wholeTiles)
            n = (n + 3) / 4 * 4;
        if (n < 1)
            return 0x78;
    } else {
        // UserPlaceModularStairsFindEnd: down the flight until the last step
        // meets a floor (the terrain or a story below), stopping at water
        // (0x1D), a pool (0x1C), a wall (0x71) or an object (0x74).
        TilePos prev = top;
        for (int k = 1; k <= 64 && !n; ++k) {
            const TilePos t = plan.tile(k - 1, top.level - 1);
            if (!inLot(t))
                return 0x71;
            if (!(t.x == prev.x && t.y == prev.y)) {
                if (wet(t.x, t.y))
                    return 0x1D;
                if (poolTile(t.x, t.y))
                    return 0x1C;
                for (int l = 0; l < top.level; ++l)
                    if (wallAhead(prev, l))
                        return 0x71;
                prev = t;
            }
            const auto p = plan.step(k - 1);
            const float z = plan.z(k - 1);
            for (int l = top.level - 1; l >= 1; --l) {
                if (!hasFloor(l - 1, t.x, t.y))
                    continue;
                const float g = l == 1 ? groundAt(p[0], p[1]) : lot_.elevationAt(l - 1, p[0], p[1]);
                if (z <= g + kReach)
                    n = k;
                break;
            }
        }
        if (!n)
            return 0x77;
    }
    plan.steps = n;
    const int last = n - 1;
    const float zLast = plan.z(last);
    const auto pLast = plan.step(last);
    const TilePos bottomTile{int(std::floor(pLast[0] + plan.u[0] * 0.25f)), int(std::floor(pLast[1] + plan.u[1] * 0.25f)), 0};
    if (!inLot(bottomTile))
        return 0x6D;
    // The bottom landing's story: the highest one below whose floor there
    // is not above the last step.
    plan.bottom = 0;
    for (int l = top.level - 1; l >= 1 && !plan.bottom; --l)
        if (hasFloor(l - 1, bottomTile.x, bottomTile.y) &&
            (l == 1 || lot_.elevationAt(l - 1, pLast[0], pLast[1]) <= zLast + kReach))
            plan.bottom = l;
    if (!plan.bottom)
        return poolTile(bottomTile.x, bottomTile.y) ? 0x6D : 0x77;
    const int B = plan.bottom;
    // Each step: the deck rule (0x78), the terrain over it lowered under
    // it (ChangeFloorAndWaterElevationCoupled on the tile's corners: the
    // downhill ones to the tile's last step, the uphill ones to its first;
    // 0x6F when one cannot move), objects (0x73/0x74), walls (0x76).
    const float px = -plan.u[1], py = plan.u[0]; // across the flight
    TilePos prev{top.x, top.y, top.level};
    for (int i = 0; i < n; ++i) {
        const auto p = plan.step(i);
        const float z = plan.z(i);
        const TilePos t = plan.tile(i, B);
        if (!inLot(t))
            return 0x71;
        if (!(t.x == prev.x && t.y == prev.y)) {
            if ((prev.level == top.level && wallAhead(prev, top.level - 1)) || wallAhead(prev, B - 1))
                return 0x76;
            prev = t;
        }
        if (B == 1) {
            // HasDeck: a floored foundation or deck tile.
            if (z - groundAt(p[0], p[1]) + kDeckSlack < kFoundationHeight && foundationTile(t.x, t.y) && hasFloor(1, t.x, t.y))
                return 0x78;
            const float g1 = groundAt(p[0] + px * 0.5f, p[1] + py * 0.5f), g2 = groundAt(p[0] - px * 0.5f, p[1] - py * 0.5f);
            if (z - g1 < kDig || z - g2 < kDig) {
                const int k = i % 4;
                // The tile's corners on the far (downhill) and near side.
                const int dx = int(plan.u[0]), dy = int(plan.u[1]);
                std::array<std::array<int, 2>, 2> far{}, nearSide{};
                if (dx) {
                    const int fx = t.x + (dx > 0 ? 1 : 0), nx = t.x + (dx > 0 ? 0 : 1);
                    far = {{{fx, t.y}, {fx, t.y + 1}}}, nearSide = {{{nx, t.y}, {nx, t.y + 1}}};
                } else {
                    const int fy = t.y + (dy > 0 ? 1 : 0), ny = t.y + (dy > 0 ? 0 : 1);
                    far = {{{t.x, fy}, {t.x + 1, fy}}}, nearSide = {{{t.x, ny}, {t.x + 1, ny}}};
                }
                for (const auto& c : far)
                    if (!move(c[0], c[1], z - kStepRise * float(3 - k)))
                        return 0x6F;
                if (k)
                    for (const auto& c : nearSide)
                        if (!move(c[0], c[1], z + kStepRise * float(k)))
                            return 0x6F;
            }
        }
        if (objectOn({t.x, t.y, B}))
            return i == 0 ? 0x73 : 0x74;
        if (i % 4 == 2)
            for (int l = B + 1; l <= top.level; ++l)
                if (objectOn({t.x, t.y, l}))
                    return 0x72;
    }
    // The bottom: no wall into it (0x76), levelled to the last step (0x6F),
    // the last step at most 0.09375 over the ground (0x77), room for the landing (0x71).
    if (wallAhead(prev, B - 1))
        return 0x76;
    if (B == 1) {
        for (int cx = bottomTile.x; cx <= bottomTile.x + 1; ++cx)
            for (int cy = bottomTile.y; cy <= bottomTile.y + 1; ++cy)
                if (!move(cx, cy, zLast))
                    return 0x6F;
        if (zLast - groundAt(pLast[0], pLast[1]) > kReach)
            return 0x77;
    } else if (std::fabs(lot_.elevationAt(B - 1, pLast[0], pLast[1]) - zLast) > kReach) {
        return 0x77;
    }
    if (objectOn({bottomTile.x, bottomTile.y, B}))
        return 0x71;
    if (objectOn({top.x, top.y, top.level}))
        return 0x70;
    return 0;
}

int BuildEditor::stairsSteps(TilePos top, int direction, std::optional<std::array<float, 2>> end, bool wholeTiles,
                             int* error) const {
    StairsPlan plan;
    const int e = planStairs(plan, top, direction, end, wholeTiles);
    if (error)
        *error = e;
    return e ? 0 : plan.steps;
}

BuildEditor::Result BuildEditor::placeStairs(const BuildStairs& product, TilePos top, int direction,
                                             std::optional<std::array<float, 2>> end, bool wholeTiles) {
    Result r;
    StairsPlan plan;
    if (const int e = planStairs(plan, top, direction, end, wholeTiles)) {
        r.error = e;
        r.message = "cannot place the stairs (error 0x" + [&] {
            char b[8];
            std::snprintf(b, sizeof b, "%02X", e);
            return std::string(b);
        }() + ")";
        return r;
    }
    const int n = plan.steps, B = plan.bottom, L = plan.top;
    r.cost = n * product.price;
    if (r.cost > funds(vm_)) {
        r.error = 0x16;
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    Geometry before = capture();
    // The terrain under the steps and the bottom tile.
    const int W = int(lot_.width), H = int(lot_.height);
    const size_t plane = size_t(W + 1) * size_t(H + 1);
    for (const auto& [g, h] : plan.terrain) {
        const size_t at = size_t(g.first) * size_t(H + 1) + size_t(g.second);
        lot_.elevations[size_t(0 - lot_.minLevel) * plane + at] = h;
    }
    // The floors over the steps go (the placeholders keep that space free;
    // inferred, as the staircase objects' stairwell).
    for (int i = 0; i < n; ++i) {
        const TilePos t = plan.tile(i, B);
        for (int l = B + 1; l <= L; ++l)
            if (lot_.hasLevel(l - 1) && !(l == L && i == 0 && t.x == top.x && t.y == top.y))
                for (int q = 0; q < 4; ++q)
                    if (lot_.floor(l - 1, uint32_t(t.x), uint32_t(t.y))[size_t(q)])
                        setFloor(lot_, l - 1, t.x, t.y, q, 0);
    }
    std::vector<Sold> created;
    auto undoAll = [&] {
        for (const Sold& x : created)
            deleteGroup(vm_, x.id);
        restore(before);
    };
    // Puts one object of the flight down (AddObject: turned, positioned).
    auto add = [&](uint32_t guid, std::array<float, 2> at, int level, int dir, std::optional<float> height,
                   int value) -> ObjectId {
        const auto def = vm_.code().definitionByGuid(guid);
        if (!def)
            return 0;
        ObjectId id = 0;
        try {
            id = createGhost(vm_, def->first, def->second);
        } catch (const std::exception&) {
            return 0;
        }
        Placement p;
        p.ok = true;
        p.level = level;
        p.direction = dir;
        Position pos{at[0], at[1], level};
        if (height) {
            pos.height = *height;
            // The saved flights keep each step's height over the bottom
            // story's floor, the top step's too (inferred: the terrain under
            // them is 0 in every saved lot).
            pos.elevation = *height - lot_.elevationAt(B - 1, at[0], at[1]);
        }
        for (ObjectId part : groupOf(vm_, id))
            p.parts.push_back({part, {int(std::floor(at[0])), int(std::floor(at[1])), level}, pos});
        if (!buy(vm_, id, p, 0)) {
            deleteGroup(vm_, id);
            return 0;
        }
        if (Object* o = vm_.object(id))
            o->data[objdata::CurrentValue] = int16_t(value);
        created.push_back({id, p});
        return id;
    };
    const auto& obj = product.objects; // landing, placeholder, step, top step, bottom step
    const uint32_t bottomStep = obj[4] ? obj[4] : obj[2];
    std::vector<ObjectId> steps;
    auto failWith = [&](int code) {
        undoAll();
        r.error = code;
        r.cost = 0;
        r.message = "cannot place the stairs: a part does not go there";
        return r;
    };
    // The top landing (0x70), the steps (0x73/0x74) with the placeholders
    // over them (0x72), the bottom landing turned round (0x71).
    if (!add(obj[0], {float(top.x) + 0.5f, float(top.y) + 0.5f}, L, direction, std::nullopt, 0))
        return failWith(0x70);
    for (int i = 0; i < n; ++i) {
        const auto p = plan.step(i);
        const uint32_t guid = i == 0 ? obj[3] : i == n - 1 ? bottomStep : obj[2];
        const ObjectId id = add(guid, p, i == 0 ? L : B, direction, plan.z(i), product.price);
        if (!id)
            return failWith(i == 0 ? 0x73 : 0x74);
        steps.push_back(id);
        if (i % 4 == 2)
            for (int l = B + 1; l <= L; ++l)
                if (!add(obj[1], p, l, direction, std::nullopt, 0))
                    return failWith(0x72);
    }
    const auto pLast = plan.step(n - 1);
    if (!add(obj[0], {pLast[0] + plan.u[0] * 0.5f, pLast[1] + plan.u[1] * 0.5f}, B, (direction + 4) % 8, std::nullopt, 0))
        return failWith(0x71);
    // LinkObjects: each step to the one before and after it.
    for (size_t i = 0; i < steps.size(); ++i)
        if (Object* o = vm_.object(steps[i])) {
            o->data[kNextLink] = int16_t(i + 1 < steps.size() ? steps[i + 1] : -1);
            o->data[kPrevLink] = int16_t(i > 0 ? steps[i - 1] : -1);
        }
    addFunds(vm_, -r.cost);
    computeLotData(vm_);
    r.changed = n;
    changed();
    commit(std::move(before), r.cost, {}, std::move(created));
    r.ok = true;
    r.message = std::to_string(n) + " steps of " + product.name + " from " + point(top.x, top.y) + " level " +
                std::to_string(L) + " down to level " + std::to_string(B) + " for " + money(r.cost) + " (" +
                std::to_string(undo_.back().created.size()) + " objects, " + std::to_string(plan.terrain.size()) +
                " ground points moved)";
    return r;
}

std::vector<ObjectId> BuildEditor::flightOf(ObjectId part) const {
    std::vector<ObjectId> out;
    const Object* o = vm_.object(part);
    if (!o || !o->position.inWorld() || !modularPart(*o))
        return out;
    auto link = [&](const Object& x, int16_t k) -> const Object* {
        const ObjectId id = x.data.size() > size_t(k) ? x.data[k] : -1;
        const Object* y = id > 0 ? vm_.object(id) : nullptr;
        return y && y->position.inWorld() && modularPart(*y) ? y : nullptr;
    };
    // A landing or placeholder: the step it belongs to (beside it).
    const Object* step = o;
    if (!link(*o, kNextLink) && !link(*o, kPrevLink)) {
        step = nullptr;
        float best = 0.8f;
        for (const auto& [id, s] : vm_.objects())
            if (s.position.inWorld() && modularPart(s) && (link(s, kNextLink) || link(s, kPrevLink))) {
                const float d = std::hypot(s.position.x - o->position.x, s.position.y - o->position.y);
                if (d < best)
                    best = d, step = &s;
            }
        if (!step)
            return {part};
    }
    while (const Object* p = link(*step, kPrevLink))
        step = p;
    std::vector<const Object*> chain{step};
    for (int guard = 0; guard < 128; ++guard)
        if (const Object* nx = link(*chain.back(), kNextLink))
            chain.push_back(nx);
        else
            break;
    for (const Object* s : chain)
        out.push_back(s->id);
    // The landings at the ends and the placeholders over the steps.
    const Object& first = *chain.front();
    const Object& last = *chain.back();
    const auto [dx, dy] = directionStep(first.data[objdata::Direction]);
    const std::array<std::array<float, 3>, 2> landings{
        {{first.position.x - float(dx) * 0.75f, first.position.y - float(dy) * 0.75f, float(first.position.level)},
         {last.position.x + float(dx) * 0.5f, last.position.y + float(dy) * 0.5f, float(last.position.level)}}};
    for (const auto& [id, x] : vm_.objects()) {
        if (!x.position.inWorld() || !modularPart(x) || std::find(out.begin(), out.end(), id) != out.end())
            continue;
        bool mine = false;
        if (modularPart(x, " Stub"))
            for (const auto& l : landings)
                mine |= std::fabs(x.position.x - l[0]) < 0.05f && std::fabs(x.position.y - l[1]) < 0.05f &&
                        x.position.level == int(l[2]);
        else if (modularPart(x, " Cutout"))
            for (const Object* s : chain)
                mine |= std::fabs(x.position.x - s->position.x) < 0.05f && std::fabs(x.position.y - s->position.y) < 0.05f &&
                        x.position.level > last.position.level;
        if (mine)
            out.push_back(id);
    }
    return out;
}

BuildEditor::Result BuildEditor::deleteStairs(ObjectId part) {
    Result r;
    const auto flight = flightOf(part);
    if (flight.empty()) {
        r.message = "no modular stairs there";
        return r;
    }
    Geometry before = capture();
    std::vector<Sold> sold;
    for (ObjectId id : flight) {
        Sold s{id, currentPlacement(vm_, id)};
        for (auto& p : s.where.parts)
            if (const Object* q = vm_.object(p.id))
                p.at = q->position;
        r.cost -= sell(vm_, id);
        sold.push_back(std::move(s));
    }
    r.changed = int(flight.size());
    changed();
    commit(std::move(before), r.cost, std::move(sold));
    r.ok = true;
    r.message = "a flight of " + std::to_string(flight.size()) + " objects sold for " + money(-r.cost);
    return r;
}

BuildEditor::Result BuildEditor::openStairwell(ObjectId staircase) {
    Result r;
    std::vector<TilePos> steps;
    for (ObjectId part : groupOf(vm_, staircase))
        if (const Object* o = vm_.object(part); o && o->definition && o->position.inWorld()) {
            const std::string& n = o->definition->fileName;
            if (n.rfind("Stairs - ", 0) == 0 && n.find(" - Lo ") != std::string::npos && n.find(" Pad") == std::string::npos)
                steps.push_back(VM::tileOf(*o));
        }
    Geometry before = capture();
    for (const TilePos& t : steps) {
        const int above = t.level; // the VM level t.level + 1 is lot level t.level
        if (!lot_.hasLevel(above))
            continue;
        for (int q = 0; q < 4; ++q)
            if (lot_.floor(above, uint32_t(t.x), uint32_t(t.y))[size_t(q)]) {
                setFloor(lot_, above, t.x, t.y, q, 0);
                ++r.changed;
            }
    }
    r.ok = true;
    if (!r.changed) {
        r.message = "no floor over the stairs";
        return r;
    }
    changed();
    commit(std::move(before), 0);
    r.message = std::to_string(r.changed) + " quarter tiles of floor opened over the stairs";
    return r;
}

bool BuildEditor::undo() {
    if (undo_.empty())
        return false;
    Op op = std::move(undo_.back());
    undo_.pop_back();
    restore(op.before);
    // What was sold with the walls goes back where it was (its refund is
    // in op.cost).
    for (const Sold& x : op.sold)
        applyPlacement(vm_, x.id, x.where);
    for (const Sold& x : op.created)
        pickUp(vm_, x.id);
    addFunds(vm_, op.cost); // the price back, or the refund paid back
    syncWorld();
    redo_.push_back(std::move(op));
    return true;
}

bool BuildEditor::redo() {
    if (redo_.empty())
        return false;
    Op op = std::move(redo_.back());
    if (op.cost > funds(vm_))
        return false;
    redo_.pop_back();
    restore(op.after);
    for (const Sold& x : op.sold)
        pickUp(vm_, x.id);
    for (const Sold& x : op.created)
        applyPlacement(vm_, x.id, x.where);
    addFunds(vm_, -op.cost);
    syncWorld();
    undo_.push_back(std::move(op));
    return true;
}

void BuildEditor::resetHistory(const std::function<void(ObjectId)>& forget) {
    auto die = [&](const Sold& x) {
        if (const Object* o = vm_.object(x.id); o && !o->position.inWorld()) {
            if (forget)
                for (ObjectId part : groupOf(vm_, x.id))
                    forget(part);
            deleteGroup(vm_, x.id);
        }
    };
    for (const Op& op : undo_)
        for (const Sold& x : op.sold)
            die(x);
    for (const Op& op : redo_) // undone decks: their columns
        for (const Sold& x : op.created)
            die(x);
    undo_.clear(), redo_.clear();
}

int BuildEditor::priceOf(const std::string& lotName, bool floor) const {
    if (auto it = prices_.find(lotName); it != prices_.end())
        return it->second;
    if (catalog_)
        try {
            if (const BuildPattern* p = catalog_->byGuid(uint32_t(std::stoul(lotName, nullptr, 16))); p && p->floor == floor)
                return p->price;
        } catch (const std::exception&) {
        }
    return 0;
}

} // namespace os2::vm
