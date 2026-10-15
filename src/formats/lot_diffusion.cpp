#include "lot_diffusion.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace os2 {

namespace {

using V3 = std::array<float, 3>;
using V2 = std::array<float, 2>;

// The directions of the floor grid (Mac BV table 0x2747cb8), index d.
constexpr int kDx[8] = {0, -1, -1, -1, 0, 1, 1, 1};
constexpr int kDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// The game's quarter of a point in its tile (Mac BV 0x15d84c): 0 west (-x),
// 1 north (+y), 2 east (+x), 3 south (-y), the order of Lot::floors.
int gameQuarter(float x, float y) {
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    if (fx <= fy)
        return 1 - fx <= fy ? 1 : 0;
    return 1 - fx <= fy ? 2 : 3;
}

uint16_t roomAtQuarter(const LotLightMapLayout& l, int level, float x, float y) {
    if (l.rooms.empty() || level < l.minLevel || level >= l.minLevel + l.levels || x < 0 || y < 0 ||
        x >= float(l.lotWidth) || y >= float(l.lotHeight))
        return 0;
    const size_t at = (size_t(level - l.minLevel) * l.lotWidth + size_t(x)) * l.lotHeight + size_t(y);
    return at < l.rooms.size() ? l.rooms[at][size_t(gameQuarter(x, y))] : 0;
}

// PackToARGB32's channel: round(256 x), 0..255.
int b8(float x) { return std::clamp(int(std::nearbyint(256.0f * x)), 0, 255); }

} // namespace

uint16_t lightRoomKey(const LotLightMapLayout& layout, int level, uint16_t room) {
    return layout.outdoor(level, room) ? 0 : room;
}

uint16_t lightRoomAt(const LotLightMapLayout& layout, int level, float x, float y) {
    return lightRoomKey(layout, level, roomAtQuarter(layout, level, x, y));
}

bool groundAt(const LotLightMapLayout& l, int level, float x, float y) {
    if (x < 0 || y < 0 || x >= float(l.lotWidth) || y >= float(l.lotHeight))
        return false;
    if (level == 0)
        return true;
    if (level < l.minLevel || level >= l.minLevel + l.levels)
        return false;
    const size_t at = (size_t(level - l.minLevel) * l.lotWidth + size_t(x)) * l.lotHeight + size_t(y);
    return at < l.floors.size() && l.floors[at][size_t(gameQuarter(x, y))] != 0;
}

// ---------------------------------------------------------------- floors

const FloorColorGrid::Half* FloorColorGrid::neighbor(int x, int y, int d, int half) const {
    const Half& h = at(x, y).h[half];
    if (!(h.nb & (1 << d)))
        return nullptr;
    const int nx = x + kDx[d], ny = y + kDy[d];
    if (nx < 0 || ny < 0 || nx >= 2 * width || ny >= 2 * height)
        return nullptr;
    return &at(nx, ny).h[(h.sel & (1 << d)) ? 1 : 0];
}

FloorColorGrid::Half* FloorColorGrid::neighbor(int x, int y, int d, int half) {
    return const_cast<Half*>(static_cast<const FloorColorGrid&>(*this).neighbor(x, y, d, half));
}

// UpdateFromWorldDB (Mac BV 0xeeb614): SetupTileGridFromWorldInfo 0xee8400,
// SetupColorGridFromWorldInfo 0xeeaaec, SetupConnection 0xee78e0,
// SetupDiagonalConnection 0xeea7f8.
void FloorColorGrid::build(const LotLightMapLayout& layout, int lvl, const DiffusionParams& params) {
    level = lvl;
    width = int(layout.lotWidth), height = int(layout.lotHeight);
    insideAmbient = params.insideFloor, outsideAmbient = params.outsideFloor;
    threshold = params.floorThreshold;
    steps = params.steps;
    samples.assign(size_t(4 * width * height), Sample{});
    tileExists.assign(size_t(width * height), 0);
    tileDiag.assign(size_t(width * height), 0);
    const int W2 = 2 * width, H2 = 2 * height;
    // A tile is there with ground under it: everywhere on the ground level,
    // above it where a quarter has floor (HasAnyGround).
    for (int ty = 0; ty < height; ++ty)
        for (int tx = 0; tx < width; ++tx) {
            bool exists = level == 0;
            if (!exists && level >= layout.minLevel && level < layout.minLevel + layout.levels) {
                const size_t i = (size_t(level - layout.minLevel) * layout.lotWidth + size_t(tx)) * layout.lotHeight + size_t(ty);
                if (i < layout.floors.size())
                    for (uint16_t f : layout.floors[i])
                        exists = exists || f != 0;
            }
            tileExists[size_t(ty * width + tx)] = exists;
            if (exists)
                for (int s = 0; s < 4; ++s) {
                    Sample& smp = at(2 * tx + (s & 1), 2 * ty + (s >> 1));
                    smp.h[0].exists = true;
                    smp.h[1].exists = false;
                }
        }
    // The light-blocking walls of this level, by their grid ends.
    std::set<std::tuple<int, int, int, int>> blocking;
    for (const auto& w : layout.lightWalls)
        if (w.level == level) {
            blocking.insert({w.ax, w.ay, w.bx, w.by});
            blocking.insert({w.bx, w.by, w.ax, w.ay});
        }
    auto wall = [&](int ax, int ay, int bx, int by) { return blocking.count({ax, ay, bx, by}) != 0; };
    for (int ty = 0; ty < height; ++ty)
        for (int tx = 0; tx < width; ++tx) {
            if (!tileExists[size_t(ty * width + tx)])
                continue;
            Half* s00 = at(2 * tx, 2 * ty).h;
            Half* s10 = at(2 * tx + 1, 2 * ty).h;
            Half* s01 = at(2 * tx, 2 * ty + 1).h;
            Half* s11 = at(2 * tx + 1, 2 * ty + 1).h;
            uint8_t& diag = tileDiag[size_t(ty * width + tx)];
            diag = 0;
            // Each wall on an edge stops light leaving the samples beside it
            // that way (doors and arches too: the masks do not look at them).
            if (wall(tx, ty, tx + 1, ty))
                s00[0].nb &= 0x7C, s10[0].nb &= 0x7C;
            if (wall(tx, ty, tx, ty + 1))
                s00[0].nb &= 0xF1, s01[0].nb &= 0xF1;
            if (wall(tx, ty + 1, tx + 1, ty + 1))
                s01[0].nb &= 0xC7, s11[0].nb &= 0xC7;
            if (wall(tx + 1, ty, tx + 1, ty + 1))
                s10[0].nb &= 0x1F, s11[0].nb &= 0x1F;
            auto split = [&] {
                for (Half* h : {s00, s10, s01, s11})
                    h[1] = h[0];
            };
            if (wall(tx + 1, ty, tx, ty + 1)) { // the diagonal x + y = 1
                split();
                s01[0].nb &= 0xCF, s01[1].nb &= 0xF9, s11[0].nb &= 0x07;
                s00[1].nb &= 0x70, s10[0].nb &= 0x9F, s10[1].nb &= 0xFC;
                diag = 1;
            }
            if (wall(tx, ty, tx + 1, ty + 1)) { // the diagonal x = y
                split();
                s00[0].nb &= 0xF3, s00[1].nb &= 0x7E, s01[0].nb &= 0xC1;
                s10[1].nb &= 0x1C, s11[0].nb &= 0xE7, s11[1].nb &= 0x3F;
                diag = 2;
            }
            // The light room of each half (FindRoomID at a point of the tile).
            auto setRoom = [&](int half, float px, float py) {
                const uint16_t r = lightRoomKey(layout, level, roomAtQuarter(layout, level, float(tx) + px, float(ty) + py));
                for (Half* h : {s00, s10, s01, s11})
                    h[half].room = r, h[half].outside = r == 0;
            };
            if (diag == 0)
                setRoom(0, 0.5f, 0.5f);
            else if (diag == 1)
                setRoom(0, 0.25f, 0.25f), setRoom(1, 0.75f, 0.75f);
            else
                setRoom(0, 0.75f, 0.25f), setRoom(1, 0.25f, 0.75f);
        }
    // Connections: the half light reaches across each step, or none.
    for (int sy = 0; sy < H2; ++sy)
        for (int sx = 0; sx < W2; ++sx)
            for (int d = 0; d < 8; ++d) {
                Sample& A = at(sx, sy);
                const int m = 1 << d;
                const int nx = sx + kDx[d], ny = sy + kDy[d];
                if (nx < 0 || ny < 0 || nx >= W2 || ny >= H2) {
                    A.h[0].nb &= uint8_t(~m), A.h[1].nb &= uint8_t(~m);
                    continue;
                }
                Sample& B = at(nx, ny);
                const int om = 1 << ((d + 4) & 7);
                Half &a0h = A.h[0], &a1h = A.h[1], &b0h = B.h[0], &b1h = B.h[1];
                const bool a0 = a0h.exists && (a0h.nb & m), a1 = a1h.exists && (a1h.nb & m);
                const bool b0 = b0h.exists && (b0h.nb & om), b1 = b1h.exists && (b1h.nb & om);
                if (a0 && b0) {
                    a0h.sel &= uint8_t(~m), b0h.sel &= uint8_t(~om);
                    if (a1 && b1)
                        a1h.sel |= uint8_t(m), b1h.sel |= uint8_t(om);
                    else
                        a1h.nb &= uint8_t(~m);
                } else if (a0 && a1) {
                    a0h.nb &= uint8_t(~m);
                    if (b1)
                        a1h.sel |= uint8_t(m), b1h.sel |= uint8_t(om);
                    else
                        a1h.nb &= uint8_t(~m);
                } else if (a0) {
                    a1h.nb &= uint8_t(~m);
                    if (b1)
                        a0h.sel |= uint8_t(m), b1h.sel &= uint8_t(~om);
                    else
                        a0h.nb &= uint8_t(~m);
                } else {
                    a0h.nb &= uint8_t(~m);
                    if (a1 && b0)
                        a1h.sel &= uint8_t(~m), b0h.sel |= uint8_t(om);
                    else if (a1 && b1)
                        a1h.sel |= uint8_t(m), b1h.sel |= uint8_t(om);
                    else
                        a1h.nb &= uint8_t(~m);
                }
            }
    // A diagonal step stays open only if one of its two L paths is open and
    // reaches the same half.
    for (int sy = 0; sy < H2; ++sy)
        for (int sx = 0; sx < W2; ++sx)
            for (int d = 1; d < 8; d += 2)
                for (int hi = 0; hi < 2; ++hi) {
                    Half& h = at(sx, sy).h[hi];
                    if (!h.exists || !(h.nb & (1 << d)))
                        continue;
                    const bool s = h.sel & (1 << d);
                    const int da = d - 1, db = (d + 1) & 7;
                    bool pathA = h.nb & (1 << da);
                    if (pathA) {
                        const Half& H1 = at(sx + kDx[da], sy + kDy[da]).h[(h.sel & (1 << da)) ? 1 : 0];
                        pathA = H1.exists && (H1.nb & (1 << db)) && bool(H1.sel & (1 << db)) == s;
                    }
                    bool pathB = h.nb & (1 << db);
                    if (pathB) {
                        const Half& H2h = at(sx + kDx[db], sy + kDy[db]).h[(h.sel & (1 << db)) ? 1 : 0];
                        pathB = H2h.exists && (H2h.nb & (1 << da)) && bool(H2h.sel & (1 << da)) == s;
                    }
                    if (!pathA && !pathB) {
                        h.nb &= uint8_t(~(1 << d));
                        at(sx + kDx[d], sy + kDy[d]).h[s ? 1 : 0].nb &= uint8_t(~(1 << ((d + 4) & 7)));
                    }
                }
}

namespace {

// The samples of a source's rectangle (DoesLightSeedThisFloor / InitGridColorsForLight).
std::array<int, 4> sampleRect(const DiffusionSource& s) {
    int x0 = int(std::floor(2 * s.rect[0] + 0.5f)), y0 = int(std::floor(2 * s.rect[1] + 0.5f));
    int x1 = int(std::floor(2 * s.rect[2] - 0.5f)), y1 = int(std::floor(2 * s.rect[3] - 0.5f));
    if (x1 < x0)
        std::swap(x0, x1);
    if (y1 < y0)
        std::swap(y0, y1);
    return {x0, y0, x1, y1};
}

// PropagateBetweenSamples (Mac BV 0xee7c6c).
int propagate(float k, FloorColorGrid::Half& c, const FloorColorGrid::Half& n, int d) {
    if (d & 1)
        k = float(double(k) * 1.4);
    const float inv = 1.0f / (k + 1.0f);
    int count = 0;
    for (size_t ch = 0; ch < 3; ++ch)
        if (n.c[ch] - c.c[ch] > k * c.c[ch]) {
            c.c[ch] = std::fma(inv, n.c[ch], 0.01f);
            ++count;
        }
    return count;
}

} // namespace

// CalculateGrid (Mac BV 0xeeccc0), DoesLightSeedThisFloor 0xeeba10,
// InitGridColorsForLight 0xeebdec, RelaxGridColors 0xeec3a4.
void FloorColorGrid::calculate(const LotLightMapLayout& layout, const std::vector<DiffusionSource>& sources) {
    const int W2 = 2 * width, H2 = 2 * height;
    for (auto& s : samples)
        for (auto& h : s.h)
            if (h.exists) {
                h.c = h.outside ? outsideAmbient : insideAmbient;
                h.dirty = false;
            }
    auto inGrid = [&](int x, int y) { return x >= 0 && y >= 0 && x < W2 && y < H2; };
    auto roomMatch = [&](const Sample& S, uint16_t room) {
        return (S.h[0].exists && S.h[0].room == room) || (S.h[1].exists && S.h[1].room == room);
    };
    auto seeds = [&](const DiffusionSource& src) {
        const auto r = sampleRect(src);
        for (int sx = r[0]; sx <= r[2]; ++sx)
            for (int sy = r[1]; sy <= r[3]; ++sy) {
                if (!inGrid(sx, sy) || !roomMatch(at(sx, sy), src.room))
                    continue;
                if (level == src.level)
                    return true;
                if (level > src.level)
                    continue;
                bool blocked = false;
                for (int l = src.level; l > level && !blocked; --l)
                    blocked = groundAt(layout, l, float(sx) * 0.5f, float(sy) * 0.5f);
                if (!blocked)
                    return true;
            }
        return false;
    };
    std::vector<size_t> list;
    for (size_t i = 0; i < sources.size(); ++i)
        if (sources[i].level >= level && seeds(sources[i]))
            list.push_back(i);
    // Bubble sort by falloff, largest first (the back has the smallest).
    for (size_t a = 0; a + 1 < list.size(); ++a)
        for (size_t b = 0; b + 1 < list.size() - a; ++b)
            if (sources[list[b]].falloff < sources[list[b + 1]].falloff)
                std::swap(list[b], list[b + 1]);
    struct Box {
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        void grow(int x, int y) {
            if (x1 < x0) {
                x0 = x1 = x, y0 = y1 = y;
                return;
            }
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
    };
    auto init = [&](const DiffusionSource& src, Box& box) {
        bool any = false;
        const auto r = sampleRect(src);
        for (int sx = r[0]; sx <= r[2]; ++sx)
            for (int sy = r[1]; sy <= r[3]; ++sy) {
                if (!inGrid(sx, sy))
                    continue;
                Sample& S = at(sx, sy);
                if (!roomMatch(S, src.room) || level > src.level)
                    continue;
                float k = src.floorScale;
                if (level < src.level) {
                    if (src.interFloor >= 1)
                        continue;
                    bool blocked = false;
                    for (int l = src.level; l > level && !blocked; --l) {
                        blocked = groundAt(layout, l, float(sx) * 0.5f, float(sy) * 0.5f);
                        k *= 1 - src.interFloor;
                    }
                    if (blocked)
                        continue;
                }
                bool changed = false;
                for (auto& h : S.h)
                    if (h.exists && h.room == src.room) {
                        bool hc = false;
                        for (size_t ch = 0; ch < 3; ++ch)
                            if (h.c[ch] < src.rgb[ch] * k)
                                h.c[ch] = src.rgb[ch] * k, hc = true;
                        if (hc)
                            h.dirty = true, changed = true;
                    }
                if (changed)
                    box.grow(sx, sy), any = true;
            }
        return any;
    };
    auto relax = [&](float k, Box box) {
        int x0 = std::max(box.x0, 0), x1 = std::min(box.x1, W2 - 1), y0 = std::max(box.y0, 0), y1 = std::min(box.y1, H2 - 1);
        for (auto& s : samples)
            s.h[0].next = s.h[1].next = false;
        for (int pass = 0; pass < steps; ++pass) {
            int count = 0;
            Box bb;
            auto pull = [&](int x, int y, std::initializer_list<int> dirs, bool needDirty) {
                bool changed = false;
                Sample& S = at(x, y);
                for (int hi = 0; hi < 2; ++hi) {
                    Half& h = S.h[hi];
                    if (!h.exists)
                        continue;
                    for (int d : dirs) {
                        const Half* n = neighbor(x, y, d, hi);
                        if (!n || !n->exists || (needDirty && !n->dirty))
                            continue;
                        const int r = propagate(k, h, *n, d);
                        if (r > 0) {
                            h.next = h.dirty = true;
                            count += r;
                            changed = true;
                        }
                    }
                }
                if (changed)
                    bb.grow(x, y);
                return changed;
            };
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0, end = x1 + 1; x <= end && x < W2; ++x)
                    if (pull(x, y, {1, 2, 3}, true) && x > x1)
                        x1 = x, end = x + 1;
                for (int x = x1, stop = x0 - 1; x >= stop && x >= 0; --x)
                    if (pull(x, y, {5, 6, 7}, true) && x < x0)
                        x0 = x, stop = x - 1;
            }
            for (int x = x0; x <= x1; ++x) {
                for (int y = y0, end = y1 + 1; y <= end && y < H2; ++y)
                    if (pull(x, y, {7, 0, 1}, false) && y > y1)
                        y1 = y, end = y + 1;
                for (int y = y1, stop = y0 - 1; y >= stop && y >= 0; --y)
                    if (pull(x, y, {3, 4, 5}, true) && y < y0)
                        y0 = y, stop = y - 1;
            }
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    for (auto& h : at(x, y).h)
                        h.dirty = h.next, h.next = false;
            x0 = bb.x0, y0 = bb.y0, x1 = bb.x1, y1 = bb.y1;
            if (count == 0)
                return;
        }
    };
    while (!list.empty()) {
        const float f = sources[list.back()].falloff;
        Box box;
        bool any = false;
        while (list.size() > 1 && std::fabs(sources[list[list.size() - 2]].falloff - f) < 0.01f) {
            any = init(sources[list.back()], box) || any;
            list.pop_back();
        }
        any = init(sources[list.back()], box) || any;
        list.pop_back();
        if (any)
            relax(f * threshold, box);
    }
}

// CopyToTexturePages (Mac BV 0xeea560): FillInGuardTilePair 0xee9874, then
// cGuardTilePair::FillTileTexels 0xee95d0 with cGuardTile::FillTileTexels
// 0xee8c88 / FillHalfTileTexels 0xee9120.
void FloorColorGrid::copy(std::vector<uint8_t>& rgb, std::vector<uint8_t>& written) const {
    constexpr int kT = LotLightMapLayout::kTexelsPerTile;
    const int pw = width * kT, ph = height * kT;
    rgb.assign(size_t(pw * ph * 3), 0);
    written.assign(size_t(pw * ph), 0);
    const V3 IA = insideAmbient, OA = outsideAmbient;
    auto minusOutside = [&](V3 c, bool outside) {
        if (outside)
            for (size_t ch = 0; ch < 3; ++ch)
                c[ch] -= OA[ch];
        return c;
    };
    const bool isL0 = level == 0;
    for (int ty = 0; ty < height; ++ty)
        for (int tx = 0; tx < width; ++tx) {
            if (!tileExists[size_t(ty * width + tx)])
                continue;
            const int diag = tileDiag[size_t(ty * width + tx)];
            V3 G[2][4][4]{}; // [guard][row j (y)][column i (x)]
            bool gExists[2] = {false, false};
            for (int g = 0; g < (diag ? 2 : 1); ++g) {
                for (int j = 1; j <= 2; ++j)
                    for (int i = 1; i <= 2; ++i) {
                        const Half& h = at(2 * tx + i - 1, 2 * ty + j - 1).h[g];
                        gExists[g] = gExists[g] || h.exists;
                        const V3 v = h.exists ? h.c : (h.outside ? OA : IA);
                        G[g][j][i] = minusOutside(v, h.outside);
                    }
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i) {
                        if (j >= 1 && j <= 2 && i >= 1 && i <= 2)
                            continue;
                        const int sx = 2 * tx + (i > 1), sy = 2 * ty + (j > 1);
                        const int d = i == 0 ? (j == 0 ? 1 : j == 3 ? 3 : 2) : i == 3 ? (j == 0 ? 7 : j == 3 ? 5 : 6) : (j == 0 ? 0 : 4);
                        const Half* n = neighbor(sx, sy, d, g);
                        if (n && n->exists) {
                            G[g][j][i] = minusOutside(n->c, n->outside);
                            continue;
                        }
                        bool done = false;
                        if (d & 1) {
                            const Half* a = neighbor(sx, sy, d - 1, g);
                            const Half* b = neighbor(sx, sy, (d + 1) & 7, g);
                            if (a && !a->exists)
                                a = nullptr;
                            if (b && !b->exists)
                                b = nullptr;
                            if (a && b) {
                                const V3 va = minusOutside(a->c, a->outside), vb = minusOutside(b->c, b->outside);
                                for (size_t ch = 0; ch < 3; ++ch)
                                    G[g][j][i][ch] = 0.5f * (va[ch] + vb[ch]);
                                done = true;
                            } else if (a || b) {
                                const Half* o = a ? a : b;
                                G[g][j][i] = minusOutside(o->c, o->outside);
                                done = true;
                            }
                        }
                        if (!done)
                            G[g][j][i] = G[g][1 + (j > 1)][1 + (i > 1)];
                    }
            }
            // The texels: v = 1 on the tile's +y side (the page's first row there).
            auto fill = [&](int g, int half, float s, float off) {
                const float N = float(kT);
                float du, dv, u0, v0;
                if (isL0) {
                    du = 1 / N, dv = -1 / N, v0 = 1 + dv / 2;
                    u0 = half < 0 ? du / 2 : 0; // the half-tile fill starts at 0 (BV 0xee9120)
                } else {
                    du = 1 / (N - 1), dv = -1 / (N - 1), u0 = 0, v0 = 1;
                }
                float v = v0;
                for (int row = 0; row < kT; ++row, v += dv) {
                    float u = u0;
                    for (int col = 0; col < kT; ++col, u += du) {
                        if (half >= 0 && (u * s + v < off) == bool(half))
                            continue;
                        const float gx = 2 * u + 0.5f, gy = 2 * v + 0.5f;
                        const int ix = std::clamp(int(std::floor(gx)), 0, 2), iy = std::clamp(int(std::floor(gy)), 0, 2);
                        const float fx = gx - float(ix), fy = gy - float(iy);
                        // Our page keeps rows from the tile's -y side.
                        const int px = tx * kT + col, py = ty * kT + (kT - 1 - row);
                        uint8_t* out = &rgb[(size_t(py) * size_t(pw) + size_t(px)) * 3];
                        for (size_t ch = 0; ch < 3; ++ch) {
                            const float c = (1 - fx) * (1 - fy) * G[g][iy][ix][ch] + fx * (1 - fy) * G[g][iy][ix + 1][ch] +
                                            (1 - fx) * fy * G[g][iy + 1][ix][ch] + fx * fy * G[g][iy + 1][ix + 1][ch];
                            out[ch] = uint8_t(b8(c)); // x 2 x scale (0.5)
                        }
                        written[size_t(py) * size_t(pw) + size_t(px)] = 1;
                    }
                }
            };
            if (diag == 0) {
                if (gExists[0])
                    fill(0, -1, 0, 0);
            } else {
                const float s = diag == 1 ? 1.0f : -1.0f, off = diag == 1 ? 0.99f : -0.01f;
                if (gExists[0])
                    fill(0, 0, s, off);
                if (gExists[1])
                    fill(1, 1, s, off);
            }
        }
}

std::vector<FloorColorGrid> diffuseFloors(const LotLightMapLayout& layout, const std::vector<DiffusionSource>& sources,
                                          const DiffusionParams& params) {
    std::vector<FloorColorGrid> grids(size_t(std::max(layout.levels, 0)));
    for (size_t li = 0; li < grids.size(); ++li) {
        grids[li].build(layout, layout.minLevel + int(li), params);
        grids[li].calculate(layout, sources);
    }
    return grids;
}

// ---------------------------------------------------------------- walls

namespace {

using RGB = std::array<int, 3>;

// M(k): the fixed-point 1 / (1 + k) of the wall code (BV 0x01c6cc14).
int fixedDivisor(float k) { return int(int16_t(std::nearbyint(65536.0f * float(0.0625 / (1.0 + double(k)))))); }
int fixedDiv(int v, int m) { return ((v << 4) * m) >> 16; }
int clamp8(int v) { return std::clamp(v, 0, 255); }
RGB pack(const V3& c) { return {b8(c[0]), b8(c[1]), b8(c[2])}; }

struct Seg {
    V2 p0{}, p1{};
    int level = 0;
    uint16_t room = 0;
    float base = 0;
    int run = -1, index = 0;
    int up = -1, down = -1; // the segment of the same tile part a story up / down
    RGB S[2][6]{};          // samples: column (along), row (0 at the top)
    RGB F[4][8]{};          // final colours with their border
};

struct Run {
    std::vector<int> segs;
    V2 dir{};
    bool diag = false, lit = false;
    int level = 0;
    uint16_t room = 0;
    int prev = -1, next = -1; // the runs round a right-angle corner (right turn)
};

class WallDiffusion {
public:
    WallDiffusion(const LotLightMapLayout& layout, const DiffusionParams& params, const std::vector<std::vector<uint8_t>>& floorPages)
        : l_(layout), p_(params), floorPages_(floorPages) {}

    // LookupWallDiffusion (BV 0xeef45c) of each face.
    std::vector<V3> lookups() const {
        std::vector<V3> out(l_.walls.size(), V3{0, 0, 0});
        for (size_t fi = 0; fi < l_.walls.size(); ++fi) {
            if (faceSeg_[fi] < 0)
                continue;
            const Seg& s = segs_[size_t(faceSeg_[fi])];
            const bool outside = runs_[size_t(s.run)].room == 0;
            for (size_t ch = 0; ch < 3; ++ch)
                out[fi][ch] = s.S[1][3][ch] / 255.0f - (outside ? p_.outsideWall[ch] : 0.0f);
        }
        return out;
    }

    std::vector<std::vector<std::array<uint8_t, 3>>> run(const std::vector<DiffusionSource>& sources) {
        build();
        for (auto& r : runs_)
            clear(r);
        std::set<int> active;
        for (const auto& src : sources)
            for (size_t ri = 0; ri < runs_.size(); ++ri)
                if (addDirectLight(int(ri), src)) {
                    setLit(int(ri));
                    active.insert(int(ri));
                }
        fillFromFloors(active);
        for (int ri : active)
            for (int i = 0; i < p_.steps; ++i)
                if (!propagateOneStep(ri))
                    break;
        for (size_t ri = 0; ri < runs_.size(); ++ri)
            for (size_t i = 0; i < runs_[ri].segs.size(); ++i)
                finalColours(int(ri), int(i));
        if (p_.darkCorners)
            darkenCorners();
        return copy();
    }

private:
    const LotLightMapLayout& l_;
    const DiffusionParams& p_;
    const std::vector<std::vector<uint8_t>>& floorPages_;
    std::vector<Seg> segs_;
    std::vector<Run> runs_;
    std::vector<int> faceSeg_;

    static V2 sub(V2 a, V2 b) { return {a[0] - b[0], a[1] - b[1]}; }
    static float dot2(V2 a, V2 b) { return a[0] * b[0] + a[1] * b[1]; }
    static V2 normal(V2 dir) { return {dir[1], -dir[0]}; } // the front of a wall going `dir`
    static V2 unit(V2 v) {
        const float n = std::sqrt(dot2(v, v));
        return n > 0 ? V2{v[0] / n, v[1] / n} : V2{1, 0};
    }
    static bool same(V2 a, V2 b) { return std::fabs(a[0] - b[0]) < 0.01f && std::fabs(a[1] - b[1]) < 0.01f; }
    V3 ambient(const Run& r) const { return r.room == 0 ? p_.outsideWall : p_.insideWall; }

    // The segments (one per wall tile and side, from the faces) and the
    // runs: straight chains of one room (SetupWallSection, Mac BV 0xeefcf4),
    // the stories' links (ConnectFloors 0xeef624).
    void build() {
        std::map<std::tuple<int, long, long, long, long>, int> byKey;
        auto key = [](int level, V2 a, V2 b) {
            return std::make_tuple(level, std::lround(a[0] * 8), std::lround(a[1] * 8), std::lround(b[0] * 8), std::lround(b[1] * 8));
        };
        faceSeg_.assign(l_.walls.size(), -1);
        // SetupWalls (Mac BV 0xef0324) takes the wall graphs 3 (the walls
        // that block light), 8 and 11 (half walls) and 10 (stage walls), and
        // the pool walls of graph 0 (docs/lighting-mac.md 11.5): the
        // layout's light walls, which keep the pool walls. Fences, deck
        // skirts and deck walls get no wall diffusion (their cells stay
        // black; inferred: the colour the game clears wall tiles to).
        std::set<std::tuple<int, long, long, long, long>> diffused;
        for (const auto& w : l_.lightWalls) {
            diffused.insert({w.level, long(w.ax), long(w.ay), long(w.bx), long(w.by)});
            diffused.insert({w.level, long(w.bx), long(w.by), long(w.ax), long(w.ay)});
        }
        for (size_t fi = 0; fi < l_.walls.size(); ++fi) {
            const auto& f = l_.walls[fi];
            V2 a = f.wallA, b = f.wallB;
            if (same(a, b)) // no grid ends given: the face's own
                a = f.origin, b = {f.origin[0] + f.along[0] * f.length, f.origin[1] + f.along[1] * f.length};
            else if (!diffused.count({f.level, std::lround(a[0]), std::lround(a[1]), std::lround(b[0]), std::lround(b[1])}))
                continue;
            const V2 dir = unit(sub(b, a));
            if (dot2(normal(dir), f.normal) < 0)
                std::swap(a, b);
            const auto k = key(f.level, a, b);
            auto it = byKey.find(k);
            if (it == byKey.end()) {
                Seg s;
                s.p0 = a, s.p1 = b, s.level = f.level, s.room = lightRoomKey(l_, f.level, f.room), s.base = f.base;
                segs_.push_back(s);
                it = byKey.emplace(k, int(segs_.size()) - 1).first;
            }
            segs_[size_t(it->second)].base = std::min(segs_[size_t(it->second)].base, f.base);
            faceSeg_[fi] = it->second;
        }
        // Chains: the next segment starts where one ends, same story, room
        // and direction, with no wall meeting that point on the front side.
        std::multimap<std::tuple<int, long, long>, int> starts, touches;
        for (size_t i = 0; i < segs_.size(); ++i) {
            const Seg& s = segs_[i];
            starts.insert({{s.level, std::lround(s.p0[0] * 8), std::lround(s.p0[1] * 8)}, int(i)});
            touches.insert({{s.level, std::lround(s.p0[0] * 8), std::lround(s.p0[1] * 8)}, int(i)});
            touches.insert({{s.level, std::lround(s.p1[0] * 8), std::lround(s.p1[1] * 8)}, int(i)});
        }
        auto pointKey = [](int level, V2 p) { return std::make_tuple(level, std::lround(p[0] * 8), std::lround(p[1] * 8)); };
        std::vector<int> nextSeg(segs_.size(), -1), prevSeg(segs_.size(), -1);
        for (size_t i = 0; i < segs_.size(); ++i) {
            const Seg& s = segs_[i];
            const V2 dir = unit(sub(s.p1, s.p0)), n = normal(dir);
            bool junction = false;
            auto [t0, t1] = touches.equal_range(pointKey(s.level, s.p1));
            for (auto t = t0; t != t1 && !junction; ++t) {
                const Seg& u = segs_[size_t(t->second)];
                const V2 q = same(u.p0, s.p1) ? u.p1 : u.p0;
                junction = dot2(sub(q, s.p1), n) > 0.01f;
            }
            if (junction)
                continue;
            auto [a0, a1] = starts.equal_range(pointKey(s.level, s.p1));
            for (auto c = a0; c != a1; ++c) {
                const Seg& t = segs_[size_t(c->second)];
                if (c->second != int(i) && t.room == s.room && dot2(unit(sub(t.p1, t.p0)), dir) > 0.999f &&
                    prevSeg[size_t(c->second)] < 0) {
                    nextSeg[i] = c->second, prevSeg[size_t(c->second)] = int(i);
                    break;
                }
            }
        }
        for (size_t i = 0; i < segs_.size(); ++i) {
            if (segs_[i].run >= 0 || prevSeg[i] >= 0)
                continue;
            Run r;
            for (int s = int(i); s >= 0 && segs_[size_t(s)].run < 0; s = nextSeg[size_t(s)]) {
                segs_[size_t(s)].run = int(runs_.size()), segs_[size_t(s)].index = int(r.segs.size());
                r.segs.push_back(s);
            }
            runs_.push_back(std::move(r));
        }
        for (size_t i = 0; i < segs_.size(); ++i) // closed chains (none expected)
            if (segs_[i].run < 0) {
                Run r;
                for (int s = int(i); s >= 0 && segs_[size_t(s)].run < 0; s = nextSeg[size_t(s)]) {
                    segs_[size_t(s)].run = int(runs_.size()), segs_[size_t(s)].index = int(r.segs.size());
                    r.segs.push_back(s);
                }
                runs_.push_back(std::move(r));
            }
        for (auto& r : runs_) {
            const Seg& s0 = segs_[size_t(r.segs[0])];
            r.dir = unit(sub(s0.p1, s0.p0));
            r.diag = std::fabs(r.dir[0]) > 0.1f && std::fabs(r.dir[1]) > 0.1f;
            r.level = s0.level, r.room = s0.room;
        }
        // The neighbours round a right-angle corner, turning right (the
        // room on the right): DarkenCornersNextToDarkWalls's test.
        std::multimap<std::tuple<int, long, long>, int> runStarts;
        for (size_t ri = 0; ri < runs_.size(); ++ri)
            runStarts.insert({pointKey(runs_[ri].level, segs_[size_t(runs_[ri].segs[0])].p0), int(ri)});
        for (size_t ri = 0; ri < runs_.size(); ++ri) {
            Run& r = runs_[ri];
            const V2 end = segs_[size_t(r.segs.back())].p1;
            auto [b0, b1] = runStarts.equal_range(pointKey(r.level, end));
            for (auto c = b0; c != b1; ++c) {
                Run& o = runs_[size_t(c->second)];
                if (c->second != int(ri) && o.room == r.room && dot2(o.dir, normal(r.dir)) > 0.99f) {
                    r.next = c->second;
                    if (o.prev < 0)
                        o.prev = int(ri);
                    break;
                }
            }
        }
        // Stories: the same tile part a level up with no floor there, a level
        // down with no floor on this one.
        for (auto& s : segs_) {
            const V2 dir = unit(sub(s.p1, s.p0)), n = normal(dir);
            const float px = (s.p0[0] + s.p1[0]) / 2 + n[0] * 0.25f, py = (s.p0[1] + s.p1[1]) / 2 + n[1] * 0.25f;
            if (auto it = byKey.find(key(s.level + 1, s.p0, s.p1)); it != byKey.end() && !groundAt(l_, s.level + 1, px, py))
                s.up = it->second;
            if (auto it = byKey.find(key(s.level - 1, s.p0, s.p1)); it != byKey.end() && !groundAt(l_, s.level, px, py))
                s.down = it->second;
        }
    }

    // Clear (BV 0xeee87c): the samples at the wall's ambient, not lit.
    void clear(Run& r) {
        const RGB a = pack(ambient(r));
        for (int s : r.segs)
            for (auto& col : segs_[size_t(s)].S)
                for (auto& v : col)
                    v = a;
        r.lit = false;
    }

    RGB* column(const Run& r, int col) { return segs_[size_t(r.segs[size_t(col / 2)])].S[col % 2]; }

    // SetAndPropagateColumnLeft / Right (BV 0xef2210 / 0xef1e70).
    void spread(const Run& r, const V3& colour, float k, int c0, bool right) {
        const int m = fixedDivisor(r.diag ? 1.4f * k : k);
        RGB cur = pack(colour);
        const int n = 2 * int(r.segs.size());
        if (right)
            for (auto& v : cur)
                v = clamp8(fixedDiv(v, m));
        for (int col = right ? c0 + 1 : c0; right ? col < n : col >= 0; col += right ? 1 : -1) {
            RGB* S = column(r, col);
            bool changed = false;
            for (int row = 1; row <= 4; ++row)
                for (size_t ch = 0; ch < 3; ++ch) {
                    const int nv = clamp8(std::max(S[row][ch], cur[ch]));
                    changed = changed || nv != S[row][ch];
                    S[row][ch] = nv;
                }
            if (!changed)
                break;
            for (auto& v : cur)
                v = clamp8(fixedDiv(v, m));
            S[0] = S[5] = cur;
        }
    }

    // cWallDiffusionManager::AddDirectLight (BV 0xef28ac) and the run's
    // (0xef2594, ClosestColumn 0xeeddac).
    bool addDirectLight(int ri, const DiffusionSource& L) {
        const Run& r = runs_[size_t(ri)];
        if (r.level > L.level || r.room != L.room)
            return false;
        const V2 c{(L.rect[0] + L.rect[2]) * 0.5f, (L.rect[1] + L.rect[3]) * 0.5f};
        float s = L.wallScale;
        for (int lvl = L.level; r.level < lvl; --lvl) {
            if (groundAt(l_, lvl, c[0], c[1]))
                break;
            s *= 1 - L.interFloor;
        }
        float k = p_.wallThreshold * L.falloff;
        const Seg& s0 = segs_[size_t(r.segs[0])];
        const V2 p0 = s0.p0, dir = r.dir, d = sub(c, p0);
        const float perp = d[0] * dir[1] - d[1] * dir[0], along = dot2(d, dir);
        const int n = int(r.segs.size());
        const float len = float(n) * (r.diag ? 1.4142135f : 1.0f);
        const float t = along / len;
        float dist;
        V2 cl;
        int col;
        const float sgn = perp > 0 ? 1.0f : perp < 0 ? -1.0f : 0.0f;
        if (t < 0) {
            dist = sgn * std::sqrt(dot2(d, d)), cl = p0, col = 0;
        } else if (t >= 1) {
            dist = sgn * std::sqrt(dot2(d, d)), cl = {p0[0] + len * dir[0], p0[1] + len * dir[1]}, col = 2 * n - 1;
        } else {
            dist = perp, cl = {p0[0] + len * t * dir[0], p0[1] + len * t * dir[1]};
            col = std::clamp(int(std::floor(t * float(2 * n))), 0, 2 * n - 1);
        }
        if (dist <= 0)
            return false;
        float f = 1, cpos = 0;
        if (L.directional && dist > 1e-4f) {
            const V2 nn{(cl[0] - c[0]) / dist, (cl[1] - c[1]) / dist};
            const float co = std::cos(L.facing) * nn[0] + std::sin(L.facing) * nn[1];
            f = (6 - 2 * co) / (6 + 2 * co);
            cpos = std::max(co, 0.0f);
            const float fd = std::clamp(L.directionalFalloff, 0.0f, 1.0f);
            s *= ((co + 1) * 0.5f) * fd + (1 - fd);
        }
        const float stepsN = std::max(1.0f, f * 2 * dist);
        const float atten = s * std::pow(1 / (1 + k), stepsN);
        const V3 colour{L.rgb[0] * atten, L.rgb[1] * atten, L.rgb[2] * atten};
        if (L.directional)
            k = k + cpos * (2 * k / stepsN - k);
        k = std::max(k, 0.0f);
        spread(r, colour, k, col, false);
        spread(r, colour, k, col, true);
        return true;
    }

    // SetIlluminatedVertical (BV 0xeef078): this run and those above it.
    void setLit(int ri) {
        Run& r = runs_[size_t(ri)];
        r.lit = true;
        for (int s : r.segs)
            if (const int u = segs_[size_t(s)].up; u >= 0 && !runs_[size_t(segs_[size_t(u)].run)].lit)
                setLit(segs_[size_t(u)].run);
    }

    // The floor light map's texel under a point (LookupFloorLightMap, BV
    // 0x6f6a24): its byte / 255 x 2; black outside the map.
    V3 floorAt(int level, float x, float y) const {
        constexpr int kT = LotLightMapLayout::kTexelsPerTile;
        const int li = level - l_.minLevel;
        const long i = long(std::floor(x * kT)), j = long(std::floor(y * kT));
        const long pw = long(l_.lotWidth) * kT, ph = long(l_.lotHeight) * kT;
        if (li < 0 || size_t(li) >= floorPages_.size() || i < 0 || j < 0 || i >= pw || j >= ph)
            return {0, 0, 0};
        const uint8_t* px = &floorPages_[size_t(li)][(size_t(j) * size_t(pw) + size_t(i)) * 3];
        return {px[0] / 255.0f * 2, px[1] / 255.0f * 2, px[2] / 255.0f * 2};
    }

    // FillWallsFromFloors (BV 0xef0e68 -> FillFromFloorTexture 0xef0b58).
    void fillFromFloors(std::set<int>& active) {
        for (size_t ri = 0; ri < runs_.size(); ++ri) {
            Run& r = runs_[ri];
            const float c = r.lit ? p_.brightWallFloor : p_.darkWallFloor;
            if (c == 0)
                continue;
            const float h = c * 0.5f;
            const int n = int(r.segs.size());
            const Seg& s0 = segs_[size_t(r.segs[0])];
            const V2 nrm = normal(r.dir);
            const V2 span{(s0.p1[0] - s0.p0[0]) * float(n), (s0.p1[1] - s0.p0[1]) * float(n)};
            bool changed = false;
            for (int i = 0; i < n; ++i) {
                Seg& s = segs_[size_t(r.segs[size_t(i)])];
                if (s.down >= 0)
                    continue;
                for (int col = 0; col < 2; ++col) {
                    const float t = std::clamp((float(2 * i + col) + 0.5f) / float(2 * n), 0.0f, 1.0f);
                    const float x = s0.p0[0] + t * span[0] + 0.125f * nrm[0], y = s0.p0[1] + t * span[1] + 0.125f * nrm[1];
                    const V3 f = floorAt(r.level, x, y);
                    const RGB v = pack({f[0] * h, f[1] * h, f[2] * h});
                    for (auto& row : s.S[col])
                        for (size_t ch = 0; ch < 3; ++ch)
                            if (v[ch] > row[ch])
                                row[ch] = v[ch], changed = true;
                }
            }
            if (changed)
                active.insert(int(ri));
        }
    }

    // max(dst, src / (1 + k)) per channel; true when it changed.
    static bool pullInto(RGB& dst, const RGB& src, int m) {
        bool changed = false;
        for (size_t ch = 0; ch < 3; ++ch) {
            const int v = clamp8(std::max(dst[ch], fixedDiv(src[ch], m)));
            changed = changed || v != dst[ch];
            dst[ch] = v;
        }
        return changed;
    }

    // Segment::PropagateInternal (BV 0xef1170).
    bool segmentInternal(Seg& s, int m0, int m1) {
        static const int dr[8] = {0, -1, -1, -1, 0, 1, 1, 1}, dc[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
        bool changed = false;
        for (int col = 0; col < 2; ++col)
            for (int d = 0; d < 8; ++d) {
                const int nc = col + dc[d];
                if (nc < 0 || nc > 1)
                    continue;
                const int m = (d & 1) ? m1 : m0;
                for (int row = std::max(-dr[d], 0); row < 6 + std::min(-dr[d], 0); ++row)
                    changed = pullInto(s.S[col][row], s.S[nc][row + dr[d]], m) || changed;
            }
        return changed;
    }

    // PropagateFromHorizontalNeighbors (BV 0xef1774).
    bool horizontal(Seg& s, const Seg* L, const Seg* R, int m0, int m1) {
        bool changed = false;
        if (L) {
            for (int r = 1; r <= 5; ++r)
                changed = pullInto(s.S[0][r], L->S[1][r - 1], m1) || changed;
            for (int r = 0; r <= 5; ++r)
                changed = pullInto(s.S[0][r], L->S[1][r], m0) || changed;
            for (int r = 0; r <= 4; ++r)
                changed = pullInto(s.S[0][r], L->S[1][r + 1], m1) || changed;
        }
        if (R) {
            for (int r = 1; r <= 5; ++r)
                changed = pullInto(s.S[1][r], R->S[0][r - 1], m1) || changed;
            for (int r = 0; r <= 5; ++r)
                changed = pullInto(s.S[1][r], R->S[0][r], m0) || changed;
            for (int r = 0; r <= 4; ++r)
                changed = pullInto(s.S[1][r], R->S[0][r + 1], m1) || changed;
        }
        return changed;
    }

    // PropagateFromVerticalNeighbor (BV 0xef1628): both ways, the facing rows.
    bool vertical(Seg& s, Seg& nb, bool up, int m0) {
        bool changed = false;
        for (int c = 0; c < 2; ++c) {
            RGB& mine = s.S[c][up ? 0 : 5];
            RGB& theirs = nb.S[c][up ? 5 : 0];
            changed = pullInto(mine, theirs, m0) || changed;
            changed = pullInto(theirs, mine, m0) || changed;
        }
        return changed;
    }

    bool internal(int ri, bool up, bool self, std::vector<char>& visited, int m0, int m1) {
        bool changed = false;
        Run& r = runs_[size_t(ri)];
        for (int si : r.segs) {
            Seg& s = segs_[size_t(si)];
            if (self)
                changed = segmentInternal(s, m0, m1) || changed;
            const int link = up ? s.up : s.down;
            if (link >= 0) {
                const int lw = segs_[size_t(link)].run;
                if (!visited[size_t(lw)]) {
                    visited[size_t(lw)] = 1;
                    changed = internal(lw, up, true, visited, m0, m1) || changed;
                }
            }
        }
        return changed;
    }

    bool external(int ri, bool up, bool self, std::vector<char>& visited, int m0, int m1) {
        bool changed = false;
        Run& r = runs_[size_t(ri)];
        const int n = int(r.segs.size());
        auto seg = [&](int i) -> Seg& { return segs_[size_t(r.segs[size_t(i)])]; };
        if (self && n >= 2) {
            changed = horizontal(seg(0), nullptr, &seg(1), m0, m1) || changed;
            changed = horizontal(seg(n - 1), &seg(n - 2), nullptr, m0, m1) || changed;
            for (int i = 1; i + 1 < n; ++i)
                changed = horizontal(seg(i), &seg(i - 1), &seg(i + 1), m0, m1) || changed;
        }
        for (int i = 0; i < n; ++i)
            if (const int link = up ? seg(i).up : seg(i).down; link >= 0)
                changed = vertical(seg(i), segs_[size_t(link)], up, m0) || changed;
        for (int i = 0; i < n; ++i)
            if (const int link = up ? seg(i).up : seg(i).down; link >= 0) {
                const int lw = segs_[size_t(link)].run;
                if (!visited[size_t(lw)]) {
                    visited[size_t(lw)] = 1;
                    changed = external(lw, up, true, visited, m0, m1) || changed;
                }
            }
        return changed;
    }

    // PropagateOneStep (BV 0xef1c9c).
    bool propagateOneStep(int ri) {
        const int m0 = fixedDivisor(p_.wallThreshold), m1 = fixedDivisor(1.4f * p_.wallThreshold);
        bool changed = false;
        std::vector<char> visited(runs_.size(), 0);
        changed = internal(ri, true, true, visited, m0, m1) || changed;
        std::fill(visited.begin(), visited.end(), 0);
        changed = internal(ri, false, false, visited, m0, m1) || changed;
        std::fill(visited.begin(), visited.end(), 0);
        changed = external(ri, true, true, visited, m0, m1) || changed;
        std::fill(visited.begin(), visited.end(), 0);
        changed = external(ri, false, false, visited, m0, m1) || changed;
        return changed;
    }

    // GetSegmentNeighbors (BV 0xeeebbc) and CopyToFinalColor (0xeed7bc).
    void finalColours(int ri, int i) {
        const Run& r = runs_[size_t(ri)];
        Seg& s = segs_[size_t(r.segs[size_t(i)])];
        const int n = int(r.segs.size());
        const Seg* out[8] = {};
        auto segOf = [&](int run, int idx) -> const Seg* {
            const Run& o = runs_[size_t(run)];
            return idx >= 0 && idx < int(o.segs.size()) ? &segs_[size_t(o.segs[size_t(idx)])] : nullptr;
        };
        out[0] = i > 0 ? &segs_[size_t(r.segs[size_t(i - 1)])] : nullptr;
        out[4] = i + 1 < n ? &segs_[size_t(r.segs[size_t(i + 1)])] : nullptr;
        if (s.up >= 0) {
            const Seg& u = segs_[size_t(s.up)];
            out[1] = segOf(u.run, u.index - 1), out[2] = &u, out[3] = segOf(u.run, u.index + 1);
        }
        if (s.down >= 0) {
            const Seg& d = segs_[size_t(s.down)];
            out[5] = segOf(d.run, d.index + 1), out[6] = &d, out[7] = segOf(d.run, d.index - 1);
        }
        if (out[0] && !out[2] && out[0]->up >= 0)
            out[1] = &segs_[size_t(out[0]->up)];
        if (out[0] && !out[6] && out[0]->down >= 0)
            out[7] = &segs_[size_t(out[0]->down)];
        if (out[4] && !out[2] && out[4]->up >= 0)
            out[3] = &segs_[size_t(out[4]->up)];
        if (out[4] && !out[6] && out[4]->down >= 0)
            out[5] = &segs_[size_t(out[4]->down)];
        const Seg *left = out[0], *right = out[4], *up = out[2], *down = out[6];
        for (int c = 0; c < 2; ++c)
            for (int row = 0; row < 6; ++row)
                s.F[1 + c][1 + row] = s.S[c][row];
        for (int row = 0; row < 6; ++row) {
            s.F[0][1 + row] = left ? left->S[1][row] : s.F[1][1 + row];
            s.F[3][1 + row] = right ? right->S[0][row] : s.F[2][1 + row];
        }
        for (int c = 0; c < 2; ++c) {
            s.F[1 + c][0] = up ? up->S[c][5] : s.F[1 + c][1];
            s.F[1 + c][7] = down ? down->S[c][0] : s.F[1 + c][6];
        }
        if (out[1])
            s.F[0][0] = out[1]->S[1][5];
        else
            s.F[0][0] = up ? up->S[0][5] : left ? left->S[1][0] : s.S[0][0];
        if (out[3])
            s.F[3][0] = out[3]->S[0][5];
        else
            s.F[3][0] = right ? right->S[0][0] : up ? up->S[1][5] : s.S[1][0];
        if (out[5])
            s.F[3][7] = out[5]->S[0][0];
        else
            s.F[3][7] = down ? down->S[1][0] : right ? right->S[0][5] : s.S[1][5];
        if (out[7])
            s.F[0][7] = out[7]->S[1][0];
        else
            s.F[0][7] = left ? left->S[1][5] : down ? down->S[0][0] : s.S[0][5];
    }

    // MixFinalColorColumn (BV 0xef10fc -> 0xef0f70): f x colour + (1 - f) x old.
    void mix(const Run& r, float f, const V3& colour, int col) {
        const int si = col >> 2, c = col & 3;
        if (si < 0 || si >= int(r.segs.size()))
            return;
        Seg& s = segs_[size_t(r.segs[size_t(si)])];
        const RGB a = pack({colour[0] * f, colour[1] * f, colour[2] * f});
        const int q = int(std::nearbyint((f - 1) * 0.5f * 65536.0f));
        for (auto& v : s.F[c])
            for (size_t ch = 0; ch < 3; ++ch)
                v[ch] = clamp8(a[ch] - (((v[ch] * 2) * q) >> 16));
    }

    // DarkenCornersNextToDarkWalls (BV 0xef4000).
    void darkenCorners() {
        for (const auto& r : runs_) {
            if (!r.lit)
                continue;
            const V3 amb = ambient(r);
            const int n = int(r.segs.size());
            if (r.prev >= 0 && !runs_[size_t(r.prev)].lit) {
                mix(r, 0.8f, amb, 0), mix(r, 0.8f, amb, 1), mix(r, 0.4f, amb, 2), mix(r, 0.4f, amb, n > 1 ? 4 : 3);
            }
            if (r.next >= 0 && !runs_[size_t(r.next)].lit) {
                const int c = 4 * n;
                mix(r, 0.8f, amb, c - 1), mix(r, 0.8f, amb, c - 2), mix(r, 0.4f, amb, c - 3), mix(r, 0.4f, amb, n > 1 ? c - 5 : c - 4);
            }
        }
    }

    // CopyToTexturePages (BV 0xef2c9c): each face's cell from its
    // segment's final colours, 8 texels along and 24 up per tile, bilinear,
    // 0.5 x (colour - the outside ambient on walls facing out).
    std::vector<std::vector<std::array<uint8_t, 3>>> copy() const {
        constexpr int kT = LotLightMapLayout::kTexelsPerTile;
        std::vector<std::vector<std::array<uint8_t, 3>>> out(l_.walls.size());
        const int mTall = fixedDivisor(0.4f * p_.wallThreshold);
        for (size_t fi = 0; fi < l_.walls.size(); ++fi) {
            const auto& f = l_.walls[fi];
            auto& texels = out[fi];
            texels.assign(size_t(f.w) * f.h, {0, 0, 0});
            if (faceSeg_[fi] < 0)
                continue;
            const Seg& s = segs_[size_t(faceSeg_[fi])];
            const Run& r = runs_[size_t(s.run)];
            const V3 amb = r.room == 0 ? p_.outsideWall : V3{0, 0, 0};
            const V2 sd = sub(s.p1, s.p0);
            const float segLen = std::sqrt(dot2(sd, sd));
            const V2 dir = unit(sd);
            auto sampleAt = [&](float u, float v) {
                u = std::clamp(u, 0.0f, 3.0f), v = std::clamp(v, 0.0f, 7.0f);
                const int i0 = int(std::floor(u)), i1 = int(std::ceil(u)), j0 = int(std::floor(v)), j1 = int(std::ceil(v));
                RGB t{};
                for (size_t ch = 0; ch < 3; ++ch) {
                    const float a = s.F[i0][j0][ch] / 255.0f + (s.F[i1][j0][ch] - s.F[i0][j0][ch]) / 255.0f * (u - float(i0));
                    const float b = s.F[i0][j1][ch] / 255.0f + (s.F[i1][j1][ch] - s.F[i0][j1][ch]) / 255.0f * (u - float(i0));
                    t[ch] = b8(p_.scale * (a + (b - a) * (v - float(j0)) - amb[ch]));
                }
                return t;
            };
            for (uint32_t j = 0; j < f.h; ++j)
                for (uint32_t i = 0; i < f.w; ++i) {
                    const float along = (float(i) - 0.5f) / float(kT);
                    const float z = f.top - (float(j) - 0.5f) / float(kT);
                    const V2 p{f.origin[0] + f.along[0] * along, f.origin[1] + f.along[1] * along};
                    const float a = segLen > 0 ? dot2(sub(p, s.p0), dir) / segLen : 0.5f;
                    const float ig = a * float(kT) - 0.5f;
                    const float u = std::clamp(0.5f + ig * 2.0f / 7.0f, 0.5f, 2.5f);
                    const float jg = std::max((z - s.base) * float(kT) - 0.5f, 0.0f); // rows from the foot
                    RGB t;
                    if (jg <= 23.0f) {
                        t = sampleAt(u, 6.5f - jg * 6.0f / 23.0f);
                    } else {
                        // Taller than three tiles: each row over the 24th the one
                        // under it through M(0.4 x wallThreshold).
                        t = sampleAt(u, 0.5f);
                        for (int e = 0, extra = int(std::lround(jg - 23.0f)); e < extra; ++e)
                            for (auto& v : t)
                                v = clamp8(fixedDiv(v, mTall));
                    }
                    texels[size_t(j) * f.w + i] = {uint8_t(t[0]), uint8_t(t[1]), uint8_t(t[2])};
                }
        }
        return out;
    }
};

} // namespace

std::vector<std::vector<std::array<uint8_t, 3>>> diffuseWalls(const LotLightMapLayout& layout,
                                                               const std::vector<DiffusionSource>& sources,
                                                               const DiffusionParams& params,
                                                               const std::vector<FloorColorGrid>&,
                                                               const std::vector<std::vector<uint8_t>>& floorPages,
                                                               std::vector<std::array<float, 3>>* lookup) {
    WallDiffusion w(layout, params, floorPages);
    auto cells = w.run(sources);
    if (lookup)
        *lookup = w.lookups();
    return cells;
}

} // namespace os2
