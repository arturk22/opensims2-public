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

std::string money(int v) { return "§" + std::to_string(v); }

// The streets the lot packages carry (named floor patterns of
// floorPatterns.txt): nothing is built over them (inferred: the game keeps
// the lot's buildable area off them).
bool street(const std::string& pattern) { return pattern.rfind("road_", 0) == 0 || pattern == "sidewalk"; }

} // namespace

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

BuildEditor::BuildEditor(VM& vm, Lot& lot, const BuildCatalog* catalog) : vm_(vm), lot_(lot), catalog_(catalog) {}

BuildEditor::Geometry BuildEditor::capture() const {
    return {lot_.walls,         lot_.wallGraphs,    lot_.floors,        lot_.rooms,
            lot_.roomLayers,    lot_.floorPatterns, lot_.wallPatterns,  lot_.staleWallLayer};
}

void BuildEditor::restore(const Geometry& g) {
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
    // The world from the lot's walls, floors and rooms; the stairs (from the
    // objects) and doors as the VM has them.
    std::vector<Stair> stairs;
    if (vm_.world)
        stairs = vm_.world->stairs();
    Lot shape = lot_;
    shape.objects.clear();
    World w = World::fromLot(shape);
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
    updateRooms(lot_);
    syncWorld();
}

void BuildEditor::commit(Geometry before, int cost) {
    Op op;
    op.before = std::move(before);
    op.after = capture();
    op.cost = cost;
    undo_.push_back(std::move(op));
    redo_.clear();
}

std::string BuildEditor::wallBlocked(int level, int ax, int ay, int bx, int by) const {
    const int li = level - 1;
    if (!lot_.hasLevel(li))
        return "no such level";
    const int W = int(lot_.width), H = int(lot_.height);
    if (std::min({ax, bx, ay, by}) < 0 || std::max(ax, bx) > W || std::max(ay, by) > H)
        return "out of the lot";
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
    for (const TilePos& t : tiles)
        for (uint16_t q : lot_.floor(li, uint32_t(t.x), uint32_t(t.y)))
            if (q)
                if (auto it = lot_.floorPatterns.find(q); it != lot_.floorPatterns.end() && street(it->second))
                    return "not on the street";
    // requiresFlatBottom (walls.txt): on the ground the terrain under the
    // wall must be level.
    if (li == 0 && std::fabs(lot_.elevation(0, ax, ay) - lot_.elevation(0, bx, by)) > 0.01f)
        return "the ground must be flat";
    // Above the ground a wall stands on a wall or next to a floor (inferred
    // from requiresSupportUnderneath; the game's exact support test is not read).
    if (li > 0 && !findWall(lot_, li - 1, ax, ay, bx, by)) {
        bool floor = false;
        for (const TilePos& t : tiles)
            for (uint16_t q : lot_.floor(li, uint32_t(t.x), uint32_t(t.y)))
                floor |= q != 0;
        if (!floor)
            return "nothing holds it up";
    }
    // Objects: a wall does not cut a multi-tile object in two, and a
    // diagonal does not cross a tile something stands on (the walls'
    // blocksPlacement; IntersectsWalls of the placement tests, §3.3).
    if (tiles.size() == 1) {
        for (ObjectId id : vm_.objectsOnTile(tiles[0]))
            if (const Object* o = vm_.object(id); o && !o->person)
                return "an object is in the way";
    } else if (tiles.size() == 2) {
        for (ObjectId a : vm_.objectsOnTile(tiles[0]))
            for (ObjectId b : vm_.objectsOnTile(tiles[1])) {
                const Object* oa = vm_.object(a);
                const Object* ob = vm_.object(b);
                if (oa && ob && multiTileGroup(*oa) == multiTileGroup(*ob))
                    return "an object is in the way";
            }
    }
    return "";
}

BuildEditor::Result BuildEditor::drawWalls(int level, int x0, int y0, int x1, int y1) {
    Result r;
    const auto sections = wallSections(x0, y0, x1, y1);
    if (sections.empty()) {
        r.message = "walls go along the grid or at 45 degrees";
        return r;
    }
    std::vector<std::array<int, 4>> todo;
    for (const auto& s : sections) {
        if (findWall(lot_, level - 1, s[0], s[1], s[2], s[3]))
            continue;
        if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3]); !why.empty()) {
            r.message = "cannot build a wall from (" + std::to_string(s[0]) + "," + std::to_string(s[1]) + ") to (" +
                        std::to_string(s[2]) + "," + std::to_string(s[3]) + "): " + why;
            return r;
        }
        todo.push_back(s);
    }
    r.cost = int(todo.size()) * kWallSectionPrice;
    if (r.cost > funds(vm_)) {
        r.cost = 0;
        r.message = "not enough money (" + money(int(todo.size()) * kWallSectionPrice) + ")";
        return r;
    }
    Geometry before = capture();
    const uint16_t blank = patternRef(lot_.wallPatterns, kDefaultWallPattern, 0);
    for (const auto& s : todo)
        if (auto id = addWall(lot_, level - 1, s[0], s[1], s[2], s[3], kWallType, {blank, blank}))
            builtWalls_.insert(*id), ++r.changed;
    if (!r.changed) {
        r.ok = true;
        r.message = "the walls are there already";
        return r;
    }
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " wall sections for " + money(r.cost);
    return r;
}

BuildEditor::Result BuildEditor::drawRoom(int level, int x0, int y0, int x1, int y1) {
    Result r;
    if (x0 == x1 || y0 == y1) {
        r.message = "a room needs an area";
        return r;
    }
    // The four sides, as one change.
    const std::array<std::array<int, 4>, 4> sides{{{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}};
    int total = 0;
    for (const auto& side : sides)
        for (const auto& s : wallSections(side[0], side[1], side[2], side[3])) {
            if (findWall(lot_, level - 1, s[0], s[1], s[2], s[3]))
                continue;
            if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3]); !why.empty()) {
                r.message = "cannot build the room: " + why;
                return r;
            }
            ++total;
        }
    if (total * kWallSectionPrice > funds(vm_)) {
        r.message = "not enough money (" + money(total * kWallSectionPrice) + ")";
        return r;
    }
    Geometry before = capture();
    const uint16_t blank = patternRef(lot_.wallPatterns, kDefaultWallPattern, 0);
    for (const auto& side : sides)
        for (const auto& s : wallSections(side[0], side[1], side[2], side[3]))
            if (auto id = addWall(lot_, level - 1, s[0], s[1], s[2], s[3], kWallType, {blank, blank}))
                builtWalls_.insert(*id), ++r.changed;
    r.cost = r.changed * kWallSectionPrice;
    r.ok = true;
    if (!r.changed)
        return r.message = "the walls are there already", r;
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.message = "a room of " + std::to_string(r.changed) + " wall sections for " + money(r.cost);
    return r;
}

BuildEditor::Result BuildEditor::deleteWalls(int level, int x0, int y0, int x1, int y1) {
    Result r;
    const auto sections = wallSections(x0, y0, x1, y1);
    if (sections.empty()) {
        r.message = "walls go along the grid or at 45 degrees";
        return r;
    }
    Geometry before = capture();
    float refund = 0;
    for (const auto& s : sections) {
        const Lot::Wall* w = findWall(lot_, level - 1, s[0], s[1], s[2], s[3]);
        // Only the wall tool's own walls (walls.txt deleteTool partition:
        // walls and fences; foundations, deck skirts and pools go with
        // their own tools).
        if (!w || !(w->type == Lot::Normal || Lot::isFence(w->type)))
            continue;
        const bool fresh = builtWalls_.count(w->id) != 0;
        refund += fresh ? float(kWallSectionPrice) : float(kWallSectionPrice) * kBuildRefundRate;
        builtWalls_.erase(w->id);
        removeWall(lot_, level - 1, s[0], s[1], s[2], s[3]);
        ++r.changed;
    }
    r.ok = true;
    if (!r.changed)
        return r.message = "no walls there", r;
    r.cost = -int(std::lround(refund));
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.message = std::to_string(r.changed) + " wall sections deleted for " + money(-r.cost);
    return r;
}

BuildEditor::Result BuildEditor::paintFloor(int level, int x0, int y0, int x1, int y1, const BuildPattern* pattern,
                                            int quarters) {
    Result r;
    const int li = level - 1;
    if (!lot_.hasLevel(li)) {
        r.message = "no such level";
        return r;
    }
    const int W = int(lot_.width), H = int(lot_.height);
    const int ax = std::max(0, std::min(x0, x1)), bx = std::min(W - 1, std::max(x0, x1));
    const int ay = std::max(0, std::min(y0, y1)), by = std::min(H - 1, std::max(y0, y1));
    Geometry before = capture();
    std::map<uint16_t, std::string> names = lot_.floorPatterns;
    const uint16_t ref = pattern ? patternRef(names, pattern->lotName(), 1) : 0;
    // cTSUserToolUndoableFloorPattern::ChangeTile: per quarter that changes,
    // + new price (placing) or - refund of the old (erasing), a quarter each.
    float cost = 0;
    std::vector<std::pair<std::array<int, 3>, uint16_t>> changes;
    for (int x = ax; x <= bx; ++x)
        for (int y = ay; y <= by; ++y) {
            const auto& q = lot_.floor(li, uint32_t(x), uint32_t(y));
            for (int k = 0; k < 4; ++k) {
                if (!(quarters & (1 << k)) || q[size_t(k)] == ref)
                    continue;
                const std::string old = q[size_t(k)] ? (names.count(q[size_t(k)]) ? names[q[size_t(k)]] : "") : "";
                if (li == 0 && street(old))
                    continue; // the street stays
                const size_t at = ((size_t(li) * size_t(W) + size_t(x)) * size_t(H) + size_t(y)) * 4 + size_t(k);
                if (pattern) {
                    cost += float(pattern->price) * 0.25f;
                } else if (q[size_t(k)]) {
                    // The pattern's price back (the full price if it was laid
                    // this session).
                    float price = 0;
                    if (auto it = prices_.find(old); it != prices_.end()) {
                        price = float(it->second);
                    } else if (catalog_) {
                        try {
                            if (const BuildPattern* p = catalog_->byGuid(uint32_t(std::stoul(old, nullptr, 16))))
                                price = float(p->price);
                        } catch (const std::exception&) {
                        }
                    }
                    cost -= price * (builtFloors_.count(at) ? 1.0f : kBuildRefundRate) * 0.25f;
                }
                changes.push_back({{x, y, k}, ref});
            }
        }
    if (changes.empty()) {
        r.ok = true;
        r.message = "nothing to change";
        return r;
    }
    r.cost = int(std::lround(cost));
    if (r.cost > funds(vm_)) {
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    lot_.floorPatterns = names;
    if (pattern)
        prices_[pattern->lotName()] = pattern->price;
    for (const auto& [t, v] : changes) {
        setFloor(lot_, li, t[0], t[1], t[2], v);
        const size_t at = ((size_t(li) * size_t(W) + size_t(t[0])) * size_t(H) + size_t(t[1])) * 4 + size_t(t[2]);
        if (v)
            builtFloors_.insert(at);
        else
            builtFloors_.erase(at);
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

BuildEditor::Result BuildEditor::paintWalls(int level, int x0, int y0, int x1, int y1, int side,
                                            const BuildPattern& pattern) {
    Result r;
    const auto sections = wallSections(x0, y0, x1, y1);
    Geometry before = capture();
    std::map<uint16_t, std::string> names = lot_.wallPatterns;
    const uint16_t ref = patternRef(names, pattern.lotName(), 0);
    std::vector<std::pair<uint32_t, int>> faces; // wall ID, pattern index
    for (const auto& s : sections) {
        const Lot::Wall* w = findWall(lot_, level - 1, s[0], s[1], s[2], s[3]);
        if (!w || Lot::isFence(w->type) || w->type == Lot::DeckSkirt)
            continue; // mayChangeSurface false
        // The wall may run the other way: its right is the run's left.
        const bool same = std::lround(w->a.x) == s[0] && std::lround(w->a.y) == s[1];
        for (int k = 0; k < 2; ++k) {
            const int runSide = (k == 0) == same ? 1 : -1;
            if ((side == 0 || side == runSide) && w->patterns[size_t(k)] != ref)
                faces.push_back({w->id, k});
        }
    }
    if (faces.empty()) {
        r.ok = true;
        r.message = "no wall faces to cover there";
        return r;
    }
    r.cost = int(faces.size()) * pattern.price;
    if (r.cost > funds(vm_)) {
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    lot_.wallPatterns = names;
    for (const auto& [id, k] : faces)
        for (auto& w : lot_.walls)
            if (w.id == id)
                w.patterns[size_t(k)] = ref, w.layered = true;
    r.changed = int(faces.size());
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " wall sides covered with " + pattern.name + " for " + money(r.cost);
    return r;
}

BuildEditor::Result BuildEditor::paintRoom(int level, int x, int y, const BuildPattern& pattern) {
    Result r;
    const int li = level - 1;
    if (lot_.rooms.empty() || !lot_.hasLevel(li) || x < 0 || y < 0 || x >= int(lot_.width) || y >= int(lot_.height)) {
        r.message = "no room there";
        return r;
    }
    uint16_t room = 0;
    for (uint16_t q : lot_.rooms[(size_t(li) * lot_.width + size_t(x)) * lot_.height + size_t(y)])
        room = room ? room : q;
    if (!room) {
        r.message = "no room there";
        return r;
    }
    Geometry before = capture();
    std::map<uint16_t, std::string> names = lot_.wallPatterns;
    const uint16_t ref = patternRef(names, pattern.lotName(), 0);
    std::vector<std::pair<Lot::Wall*, int>> faces;
    for (auto& w : lot_.walls) {
        if (w.a.level != li || Lot::isFence(w.type) || w.type == Lot::DeckSkirt)
            continue;
        for (int k = 0; k < 2; ++k)
            if (wallSideRoom(lot_, w, k) == room && w.patterns[size_t(k)] != ref)
                faces.push_back({&w, k});
    }
    if (faces.empty()) {
        r.ok = true;
        r.message = "no wall faces to cover in that room";
        return r;
    }
    r.cost = int(faces.size()) * pattern.price;
    if (r.cost > funds(vm_)) {
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    lot_.wallPatterns = names;
    for (auto& [w, k] : faces)
        w->patterns[size_t(k)] = ref, w->layered = true;
    r.changed = int(faces.size());
    addFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = "room " + std::to_string(room) + ": " + std::to_string(r.changed) + " wall sides covered with " +
                pattern.name + " for " + money(r.cost);
    return r;
}

bool BuildEditor::undo() {
    if (undo_.empty())
        return false;
    Op op = std::move(undo_.back());
    undo_.pop_back();
    restore(op.before);
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
    addFunds(vm_, -op.cost);
    syncWorld();
    undo_.push_back(std::move(op));
    return true;
}

void BuildEditor::resetHistory() {
    undo_.clear(), redo_.clear();
    builtWalls_.clear(), builtFloors_.clear();
}

} // namespace os2::vm
