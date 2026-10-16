// Build mode's walls (the wall and room tools, erasing walls and what hangs
// on them), fences and fence arches, and roofs (drawn, dormers, auto-roof,
// the attic walls under them). Part of the build editor (build_edit.cpp).

#include "build_edit.hpp"
#include "build_edit_internal.hpp"

#include "formats/lot_edit.hpp"
#include "formats/lot_roof.hpp"
#include "lot.hpp"
#include "placement.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace os2::vm {

using namespace build_edit_internal;

namespace {

// Object data 0x3E: an object in use (UserMayEraseWallOrFence: > 0, or
// IsOccupied).
constexpr int16_t kInUseData = 0x3E;

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

} // namespace

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
    // UserMayPlacePartition's common test (BV 0x9a7384, the symxfer's
    // "cTSUserToolAutoRoof::RemoveAllRoofs"): off the lot 0x40 (a fence
    // needs one side on it), next to an arch 0x4D, locked 0x45.
    if (const int e = partitionCommonError(li, ax, ay, bx, by, fence ? PartitionKind::Fence : PartitionKind::Wall); e)
        return fail(e, e == 0x40 ? "out of the lot" : e == 0x45 ? "on a locked tile (the road strip)"
                       : e == 0x4D ? "next to an arch" : "a partition is on that edge");
    std::erase_if(tiles, [&](const TilePos& t) { return t.x < 0 || t.y < 0 || t.x >= W || t.y >= H; });
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
    // requiresSupportUnderneath (FUN_0016b740, docs/buy-build-mac.md §7.21):
    // above the ground one side of the wall holds it, its quarters facing
    // the wall SupportedUnderneath or floored on this level or on a
    // mansard's top (DoesTileHasRoofSupport), or a wall of
    // styles 1, 3, 5-10, 0x0F, 0x16, 0x17 (walls, attic, foundation; not
    // decks, pools or fences) on the same edge below.
    if (li > 0) {
        // The side tiles with their facing quarters (bits 1 west, 2 north,
        // 4 east, 8 south): FUN_008b4e9c / OppositeWallLocator.
        std::vector<std::pair<TilePos, int>> sides;
        if (ax != bx && ay != by) {
            const bool slash = (bx - ax) * (by - ay) > 0; // (x, y)-(x+1, y+1)
            sides = {{tiles[0], slash ? 0x3 : 0x6}, {tiles[0], slash ? 0xC : 0x9}};
        } else if (ay == by) {
            sides = {{tiles[0], 0x2}, {tiles[1], 0x8}};
        } else {
            sides = {{tiles[0], 0x4}, {tiles[1], 0x1}};
        }
        bool held = false;
        for (const auto& [t, mask] : sides) {
            if (t.x < 0 || t.y < 0 || t.x >= W || t.y >= H)
                continue;
            if (supportedUnderneath(level, t.x, t.y, mask) || roofSupported(level, t.x, t.y))
                held = true;
            const auto& f = lot_.floor(li, uint32_t(t.x), uint32_t(t.y));
            for (int q = 0; q < 4; ++q)
                held |= (mask & (1 << q)) && f[size_t(q)];
        }
        if (!held)
            if (const Lot::Wall* below = findWall(lot_, li - 1, ax, ay, bx, by)) {
                const uint32_t s = below->type;
                held = s == 1 || s == 3 || s == 5 || s == 6 || (s >= 7 && s <= 10) || s == 0x0F || s == 0x16 ||
                       s == 0x17;
            }
        if (!held)
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

BuildEditor::Result BuildEditor::drawWalls(int level, int x0, int y0, int x1, int y1) {
    Result r;
    const auto sections = wallSections(x0, y0, x1, y1);
    if (sections.empty()) {
        r.message = "walls go along the grid or at 45 degrees";
        return r;
    }
    Geometry before = capture();
    // PlaceWallLine (BV 0x9a9fec): section by section from the start; each
    // goes through TryPlacePartition (0x9a9774): the ground under it is
    // levelled to the height of the drag's start vertex (the walls'
    // leveller, levelForWall), then UserMayPlacePartition; one that is there
    // already is skipped (0x41). The first refusal makes the whole drag
    // invalid, and the tool's release undoes what the drag laid
    // (ChangeWallsAndPlaceCursorAtEnd / OnRelease, docs/buy-build-mac.md
    // §7.24): nothing is built.
    const int li = level - 1;
    const float height = lot_.hasLevel(li) ? lot_.elevation(li, x0, y0) : 0.0f;
    std::vector<std::array<int, 4>> todo;
    for (const auto& s : sections) {
        if (findWall(lot_, li, s[0], s[1], s[2], s[3]))
            continue;
        int code = 0;
        if (!levelForWall(li, sectionPoints(s), height, true, &code) && code) {
            restore(before);
            r.error = code;
            r.message = "cannot build a wall from " + point(s[0], s[1]) + " to " + point(s[2], s[3]) +
                        ": the ground there cannot be levelled";
            return r;
        }
        if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3], &r.error); !why.empty()) {
            restore(before);
            r.message = "cannot build a wall from " + point(s[0], s[1]) + " to " + point(s[2], s[3]) + ": " + why;
            return r;
        }
        todo.push_back(s);
    }
    r.cost = int(todo.size()) * kWallSectionPrice;
    if (todo.empty() || !canAfford(vm_, r.cost)) {
        restore(before);
        if (!todo.empty())
            r.message = "not enough money (" + money(r.cost) + ")";
        else
            r.ok = true, r.message = "the walls are there already";
        r.cost = 0;
        return r;
    }
    const uint16_t blank = patternRef(lot_.wallPatterns, kDefaultWallPattern, 0);
    for (const auto& s : todo)
        if (addWall(lot_, li, s[0], s[1], s[2], s[3], kWallType, {blank, blank}))
            ++r.changed;
    chargeFunds(vm_, -r.cost);
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
    // UserAddPartitions type 1 (BV 0x9aa90c): the two "L"s round the
    // rectangle (each section as the wall tool lays it, levelled to the
    // start vertex's height) must both pass, so any refusal builds nothing;
    // then FUN_009a7dd8 levels every grid point of the rectangle, corners
    // included, to that height (the room's floor comes out flat), and one
    // that cannot go refuses the room too (docs/buy-build-mac.md §7.24).
    const std::array<std::array<int, 4>, 4> sides{{{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}};
    Geometry before = capture();
    const int li = level - 1;
    const float height = lot_.hasLevel(li) ? lot_.elevation(li, x0, y0) : 0.0f;
    std::vector<std::array<int, 4>> todo;
    for (const auto& side : sides)
        for (const auto& s : wallSections(side[0], side[1], side[2], side[3])) {
            if (findWall(lot_, li, s[0], s[1], s[2], s[3]))
                continue;
            int code = 0;
            if (!levelForWall(li, sectionPoints(s), height, true, &code) && code) {
                restore(before);
                r.error = code;
                r.message = "cannot build the room: the ground under " + point(s[0], s[1]) + " cannot be levelled";
                return r;
            }
            if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3], &r.error); !why.empty()) {
                restore(before);
                r.message = "cannot build the room: " + why;
                return r;
            }
            todo.push_back(s);
        }
    std::vector<std::array<int, 2>> inside;
    for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x)
        for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
            inside.push_back({x, y});
    if (int code = 0; !levelForWall(li, inside, height, true, &code)) {
        restore(before);
        r.error = code;
        r.message = "cannot build the room: the ground inside cannot be levelled";
        return r;
    }
    if (!canAfford(vm_, int(todo.size()) * kWallSectionPrice)) {
        restore(before);
        r.message = "not enough money (" + money(int(todo.size()) * kWallSectionPrice) + ")";
        return r;
    }
    const uint16_t blank = patternRef(lot_.wallPatterns, kDefaultWallPattern, 0);
    for (const auto& s : todo)
        if (addWall(lot_, li, s[0], s[1], s[2], s[3], kWallType, {blank, blank}))
            ++r.changed;
    r.cost = r.changed * kWallSectionPrice;
    r.ok = true;
    if (!r.changed && lot_.elevations == before.elevations) {
        restore(before);
        return r.message = "the walls are there already", r;
    }
    chargeFunds(vm_, -r.cost);
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
    // UserMayEraseWallOrFence for every section first: EraseWallLine (BV
    // 0x9ac0e8) gives up at the first section that refuses, and the tool's
    // release then undoes the whole drag (docs/buy-build-mac.md §7.24): a
    // Sim beside a section (0x4B) or an attached object in use (0x1B)
    // deletes nothing. No wall there is skipped (0x42).
    auto erasable = [&](const Lot::Wall* w) {
        // Only the partitions the wall tool deletes (walls.txt deleteTool
        // partition: walls and fences; foundations, deck skirts and pools go
        // with their own tools).
        return w && (w->type == Lot::Normal || Lot::isFence(w->type)) && (!fences || Lot::isFence(w->type));
    };
    for (const auto& s : sections) {
        if (!erasable(findWall(lot_, level - 1, s[0], s[1], s[2], s[3])))
            continue;
        int code = 0;
        if (wallBlocked(level, s[0], s[1], s[2], s[3], &code, true) == "a Sim is in the way") {
            r.error = 0x4B;
            r.message = "cannot delete the wall: a Sim is in the way";
            return r;
        }
        bool inUse = false;
        attachedObjects(level, s[0], s[1], s[2], s[3], &inUse);
        if (inUse) {
            r.error = 0x1B;
            r.message = "cannot delete the wall: an object on the wall is in use";
            return r;
        }
    }
    for (const auto& s : sections) {
        const Lot::Wall* w = findWall(lot_, level - 1, s[0], s[1], s[2], s[3]);
        if (!erasable(w))
            continue;
        const uint32_t type = w->type;
        bool inUse = false;
        const auto attached = attachedObjects(level, s[0], s[1], s[2], s[3], &inUse);
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
        r.ok = true;
        r.message = "no walls there";
        return r;
    }
    r.ok = true;
    r.cost = -(refund + objectRefund);
    chargeFunds(vm_, refund);
    changed();
    r.message = std::to_string(r.changed) + (fences ? " fence" : " wall") + " sections deleted for " + money(refund) +
                (sold.empty() ? "" : ", " + std::to_string(sold.size()) + " attached objects sold for " +
                                         money(objectRefund));
    commit(std::move(before), r.cost, std::move(sold));
    return r;
}

int BuildEditor::partitionCommonError(int li, int ax, int ay, int bx, int by, PartitionKind kind) const {
    const int W = int(lot_.width), H = int(lot_.height);
    // The tiles on the two sides (FUN_008b4e9c / FUN_008b51d4); one for a diagonal.
    TilePos t1{}, t2{};
    if (ax != bx && ay != by)
        t1 = t2 = {std::min(ax, bx), std::min(ay, by), li + 1};
    else if (ay == by)
        t1 = {std::min(ax, bx), ay - 1, li + 1}, t2 = {std::min(ax, bx), ay, li + 1};
    else
        t1 = {ax - 1, std::min(ay, by), li + 1}, t2 = {ax, std::min(ay, by), li + 1};
    auto onLot = [&](const TilePos& t) { return t.x >= 0 && t.y >= 0 && t.x < W && t.y < H; };
    const bool fence = kind == PartitionKind::Fence; // PartitionStyleIsFence: types 1 and 3
    if (!onLot(t1) && !fence)
        return 0x40;
    if (!onLot(t2) && (!fence || !onLot(t1)))
        return 0x40;
    // FUN_009a6b20: the graphs the style's PartitionsBlocked names (walls
    // 0xF by the cTSProductPartition default, fences 0xB, arches 5) refuse a
    // partition of their type on the edge (0x41); the graphs of
    // AdjacentPartitionsBlocked (walls 4: arches; arches 1: walls) refuse one
    // ending at either end: a wall next to an arch 0x4D, an arch next to a
    // wall 0x50. Graph = partition type: 0 walls, 1 fences, 2 arches.
    const unsigned blocked = kind == PartitionKind::Wall ? 0xFu : kind == PartitionKind::Fence ? 0xBu : 0x5u;
    const unsigned adjacent = kind == PartitionKind::Wall ? 0x4u : kind == PartitionKind::Fence ? 0x0u : 0x1u;
    auto touches = [&](float x, float y) {
        return (std::lround(x) == ax && std::lround(y) == ay) || (std::lround(x) == bx && std::lround(y) == by);
    };
    bool same[3] = {}, next[3] = {};
    for (const auto& w : lot_.walls) {
        if (w.a.level != li)
            continue;
        const int graph = Lot::isFence(w.type) ? 1 : 0;
        const bool on = (std::lround(w.a.x) == ax && std::lround(w.a.y) == ay && std::lround(w.b.x) == bx &&
                         std::lround(w.b.y) == by) ||
                        (std::lround(w.a.x) == bx && std::lround(w.a.y) == by && std::lround(w.b.x) == ax &&
                         std::lround(w.b.y) == ay);
        same[graph] |= on;
        next[graph] |= !on && (touches(w.a.x, w.a.y) || touches(w.b.x, w.b.y));
    }
    for (const auto& f : fenceArches(lot_)) {
        if (f.level != li)
            continue;
        const bool on = (f.ax == ax && f.ay == ay && f.bx == bx && f.by == by) ||
                        (f.ax == bx && f.ay == by && f.bx == ax && f.by == ay);
        same[2] |= on;
        next[2] |= !on && (touches(float(f.ax), float(f.ay)) || touches(float(f.bx), float(f.by)));
    }
    for (int graph = 0; graph < 3; ++graph) {
        if ((blocked & (1u << graph)) && same[graph])
            return 0x41;
        if ((adjacent & (1u << graph)) && next[graph]) {
            if (kind == PartitionKind::Wall && graph == 2)
                return 0x4D;
            if (kind == PartitionKind::Arch && graph == 0)
                return 0x50;
        }
    }
    // A side off the lot or locked (cIWorldDB slot 0xc4) -> 0x45: either
    // side, both for a fence (the toolbar's TilesUnlocked cheat skips it).
    const bool bad1 = !onLot(t1) || tileLocked(li, t1.x, t1.y), bad2 = !onLot(t2) || tileLocked(li, t2.x, t2.y);
    if (fence ? bad1 && bad2 : bad1 || bad2)
        return 0x45;
    // RequiresFlatBottom (walls and arches; fences say false): the two ends
    // at different floor heights -> 0x49. The wall tool levels the ground
    // first, so only the arch asks here.
    if (kind == PartitionKind::Arch && lot_.hasLevel(li) &&
        std::fabs(lot_.elevation(li, ax, ay) - lot_.elevation(li, bx, by)) > 0.001f)
        return 0x49;
    return 0;
}

int BuildEditor::fenceArchBlocked(int li, int ax, int ay, int bx, int by) const {
    const int W = int(lot_.width), H = int(lot_.height);
    const int dx = bx - ax, dy = by - ay; // a unit edge along x or y
    // The tiles on the two sides of an edge (from its lower end).
    auto sides = [&](int x0, int y0) {
        const int x = std::min(x0, x0 + dx), y = std::min(y0, y0 + dy);
        return dx ? std::array<std::array<int, 2>, 2>{{{x, y - 1}, {x, y}}} : std::array<std::array<int, 2>, 2>{{{x - 1, y}, {x, y}}};
    };
    // 1. Neither side tile in bounds (cIWorldDB 0x108 IsInBounds: strict,
    // the outer ring out; and here the editable area, inferred) -> 0x45.
    bool any = false;
    for (const auto& t : sides(ax, ay))
        any |= t[0] > 0 && t[1] > 0 && t[0] < W - 1 && t[1] < H - 1 && editableTile(t[0], t[1]);
    if (!any)
        return 0x45;
    const int level = li + 1; // the VM's
    // 2. An object on the fence post of either end (FUN_00163758: the one at
    // the vertex) -> 0x4F.
    for (const auto& v : {std::array<int, 2>{ax, ay}, std::array<int, 2>{bx, by}})
        for (const auto& [id, o] : vm_.objects())
            if (o.position.inWorld() && !o.person && o.position.level == level &&
                std::fabs(o.position.x - float(v[0])) < 0.05f && std::fabs(o.position.y - float(v[1])) < 0.05f)
                return 0x4F;
    // 3. MayPlaceFenceArchWithRespectToObjects -> 0x0C: an arch at either end
    // (the scene graph's FenceArchAtVertex, inferred), or an object of the
    // two tiles of the edge and of its 1-tile extensions (not od 0x22, not od
    // 0x2A bit 0x400) whose footprint meets the +/-0.05 box round a post.
    // Footprints are whole tiles here (as in the routing), so an object of
    // one of the four tiles round a post meets it (inferred).
    for (const auto& f : fenceArches(lot_))
        if (f.level == li && ((f.ax == ax && f.ay == ay) || (f.ax == bx && f.ay == by) || (f.bx == ax && f.by == ay) ||
                              (f.bx == bx && f.by == by)))
            return 0x0C;
    std::set<std::pair<int, int>> tiles;
    for (const auto& [sx, sy] : {std::array<int, 2>{ax - dx, ay - dy}, std::array<int, 2>{ax, ay}, std::array<int, 2>{bx, by}})
        for (const auto& t : sides(sx, sy))
            tiles.insert({t[0], t[1]});
    for (const auto& [id, o] : vm_.objects()) {
        if (!o.position.inWorld() || o.person || !o.definition || o.position.level != level || o.data[0x22] ||
            (o.data[0x2A] & 0x400))
            continue;
        const TilePos t = VM::tileOf(o);
        if (!tiles.count({t.x, t.y}))
            continue;
        for (const auto& v : {std::array<int, 2>{ax, ay}, std::array<int, 2>{bx, by}})
            if (float(t.x) <= float(v[0]) + 0.05f && float(t.x + 1) >= float(v[0]) - 0.05f &&
                float(t.y) <= float(v[1]) + 0.05f && float(t.y + 1) >= float(v[1]) - 0.05f)
                return 0x0C;
    }
    return 0;
}

BuildEditor::Result BuildEditor::placeFenceArch(int level, int ax, int ay, int bx, int by, const BuildFenceArch& arch) {
    Result r;
    const int li = level - 1;
    if (ax != bx && ay != by) {
        // cTSProductFenceArch: mayPlaceDiagonally false -> 0x4C.
        r.error = 0x4C;
        r.message = "cannot build diagonally";
        return r;
    }
    if (std::abs(bx - ax) + std::abs(by - ay) != 1) {
        r.message = "an arch spans one tile edge";
        return r;
    }
    if (hasFenceArch(lot_, li, ax, ay, bx, by)) {
        r.ok = true;
        r.message = "the arch is there already";
        return r;
    }
    // UserMayPlacePartition's common test (BV 0x9a7384) for the arch's
    // partition style (cTSProductFenceArch's constructor, BV 0xf20544:
    // partition type 2, not a fence, so both sides count; partitions
    // blocked 5, adjacent partitions blocked 1, requires a flat bottom). The
    // binary does not ask for a fence under the arch: "Must be placed on a
    // fence" (0x79, 0x88) is the error of objects that hang on fences.
    if (const int e = partitionCommonError(li, ax, ay, bx, by, PartitionKind::Arch); e) {
        r.error = e;
        r.message = e == 0x40   ? "cannot place the arch off the lot"
                    : e == 0x45 ? "cannot place the arch on a locked tile (the road strip)"
                    : e == 0x41 ? "cannot place the arch: a wall or an arch is on that edge"
                    : e == 0x50 ? "cannot place the arch next to a wall"
                                : "cannot place the arch: the ground is not flat";
        return r;
    }
    // UserMayPlaceFenceArch (BV 0x9a92fc, docs/buy-build-mac.md §7.19.1).
    if (const int e = fenceArchBlocked(li, ax, ay, bx, by); e) {
        r.error = e;
        r.message = e == 0x45   ? "cannot place the arch off the lot"
                    : e == 0x4F ? "cannot place the arch: an object is on its fence post"
                                : "cannot place the arch: it would intersect other objects";
        return r;
    }
    r.cost = arch.price;
    if (!canAfford(vm_, r.cost)) {
        r.error = 0x16;
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    Geometry before = capture();
    addFenceArch(lot_, li, ax, ay, bx, by);
    chargeFunds(vm_, -r.cost);
    r.changed = 1;
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = arch.name + " on " + point(ax, ay) + "-" + point(bx, by) + " for " + money(r.cost);
    return r;
}

BuildEditor::Result BuildEditor::deleteFenceArch(int level, int ax, int ay, int bx, int by, const BuildFenceArch* arch) {
    Result r;
    if (!hasFenceArch(lot_, level - 1, ax, ay, bx, by)) {
        r.ok = true;
        r.message = "no arch there";
        return r;
    }
    Geometry before = capture();
    removeFenceArch(lot_, level - 1, ax, ay, bx, by);
    r.cost = -int(float(arch ? arch->price : 0) * kBuildRefundRate);
    chargeFunds(vm_, -r.cost);
    r.changed = 1;
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = "arch taken off for " + money(-r.cost);
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
        // As the wall tool (PlaceWallLine): the first refusal makes the
        // whole drag invalid and nothing is built (docs/buy-build-mac.md
        // §7.24). Fences need no flat bottom: nothing is levelled.
        if (const std::string why = wallBlocked(level, s[0], s[1], s[2], s[3], &r.error, true); !why.empty()) {
            r.message = "cannot put a fence from " + point(s[0], s[1]) + " to " + point(s[2], s[3]) + ": " + why;
            return r;
        }
        todo.push_back(s);
    }
    r.cost = int(todo.size()) * fence.price;
    if (todo.empty() || !canAfford(vm_, r.cost)) {
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
    chargeFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " sections of " + fence.name + " for " + money(r.cost);
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

int BuildEditor::roofError(const Lot::Roof& roof) const {
    // cTSUserToolRoof::IsRoofValid (BV 0x1d9fd4; docs/buy-build-mac.md
    // §7.22). The drag's two corners: start (x0, y0), end (x1, y1).
    const float ax = roof.x0, ay = roof.y0, bx = roof.x1, by = roof.y1;
    // FUN_0099545c: |dx| x |dy| under 1840, else 0x10 "Can't support the size".
    if (std::fabs(ax - bx) * std::fabs(ay - by) >= 1840.0f)
        return 0x10;
    // cAutoRoofData::ApplyFloorPattern (the symxfer's name; BV 0x9992d0):
    // the tiles round each corner on the lot and not locked, else 0x7E.
    for (const auto& [cx, cy] : std::array<std::array<float, 2>, 2>{{{ax, ay}, {bx, by}}}) {
        const int vx = int(cx), vy = int(cy);
        for (int tx = vx - 1; tx <= vx; ++tx)
            for (int ty = vy - 1; ty <= vy; ++ty)
                if (!editableTile(tx, ty))
                    return 0x7E;
    }
    // RoofIntersectWithObjects (BV 0x1d9908) -> DoesTileHasObjectIntersectingRoof
    // (0x1d4354): on each tile of the roof's box whose centre the roof
    // covers, the first object of the roof's level (od 0x22 zero, od 42
    // without the roof bit 0x8000) decides the tile: free when a roof
    // already in the world stands over the centre (the tool's member +0x6c,
    // cIWorldRoof slot 0x7c IsAnyRoofAtLocation) and leaves the clearance
    // (slot 0xa0 DoesLocationHasClearanceHeight with StandardLevelHeight 3:
    // the highest roof of levels L-1..L+2 there at least 3 over the floor,
    // or at most 0.1); else refused (0x7F) where the new roof at the centre
    // is lower than the floor there plus 3 (docs/buy-build-mac.md §7.22).
    {
        Lot one = lot_;
        one.roofs = {roof};
        const LotRoofs shape(one);
        const LotRoofs existing(lot_);
        const roofgame::World& world = existing.world();
        auto clear = [&](float cx, float cy) {
            const int level = roof.level;
            if (!world.anyRoofAt(cx, cy, level))
                return false;
            if (!world.anyRoofAtOrBelow(cx, cy, level))
                return true;
            const float floor = lot_.elevationAt(level, cx, cy), top = world.clearanceTop(cx, cy, level);
            return floor + 3.0f <= top || top <= floor + 0.1f;
        };
        const int W = int(lot_.width), H = int(lot_.height);
        int tx0 = int(std::floor(std::min(ax, bx))), tx1 = int(std::ceil(std::max(ax, bx)));
        int ty0 = int(std::floor(std::min(ay, by))), ty1 = int(std::ceil(std::max(ay, by)));
        if (roof.type >= Lot::DiagonalLongGable && roof.type <= Lot::DiagonalShedHip) {
            // GetMinMaxForDiagonals: the diamond's box.
            const float vx = bx - ax, vy = by - ay, yD = (vy - vx) / 2, dx = vy - yD;
            tx0 = int(std::floor(std::min({ax, ax + dx, bx, ax - yD})));
            tx1 = int(std::ceil(std::max({ax, ax + dx, bx, ax - yD})));
            ty0 = int(std::floor(std::min({ay, ay + dx, by, ay + yD})));
            ty1 = int(std::ceil(std::max({ay, ay + dx, by, ay + yD})));
        }
        // The game's own roof types: the box is the roof's bounds with the
        // eaves, int(min) to int(max - 1) (RoofIntersectWithObjects), the
        // centre covered by Contains and the height its GetElevation(x, y, 0).
        std::optional<roofgame::Roof> game;
        if (roofgame::Roof::supported(roof.type)) {
            game.emplace(roof, lot_.elevationAt(roof.level, std::clamp(roof.x0, 0.0f, float(W)),
                                                std::clamp(roof.y0, 0.0f, float(H))));
            tx0 = int(game->minX), tx1 = int(game->maxX - 1) + 1;
            ty0 = int(game->minY), ty1 = int(game->maxY - 1) + 1;
        }
        for (int tx = std::max(0, tx0); tx < std::min(W, tx1); ++tx)
            for (int ty = std::max(0, ty0); ty < std::min(H, ty1); ++ty) {
                const float cx = float(tx) + 0.5f, cy = float(ty) + 0.5f;
                float top = std::numeric_limits<float>::infinity();
                if (game)
                    top = game->contains(cx, cy) ? game->elevation(cx, cy, 0.0f) : top;
                else
                    top = shape.heightAt(cx, cy, roof.level, top);
                if (top == std::numeric_limits<float>::infinity())
                    continue;
                for (ObjectId id : vm_.objectsOnTile({tx, ty, roof.level + 1}))
                    if (const Object* o = vm_.object(id);
                        o && !o->data[0x22] && o->data[objdata::PlacementFlags] >= 0) {
                        if (clear(cx, cy) || top >= lot_.elevationAt(roof.level, cx, cy) + 3.0f)
                            break; // the first such object decides the tile
                        return 0x7F;
                    }
            }
    }
    // The start corner's level below the ground, or its tile over a pool
    // (FUN_001648ac: the tile a level down is a pool), 0x1C.
    if (roof.level < 0)
        return 0x1C;
    if (roof.level == 1 && poolTile(int(std::floor(ax)), int(std::floor(ay))))
        return 0x1C;
    return 0;
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
    Lot::Roof roof;
    for (const auto& o : lot_.roofs)
        roof.id = std::max(roof.id, o.id + 1);
    roof.id = std::max(roof.id, 1u);
    roof.x0 = float(x0), roof.y0 = float(y0), roof.x1 = float(x1), roof.y1 = float(y1);
    roof.level = roof.levelTo = li;
    roof.type = type;
    roof.pattern = lot_.roofs.empty() ? 0 : lot_.roofs.back().pattern;
    roof.angle = 0.5f;
    if (const int e = roofError(roof); e) {
        r.error = e;
        r.message = e == 0x10   ? "cannot put a roof there: too big"
                    : e == 0x7E ? "cannot put a roof there: a corner is off the editable lot"
                    : e == 0x7F ? "cannot put a roof there: it would cut an object"
                                : "cannot put a roof there: over a pool";
        return r;
    }
    Geometry before = capture();
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
    if (!canAfford(vm_, r.cost)) {
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
    chargeFunds(vm_, -r.cost);
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

} // namespace os2::vm
