// The game's roofs: docs/roof-fence-water-mac.md §1.5 (round n13). The BV
// addresses name the function each part follows.
#include "lot_roof_game.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace os2::roofgame {

bool nearlyEqual(float a, float b) {
    int32_t ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    const uint32_t sa = uint32_t(ia >> 31), sb = uint32_t(ib >> 31);
    const int32_t oa = int32_t((uint32_t(ia) ^ (sa >> 1)) - sa), ob = int32_t((uint32_t(ib) ^ (sb >> 1)) - sb);
    const int32_t d = int32_t(uint32_t(oa) - uint32_t(ob));
    return (d >> 5) == (d >> 31);
}

namespace {

// FUN_009d3a30: the eave drop for slopes under 0.499.
float slopeOffset(float s) { return s >= 0.499f ? 0.0f : 0.5f - s; }

int gridIndex(float d) { return int(std::lround(2 * d)); }

// FUN_009c642c: min of the distances to the four borders.
std::vector<float> hipGrid(int w, int h) {
    std::vector<float> g(size_t(w) * size_t(h));
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            g[size_t(j) * size_t(w) + size_t(i)] = float(std::min({i, w - 1 - i, j, h - 1 - j}));
    return g;
}

} // namespace

Roof::Roof(const Lot::Roof& r, float baseElevation)
    : id(r.id), level(r.level), type(r.type), x0(r.x0), y0(r.y0), x1(r.x1), y1(r.y1), slope(r.angle),
      base(baseElevation) {
    // The type constructors (0x9d0870, 0x9c8a6c, 0x9c893c, 0x9d1be0,
    // 0x9d1228, 0x9c8b9c): which way the slopes run.
    switch (type) {
    case Lot::ShedGable:
    case Lot::LongGable:
    case Lot::Hip: orientation = std::fabs(y1 - y0) <= std::fabs(x1 - x0); break;
    case Lot::ShedHip:
    case Lot::ShortGable: orientation = std::fabs(x1 - x0) < std::fabs(y1 - y0); break;
    default: orientation = false; break;
    }
    xUp = x1 - x0 >= 0;
    yUp = y1 - y0 >= 0;
    // Bounds: the eave on every side but the high side of the shed roofs.
    const bool shedX = (type == Lot::ShedGable && !orientation) || (type == Lot::ShedHip && orientation);
    const bool shedY = (type == Lot::ShedGable && orientation) || (type == Lot::ShedHip && !orientation);
    const float e = eaves ? 0.5f : 0.0f;
    minX = std::min(x0, x1) - ((shedX && !xUp) ? 0.0f : e);
    maxX = std::max(x0, x1) + ((shedX && xUp) ? 0.0f : e);
    minY = std::min(y0, y1) - ((shedY && !yUp) ? 0.0f : e);
    maxY = std::max(y0, y1) + ((shedY && yUp) ? 0.0f : e);
    w = gridIndex(maxX - minX) + 1;
    h = gridIndex(maxY - minY) + 1;
    grid.assign(size_t(w) * size_t(h), 0.0f);
    auto set = [&](int i, int j, float v) { grid[size_t(j) * size_t(w) + size_t(i)] = v; };
    switch (type) {
    case Lot::LongGable:  // FUN_009d3c7c with no offset
    case Lot::ShortGable: // cTallGableRoofType::ComputeGridElevation 0x9c5f20
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                set(i, j, float(orientation ? std::min(j, h - 1 - j) : std::min(i, w - 1 - i)));
        break;
    case Lot::Hip: grid = hipGrid(w, h); break;
    case Lot::Mansard: {
        const int k = mansardTop();
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                set(i, j, float(std::min({i, w - 1 - i, j, h - 1 - j, k})));
        break;
    }
    case Lot::ShedGable: // FUN_009d3ad8 with the 0.08 offset
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                set(i, j, 0.08f + float(orientation ? (yUp ? j : h - 1 - j) : (xUp ? i : w - 1 - i)));
        break;
    case Lot::ShedHip: // FUN_009d0094: half of a hip grid twice as long
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                const int d = orientation ? std::min({xUp ? i : w - 1 - i, j, h - 1 - j})
                                          : std::min({yUp ? j : h - 1 - j, i, w - 1 - i});
                set(i, j, float(d));
            }
        break;
    default: break;
    }
}

int Roof::mansardTop() const {
    // FUN_009c6568 / the mansard IsOnPeak (0x9cc0cc): max(1, min(min(W/2, H/2) - 1, 7)).
    const int k = std::min(std::min(w / 2, h / 2) - 1, 7);
    return k == 0 ? 1 : k;
}

float Roof::gridElevation(int i, int j) const {
    if (i < 0 || j < 0 || i >= w || j >= h)
        return 0.0f;
    return (eaves ? -0.42f : 0.42f) + slope * notch(i, j) + (base + slopeOffset(slope));
}

float Roof::maxElevation() const {
    float g = 0;
    switch (type) {
    case Lot::LongGable:
    case Lot::Hip: g = grid[grid.size() / 2]; break;
    case Lot::ShortGable: g = float(orientation ? h / 2 : w / 2); break;
    case Lot::ShedGable: g = std::max(grid.front(), grid.back()); break;
    case Lot::ShedHip: g = float(std::min(std::min(w, h) - 1, std::max(w / 2, h / 2))); break;
    case Lot::Mansard: g = float(std::min(7, std::min(w / 2, h / 2) - 1)); break; // 0x9c67a4
    default: break;
    }
    return (eaves ? -0.42f : 0.42f) + slope * g + (base + slopeOffset(slope));
}

float Roof::elevation(float x, float y, float margin) const {
    if (maxX - margin < x || x < margin + minX || maxY - margin < y || y < margin + minY)
        return kNoElevation;
    const float fx = 2 * (x - minX), fy = 2 * (y - minY);
    const int i = std::clamp(int(std::floor(fx)), 0, w - 2), j = std::clamp(int(std::floor(fy)), 0, h - 2);
    const float u = fx - float(i), v = fy - float(j);
    const float h00 = gridElevation(i, j), h10 = gridElevation(i + 1, j), h11 = gridElevation(i + 1, j + 1),
                h01 = gridElevation(i, j + 1);
    if (((i + j) & 1) == 0) // diagonal (i,j)-(i+1,j+1)
        return u >= v ? h00 + (h10 - h00) * u + (h11 - h10) * v : h00 + (h11 - h01) * u + (h01 - h00) * v;
    // diagonal (i+1,j)-(i,j+1)
    return u + v <= 1 ? h00 + (h10 - h00) * u + (h01 - h00) * v
                      : h11 + (h01 - h11) * (1 - u) + (h10 - h11) * (1 - v);
}

bool Roof::contains(float x, float y) const {
    if (!(x <= maxX && minX <= x && y <= maxY && minY <= y))
        return false;
    const int i = gridIndex(x - minX), j = gridIndex(y - minY);
    return i < w && j < h;
}

bool Roof::inEaves(float x, float y) const {
    if (!eaves || !contains(x, y))
        return false;
    return x < std::min(x0, x1) || std::max(x0, x1) < x || y < std::min(y0, y1) || std::max(y0, y1) < y;
}

int Roof::onPeak(float x, float y) const {
    const int i = gridIndex(x - minX), j = gridIndex(y - minY);
    const float e = gridElevation(i, j);
    if (!(e > 0.0f) || !nearlyEqual(maxElevation(), e))
        return 0;
    switch (type) {
    case Lot::ShedGable: return h <= w ? 0xC : 3; // 0x9cc350
    case Lot::Mansard: {                           // 0x9cc0cc
        const int k = mansardTop();
        int m = k == i ? 0x12 : 0x10;
        if (k == j)
            m |= 8;
        if (i == w - k - 1)
            m |= 1;
        if (j == h - k - 1)
            m |= 4;
        return m;
    }
    default: return orientation ? 0xC : 3; // 0x9cb8d0, 0x9cbc18, 0x9cc92c, 0x9ccdc0
    }
}

namespace {

// FUN_009d3728: the hip lines of a hip grid, up to m = min(W/2, H/2) from
// the corners (FUN_009d3e70, the mansard's: up to its flat top k, the last
// corner only within 7).
int hipDiagonal(int i, int j, int w, int h, int m, bool mansard = false) {
    if (m < i)
        return m < j ? (w - i - 1 == h - j - 1 && (!mansard || w - i - 1 <= 7) ? 5 : 0) : (j == w - i - 1 ? 9 : 0);
    return m < j ? (i == h - j - 1 ? 6 : 0) : (i == j ? 0xA : 0);
}

} // namespace

int Roof::onDiagonal(float x, float y) const {
    const int i = gridIndex(x - minX), j = gridIndex(y - minY);
    switch (type) {
    case Lot::Hip: return hipDiagonal(i, j, w, h, std::min(w / 2, h / 2)); // 0x9cba88
    case Lot::ShedHip: {                                                   // 0x9cc70c -> 0x9d37d4
        if (w < h) {
            const int w2 = 2 * w - 1;
            return hipDiagonal(xUp ? i : i + w2 / 2, j, w2, h, std::min(w2 / 2, h / 2));
        }
        const int h2 = 2 * h - 1;
        return hipDiagonal(i, yUp ? j : j + h2 / 2, w, h2, std::min(w / 2, h2 / 2));
    }
    case Lot::Mansard: return hipDiagonal(i, j, w, h, mansardTop(), true); // 0x9cbf3c -> 0x9d3e70
    default: return 0; // 0x1972fa0
    }
}

// --- cWorldRoof ---

std::vector<size_t> World::near(int level) const {
    std::vector<size_t> out;
    for (size_t k = 0; k < roofs_.size(); ++k)
        if (roofs_[k].level >= std::max(0, level - 1) && roofs_[k].level <= level + 2)
            out.push_back(k);
    return out;
}

float World::sampledElevation(size_t roof, float x, float y, int level) const {
    const Roof& r = roofs_[roof];
    return r.elevation(x, y, level < r.level ? 0.5f : 0.0f);
}

bool World::anyRoofAt(float x, float y, int level) const {
    if (level < 0)
        return false;
    for (const Roof& r : roofs_)
        if (r.level == level && r.contains(x, y))
            return true;
    return false;
}

bool World::anyRoofAtOrBelow(float x, float y, int level) const {
    return anyRoofAt(x, y, level) || anyRoofAt(x, y, level - 1);
}

float World::clearanceTop(float x, float y, int level) const {
    float top = 0.0f;
    for (size_t k : near(level)) {
        const Roof& r = roofs_[k];
        float e = r.elevation(x, y, level < r.level ? 0.5f : 0.0f);
        // GetSampledElevationByRoofIDForClearance: a roof a level up that
        // misses the point by -1 counts as the floor there + 0.001 when the
        // point is near it (slot 0x44); the shapes here never give -1.
        top = std::max(top, e);
    }
    return top;
}

} // namespace os2::roofgame
