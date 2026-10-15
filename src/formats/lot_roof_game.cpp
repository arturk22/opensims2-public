// The game's roofs: docs/roof-fence-water-mac.md §1.5 (round n13). The BV
// addresses name the function each part follows.
#include "lot_roof_game.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

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


// --- IsTriangleCenterLessThanAnyOtherRoofTriangleCenter (0xc63558) ---

namespace {

// The cone, dome, octagonal and pagoda types never hide a triangle (0x470380).
bool skippedByHiding(uint32_t type) { return type <= 0x16 && ((1u << type) & 0x470380u); }
bool diagonalType(uint32_t type) { return type < 0x16 && ((1u << type) & 0x387c00u); } // IsDiagonalRoofType 0x163c6c
bool gableLike(uint32_t type) { return type == 4 || type == 3 || type == 1; }
// The slope of the whole roof world (cWorldRoof +0x40): 0.5 on every lot
// shipped (inferred: where the game sets it was not read).
constexpr float kWorldSlope = 0.5f;

struct Rect {
    float x0, y0, x1, y1; // FUN_00c53ec8: the walls' rectangle
};
Rect wallRect(const Roof& r) {
    return {std::min(r.x0, r.x1), std::min(r.y0, r.y1), std::max(r.x0, r.x1), std::max(r.y0, r.y1)};
}

} // namespace

bool World::centerBelowOtherRoof(size_t me, float cx, float cy, bool lowerLayer, float* own, int atLevel) const {
    const Roof& m = roofs_[me];
    const bool strict = !lowerLayer && nearlyEqual(kWorldSlope, 0.5f);
    if (!m.contains(cx, cy))
        return false;
    const int level = atLevel == std::numeric_limits<int>::min() ? m.level : atLevel;
    const float e = sampledElevation(me, cx, cy, level), thr = e + 0.01f;
    for (size_t k : near(level)) {
        if (k == me)
            continue;
        const Roof& o = roofs_[k];
        float eo = sampledElevation(k, cx, cy, level);
        if (skippedByHiding(o.type))
            continue;
        bool test = true; // reach the hiding test (c63964)
        if (diagonalType(o.type) || !nearlyEqual(o.slope, m.slope)) {
            if (o.inEaves(cx, cy))
                continue;
            eo -= m.slope;
            if (lowerLayer)
                eo -= m.slope;
        } else if (thr < eo && gableLike(o.type) && m.level == o.level && o.inEaves(cx, cy)) {
            // Two gables of one orientation side by side: hide only beyond
            // the wall the two share.
            if (!(gableLike(m.type) || m.type == 0xF) || m.orientation != o.orientation)
                continue;
            const Rect a = wallRect(m), b = wallRect(o);
            if (!m.orientation)
                test = (cy < a.y0 && nearlyEqual(a.y0, b.y0)) || (cy > a.y1 && nearlyEqual(a.y1, b.y1));
            else
                test = (cx < a.x0 && nearlyEqual(a.x0, b.x0)) || (cx > a.x1 && nearlyEqual(a.x1, b.x1));
            if (!test)
                continue;
        }
        if (eo <= thr)
            continue;
        if (strict || m.level == o.level || eo > e + m.slope)
            return true;
    }
    if (own)
        *own = e;
    return false;
}

// --- The triangles (classes c{Even,Odd}Triangle{First,Second}..., 0xec1a50..0xec9600) ---

namespace {

enum Cls { EF, ES, OF, OS }; // even/odd cell, first/second triangle

// cVertexProperty: bit 0 peak, 1 diagonal, 2 shared; the masks of the other roof.
struct VP {
    uint8_t bits = 0, diagMask = 0, peakMask = 0;
    bool bit(int b) const { return (bits >> b) & 1; }
};
// cVertexPropertyEx: the bits, the roofs through the vertex at its height
// and, per roof, its own properties there.
struct VPX {
    uint8_t bits = 0;
    std::set<uint32_t> roofs;
    std::map<uint32_t, VP> per;
    bool bit(int b) const { return (bits >> b) & 1; }
};

// Corners of the triangles in each cell (AddPositionIndicesToPolygon).
constexpr int kCorner[4][3][2] = {{{0, 0}, {1, 1}, {0, 1}},
                                  {{0, 0}, {1, 0}, {1, 1}},
                                  {{0, 0}, {1, 0}, {0, 1}},
                                  {{1, 0}, {1, 1}, {0, 1}}};
// The underside's texture coordinates (AddTexCoordIndicesToPolygon, lower).
constexpr int kLowerUV[4][3] = {{0, 9, 90}, {9, 99, 90}, {0, 99, 90}, {0, 9, 99}};
// IsDiagonalShared: the two vertices on the cell's diagonal.
constexpr int kDiagonalPair[4][2] = {{0, 1}, {0, 2}, {1, 2}, {0, 2}};
// TexCoordIndicesForNormal{NegX, PosX, NegY, PosY}: offsets from the base
// (0 = u0 v0, 1 = u1 v0, 10 = u0 v1, 11 = u1 v1); the mirror swaps u.
constexpr int kCornerUV[4][4][3] = {
    {{11, 0, 10}, {0, 11, 1}, {10, 1, 0}, {1, 10, 11}}, // EF 0xec5304..0xec554c
    {{11, 1, 0}, {0, 10, 11}, {10, 11, 1}, {1, 0, 10}}, // ES 0xec560c..0xec5854
    {{11, 1, 10}, {0, 10, 1}, {10, 11, 0}, {1, 0, 11}}, // OF 0xec5914..0xec5b5c
    {{1, 0, 10}, {10, 11, 1}, {11, 1, 0}, {0, 10, 11}}, // OS 0xec5c1c..0xec5e5c
};

// ComputeTextureIndex{PosX,NegX,PosY,NegY} (gable classes): two shapes.
// S: one shared vertex a (with a sibling sharing one) -> 0x14/0x1a, two
//    shared (a and b) -> 0x2e/6.  D: the cell's diagonal shared -> 0x16/0x2a,
//    else vertex a shared -> 0x14/0x1a or, the sibling sharing two, 0x2e/6.
struct CellRule {
    bool diagonal;  // D shape
    int a, b;       // vertices
    bool mirrorA;   // S: the one-vertex mirror; D: the diagonal's mirror
    bool mirrorP;   // D: the one-vertex mirror
};
// [class][PosX, NegX, PosY, NegY]
constexpr CellRule kCellRules[4][4] = {
    {{true, 1, 1, false, true}, {false, 0, 2, true, false}, {false, 1, 2, false, false}, {true, 0, 0, true, false}},
    {{false, 2, 1, true, false}, {true, 0, 0, false, true}, {true, 2, 2, true, false}, {false, 0, 1, false, false}},
    {{true, 1, 1, true, false}, {false, 2, 0, false, false}, {true, 2, 2, false, true}, {false, 1, 0, true, false}},
    {{false, 0, 1, false, false}, {true, 2, 2, true, false}, {false, 2, 1, true, false}, {true, 0, 0, false, true}},
};

struct Triangle {
    Cls cls;
    std::array<VP, 3> p{};
    int sibling = 0; // shared vertices of the cell's other triangle
    float nx = 0, ny = 0;
    int base = 60;
    bool mirror = false;

    int count(int b) const { return p[0].bit(b) + p[1].bit(b) + p[2].bit(b); }
    int peaks() const { return count(0); }      // slot 0x34
    int shared() const { return count(2); }     // GetSharedCount
    int diagonals() const { return count(1); }  // UpdateDiagonalInfo
    bool diagonalShared() const { return p[size_t(kDiagonalPair[cls][0])].bit(2) && p[size_t(kDiagonalPair[cls][1])].bit(2); }
    bool P(int k) const { return p[size_t(k)].bit(2); }
    bool D(int k) const { return p[size_t(k)].bit(1); }

    void cellRule(int dir, int pc, int sc, bool ds) {
        const CellRule& r = kCellRules[cls][dir];
        if (!r.diagonal) {
            if (sc == 1) {
                if (sibling == 1 && P(r.a))
                    base = pc == 1 ? 0x1a : 0x14, mirror = r.mirrorA;
            } else if (sc == 2) {
                if (P(r.a) && P(r.b))
                    base = pc == 1 ? 6 : 0x2e, mirror = false;
            }
            return;
        }
        if (ds) {
            base = pc == 2 ? 0x2a : 0x16, mirror = r.mirrorA;
        } else if (sc != 2) {
            if (sibling == 2 && P(r.a))
                base = pc == 2 ? 6 : 0x2e, mirror = false;
            else if (P(r.a))
                base = pc == 2 ? 0x1a : 0x14, mirror = r.mirrorP;
        }
    }

    // cE/O Triangle First/Second Gable::ComputeBaseTextureIndex (0xec60a8...).
    void gableBase() {
        const int pc = peaks(), sc = shared();
        const bool ds = diagonalShared();
        if (pc == 0)
            base = 0x3c;
        else
            base = 4, mirror = false;
        if (sc != 1 && sc != 2)
            return;
        if (nx < -0.1f)
            cellRule(1, pc, sc, ds);
        else if (nx > 0.1f)
            cellRule(0, pc, sc, ds);
        else if (ny < -0.1f)
            cellRule(3, pc, sc, ds);
        else if (ny > 0.1f)
            cellRule(2, pc, sc, ds);
    }

    // UpdateTextureMirrorForTwoDiagonalVertices (0xec2be0, 0xec26a0, 0xec20e8, 0xec1ba0).
    void twoDiagonals() {
        const int sc = shared();
        base = 0;
        auto first = [&](int a, int b, int one, bool mirrorOne, bool twoMirror) {
            // the face whose two diagonal vertices give 8 / 0x2c, or 0x52
            if (sc == 1 && P(one))
                mirror = mirrorOne, base = 8;
            else if (sc == 2 && P(a) && P(b))
                base = 0x2c, mirror = twoMirror;
            else
                base = 0x52, mirror = false;
        };
        auto second = [&](int a, int b, int one, bool mirrorOne, bool twoMirror) {
            // the other face: 8 / 0x2c or nothing
            if (sc == 2) {
                if (P(a) && P(b))
                    base = 0x2c, mirror = twoMirror;
            } else if (sc == 1 && P(one)) {
                mirror = mirrorOne, base = 8;
            }
        };
        switch (cls) {
        case EF:
            if (nx < -0.1f)
                first(0, 2, 0, false, true);
            else if (ny > 0.1f)
                second(1, 2, 1, true, false);
            break;
        case ES:
            if (ny < -0.1f)
                second(0, 1, 0, true, false);
            else if (nx > 0.1f)
                first(1, 2, 2, false, true);
            break;
        case OF:
            if (ny < -0.1f)
                first(0, 1, 1, false, true);
            else if (nx < -0.1f)
                second(0, 2, 2, true, false);
            break;
        case OS:
            if (nx > 0.1f)
                second(0, 1, 0, true, false);
            else if (ny > 0.1f)
                first(1, 2, 2, false, true);
            break;
        }
    }

    // UpdateTextureMirrorForOneDiagonalVertex (0xec2d70, 0xec2830, 0xec227c, 0xec1d30).
    void oneDiagonal() {
        const int pc = peaks(), sc = shared();
        const bool ds = diagonalShared(), z = pc == 0;
        base = z ? 2 : 0x42;
        const int kHip = z ? 0x28 : 0x2a, kTwo = z ? 0x18 : 0x56, kOne = z ? 0x3e : 0x40;
        // The face crossed by the hip line: the diagonal vertex must be v,
        // then the shared vertices pick the cell (EF -y, EF +x, ES -x, ES +y,
        // OF +x, OF +y, OS -y, OS -x).
        auto across = [&](int v, int a, int b, bool m, bool noDiagMirror) {
            if (v >= 0 && !D(v)) {
                mirror = noDiagMirror;
                return;
            }
            if (ds)
                base = kHip;
            else if (sc == 1) {
                if (sibling == 2) {
                    if (P(a))
                        base = kTwo;
                } else if (sibling == 1 && P(b)) {
                    base = kOne;
                }
            }
            mirror = m;
        };
        // The faces along the hip line: two shared (a, b) -> kTwo, one (c)
        // with the sibling sharing one -> kOne.
        auto along = [&](int a, int b, int c, bool m) {
            if (sc == 2) {
                if (P(a) && P(b))
                    base = kTwo;
            } else if (sc == 1 && sibling == 1 && P(c)) {
                base = kOne;
            }
            mirror = m;
        };
        switch (cls) {
        case EF:
            if (ny < -0.1f)
                across(1, 0, 0, true, false);
            else if (nx < -0.1f)
                along(0, 2, 0, false);
            else if (nx > 0.1f)
                across(0, 1, 1, false, true);
            else if (ny > 0.1f)
                along(1, 2, 1, true);
            break;
        case ES:
            if (ny < -0.1f)
                along(0, 1, 0, true);
            else if (nx < -0.1f)
                across(-1, 0, 0, false, false);
            else if (nx > 0.1f)
                along(1, 2, 2, false);
            else if (ny > 0.1f)
                across(0, 2, 2, true, false);
            break;
        case OF:
            if (ny < -0.1f)
                along(0, 1, 1, false);
            else if (nx < -0.1f)
                along(0, 2, 2, true);
            else if (nx > 0.1f) {
                if (!D(2)) {
                    // 0xec2474: the sibling's single shared vertex still picks
                    // kOne (vertex 1, or vertex 2), the mirror left as it is.
                    if (sc != 1 || sibling != 1) {
                        mirror = false;
                        return;
                    }
                    if (P(1) || P(2)) {
                        base = kOne;
                        return;
                    }
                    mirror = false;
                    return;
                }
                across(-1, 1, 1, true, true);
            } else if (ny > 0.1f)
                across(1, 2, 2, false, true);
            break;
        case OS:
            if (ny < -0.1f)
                across(-1, 0, 0, false, false);
            else if (nx < -0.1f)
                across(0, 2, 2, true, false);
            else if (nx > 0.1f)
                along(0, 1, 0, true);
            else if (ny > 0.1f)
                along(1, 2, 2, false);
            break;
        }
    }

    // c...Hipped::ComputeBaseTextureIndex (0xec61fc...): the hip lines first.
    void hippedBase() {
        const int d = diagonals();
        if (d == 2)
            twoDiagonals();
        else if (d == 1)
            oneDiagonal();
        else
            gableBase();
    }

    // AddTexCoordIndicesToPolygon (top): the three atlas indices.
    std::array<int, 3> uv() const {
        int dir;
        if (nx < -0.1f)
            dir = 0;
        else if (nx > 0.1f)
            dir = 1;
        else if (ny < -0.1f)
            dir = 2;
        else if (ny > 0.1f)
            dir = 3;
        else {
            const int flat = (cls == EF || cls == ES) ? 0x3c : base;
            return {flat, flat, flat};
        }
        std::array<int, 3> out{};
        for (int k = 0; k < 3; ++k) {
            int c = kCornerUV[cls][dir][k];
            if (mirror)
                c = c == 0 ? 1 : c == 1 ? 0 : c == 10 ? 11 : 10;
            out[size_t(k)] = base + c;
        }
        return out;
    }
};

// AddTexCoordToMeshBuilder (0xc543c4): index k -> (t[k % 10], t[k / 10]).
std::array<float, 2> atlasUV(int k) {
    static constexpr float t[10] = {0.0f,      0.19921875f, 0.19921875f, 0.3984375f, 0.3984375f,
                                    0.59765625f, 0.59765625f, 0.796875f,  0.796875f, 0.99609375f};
    k = std::clamp(k, 0, 99);
    return {t[k % 10], t[k / 10]};
}

int normalType(float nx, float ny) { // 0xc4e180
    if (nx < -0.1f)
        return 2;
    if (ny < -0.1f)
        return 8;
    if (nx > 0.1f)
        return 1;
    return ny > 0.1f ? 4 : 0;
}

} // namespace

// SetVertexProperties (0xc689cc), the roof-meets-roof choice (FUN_00c63b04)
// and SetVertexPropertiesEx (0xc67ca8), for one triangle of roof `me`.
struct TriangleBuilder {
    const World& world;
    size_t me;
    const Roof& m;

    TriangleBuilder(const World& w, size_t idx) : world(w), me(idx), m(w.roofs_[idx]) {}

    uint32_t idOf(size_t k) const { return world.roofs_[k].id; }
    size_t indexOf(uint32_t id) const {
        for (size_t k = 0; k < world.roofs_.size(); ++k)
            if (world.roofs_[k].id == id)
                return k;
        return me;
    }
    float elev(size_t k, float x, float y) const { return world.sampledElevation(k, x, y, m.level); }

    static int vertexCountOnRoof(const std::array<VPX, 3>& info, uint32_t id) {
        int n = 0;
        for (const VPX& v : info)
            n += v.roofs.count(id) ? 1 : 0;
        return n;
    }
    static VP& per(std::array<VPX, 3>& info, int k, uint32_t id) { return info[size_t(k)].per[id]; }
    static bool special(std::array<VPX, 3>& info, uint32_t id) { // TriangleHasSpecialVertexProperties
        for (int k = 0; k < 3; ++k) {
            const VP& v = per(info, k, id);
            if (v.bit(1) || v.bit(0))
                return true;
        }
        return false;
    }
    static int flagCount(std::array<VPX, 3>& info, uint32_t id, int b) { // TriangleFlagCount
        int n = 0;
        for (int k = 0; k < 3; ++k)
            if (info[size_t(k)].roofs.count(id) && per(info, k, id).bit(b))
                ++n;
        return n;
    }

    // FUN_00c63b04: true hides the triangle.
    bool resolveShared(std::set<uint32_t>& S, std::array<VPX, 3>& info, std::array<VP, 3>& props, int nt, float cx,
                       float cy) const {
        const uint32_t self = m.id;
        uint32_t minId = 10000000;
        std::vector<uint32_t> V;
        int sharedCnt = 0;
        for (const VPX& v : info)
            sharedCnt += v.bit(2);
        bool anyPlain = false, anySpecial = false;
        int r19 = 3, r20 = 3;
        if (sharedCnt == 3) {
            for (uint32_t id : S) {
                if (vertexCountOnRoof(info, id) != 3)
                    continue;
                const bool sp = special(info, id);
                anyPlain = anyPlain || !sp;
                if (!sp) {
                    minId = std::min(minId, id);
                } else {
                    const int d = flagCount(info, id, 1), p = flagCount(info, id, 0);
                    if (d == 1 && (r19 > 1 || r20 != 0))
                        r20 = p, r19 = 1;
                    else if (p == 1 && r19 > 1)
                        r19 = d, r20 = 1;
                    anySpecial = true;
                }
                for (VPX& v : info) {
                    v.roofs.erase(id);
                    if (v.roofs.size() == 1)
                        v.bits = uint8_t(v.bits & ~4);
                }
                V.push_back(id);
            }
        } else {
            for (uint32_t id : S)
                for (int k = 0; k < 3; ++k) {
                    if (!info[size_t(k)].roofs.count(id))
                        continue;
                    const VP& v = per(info, k, id);
                    if ((v.bit(1) && (v.diagMask & nt)) || (v.bit(0) && (v.peakMask & nt))) {
                        V.push_back(id);
                        break;
                    }
                }
        }
        for (uint32_t id : V)
            S.erase(id);
        int shared2 = 0;
        for (const VPX& v : info)
            shared2 += v.bit(2);
        if (shared2 > 1 && V.empty()) {
            bool found = false;
            for (uint32_t id : S)
                if (vertexCountOnRoof(info, id) == shared2) {
                    found = true;
                    break;
                }
            if (!found && !S.empty()) {
                const uint32_t first = *S.begin();
                for (VPX& v : info) {
                    v.bits = uint8_t(v.bits & ~4);
                    if (v.roofs.count(first))
                        v.bits |= 4;
                }
            }
        }
        for (int k = 0; k < 3; ++k)
            props[size_t(k)].bits = info[size_t(k)].bits;
        const bool specialMe = special(info, self);
        if (anyPlain) {
            const size_t other = indexOf(minId);
            if (!specialMe) {
                if (self <= minId)
                    return false;
                return !world.centerBelowOtherRoof(other, cx, cy, false, nullptr, m.level);
            }
            return !world.centerBelowOtherRoof(other, cx, cy, false, nullptr, m.level);
        }
        if (!anySpecial || !specialMe)
            return false;
        const int d = flagCount(info, self, 1), p = flagCount(info, self, 0);
        const bool a = r19 == 1;
        // The decision table at 0xc63fbc..0xc64090.
        auto K = [&] { return p != 3; };                 // c64088: hidden unless p == 3
        auto L = [&] { return d != 1 ? true : !a; };     // c64028
        auto I = [&] { return p != 1 ? K() : L(); };     // c6400c
        auto J = [&] { return p != 2 ? K() : L(); };     // c64020
        auto H = [&] { return r20 == 2 ? J() : K(); };   // c64080
        auto G = [&] { return r20 == 1 ? I() : H(); };   // c6407c
        auto F = [&] { return (p == 1 && d == 2 && r19 == 2) ? false : G(); }; // c63ff0
        if (a && (d > 1 || p != 0))
            return r20 != 1 ? H() : F();
        if (r20 != 1 || d <= 1)
            return false;
        return F();
    }

    // SetVertexProperties: false hides the triangle; `props` gets the three
    // vertices' bits (peak, diagonal, shared).
    bool setVertexProperties(const float (&v)[3][2], int nt, float cx, float cy, std::array<VP, 3>& props) const {
        std::array<VPX, 3> info;
        std::set<uint32_t> S;
        const std::vector<size_t> ids = world.near(m.level);
        for (int k = 0; k < 3; ++k) {
            const float x = v[k][0], y = v[k][1];
            const float e = elev(me, x, y);
            VPX& in = info[size_t(k)];
            in.roofs.insert(m.id);
            if (m.onPeak(x, y))
                in.bits |= 1, in.per[m.id].bits |= 1;
            if (m.onDiagonal(x, y))
                in.bits |= 2, in.per[m.id].bits |= 2;
            for (size_t o : ids) {
                if (o == me)
                    continue;
                if (!nearlyEqual(e, elev(o, x, y)))
                    continue;
                const Roof& ro = world.roofs_[o];
                VP pv;
                if (int pk = ro.onPeak(x, y))
                    pv.peakMask = uint8_t(pk), pv.bits |= 1;
                if (int dg = ro.onDiagonal(x, y))
                    pv.diagMask = uint8_t(dg), pv.bits |= 2;
                in.per[ro.id] = pv;
                in.bits |= 4;
                in.roofs.insert(ro.id);
                S.insert(ro.id);
            }
        }
        const size_t before = S.size();
        if (resolveShared(S, info, props, nt, cx, cy))
            return false;
        if (S.size() != before) {
            // SetVertexPropertiesEx with the roofs left.
            for (int k = 0; k < 3; ++k) {
                const float x = v[k][0], y = v[k][1];
                const float e = elev(me, x, y);
                VP& pr = props[size_t(k)];
                pr.bits = 0;
                if (m.onPeak(x, y))
                    pr.bits |= 1;
                if (m.onDiagonal(x, y))
                    pr.bits |= 2;
                for (uint32_t id : S) {
                    if (id == m.id)
                        continue;
                    const size_t o = indexOf(id);
                    if (!nearlyEqual(e, elev(o, x, y)))
                        continue;
                    const Roof& ro = world.roofs_[o];
                    const int pk = ro.onPeak(x, y), dg = ro.onDiagonal(x, y);
                    if ((pk == 0 || (pk & nt) == 0) && (dg == 0 || (dg & nt) == 0))
                        pr.bits |= 4;
                }
            }
        }
        return true;
    }
};

void World::build(size_t idx, Meshes& out) const {
    const Roof& r = roofs_[idx];
    const int w = r.w, h = r.h;
    if (w < 2 || h < 2)
        return;
    TriangleBuilder tb(*this, idx);
    // The gable triangle classes (long, short and shed gables) or the hipped ones.
    const bool gableFamily = r.type == Lot::LongGable || r.type == Lot::ShortGable || r.type == Lot::ShedGable;
    // Three layers (FillMeshBuilderData): the top, 0.0875 and 0.25 under it.
    constexpr float kLayer[3] = {0.0f, -0.0875f, -0.25f};
    auto pos = [&](int layer, int i, int j) -> std::array<float, 3> {
        return {r.minX + 0.5f * float(i), r.minY + 0.5f * float(j), r.gridElevation(i, j) + kLayer[layer]};
    };
    // The fixed normals at 45 degrees (FillMeshBuilderData): the long
    // gable's own pair along its slope axis; the other types' list -y, +y,
    // -x, +x (FUN_00c54f44). The underside takes the opposite.
    constexpr float d = 0.70710677f;
    auto slopeNormal = [&](int index, bool under) -> std::array<float, 3> {
        std::array<float, 3> n{};
        if (r.type == Lot::LongGable) {
            const float s = index == 0 ? -d : d;
            n = !r.orientation ? std::array<float, 3>{s, 0, d} : std::array<float, 3>{0, s, d};
        } else {
            switch (index) {
            case 0: n = {0, -d, d}; break;
            case 1: n = {0, d, d}; break;
            case 2: n = {-d, 0, d}; break;
            default: n = {d, 0, d}; break;
            }
        }
        if (under)
            n = {-n[0], -n[1], -n[2]};
        return n;
    };
    auto emit = [](MeshGroup& g, const std::array<std::array<float, 3>, 3>& p, const std::array<std::array<float, 2>, 3>& uv,
                   const std::array<float, 3>& n) {
        const uint32_t base = uint32_t(g.positions.size());
        for (int k = 0; k < 3; ++k) {
            g.positions.push_back(p[size_t(k)]);
            g.uvs.push_back(uv[size_t(k)]);
            g.normals.push_back(n);
        }
        g.triangles.insert(g.triangles.end(), {base, base + 1, base + 2});
    };

    // ConnectHeightField (0xc7a188 and the other types').
    for (int j = 0; j + 1 < h; ++j) {
        const float y = r.minY + 0.5f * float(j);
        for (int i = 0; i + 1 < w; ++i) {
            const float x = r.minX + 0.5f * float(i);
            const bool odd = ((int(2 * x) + int(2 * y)) & 1) != 0;
            const Cls first = odd ? OF : EF, second = odd ? OS : ES;
            const Cls cls[2] = {first, second};
            Triangle tri[2];
            bool visible[2];
            int sharedOf[2] = {0, 0};
            for (int t = 0; t < 2; ++t) {
                tri[t].cls = cls[t];
                float v[3][2];
                for (int k = 0; k < 3; ++k)
                    v[k][0] = x + 0.5f * float(kCorner[cls[t]][k][0]), v[k][1] = y + 0.5f * float(kCorner[cls[t]][k][1]);
                const float cx = (v[0][0] + v[1][0] + v[2][0]) / 3, cy = (v[0][1] + v[1][1] + v[2][1]) / 3;
                visible[t] = !centerBelowOtherRoof(idx, cx, cy, false);
                if (!visible[t])
                    continue;
                // The triangle's normal from the notch grid (the ctor's cross
                // product, pointing up).
                auto g = [&](int k) {
                    return r.notch(i + kCorner[cls[t]][k][0], j + kCorner[cls[t]][k][1]);
                };
                const float ax = float(kCorner[cls[t]][1][0] - kCorner[cls[t]][0][0]),
                            ay = float(kCorner[cls[t]][1][1] - kCorner[cls[t]][0][1]), az = g(1) - g(0);
                const float bx = float(kCorner[cls[t]][2][0] - kCorner[cls[t]][0][0]),
                            by = float(kCorner[cls[t]][2][1] - kCorner[cls[t]][0][1]), bz = g(2) - g(0);
                float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                if (nz < 0)
                    nx = -nx, ny = -ny;
                tri[t].nx = nx, tri[t].ny = ny;
                visible[t] = tb.setVertexProperties(v, normalType(nx, ny), cx, cy, tri[t].p);
                sharedOf[t] = tri[t].count(2);
            }
            for (int t = 0; t < 2; ++t) {
                if (!visible[t])
                    continue;
                Triangle& T = tri[t];
                T.sibling = sharedOf[1 - t];
                T.mirror = false;
                if (gableFamily)
                    T.gableBase();
                else
                    T.hippedBase();
                // The normal of the vertices: the long and short gables by the
                // half of the grid, the shed gable always the first (its
                // ConnectHeightField 0xc7e908 passes the base), the others by
                // the face (0xc4f7f8).
                int ni;
                if (r.type == Lot::LongGable || r.type == Lot::ShortGable)
                    ni = r.orientation ? (j >= h / 2 ? 1 : 0) : (i >= w / 2 ? 1 : 0);
                else if (r.type == Lot::ShedGable)
                    ni = 0;
                else
                    ni = T.nx > 0.1f ? 3 : T.nx < -0.1f ? 2 : T.ny > 0.1f ? 1 : 0;
                std::array<std::array<float, 3>, 3> p{}, q{};
                for (int k = 0; k < 3; ++k) {
                    p[size_t(k)] = pos(0, i + kCorner[T.cls][k][0], j + kCorner[T.cls][k][1]);
                    q[size_t(2 - k)] = pos(2, i + kCorner[T.cls][k][0], j + kCorner[T.cls][k][1]);
                }
                const std::array<int, 3> top = T.uv();
                emit(out.top, p, {atlasUV(top[0]), atlasUV(top[1]), atlasUV(top[2])}, slopeNormal(ni, false));
                const int* lo = kLowerUV[T.cls];
                emit(out.under, q, {atlasUV(lo[0]), atlasUV(lo[1]), atlasUV(lo[2])}, slopeNormal(ni, true));
            }
        }
    }

    // DrawOuterStrips (0xc66504): a quad per half tile along the grid's
    // border between two layers, hidden where its middle is under another
    // roof; normals -x, -y, +x, +y; texture indices 0, 1, 9, 8.
    auto strips = [&](MeshGroup& g, int upper, int lower) {
        auto quad = [&](int i0, int j0, int i1, int j1, std::array<float, 3> n) {
            const std::array<float, 3> a = pos(upper, i0, j0), b = pos(upper, i1, j1), c = pos(lower, i1, j1),
                                       dd = pos(lower, i0, j0);
            const float mx = 0.5f * (a[0] + b[0]), my = 0.5f * (a[1] + b[1]);
            if (centerBelowOtherRoof(idx, mx, my, false))
                return;
            const auto u0 = atlasUV(0), u1 = atlasUV(1), u9 = atlasUV(9), u8 = atlasUV(8);
            const uint32_t base = uint32_t(g.positions.size());
            for (const auto& [pp, uv] : {std::pair{a, u0}, std::pair{b, u1}, std::pair{c, u9}, std::pair{dd, u8}}) {
                g.positions.push_back(pp);
                g.uvs.push_back(uv);
                g.normals.push_back(n);
            }
            // Counter-clockwise seen from the side the normal points to.
            const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
            const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
            const float facing = (uy * vz - uz * vy) * n[0] + (uz * vx - ux * vz) * n[1] + (ux * vy - uy * vx) * n[2];
            if (facing >= 0)
                g.triangles.insert(g.triangles.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            else
                g.triangles.insert(g.triangles.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        };
        for (int j = 0; j + 1 < h; ++j)
            quad(0, j, 0, j + 1, {-1, 0, 0});
        for (int i = 0; i + 1 < w; ++i)
            quad(i, 0, i + 1, 0, {0, -1, 0});
        for (int j = 0; j + 1 < h; ++j)
            quad(w - 1, j, w - 1, j + 1, {1, 0, 0});
        for (int i = 0; i + 1 < w; ++i)
            quad(i, h - 1, i + 1, h - 1, {0, 1, 0});
        if (r.type == Lot::Mansard) {
            // The ring at grid lines 7 and n-8, facing in (the mansard's
            // IsTriangleCenterSupportedByOtherRoof test 0xc660d0 not ported).
            for (int j = 7; j < h - 8; ++j)
                if (w - 8 >= 0)
                    quad(w - 8, j, w - 8, j + 1, {-1, 0, 0});
            for (int i = 7; i < w - 8; ++i)
                if (h - 8 >= 0)
                    quad(i, h - 8, i + 1, h - 8, {0, -1, 0});
            for (int j = 7; j < h - 8; ++j)
                if (w > 7)
                    quad(7, j, 7, j + 1, {1, 0, 0});
            for (int i = 7; i < w - 8; ++i)
                if (h > 7)
                    quad(i, 7, i + 1, 7, {0, 1, 0});
        }
    };
    strips(out.upperStrip, 0, 1);
    strips(out.lowerStrip, 1, 2);
}

} // namespace os2::roofgame
