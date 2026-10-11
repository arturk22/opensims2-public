// Build mode's tools over a lot in the VM (build_edit.hpp).

#include "build_edit.hpp"

#include "formats/lot_edit.hpp"
#include "lot.hpp"
#include "placement.hpp"

#include <algorithm>
#include <cmath>
#include <map>

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
    lot_.elevations = g.elevations;
    lot_.floorRotations = g.floorRotations;
    lot_.objects = g.objects;
    lot_.fencePosts = g.fencePosts;
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
    for (const Stair& s : loadedStairs_)
        w.addStair(s);
    for (const Stair& s : objectStairs(vm_, lot_))
        if (std::none_of(loadedStairs_.begin(), loadedStairs_.end(),
                         [&](const Stair& o) { return o.bottom == s.bottom && o.top == s.top; }))
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
    updateRooms(lot_);
    syncWorld();
}

void BuildEditor::commit(Geometry before, int cost, std::vector<Sold> sold) {
    Op op;
    op.before = std::move(before);
    op.after = capture();
    op.cost = cost;
    op.sold = std::move(sold);
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
        removeWall(lot_, level - 1, s[0], s[1], s[2], s[3]);
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
    addFunds(vm_, -op.cost);
    syncWorld();
    undo_.push_back(std::move(op));
    return true;
}

void BuildEditor::resetHistory(const std::function<void(ObjectId)>& forget) {
    for (const Op& op : undo_)
        for (const Sold& x : op.sold)
            if (const Object* o = vm_.object(x.id); o && !o->position.inWorld()) {
                if (forget)
                    for (ObjectId part : groupOf(vm_, x.id))
                        forget(part);
                deleteGroup(vm_, x.id);
            }
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
