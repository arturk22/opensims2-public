// Build mode's stairs: the staircase objects in the world (objectStairs) and
// the modular stairs (planned, placed, deleted, railings, the stairwell
// opened over a staircase). Part of the build editor (build_edit.cpp).

#include "build_edit.hpp"
#include "build_edit_internal.hpp"
#include "formats/log_once.hpp"

#include "lot.hpp"
#include "placement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>

namespace os2::vm {

using namespace build_edit_internal;

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
    auto inLot = [&](const TilePos& t) { return t.x >= 0 && t.y >= 0 && t.x < W && t.y < H && editableTile(t.x, t.y); };
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
    int limit = 0; // FUN_009e9550's height limits (0x65/0x66) when a move failed on them
    auto move = [&](int gx, int gy, float h) {
        if (gx < 0 || gy < 0 || gx > W || gy > H || plan.ground.empty())
            return true;
        float& g = plan.ground[size_t(gx) * rowH + size_t(gy)];
        if (std::fabs(g - h) < 1e-4f)
            return true;
        // FUN_009e9550 with the stairs' flags (the global 0x02c81020, 0x2BB7
        // like the walls': docs/buy-build-mac.md §7.24).
        if (const int e = vertexMoveError(0, gx, gy, h, vertextest::kWalls)) {
            if (e == 0x65 || e == 0x66)
                limit = e;
            return false;
        }
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
    // UserPlaceModularStairsFindEnd (BV 0x9ece38) without a drag: the end
    // point the flight would be dragged to. For each lot level from the
    // top's down, from the tile after the top (z = the top's height), a
    // tile at a time (z falls 4 steps, 0.75, a tile): water 0x1D, a pool
    // 0x1C, the step not placeable there 0x71 end that level's walk; on a
    // tile with a floor on that level, with its corner heights (the near and
    // far side, the far one the higher unless it may be dug to the tile's
    // bottom height), the floor meets the flight in the tile when a near
    // corner is under the tile's top height and the far height above its
    // bottom: on the first tile that is 0x78 (unless the far corners may be
    // dug, then the walk goes on), else the end is the tile's far edge less
    // round((far - bottom) / 0.1875) steps (at most 4; the far edge itself
    // when the floor falls away within the tile) and the tile after it must
    // take the bottom landing (0x71, 0x1D, 0x1C). A tile that is not flat
    // whose far corners cannot be dug gives 0x6F. Walking on: 0x77 if
    // nothing else was met (the first error met is the one reported).
    std::optional<std::array<float, 2>> found = end;
    if (!found) {
        const int ux = int(plan.u[0]), uy = int(plan.u[1]);
        // The far corners (FUN_009ec03c: the tile's corners turned by the direction).
        auto farCorners = [&](const TilePos& t) {
            const int fx = t.x + (ux > 0 ? 1 : 0), fy = t.y + (uy > 0 ? 1 : 0);
            if (ux)
                return std::array<std::array<int, 2>, 2>{{{fx, t.y}, {fx, t.y + 1}}};
            return std::array<std::array<int, 2>, 2>{{{t.x, fy}, {t.x + 1, fy}}};
        };
        auto nearCorners = [&](const TilePos& t) {
            const int nx = t.x + (ux > 0 ? 0 : 1), ny = t.y + (uy > 0 ? 0 : 1);
            if (ux)
                return std::array<std::array<int, 2>, 2>{{{nx, t.y}, {nx, t.y + 1}}};
            return std::array<std::array<int, 2>, 2>{{{t.x, ny}, {t.x + 1, ny}}};
        };
        // FUN_009e9550 (the stairs' flags, 0x2BB7) for a ground grid point
        // going to `h` (upper floors' points are not dug here: inferred). A
        // point already at that height passes the edge test alone.
        auto canMove = [&](int l, const std::array<int, 2>& c, float h) {
            return l == 0 && vertexMoveError(0, c[0], c[1], h, vertextest::kWalls) == 0;
        };
        auto flat = [&](int l, const TilePos& t) {
            const float z0 = lot_.elevation(l, t.x, t.y);
            return std::fabs(lot_.elevation(l, t.x + 1, t.y) - z0) < 0.001f &&
                   std::fabs(lot_.elevation(l, t.x, t.y + 1) - z0) < 0.001f &&
                   std::fabs(lot_.elevation(l, t.x + 1, t.y + 1) - z0) < 0.001f;
        };
        // FUN_009ebdc8: the step object may stand on the tile (here: no wall
        // into it, nothing in the way on that level; inferred).
        auto stepBlocked = [&](const TilePos& prevT, const TilePos& t, int l) {
            return wallAhead(prevT, l) || objectOn({t.x, t.y, l + 1});
        };
        const TilePos first{top.x + ux, top.y + uy, top.level};
        if (objectOn(top))
            return 0x70;
        if (wallAhead(top, li))
            return 0x76;
        if (!inLot(first))
            return 0x74;
        int pending = 0;
        for (int l = li; l >= 0 && !found; --l) {
            float z = plan.topZ;
            bool dug = false;
            TilePos prevT = top;
            for (TilePos t = first; inLot(t); prevT = t, t.x += ux, t.y += uy) {
                const float after = z - 4 * kStepRise;
                if (l == 0 && wet(t.x, t.y)) {
                    pending = pending ? pending : 0x1D;
                    break;
                }
                if (l == 0 && poolTile(t.x, t.y)) {
                    pending = pending ? pending : 0x1C;
                    break;
                }
                if (stepBlocked(prevT, t, l)) {
                    pending = pending ? pending : 0x71;
                    break;
                }
                if (hasFloor(l, t.x, t.y)) {
                    const auto fc = farCorners(t), nc = nearCorners(t);
                    const float near1 = dug ? z : lot_.elevation(l, nc[0][0], nc[0][1]);
                    const float near2 = dug ? z : lot_.elevation(l, nc[1][0], nc[1][1]);
                    const float far1 = lot_.elevation(l, fc[0][0], fc[0][1]), far2 = lot_.elevation(l, fc[1][0], fc[1][1]);
                    float farH;
                    if (far2 <= far1)
                        farH = canMove(l, fc[0], after) ? far2 : far1;
                    else
                        farH = canMove(l, fc[1], after) ? far1 : far2;
                    const bool meets = (near1 < z + 0.001f || near2 < z + 0.001f) && after - 0.001f < farH;
                    if (meets) {
                        const bool firstTile = t.x == first.x && t.y == first.y;
                        bool goOn = false;
                        if (firstTile) {
                            if (!(canMove(l, fc[0], after) && canMove(l, fc[1], after)))
                                return 0x78;
                            goOn = true;
                        } else if (!flat(l, t)) {
                            if (!(canMove(l, fc[0], after) && canMove(l, fc[1], after))) {
                                pending = pending ? pending : 0x6F;
                                break;
                            }
                            goOn = true;
                        }
                        if (goOn) {
                            dug = true;
                        } else {
                            const int k = std::min(4, int(std::lround((farH - after) / kStepRise)));
                            const float cx = float(t.x) + 0.5f, cy = float(t.y) + 0.5f;
                            std::array<float, 2> e{cx + plan.u[0] * 0.5f - plan.u[0] * float(k) * kStepRun,
                                                   cy + plan.u[1] * 0.5f - plan.u[1] * float(k) * kStepRun};
                            if (farH + 0.001f < std::min(near1, near2))
                                e = {cx + plan.u[0] * 0.5f, cy + plan.u[1] * 0.5f};
                            const TilePos b{int(std::floor(e[0] + plan.u[0] * 0.5f)), int(std::floor(e[1] + plan.u[1] * 0.5f)), l + 1};
                            if (!inLot(b))
                                return 0x71;
                            if (l == 0 && wet(b.x, b.y))
                                return 0x1D;
                            if (l == 0 && poolTile(b.x, b.y))
                                return 0x1C;
                            if (objectOn(b))
                                return 0x71;
                            found = e;
                            break;
                        }
                    }
                }
                pending = pending ? pending : 0x77;
                z = after;
            }
        }
        if (!found)
            return pending ? pending : 0x77;
    }
    // The number of steps (UserPlaceModularStairs from the end point).
    int n = 0;
    {
        const std::array<float, 2> endPoint = *found;
        const TilePos et{int(std::floor(endPoint[0])), int(std::floor(endPoint[1])), top.level};
        if (et.x == top.x && et.y == top.y)
            return 0x78;
        if (et.x != top.x && et.y != top.y)
            return 0x71;
        if (directionOf(endPoint[0] - center[0], endPoint[1] - center[1]) != direction)
            return 0x71;
        const float d = std::hypot(endPoint[0] - center[0], endPoint[1] - center[1]);
        n = int(std::ceil((d - 0.5f) * 4.0f - 1e-4f));
        if (wholeTiles && end)
            n = (n + 3) / 4 * 4;
        if (n < 1)
            return 0x78;
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
                        return limit ? limit : 0x6F;
                if (k)
                    for (const auto& c : nearSide)
                        if (!move(c[0], c[1], z + kStepRise * float(k)))
                            return limit ? limit : 0x6F;
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
                    return limit ? limit : 0x6F;
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
    if (!canAfford(vm_, r.cost)) {
        r.error = 0x16;
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    Geometry before = capture();
    // The terrain under the steps and the bottom tile.
    // (ChangeFloorAndWaterElevationCoupled: the stories above go along.)
    for (const auto& [g, h] : plan.terrain)
        moveVertexCoupled(0, g.first, g.second, h);
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
        } catch (const std::exception& err) {
            os2::logOnce("stairs piece", std::to_string(guid), err);
            return 0;
        }
        Placement p;
        p.ok = true;
        p.level = level;
        p.direction = dir;
        Position pos{at[0], at[1], level};
        if (height) {
            pos.height = *height;
            // The saved "elevation" (cTSObject +0x20c, WriteHeader BV
            // 0xb07790) is the float SetPosition was given: for the steps
            // the flight's height itself (UserPlaceModularStairs ->
            // AddObject -> TrySetPosition; docs/buy-build-mac.md §7.24).
            pos.elevation = *height;
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
    chargeFunds(vm_, -r.cost);
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
        // RemoveFlight: each part's delete gives back its value plus its
        // railing (RailingCost: 2x the Railing Tool's price for model 4,
        // 1x for 2-3), read before the part leaves the world.
        const int railing = railingCost(vm_, id);
        if (railing)
            chargeFunds(vm_, railing);
        r.cost -= sell(vm_, id) + railing;
        sold.push_back(std::move(s));
    }
    r.changed = int(flight.size());
    changed();
    commit(std::move(before), r.cost, std::move(sold));
    r.ok = true;
    r.message = "a flight of " + std::to_string(flight.size()) + " objects sold for " + money(-r.cost);
    return r;
}

BuildEditor::Result BuildEditor::changeRailing(ObjectId part, bool side, bool remove) {
    Result r;
    const Object* o = vm_.object(part);
    if (!o || !o->definition || !o->position.inWorld()) {
        r.message = "no staircase there";
        return r;
    }
    std::vector<ObjectId> parts;
    if (auto flight = flightOf(part); !flight.empty()) {
        for (ObjectId id : flight)
            if (const Object* x = vm_.object(id); x && x->definition && x->definition->objectType() - 0xBu < 2)
                parts.push_back(id);
    } else if (o->definition->objectType() - 10u < 3) {
        parts.push_back(part);
    }
    if (parts.empty()) {
        r.message = "no staircase there";
        return r;
    }
    const int price = vm_.railingPrice;
    std::vector<std::array<int, 3>> swaps;
    for (ObjectId id : parts) {
        const Object* x = vm_.object(id);
        int m = 0;
        try {
            if (auto t = vm_.runFunction(id, 48))
                m = t->temps[0]; // ObjectModel: temp 0 whatever the tree returned
        } catch (const VMError&) {
        }
        if (m == 0)
            m = x->definition->objectType() == 10 ? 4 : 1;
        int to = 0;
        if (!remove) {
            if (m == 1)
                to = side ? 3 : 2;
            else if (m != (side ? 3 : 2) && m != 4)
                to = 4;
        } else {
            if (m == 4)
                to = side ? 2 : 3;
            else if (m == (side ? 3 : 2))
                to = 1;
        }
        if (to)
            swaps.push_back({int(id), m, to});
    }
    if (swaps.empty()) {
        r.ok = true;
        r.message = remove ? "no railing on that side" : "the railing is there already";
        return r;
    }
    const int cost = (remove ? -price : price) * int(swaps.size());
    if (!canAfford(vm_, cost)) {
        r.error = 0x16;
        r.message = "not enough money (" + money(cost) + ")";
        return r;
    }
    Geometry before = capture();
    for (const auto& s : swaps)
        setRailingModel(vm_, ObjectId(s[0]), s[2]);
    chargeFunds(vm_, -cost);
    computeLotData(vm_);
    r.cost = cost;
    r.changed = int(swaps.size());
    changed();
    commit(std::move(before), cost);
    undo_.back().models = swaps;
    r.ok = true;
    r.message = std::string(remove ? "railing taken off " : "railing put on ") + std::to_string(swaps.size()) +
                " steps for " + money(cost < 0 ? -cost : cost);
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

} // namespace os2::vm
