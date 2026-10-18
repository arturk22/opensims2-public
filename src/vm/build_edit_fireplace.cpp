// The fireplace tool (build_edit.hpp; docs/buy-build-mac.md §7.26):
// cTSUserToolFireplace::TryPlace's chimney and SubmitUndoableForPlace.

#include "build_edit.hpp"
#include "build_edit_internal.hpp"
#include "formats/lot_roof.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace os2::vm {

using namespace build_edit_internal;

namespace {

// FUN_00163808: the highest level a lot may have (lot level 4, as the
// dormer tool takes it: inferred).
constexpr int kTopLotLevel = 4;
// cWorldDB::StandardLevelHeight.
constexpr float kStory = 3.0f;

// FUN_0015d2f0: the quarter of its tile a point is in (1 west, 2 north,
// 4 east, 8 south; the tile's centre is in the north one).
int quarterOf(float x, float y) {
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    if (fx <= fy)
        return 1.0f - fx <= fy ? 2 : 1;
    return 1.0f - fx <= fy ? 4 : 8;
}

// cWorldRoof::RoofHeightAt (BV 0xc23a18): over every level, a roof at the
// point (IsAnyRoofAtLocation) gives its elevation there; the highest, 0
// without one. (The diagonal and pagoda roofs, not in the game's world
// here, are left out.)
float roofHeightAt(const roofgame::World& w, float x, float y) {
    float top = 0;
    for (const auto& r : w.roofs())
        if (r.contains(x, y))
            top = std::max(top, r.elevation(x, y, 0));
    return top;
}

} // namespace

BuildEditor::ChimneyPlan BuildEditor::chimneyPlan(ObjectId mantleId, const Position& at, int direction) const {
    ChimneyPlan plan;
    if (!vm_.object(mantleId)) {
        plan.error = -1, plan.why = "no mantle";
        return plan;
    }
    // FUN_002035cc on the mantle: a stove (type 4) has its pipe over itself,
    // the others the chimney on the tile behind (the facing turned round:
    // WorldOrientation 0 -> y - 1, 2 -> x + 1, 4 -> y + 1, 6 -> x - 1 in the
    // binary's terms, behind it in ours).
    const auto links = fireplaceLinks(vm_, mantleId);
    const bool stove = links && links->type == 4;
    const auto behind = stove ? std::array<int, 2>{0, 0} : directionStep(direction + 4);
    const float cx = at.x + float(behind[0]), cy = at.y + float(behind[1]);
    const int mtx = int(std::floor(at.x)), mty = int(std::floor(at.y));
    const int L0 = at.level;                              // the mantle's story (VM level)
    const int topVm = lot_.minLevel + lot_.levelCount;    // the lot's top story (VM level)
    // SampledFloorElevation; a story the lot has not got yet stands a
    // StandardLevelHeight over the one under it (inferred: the level the
    // tool would make).
    auto floorAt = [&](int vmLevel, float x, float y) {
        int li = vmLevel - 1;
        float extra = 0;
        while (!lot_.hasLevel(li) && li > lot_.minLevel)
            --li, extra += kStory;
        const float W = float(lot_.width), H = float(lot_.height);
        return lot_.elevationAt(li, std::clamp(x, 0.0f, W), std::clamp(y, 0.0f, H)) + extra;
    };
    // The top story: up from the mantle's while the mantle's place has
    // support (HasSupport of the floor tile FUN_0015d84c makes of the point:
    // its tile and the one quarter the point is in, the north one for a
    // tile's centre), no higher than the lot's top.
    const int quarter = quarterOf(at.x, at.y);
    int level = L0;
    while (level <= topVm) {
        if (!hasSupport(level, mtx, mty, quarter)) {
            --level;
            break;
        }
        ++level;
    }
    const int T = std::max(std::min(level, topVm), L0);
    int prev = -1; // the last piece placed (the one above the next), kMantleLink for the mantle
    auto add = [&](ChimneyPiece::Kind kind, int vmLevel, float elevation) {
        ChimneyPiece p;
        p.kind = kind;
        p.at = Position{cx, cy, vmLevel};
        p.at.elevation = elevation;
        p.above = prev;
        plan.pieces.push_back(p);
        const int i = int(plan.pieces.size()) - 1;
        if (prev >= 0)
            plan.pieces[size_t(prev)].below = i;
        else if (prev == kMantleLink)
            plan.mantleBelow = i;
        return i;
    };
    int below = T; // the story of the first hearth piece (TryPlace's local_134)
    if (T <= L0) {
        // One story: the cap with the footprint on the story over it, or a
        // cap at the story's height when no level can be made; the story
        // must be a full one.
        const float rise = floorAt(T + 1, cx, cy) - floorAt(T, cx, cy);
        if (rise < kStory) {
            plan.error = -1, plan.why = "the story is too low for the chimney";
            return plan;
        }
        if (T < topVm) {
            prev = add(ChimneyPiece::TopWithFootprint, T + 1, 0);
        } else if (T - 1 < kTopLotLevel) {
            plan.newLevel = true;
            prev = add(ChimneyPiece::TopWithFootprint, T + 1, 0);
        } else {
            prev = add(ChimneyPiece::Top, T, rise);
        }
    } else {
        // Over the top story the caps, a unit apart, from
        // floor(max(2, roof - floor + 2)) down to 0 (the cap with the
        // footprint at 0).
        float h = std::max(2.0f, roofHeightAt(LotRoofs(lot_).world(), cx, cy) - floorAt(T, cx, cy) + 2.0f);
        for (h = std::floor(h); h >= 0.0f; h -= 1.0f)
            prev = add(roofgame::nearlyEqual(h, 0.0f) ? ChimneyPiece::TopWithFootprint : ChimneyPiece::Top, T, h);
        below = T - 1;
    }
    // The hearth pieces, down to the ground (a stove's pipes down to the
    // story over it); a stove whose pipe has none links to its lowest cap.
    const int bottom = stove ? L0 + 1 : 1;
    if (stove && below < bottom) {
        plan.mantleAbove = prev;
        if (prev >= 0)
            plan.pieces[size_t(prev)].below = kMantleLink;
    }
    const auto* bearing = [&]() -> const std::vector<std::array<uint16_t, 4>>* {
        const auto it = lot_.roomLayers.find(0x0A);
        return it == lot_.roomLayers.end() || it->second.empty() ? nullptr : &it->second;
    }();
    const LotRoofs roofs(lot_);
    const int tx = int(std::floor(cx)), ty = int(std::floor(cy));
    for (int lvl = below; lvl >= bottom; --lvl) {
        // Under the mantle's story the chimney stops where its tile is in a
        // load-bearing room (cPartitionManager 0x13c, definition 1: 3ARY
        // 0x0A; any quarter, inferred) or under a roof.
        if (lvl < L0) {
            bool inside = false;
            if (bearing && tx >= 0 && ty >= 0 && tx < int(lot_.width) && ty < int(lot_.height) && lot_.hasLevel(lvl - 1)) {
                const size_t i = (size_t(lvl - 1 - lot_.minLevel) * lot_.width + size_t(tx)) * lot_.height + size_t(ty);
                if (i < bearing->size())
                    for (uint16_t q : (*bearing)[i])
                        inside = inside || q != 0;
            }
            if (inside || roofs.world().anyRoofAt(cx, cy, lvl - 1))
                break;
        }
        ChimneyPiece::Kind kind = ChimneyPiece::Hearth;
        if (lvl == 1 && lvl != L0) {
            // HasFoundation (a quarter of the tile a foundation, type 2) or
            // FUN_0027741c (the ground floor not a full story over the
            // ground there).
            bool foundation = false;
            if (!lot_.deckFoundationStyles.empty())
                for (int q = 0; q < 4; ++q)
                    foundation = foundation || deckFoundationType(lot_.deckFoundationStyle(0, tx, ty, q)) == kDeckTypeFoundation;
            const bool raised = !roofgame::nearlyEqual(floorAt(2, cx, cy) - floorAt(1, cx, cy), kStory);
            if (foundation || raised)
                kind = ChimneyPiece::Foundation;
        }
        const int i = add(kind, lvl, 0);
        if ((stove ? 1 : 0) + L0 == lvl) {
            plan.pieces[size_t(i)].below = kMantleLink;
            plan.mantleAbove = i;
            prev = kMantleLink;
        } else {
            prev = i;
        }
    }
    return plan;
}

BuildEditor::Result BuildEditor::placeFireplace(const BuildFireplace& product, ObjectId mantle, TilePos tile, int direction,
                                                std::vector<ObjectId>* placed) {
    Result r;
    auto fail = [&](int code, const std::string& why) {
        r.error = code;
        r.message = "cannot place the fireplace: " + why;
        return r;
    };
    // The mantle by the object tool's TryPlace (the position tests).
    const Placement p = checkPlacement(vm_, mantle, tile, direction, false);
    if (!p.ok)
        return fail(p.error, p.reason);
    // AreObjectBoundsInSameRoom (BV 0x277908): a multi-tile mantle's parts in
    // one room (the tool's error 3).
    {
        std::set<int> rooms;
        for (const auto& part : p.parts)
            if (vm_.world)
                rooms.insert(vm_.world->room(part.tile));
        if (rooms.size() > 1)
            return fail(0, "its parts are not in one room (the tool's error 3)");
    }
    const Position at{float(tile.x) + 0.5f, float(tile.y) + 0.5f, tile.level};
    // The pieces' definitions.
    auto definitionOf = [&](ChimneyPiece::Kind k) -> std::optional<std::pair<uint32_t, uint16_t>> {
        const std::string& name = k == ChimneyPiece::Hearth ? product.hearth
                                  : k == ChimneyPiece::Top ? product.top
                                  : k == ChimneyPiece::TopWithFootprint ? product.topWithFootprint
                                                                        : product.foundation;
        return name.empty() ? std::nullopt : vm_.code().definitionByName(name);
    };
    // Every part of `id` where it stands (its exact place and elevation),
    // to put it back.
    auto whereNow = [&](ObjectId id) {
        Placement now = currentPlacement(vm_, id);
        for (auto& part : now.parts)
            if (const Object* o = vm_.object(part.id))
                part.at = o->position;
        return now;
    };
    Geometry before = capture();
    std::vector<ObjectId> pieces;
    std::vector<std::pair<ObjectId, Placement>> taken; // the pieces a rebuilt chimney top took away
    std::vector<std::array<int, 5>> relinked;          // the chimney's own pieces linked again
    std::optional<Lot> levelsBefore;
    auto revert = [&](const std::string& why, int code) {
        for (const auto& l : relinked)
            for (ObjectId part : groupOf(vm_, ObjectId(l[0])))
                linkFireplaceObject(vm_, part, ObjectId(l[1]), ObjectId(l[2]));
        for (ObjectId id : pieces)
            deleteGroup(vm_, id);
        pickUp(vm_, mantle);
        for (const auto& [id, where] : taken)
            applyPlacement(vm_, id, where);
        if (levelsBefore)
            lot_ = *levelsBefore;
        return fail(code, why);
    };
    // TryPlaceObject (BV 0x279d40): a piece made from its product OBJD,
    // tested where it goes at its elevation (TrySetPosition: over the floor
    // there) and put there; with `hearth` FUN_0027664c on its tiles (on the
    // ground not over a pool; nothing there without od 42 bit 0x01), which
    // TryPlace asks of the hearth pieces alone.
    auto placePiece = [&](const ChimneyPiece& piece, int dir, bool hearth, std::string& why, int& code) -> ObjectId {
        const auto def = definitionOf(piece.kind);
        ObjectId id = 0;
        try {
            id = createGhost(vm_, def->first, def->second);
        } catch (const std::exception& e) {
            why = e.what(), code = 0;
            return 0;
        }
        pieces.push_back(id);
        const TilePos t{int(std::floor(piece.at.x)), int(std::floor(piece.at.y)), piece.at.level};
        Placement pp = checkPlacement(vm_, id, t, dir, false, &piece.at);
        if (!pp.ok) {
            why = "a chimney piece: " + pp.reason, code = pp.error;
            return 0;
        }
        for (auto& part : pp.parts)
            if (part.at && lot_.hasLevel(part.at->level - 1)) {
                part.at->elevation = piece.at.elevation;
                part.at->height = lot_.elevationAt(part.at->level - 1, part.at->x, part.at->y) + piece.at.elevation;
            }
        applyPlacement(vm_, id, pp);
        if (hearth)
            for (const auto& part : pp.parts) {
                const TilePos u = part.tile;
                if (u.level == 1 && vm_.world && vm_.world->pool(u)) {
                    why = "over a pool", code = 0x8F;
                    return 0;
                }
                for (ObjectId other : vm_.objectsOnTile(u))
                    if (const Object* x = vm_.object(other); x && !x->person && !(x->data[objdata::PlacementFlags] & 0x01)) {
                        why = "\"" + (x->definition ? x->definition->fileName : std::string("?")) + "\" is on its tile";
                        code = 0x0C;
                        return 0;
                    }
            }
        return id;
    };
    auto finish = [&]() {
        // The price: every undoable's (the objects' values). The pieces'
        // OBJDs cost nothing; the mantle's value is the product's price (the
        // saved mantles hold it, their OBJDs 0; where the tool sets it is not
        // read: inferred).
        int cost = 0;
        if (Object* o = vm_.object(mantle)) {
            if (product.price > 0)
                o->data[objdata::CurrentValue] = int16_t(std::min(product.price, 32767));
            cost += o->data[objdata::CurrentValue];
        }
        for (ObjectId id : pieces)
            if (const Object* o = vm_.object(id))
                cost += o->data[objdata::CurrentValue];
        if (!canAfford(vm_, cost))
            return revert("not enough money (" + money(cost) + ")", 0x16);
        runUserPlacement(vm_, mantle);
        for (ObjectId id : pieces)
            runUserPlacement(vm_, id);
        chargeFunds(vm_, -cost);
        for (ObjectId part : groupOf(vm_, mantle))
            vm_.startMain(part);
        for (ObjectId id : pieces)
            for (ObjectId part : groupOf(vm_, id))
                vm_.startMain(part);
        computeLotData(vm_);
        if (placed) {
            placed->push_back(mantle);
            placed->insert(placed->end(), pieces.begin(), pieces.end());
        }
        // One undoable change: the pieces taken away stay out of the world
        // for its undo, the links of the chimney's own pieces go back.
        std::vector<Sold> created{{mantle, whereNow(mantle)}}, gone;
        for (ObjectId id : pieces)
            created.push_back({id, whereNow(id)});
        for (const auto& [id, where] : taken)
            gone.push_back({id, where});
        for (auto& l : relinked)
            if (const auto now = fireplaceLinks(vm_, ObjectId(l[0])))
                l[3] = now->below, l[4] = now->above;
        changed();
        commit(std::move(before), cost, std::move(gone), std::move(created));
        undo_.back().links = relinked;
        r.ok = true;
        r.cost = cost;
        r.changed = int(pieces.size());
        r.message = product.name + " placed with " + std::to_string(pieces.size()) + " chimney pieces for " + money(cost);
        return r;
    };
    // LinkOneObject on every part of `id` (LinkObjects(true)).
    auto link = [&](ObjectId id, ObjectId below, ObjectId above) {
        for (ObjectId part : groupOf(vm_, id))
            linkFireplaceObject(vm_, part, below, above);
    };

    // A multi-tile mantle with a chimney behind it (IsChimneyBehindHeldMantle,
    // BV 0x278a34: a hearth or a cap piece on the tile behind one of its
    // parts) goes in front of that chimney (OnRelease 0x27e0ac).
    const auto back = directionStep(p.direction + 4);
    auto behind = [&](TilePos t) { return TilePos{t.x + back[0], t.y + back[1], t.level}; };
    bool chimneyBehind = false;
    if (groupOf(vm_, mantle).size() > 1)
        for (const auto& part : p.parts)
            for (ObjectId id : vm_.objectsOnTile(behind(part.tile)))
                if (const auto l = fireplaceLinks(vm_, id); l && (l->type == 2 || l->type == 3))
                    chimneyBehind = true;
    if (chimneyBehind) {
        // IsValidChimneyBehindHeldMantle (BV 0x2784a8): behind each part one
        // piece, a hearth or the cap at the story's height (elevation 0),
        // and none with a mantle under it (error 6); else error 5. The
        // piece behind the first part is the one the mantle goes under.
        ObjectId piece = -1;
        int parts = 0, valid = 0;
        for (const auto& part : p.parts) {
            for (ObjectId id : vm_.objectsOnTile(behind(part.tile))) {
                const auto l = fireplaceLinks(vm_, id);
                const Object* x = vm_.object(id);
                if (!l || !x || !(l->type == 2 || (l->type == 3 && roofgame::nearlyEqual(x->position.elevation, 0.0f))))
                    continue;
                if (parts == 0 && groupOf(vm_, id).size() > 1)
                    piece = multiTileGroup(*x);
                if (const auto under = l->below != -1 ? fireplaceLinks(vm_, l->below) : std::nullopt; under && under->type == 1)
                    return fail(0, "a mantle is in front of that chimney already (the tool's error 6)");
                ++valid;
            }
            ++parts;
        }
        if (valid != parts || piece == -1)
            return fail(0, "the chimney behind does not fit the mantle (the tool's error 5)");
        // CreateClonedUndoableListFromIDLinkedList (BV 0x27d3ac) makes the
        // chain again (each piece a copy, its undo's) and turns the copies
        // to the mantle's facing: a chimney facing another way is not done.
        if (const Object* x = vm_.object(piece); !x || x->data[objdata::Direction] != p.direction)
            return fail(0, "the chimney behind faces another way (not done)");
        const auto pl = fireplaceLinks(vm_, piece);
        applyPlacement(vm_, mantle, p);
        ObjectId under = pl->below, over = piece;
        auto keep = [&](ObjectId id) {
            if (const auto l = id != -1 ? fireplaceLinks(vm_, id) : std::nullopt)
                relinked.push_back({id, l->below, l->above, l->below, l->above});
        };
        keep(under);
        if (pl->type != 3)
            keep(over);
        if (pl->type == 3) {
            // The cap at the story's height (a story built over a chimney's
            // top): it and the caps over it go, and the chimney is made again
            // behind the mantle (CreateUndoableListBehindObjectHeld, BV
            // 0x27cab8: TryPlace's caps over the top story, then a hearth
            // piece on each story down to the mantle's, no foundation and no
            // FUN_0027664c), the mantle under its story's hearth.
            for (ObjectId x = piece; x > 0 && vm_.object(x) && taken.size() < 64;) {
                const auto l = fireplaceLinks(vm_, x);
                taken.emplace_back(x, whereNow(x));
                x = l ? l->above : -1;
            }
            for (const auto& [id, where] : taken)
                pickUp(vm_, id);
            ChimneyPlan plan = chimneyPlan(mantle, at, p.direction);
            if (plan.error)
                return revert(plan.why, 0);
            if (plan.newLevel)
                levelsBefore = lot_, addUpperLevel(lot_);
            ObjectId prev = -1, atMantle = -1;
            for (const ChimneyPiece& cp : plan.pieces) {
                if (cp.at.level < at.level)
                    break;
                if (!definitionOf(cp.kind))
                    return revert("no object for a chimney piece", 0);
                std::string why;
                int code = 0;
                const ObjectId id = placePiece(cp, p.direction, false, why, code);
                if (!id)
                    return revert(why, code);
                link(id, -1, prev);
                if (prev != -1)
                    if (const auto lp = fireplaceLinks(vm_, prev))
                        link(prev, id, lp->above);
                prev = id;
                if (cp.at.level == at.level)
                    atMantle = id;
            }
            if (atMantle == -1)
                return revert("no hearth piece on the mantle's story", 0);
            over = atMantle;
            link(over, -1, fireplaceLinks(vm_, over)->above);
        }
        // The mantle under the piece behind it, over the one under that.
        link(mantle, under, over);
        if (under != -1)
            if (const auto lu = fireplaceLinks(vm_, under))
                link(under, lu->below, mantle);
        link(over, mantle, fireplaceLinks(vm_, over)->above);
        return finish();
    }

    ChimneyPlan plan = chimneyPlan(mantle, at, p.direction);
    if (plan.error)
        return fail(0, plan.why);
    for (const ChimneyPiece& piece : plan.pieces)
        if (!definitionOf(piece.kind))
            return fail(0, "no object for a chimney piece");
    if (plan.newLevel)
        levelsBefore = lot_, addUpperLevel(lot_);
    applyPlacement(vm_, mantle, p);
    for (const ChimneyPiece& piece : plan.pieces) {
        std::string why;
        int code = 0;
        if (!placePiece(piece, p.direction, piece.kind == ChimneyPiece::Hearth || piece.kind == ChimneyPiece::Foundation, why, code))
            return revert(why, code);
    }
    // LinkObjects(true), twice (the loop of BV 0x27a014): every part of each.
    auto idOf = [&](int l) -> ObjectId { return l == kMantleLink ? mantle : l >= 0 ? pieces[size_t(l)] : ObjectId(-1); };
    for (int pass = 0; pass < 2; ++pass) {
        link(mantle, idOf(plan.mantleBelow), idOf(plan.mantleAbove));
        for (size_t i = 0; i < pieces.size(); ++i)
            link(pieces[i], idOf(plan.pieces[i].below), idOf(plan.pieces[i].above));
    }
    return finish();
}

BuildEditor::Result BuildEditor::deleteFireplace(ObjectId part) {
    Result r;
    const Object* o = vm_.object(part);
    if (!o || !o->position.inWorld() || !isFireplaceObject(vm_, part, true)) {
        r.message = "no fireplace there";
        return r;
    }
    Geometry before = capture();
    std::vector<Sold> sold;
    std::set<ObjectId> done;
    for (ObjectId id : linkedFireplace(vm_, multiTileGroup(*o))) {
        const Object* x = vm_.object(id);
        if (!x || !x->position.inWorld() || !done.insert(multiTileGroup(*x)).second)
            continue;
        const ObjectId lead = multiTileGroup(*x);
        Sold s{lead, currentPlacement(vm_, lead)};
        for (auto& q : s.where.parts)
            if (const Object* y = vm_.object(q.id))
                q.at = y->position;
        r.cost -= sell(vm_, lead);
        sold.push_back(std::move(s));
    }
    r.changed = int(sold.size());
    changed();
    commit(std::move(before), r.cost, std::move(sold));
    r.ok = true;
    r.message = "a fireplace of " + std::to_string(r.changed) + " objects sold for " + money(-r.cost);
    return r;
}

} // namespace os2::vm
