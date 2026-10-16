// Build mode's ground: foundations and decks, the terrain tools (raise,
// lower, flatten, smooth, water, paint) and pools. Part of the build editor
// (build_edit.cpp).

#include "build_edit.hpp"
#include "build_edit_internal.hpp"
#include "formats/log_once.hpp"

#include "formats/lot_edit.hpp"
#include "lot.hpp"
#include "placement.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <set>

namespace os2::vm {

using namespace build_edit_internal;

namespace {

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

// The pool's product (deckfoundations.txt "pool 3"): its marker.
constexpr int32_t kPoolProduct = 3;

} // namespace

int deckFoundationType(int32_t style) {
    switch (style) {
    case 0: return kDeckTypeFoundation;
    case 1:
    case 2: return kDeckTypeDeck;
    case kPoolProduct: return kDeckTypePool;
    default: return 0;
    }
}

void BuildEditor::setMarkers(int level, int x0, int y0, int x1, int y1, int32_t style) {
    // A lot saved without the array (N002_Lot32) gets one from what it has:
    // foundations by their heights and the pools (inferred: their product
    // is not known, taken as the foundation's).
    if (lot_.deckFoundationStyles.empty() && lot_.levelCount > 1) {
        std::vector<std::array<int, 2>> found, pools;
        for (int x = 0; x < int(lot_.width); ++x)
            for (int y = 0; y < int(lot_.height); ++y) {
                if (foundationTile(x, y))
                    found.push_back({x, y});
                if (poolTile(x, y))
                    pools.push_back({x, y});
            }
        lot_.deckFoundationBase = lot_.minLevel;
        lot_.deckFoundationStyles.assign(size_t(lot_.width) * lot_.height * size_t(lot_.levelCount - 1), {-1, -1, -1, -1});
        for (const auto& [x, y] : found)
            for (int q = 0; q < 4; ++q)
                setDeckFoundationMarker(lot_, 0, x, y, q, 0);
        for (const auto& [x, y] : pools)
            for (int q = 0; q < 4; ++q)
                setDeckFoundationMarker(lot_, -1, x, y, q, kPoolProduct);
    }
    for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x)
        for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
            for (int q = 0; q < 4; ++q)
                setDeckFoundationMarker(lot_, level, x, y, q, style);
}

bool BuildEditor::foundationTile(int x, int y) const {
    if (!lot_.hasLevel(0) || !lot_.hasLevel(1) || x < 0 || y < 0 || x >= int(lot_.width) || y >= int(lot_.height))
        return false;
    // The ground's markers (3ARY 0x14) when the lot keeps them: a quarter of
    // a foundation or a deck (the game's HasFoundation / the deck's
    // IsDeckFoundationOfType 1). The level 1 grid points one past a
    // foundation are raised too (MoveFloorElevations, docs/buy-build-mac.md
    // §7.25): the heights cannot tell the tile beside it from it.
    if (!lot_.deckFoundationStyles.empty()) {
        for (int q = 0; q < 4; ++q)
            if (const int t = deckFoundationType(lot_.deckFoundationStyle(0, x, y, q));
                t == kDeckTypeFoundation || t == kDeckTypeDeck)
                return true;
        return false;
    }
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
            if (!editableTile(x, y))
                return fail(0x45, "not editable at " + point(x, y));
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
    if (!canAfford(vm_, r.cost)) {
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
    // SetDeckFoundationMarkers: every quarter of the rectangle's tiles, on
    // the ground, the product (docs/buy-build-mac.md §7.25).
    setMarkers(0, ax, ay, bx, by, product);
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
                } catch (const std::exception& err) {
                    os2::logOnce("deck columns", std::to_string(kDeckColumnGuid), err);
                }
            }
    r.changed = int(tiles.size());
    chargeFunds(vm_, -r.cost);
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
    for (const auto& [x, y] : tiles)
        setMarkers(0, x, y, x, y, -1);
    foundationWalls(0, &removed, false);
    r.changed = int(tiles.size());
    r.cost = -refund;
    chargeFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost, std::move(columns));
    r.ok = true;
    r.message = std::to_string(r.changed) + " foundation tiles deleted for " + money(-r.cost);
    return r;
}

int terrainLimitError(float height) {
    if (height < kMinTerrainElevation - 1e-4f)
        return 0x65;
    if (height > kMaxTerrainElevation + 1e-4f)
        return 0x66;
    return 0;
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

int BuildEditor::vertexMoveError(int li, int x, int y, float height, uint32_t mask) const {
    using namespace vertextest;
    const int W = int(lot_.width), H = int(lot_.height);
    // FUN_009e75e0: the point is in the world (WorldDB 0xfc), else 0x59.
    if (!lot_.hasLevel(li) || lot_.elevations.empty() || x < 0 || y < 0 || x > W || y > H)
        return 0x59;
    // Bit 0 (FUN_009e8780): on the world's edge (level 0), or a tile round
    // it off the lot or locked: 0x62.
    const int edge = (mask & Edge) && !editablePoint(x, y, li) ? 0x62 : 0;
    // Already at the height: only the edge test counts.
    if (std::fabs(lot_.elevation(li, x, y) - height) < 1e-4f)
        return edge;
    if (edge)
        return edge;
    if (mask & Limits)
        if (const int e = terrainLimitError(height))
            return e;
    // (The sim system's vtable 0xd0 -- the constrainFloorElevation cheat,
    // inferred -- is on: the tests below run.)
    auto tilesRound = [&](auto&& f) {
        for (int tx = x - 1; tx <= x; ++tx)
            for (int ty = y - 1; ty <= y; ++ty)
                if (tx >= 0 && ty >= 0 && tx < W && ty < H && f(tx, ty))
                    return true;
        return false;
    };
    // Bit 2 (FUN_009e856c): a floor quarter on a tile round it, on its level.
    if ((mask & Floor) && tilesRound([&](int tx, int ty) {
            for (uint16_t q : lot_.floor(li, uint32_t(tx), uint32_t(ty)))
                if (q)
                    return true;
            return false;
        }))
        return 0x5A;
    // Bit 7 (FUN_009e8e0c): the objects round it.
    if (mask & Objects)
        if (const int e = vertexObjectsError(li, x, y, height))
            return e;
    // Bits 4 and 5 (FUN_009e9c98 / 0x9e9a78): the point a story up (down)
    // that the new height would bring within MinLevelHeight + a quarter of
    // FloorElevationUnit must be able to follow, MinLevelHeight away.
    constexpr float kSlack = kFloorElevationUnit * 0.25f;
    if ((mask & Above) && lot_.hasLevel(li + 1) && lot_.elevation(li + 1, x, y) <= height + kMinLevelHeight + kSlack &&
        vertexMoveError(li + 1, x, y, height + kMinLevelHeight, mask & ~Below))
        return 0x69;
    if ((mask & Below) && lot_.hasLevel(li - 1) && lot_.elevation(li - 1, x, y) >= height - kMinLevelHeight - kSlack &&
        vertexMoveError(li - 1, x, y, height - kMinLevelHeight, mask & ~Above))
        return 0x6A;
    // Bit 8 (FUN_009e8048 -> FUN_009e7d08): a partition on one of the four
    // edges from the point, on its level with requiresFlatBottom (walls.txt:
    // walls and attic walls; foundation, deck, pool walls and fences say
    // false) -> 0x5B, on the level below with requiresFlatTop (all walls but
    // the attic's; fences say false) -> 0x5C.
    if (mask & Partitions) {
        auto flatBottom = [](uint32_t t) {
            return !Lot::isFence(t) && t != Lot::Foundation && t != 0x10 && t != 0x18 && t != Lot::DeckSkirt &&
                   t != Lot::Pool;
        };
        auto flatTop = [](uint32_t t) { return !Lot::isFence(t) && t != Lot::Attic; };
        for (const auto& w : lot_.walls) {
            const int ax = int(std::lround(w.a.x)), ay = int(std::lround(w.a.y));
            const int bx = int(std::lround(w.b.x)), by = int(std::lround(w.b.y));
            const bool axisEdge = std::abs(ax - bx) + std::abs(ay - by) == 1;
            if (!axisEdge || !((ax == x && ay == y) || (bx == x && by == y)))
                continue;
            if (w.a.level == li && flatBottom(w.type))
                return 0x5B;
            if (w.a.level == li - 1 && flatTop(w.type))
                return 0x5C;
        }
    }
    // Bit 9 (FUN_009e8268 -> FUN_001648ac): a tile round it over a pool.
    if ((mask & Pool) && li == 0 && tilesRound([&](int tx, int ty) { return poolTile(tx, ty); }))
        return kSilentRefusal;
    // Bit 11 (FUN_009e8ad0): a fence from the point would get steeper than
    // sqrt(3) (60°).
    if (mask & FenceSlope)
        for (const auto& w : lot_.walls) {
            if (!Lot::isFence(w.type) || w.a.level != li)
                continue;
            const int ax = int(std::lround(w.a.x)), ay = int(std::lround(w.a.y));
            const int bx = int(std::lround(w.b.x)), by = int(std::lround(w.b.y));
            int ox = 0, oy = 0;
            if (ax == x && ay == y)
                ox = bx, oy = by;
            else if (bx == x && by == y)
                ox = ax, oy = ay;
            else
                continue;
            const float len = std::hypot(float(ox - x), float(oy - y));
            if (len > 0 && std::fabs(height - lot_.elevation(li, ox, oy)) / len > std::sqrt(3.0f))
                return 0x31;
        }
    // Bit 13 (FUN_009e8124): a tile round it with a deck, foundation or pool
    // marker (DeckFoundationStyle != -1). The lots keep the markers of
    // foundations and decks on level 0, under their raised floor, and the
    // pools' on level -1.
    if ((mask & DeckFoundation) &&
        tilesRound([&](int tx, int ty) { return (li == 0 && foundationTile(tx, ty)) || (li == -1 && poolTile(tx, ty)); }))
        return kSilentRefusal;
    return 0;
}

float BuildEditor::hypotheticalFloor(int li, int vx, int vy, float height, float px, float py) const {
    // nTSWorld::HypotheticalFloorElevation: the floor at (px, py) with grid
    // point (vx, vy) at `height` (the lot's bilinear sampling, as
    // elevationAt; the game's own sampling is inferred to agree).
    const int ix = int(std::floor(px)), iy = int(std::floor(py));
    auto at = [&](int gx, int gy) { return gx == vx && gy == vy ? height : lot_.elevation(li, gx, gy); };
    const float fx = px - float(ix), fy = py - float(iy);
    const float a = at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx;
    const float b = at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx;
    return a * (1 - fy) + b * fy;
}

int BuildEditor::vertexObjectsError(int li, int x, int y, float height) const {
    // FUN_009e8e0c (the vertex test's bit 7, docs/buy-build-mac.md §7.25), on
    // each of the four tiles round the point on its level, on the lot:
    const int W = int(lot_.width), H = int(lot_.height);
    // ... the story under it would be left lower than StandardLevelHeight
    // (the point less than 3 over the same point a level down).
    const bool lowCeiling =
        li > lot_.minLevel && height - lot_.elevation(li - 1, x, y) + 0.001f < kStandardLevelHeight;
    // OS2_DEBUG_VERTEX=1: which object refused which point.
    auto refuse = [&](int code, const Object& o) {
        if (std::getenv("OS2_DEBUG_VERTEX"))
            std::fprintf(stderr, "vertex (%d,%d) level %d to %.3f: %#x by object %u %s\n", x, y, li, height, unsigned(code),
                         unsigned(o.id), o.definition ? o.definition->fileName.c_str() : "?");
        return code;
    };
    const auto here = vm_.objectsAround({x - 1, y - 1, li + 1}, 1);
    const auto below = lowCeiling ? vm_.objectsAround({x - 1, y - 1, li}, 1) : decltype(here){};
    for (int tx = x - 1; tx <= x; ++tx)
        for (int ty = y - 1; ty <= y; ++ty) {
            if (tx < 0 || ty < 0 || tx >= W || ty >= H)
                continue;
            const size_t k = size_t(tx - (x - 2)) * 3 + size_t(ty - (y - 2));
            // HasGroundPinningObjects (BV 0x16c5b4; the flags' bit 12 is
            // clear in 0x2BB7 and 0x2BF7, so FlattensTerrainOnPlace is not
            // asked): a person, or an object without placement flags (od 42)
            // 0x100 -> 0x61 "Can't move occupied terrain".
            for (ObjectId id : here[k])
                if (const Object* o = vm_.object(id); o && (o->person || !(o->data[objdata::PlacementFlags] & 0x100)))
                    return refuse(0x61, *o);
            // HasNonCeilingConformingObjects (0x16c7a8) on the tile a level
            // down: an object that is none of three kinds of the selector
            // service and does not ConformsToCeiling (a property of its
            // model's parts, interface 0xaae90adb, not read) -> 0x60. Here
            // every object but a support (OBJD type 5, the deck columns)
            // fails it (inferred).
            if (lowCeiling)
                for (ObjectId id : below[k])
                    if (const Object* o = vm_.object(id);
                        o && !(o->definition && o->definition->objectType() == kObjectTypeSupport))
                        return refuse(0x60, *o);
            // An object with od 42 bit 0x80 that the floor would rise over
            // -> 0x68 "Can't raise terrain above stairs": a modular step
            // (OBJD type 11, 12) by the four corners of its footprint for its
            // facing (the two of its high side a FloorElevationUnit up),
            // another object at its position.
            for (ObjectId id : here[k]) {
                const Object* o = vm_.object(id);
                if (!o || !(o->data[objdata::PlacementFlags] & 0x80))
                    continue;
                const float ox = o->position.x, oy = o->position.y;
                // Its elevation (vtable 0xb0: the float +0x20c, the saved
                // state's "elevation": a modular step's height, 0 for most).
                const float oz = o->position.elevation;
                const uint16_t type = o->definition ? o->definition->objectType() : 0;
                if (type == kObjectTypeModularStairs || type == kObjectTypeModularStairsPortal) {
                    static const float kCorners[4][4][2] = {
                        {{-0.5f, 0}, {-0.5f, -0.25f}, {0.5f, -0.25f}, {0.5f, 0}},  // facing 0
                        {{0, -0.5f}, {0.25f, -0.5f}, {0.25f, 0.5f}, {0, 0.5f}},   // 2
                        {{0.5f, 0}, {0.5f, 0.25f}, {-0.5f, 0.25f}, {-0.5f, 0}},   // 4
                        {{0, 0.5f}, {-0.25f, 0.5f}, {-0.25f, -0.5f}, {0, -0.5f}}, // 6
                    };
                    const int dir = o->data[objdata::Direction];
                    if (dir < 0 || dir > 6 || dir % 2)
                        continue;
                    for (int i = 0; i < 4; ++i) {
                        const float px = ox + kCorners[dir / 2][i][0], py = oy + kCorners[dir / 2][i][1];
                        const float pz = oz + (i == 1 || i == 2 ? kFloorElevationUnit : 0.0f);
                        if (px >= float(tx) && px <= float(tx + 1) && py >= float(ty) && py <= float(ty + 1) &&
                            hypotheticalFloor(li, x, y, height, px, py) > pz)
                            return refuse(0x68, *o);
                    }
                } else if (hypotheticalFloor(li, x, y, height, ox, oy) > oz) {
                    return refuse(0x68, *o);
                }
            }
        }
    return 0;
}

void BuildEditor::moveVertexCoupled(int li, int x, int y, float height) {
    const int W = int(lot_.width), H = int(lot_.height);
    if (!lot_.hasLevel(li) || x < 0 || y < 0 || x > W || y > H || lot_.elevations.empty())
        return;
    const size_t plane = size_t(W + 1) * size_t(H + 1);
    const size_t i = size_t(li - lot_.minLevel) * plane + size_t(x) * size_t(H + 1) + size_t(y);
    const float d = height - lot_.elevations[i];
    for (int k = li; lot_.hasLevel(k); ++k)
        lot_.elevations[i + size_t(k - li) * plane] += d;
}

bool BuildEditor::levelForWall(int li, const std::vector<std::array<int, 2>>& points, float height, bool flatTop,
                               int* code) {
    const int W = int(lot_.width), H = int(lot_.height);
    auto fail = [&](int c) {
        if (code)
            *code = c == kSilentRefusal ? 0 : c;
        return false;
    };
    if (lot_.elevations.empty() || !lot_.hasLevel(li))
        return true;
    std::vector<std::array<float, 4>> moves; // level, x, y, height
    for (const auto& [x, y] : points) {
        if (x < 0 || y < 0 || x > W || y > H)
            continue;
        // FUN_009a5a68: a point under water refuses (IsWaterVertex, 0x47),
        // even when it is at the height already.
        if (li == 0 && lot_.waterLevel(x, y) > lot_.elevation(0, x, y) + 1e-4f)
            return fail(0x47);
        if (const int e = vertexMoveError(li, x, y, height, vertextest::kWalls))
            return fail(e);
        moves.push_back({float(li), float(x), float(y), height});
        // FUN_009a79d0: with requiresFlatTop the point a story up (when the
        // lot has it) goes to the height + StandardLevelHeight, once the
        // coupled move has taken it along.
        if (flatTop && lot_.hasLevel(li + 1)) {
            const float above = lot_.elevation(li + 1, x, y) + (height - lot_.elevation(li, x, y));
            if (std::fabs(above - (height + 3.0f)) > 1e-4f) {
                if (const int e = vertexMoveError(li + 1, x, y, height + 3.0f, vertextest::kWalls))
                    return fail(e);
                moves.push_back({float(li + 1), float(x), float(y), height + 3.0f});
            }
        }
    }
    for (const auto& m : moves)
        moveVertexCoupled(int(m[0]), int(m[1]), int(m[2]), m[3]);
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
    if (tx < 0 || ty < 0 || tx >= W || ty >= H || !editableTile(tx, ty) || !terrainFree(tx, ty) || !terrainFree(tx + 1, ty + 1)) {
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
            if (!p)
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
            // ChangeTerrainElevation (BV 0x4010e0): FUN_009e9550 with the
            // brush's flags (the global 0x02bafd10, 0x2BF7, set by a static
            // initializer: docs/buy-build-mac.md §7.24) -- the edge and locked
            // tiles, the height limits (0x65/0x66, kept as the stroke's
            // error), a floor round the point, the stories above, a wall that
            // needs a flat bottom there, pools, foundations and decks round
            // it, fences turned too steep; a point that may not go stays (was
            // the inferred "no floor, foundation, wall or fence end round it").
            if (const int e = vertexMoveError(0, x, y, n, vertextest::kBrush)) {
                if (e == 0x65 || e == 0x66)
                    r.error = e;
                continue;
            }
            changes.push_back({i, n});
            moved += std::fabs(n - h);
        }
    // UpdateFundsForHeightChange: the stroke's total change, rounded, x §1;
    // what it adds is paid now, refused when the funds are short.
    const int owed = int(std::lround((st.moved + moved) * 1.0f)) - st.paid;
    if (!canAfford(vm_, owed)) {
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
    chargeFunds(vm_, -owed);
    r.ok = !changes.empty() || !r.error;
    r.cost = owed;
    r.changed = int(changes.size());
    if (r.changed)
        ++revision_;
    r.message = std::to_string(r.changed) + " terrain points moved" + (owed ? " for " + money(owed) : "");
    if (r.error && changes.empty())
        r.message = r.error == 0x65 ? "cannot: can't lower terrain further" : "cannot: can't raise terrain further";
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

int BuildEditor::poolDigError(int x0, int y0, int x1, int y1, std::string* why, int* tiles) const {
    auto fail = [&](int code, const std::string& text) {
        if (why)
            *why = text;
        return code;
    };
    if (!lot_.hasLevel(0) || lot_.elevations.empty())
        return fail(0x8A, "no terrain");
    const int W = int(lot_.width), H = int(lot_.height);
    // A corner off the lot refuses the drag (the cursor's square goes red).
    for (int x : {x0, x1})
        for (int y : {y0, y1})
            if (x < 0 || y < 0 || x >= W || y >= H)
                return fail(0x02, "out of the lot");
    // The tile where the drag started already has a deck, foundation or
    // pool (IsDeckFoundationOfType 2, 1, 4): 0x86 (§7.18).
    if (poolTile(x0, y0) || foundationTile(x0, y0))
        return fail(0x86, "the drag starts on a pool or a foundation");
    const int ax = std::max(0, std::min(x0, x1)), bx = std::min(W - 1, std::max(x0, x1));
    const int ay = std::max(0, std::min(y0, y1)), by = std::min(H - 1, std::max(y0, y1));
    // Tiles of a pool already there are skipped: a rectangle over or next to
    // a pool extends it (the deck tool's CanPlaceTile lets a pool tile go
    // beside a pool), which gives pools that are not rectangles; the pool
    // keeps the ground height it has (inferred).
    int fresh = 0;
    for (int x = ax; x <= bx; ++x)
        for (int y = ay; y <= by; ++y) {
            if (poolTile(x, y))
                continue;
            // The binary's codes (cTSUserToolDeckFoundation, docs/buy-build-mac.md
            // §7.18): a corner of the drag locked is 0x02 (MayPlaceQuadrilateral),
            // a tile inside whose ground cannot be moved 0x81 "Can't level terrain".
            if (!editableTile(x, y))
                return fail((x == x0 || x == x1) && (y == y0 || y == y1) ? 0x02 : 0x81, "not editable at " + point(x, y));
            ++fresh;
            if (foundationTile(x, y)) // a deck or foundation there: 0x86 (IsDeckFoundationOfType)
                return fail(0x86, "pools only go in the terrain");
            for (uint16_t q : lot_.floor(0, uint32_t(x), uint32_t(y)))
                if (q)
                    // A ground floor: its corners cannot be dug, and
                    // MayPlaceQuadrilateral reports every vertex that
                    // FUN_009e9550 refuses as 0x81 "Can't level terrain"
                    // (0x8A is SelectionIsDegenerate's pool above the
                    // ground). Which of the vertex tests refuses is inferred.
                    return fail(0x81, "a floor is on the ground there");
            for (int cx = x; cx <= x + 1; ++cx)
                for (int cy = y; cy <= y + 1; ++cy)
                    if (lot_.waterLevel(cx, cy) > lot_.elevation(0, cx, cy))
                        return fail(0x1E, "water at " + point(x, y)); // IsPartWater (no text in the base game)
            for (ObjectId id : vm_.objectsOnTile({x, y, 1}))
                if (const Object* o = vm_.object(id); o && (o->person || !(o->data[objdata::PlacementFlags] & 0x200)))
                    return fail(0x0C, "something is in the way at " + point(x, y));
        }
    for (const auto& w : lot_.walls) {
        if (w.a.level != 0)
            continue;
        const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
        if (mx > float(ax) + 0.01f && mx < float(bx + 1) - 0.01f && my > float(ay) + 0.01f && my < float(by + 1) - 0.01f)
            return fail(0x46, "a wall is in the way"); // FUN_00352750 (no text in the base game)
    }
    // cWorldPool::AreExpansionBoundsLegal (cIWorldPool slot 0x40, BV
    // 0xc22dfc): every pool whose rectangle meets the drag's R (R grown by
    // 0.01: touching counts) must make one rectangle with R (the bounding
    // box's area = R's + the pool's: no overlap, a whole side shared), and R
    // with those pools may not pass 900 tiles. MayPlaceQuadrilateral (BV
    // 0x352a30) asks it of each pool the drag meets (the tool's list at
    // +0x9c) and refuses with 0x86 "Can't extend pool" (0x02 when a corner
    // is off the lot or locked, tested before here).
    const int rx0 = ax, ry0 = ay, rx1 = bx + 1, ry1 = by + 1;
    {
        const float rArea = float((rx1 - rx0) * (ry1 - ry0));
        float total = rArea;
        for (const auto& p : lot_.pools) {
            if (p.level != 0)
                continue;
            const float px1 = p.x + float(p.width), py1 = p.y + float(p.height);
            if (p.x > float(rx1) + 0.01f || px1 < float(rx0) - 0.01f || p.y > float(ry1) + 0.01f ||
                py1 < float(ry0) - 0.01f)
                continue;
            const float pArea = float(p.width) * float(p.height);
            total += pArea;
            const float bw = std::max(px1, float(rx1)) - std::min(p.x, float(rx0));
            const float bh = std::max(py1, float(ry1)) - std::min(p.y, float(ry0));
            if (bw * bh != rArea + pArea)
                return fail(0x86, "the pool and the drag must make one rectangle");
        }
        if (total > 900.0f)
            return fail(0x86, "a pool of more than 900 tiles");
    }
    if (tiles)
        *tiles = fresh;
    return 0;
}

BuildEditor::Result BuildEditor::drawPool(int x0, int y0, int x1, int y1) {
    Result r;
    auto fail = [&](int code, const std::string& why) {
        r.error = code;
        r.message = "cannot dig a pool: " + why;
        return r;
    };
    {
        std::string why;
        int n = 0;
        if (const int e = poolDigError(x0, y0, x1, y1, &why, &n))
            return fail(e, why);
        if (n == 0) {
            r.ok = true;
            r.message = "the pool is there already";
            return r;
        }
    }
    const int W = int(lot_.width), H = int(lot_.height);
    const int ax = std::min(x0, x1), bx = std::max(x0, x1), ay = std::min(y0, y1), by = std::max(y0, y1);
    const int rx0 = ax, ry0 = ay, rx1 = bx + 1, ry1 = by + 1;
    std::optional<float> poolHeight;
    for (int x = ax - 1; x <= bx + 1 && !poolHeight; ++x)
        for (int y = ay - 1; y <= by + 1 && !poolHeight; ++y)
            if (poolTile(x, y))
                poolHeight = lot_.elevation(0, x, y);
    std::vector<std::array<int, 2>> fresh;
    for (int x = ax; x <= bx; ++x)
        for (int y = ay; y <= by; ++y)
            if (!poolTile(x, y))
                fresh.push_back({x, y});
    const int tiles = int(fresh.size());
    if (!tiles) {
        r.ok = true;
        r.message = "the pool is there already";
        return r;
    }
    r.cost = tiles * kPoolTilePrice;
    if (!canAfford(vm_, r.cost)) {
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
    // The POOL entries as cWorldPool keeps them: the drag's rectangle is a
    // pool of its own (cTSUserToolUndoablePool::TryAddPool only calls
    // AddPool, vtable 0x30: one entry per rectangle; Save, 0xc21154, writes
    // them one by one), next to the pools it extends.
    uint32_t id = 1;
    for (const auto& p : lot_.pools)
        id = std::max(id, p.id + 1);
    std::vector<Lot::SwimmingPool> kept = lot_.pools;
    {
        Lot::SwimmingPool pool;
        pool.id = id++;
        pool.x = float(rx0), pool.y = float(ry0);
        pool.level = 0;
        pool.width = uint32_t(rx1 - rx0), pool.height = uint32_t(ry1 - ry0);
        pool.depth = 0.25f;
        kept.push_back(pool);
    }
    lot_.pools = std::move(kept);
    // The pool's markers sit a level down (SetDeckFoundationMarkers, type 3).
    setMarkers(-1, ax, ay, bx, by, kPoolProduct);
    poolWalls();
    r.changed = tiles;
    chargeFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = "a pool of " + std::to_string(tiles) + " tiles for " + money(r.cost);
    return r;
}

BuildEditor::Result BuildEditor::erasePool(int x0, int y0, int x1, int y1) {
    Result r;
    auto fail = [&](int code, const std::string& why) {
        r.error = code;
        r.message = "cannot erase the pool: " + why;
        return r;
    };
    const int rx0 = std::min(x0, x1), ry0 = std::min(y0, y1), rx1 = std::max(x0, x1) + 1, ry1 = std::max(y0, y1) + 1;
    // cWorldPool::IsQuadLegalForPoolErase (cIWorldPool slot 0x38, BV
    // 0xc22358): R may cross a pool only right through it (the overlap the
    // pool's whole width or whole height), never leaving a strip of it one
    // tile wide. The refusal code is inferred ("Can't delete pool").
    bool any = false;
    for (const auto& p : lot_.pools) {
        const int px0 = int(std::lround(p.x)), py0 = int(std::lround(p.y));
        const int px1 = px0 + int(p.width), py1 = py0 + int(p.height);
        const int ix0 = std::max(px0, rx0), iy0 = std::max(py0, ry0), ix1 = std::min(px1, rx1), iy1 = std::min(py1, ry1);
        if (p.level != 0 || ix0 >= ix1 || iy0 >= iy1)
            continue;
        any = true;
        if (ix1 - ix0 != px1 - px0 && iy1 - iy0 != py1 - py0)
            return fail(0x87, "the drag must cross the pool through");
        if (std::abs(ix0 - px0) == 1 || std::abs(ix1 - px1) == 1 || std::abs(iy0 - py0) == 1 || std::abs(iy1 - py1) == 1)
            return fail(0x87, "it would leave a strip of the pool one tile wide");
    }
    if (!any) {
        r.ok = true;
        r.message = "no pool there";
        return r;
    }
    for (const auto& [id, o] : vm_.objects()) {
        if (!o.position.inWorld())
            continue;
        const TilePos t = VM::tileOf(o);
        const bool in = t.x >= rx0 && t.x < rx1 && t.y >= ry0 && t.y < ry1;
        if (o.person && in && t.level <= 1 && poolTile(t.x, t.y))
            return fail(0x2D, "a Sim is in it");
    }
    Geometry before = capture();
    // QuadListOnPoolErase (slot 0x3c, BV 0xc22810): each pool R crosses gives
    // way to what is left of it outside (one or two rectangles along the axis
    // it is crossed).
    uint32_t nextId = 1;
    for (const auto& p : lot_.pools)
        nextId = std::max(nextId, p.id + 1);
    std::vector<Lot::SwimmingPool> kept;
    for (const auto& p : lot_.pools) {
        const int px0 = int(std::lround(p.x)), py0 = int(std::lround(p.y));
        const int px1 = px0 + int(p.width), py1 = py0 + int(p.height);
        const int ix0 = std::max(px0, rx0), iy0 = std::max(py0, ry0), ix1 = std::min(px1, rx1), iy1 = std::min(py1, ry1);
        if (p.level != 0 || ix0 >= ix1 || iy0 >= iy1) {
            kept.push_back(p);
            continue;
        }
        auto piece = [&](int qx0, int qy0, int qx1, int qy1) {
            if (qx1 <= qx0 || qy1 <= qy0)
                return;
            Lot::SwimmingPool q = p;
            q.id = nextId++;
            q.x = float(qx0), q.y = float(qy0);
            q.width = uint32_t(qx1 - qx0), q.height = uint32_t(qy1 - qy0);
            kept.push_back(q);
        };
        if (ix1 - ix0 == px1 - px0) { // crossed along x: what is left above and below
            piece(px0, py0, px1, iy0);
            piece(px0, iy1, px1, py1);
        } else {
            piece(px0, py0, ix0, py1);
            piece(ix1, py0, px1, py1);
        }
    }
    lot_.pools = std::move(kept);
    r.changed = 0;
    for (int tx = rx0; tx < rx1; ++tx)
        for (int ty = ry0; ty < ry1; ++ty)
            if (!poolTile(tx, ty) && lot_.hasLevel(-1) && lot_.floor(-1, uint32_t(tx), uint32_t(ty))[0]) {
                ++r.changed;
                for (int qq = 0; qq < 4; ++qq)
                    setFloor(lot_, -1, tx, ty, qq, 0);
                setMarkers(-1, tx, ty, tx, ty, -1);
            }
    poolWalls();
    r.cost = -r.changed * int(float(kPoolTilePrice) * kBuildRefundRate);
    chargeFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " pool tiles erased for " + money(-r.cost) + " back";
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
                        setMarkers(-1, tx, ty, tx, ty, -1);
                    }
        }
        poolWalls();
        r.cost = -r.changed * int(float(kPoolTilePrice) * kBuildRefundRate);
        chargeFunds(vm_, -r.cost);
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

} // namespace os2::vm
