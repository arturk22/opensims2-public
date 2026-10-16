#include "object_shadow.hpp"

#include "lot.hpp"
#include "model.hpp"
#include "xform.hpp"

#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace os2 {

namespace {

constexpr const char* kNames[kGuobCount] = {"groundshadow",    "wallshadow",     "northwallshadow",
                                             "southwallshadow", "eastwallshadow", "westwallshadow"};

bool sameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// The binary rounds with the float trick (v + 12582912) - 12582912: to the
// nearest integer, halves to even (the FPU's default mode).
int roundEven(float v) { return int(std::nearbyint(v)); }

} // namespace

int guobIndex(std::string_view subset) {
    for (int i = 0; i < kGuobCount; ++i)
        if (sameNoCase(subset, kNames[i]))
            return i;
    return -1;
}

const char* guobSubsetName(int index) { return index >= 0 && index < kGuobCount ? kNames[index] : ""; }

GuobModelInfo guobModelInfo(const ObjectModel& model) {
    GuobModelInfo out;
    constexpr float inf = std::numeric_limits<float>::infinity();
    out.lo = {inf, inf, inf};
    out.hi = {-inf, -inf, -inf};
    auto grow = [&](const std::array<float, 3>& p) {
        for (int k = 0; k < 3; ++k) {
            out.lo[size_t(k)] = std::min(out.lo[size_t(k)], p[size_t(k)]);
            out.hi[size_t(k)] = std::max(out.hi[size_t(k)], p[size_t(k)]);
        }
        out.hasBounds = true;
    };
    // The shapes' bounds (their GMDCs' bounding meshes); a model built
    // without them (tests, generated models): its vertices.
    for (const auto& b : model.bounds)
        if (!b.hidden)
            for (const auto& p : b.points)
                grow(p);
    for (const auto& part : model.parts) {
        if (model.bounds.empty())
            for (const auto& p : part.mesh.positions)
                grow(p);
        const int i = guobIndex(part.mesh.name);
        if (i >= 0 && !part.material.empty() && !sameNoCase(part.material, "GUOBOff"))
            out.present |= uint8_t(1u << i);
    }
    if (!out.hasBounds)
        out.lo = out.hi = {0, 0, 0};
    return out;
}

void guobWorldBox(const Xform& at, const std::array<float, 3>& lo, const std::array<float, 3>& hi,
                  std::array<float, 3>& worldLo, std::array<float, 3>& worldHi) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    worldLo = {inf, inf, inf};
    worldHi = {-inf, -inf, -inf};
    for (int c = 0; c < 8; ++c) {
        const Vec3 w = at.apply(Vec3{c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]});
        for (int k = 0; k < 3; ++k) {
            worldLo[size_t(k)] = std::min(worldLo[size_t(k)], w[size_t(k)]);
            worldHi[size_t(k)] = std::max(worldHi[size_t(k)], w[size_t(k)]);
        }
    }
}

LotWallEdges::LotWallEdges(const Lot& lot) {
    for (const auto& w : lot.walls) {
        const int ax = int(std::lround(w.a.x)), ay = int(std::lround(w.a.y));
        const int bx = int(std::lround(w.b.x)), by = int(std::lround(w.b.y));
        if (ax != bx && ay != by)
            continue; // diagonal
        if (ay == by)
            for (int x = std::min(ax, bx); x < std::max(ax, bx); ++x)
                edges_.insert({w.a.level, x, ay, true});
        else
            for (int y = std::min(ay, by); y < std::max(ay, by); ++y)
                edges_.insert({w.a.level, ax, y, false});
    }
}

bool LotWallEdges::operator()(int x0, int y0, int x1, int y1, int level) const {
    if (y0 == y1 && std::abs(x1 - x0) == 1)
        return edges_.count({level, std::min(x0, x1), y0, true}) != 0;
    if (x0 == x1 && std::abs(y1 - y0) == 1)
        return edges_.count({level, x0, std::min(y0, y1), false}) != 0;
    return false;
}

namespace {
void addBeside(std::set<std::tuple<int, int, int>>& out, const std::tuple<int, int, int, bool>& e) {
    const auto [level, x, y, alongX] = e;
    out.insert({level, x, y});
    out.insert(alongX ? std::tuple{level, x, y - 1} : std::tuple{level, x - 1, y});
}
} // namespace

std::set<std::tuple<int, int, int>> LotWallEdges::tilesBeside() const {
    std::set<std::tuple<int, int, int>> out;
    for (const auto& e : edges_)
        addBeside(out, e);
    return out;
}

std::set<std::tuple<int, int, int>> LotWallEdges::tilesBeside(int level) const {
    std::set<std::tuple<int, int, int>> out;
    for (const auto& e : edges_)
        if (std::get<0>(e) == level)
            addBeside(out, e);
    return out;
}

std::set<std::tuple<int, int, int>> LotWallEdges::tilesBesideChanges(const LotWallEdges& a, const LotWallEdges& b) {
    std::set<std::tuple<int, int, int>> out;
    for (const auto& e : a.edges_)
        if (!b.edges_.count(e))
            addBeside(out, e);
    for (const auto& e : b.edges_)
        if (!a.edges_.count(e))
            addBeside(out, e);
    return out;
}

GuobTileRect guobTileRect(const std::array<float, 3>& lo, const std::array<float, 3>& hi) {
    // Low edges: round(v - 0.5), one more when that lands exactly on v;
    // high edges: round(v + 0.5), one less likewise (FUN_00a8e9ac).
    auto low = [](float v) {
        int a = roundEven(v - 0.5f);
        if (float(a + 1) == v)
            ++a;
        return a;
    };
    auto high = [](float v) {
        int b = roundEven(v + 0.5f);
        if (float(b - 1) == v)
            --b;
        return b;
    };
    return {low(lo[0]), low(lo[1]), high(hi[0]), high(hi[1])};
}

uint8_t guobWallSides(const GuobTileRect& r, int direction, int level, bool levelShown, const GuobWorld& world) {
    if (!levelShown || world.wallMode != 2 || !world.wallExists)
        return 0;
    // Each side holds when every tile edge along it has a wall (a side with
    // no edge holds too); the loops stop at the first edge without one.
    bool a = true, b = true, c = true, d = true;
    for (int x = r.x0; x < r.x1 && a; ++x)
        a = world.wallExists(x + 1, r.y0, x, r.y0, level); // along y = y0
    for (int x = r.x0; x < r.x1 && b; ++x)
        b = world.wallExists(x + 1, r.y1, x, r.y1, level); // along y = y1
    for (int y = r.y0; y < r.y1 && c; ++y)
        c = world.wallExists(r.x0, y, r.x0, y + 1, level); // along x = x0
    for (int y = r.y0; y < r.y1 && d; ++y)
        d = world.wallExists(r.x1, y, r.x1, y + 1, level); // along x = x1
    // The planes are named in the model's frame (north +y, east +x), turned
    // by the object's direction: (-direction / 2) & 3, truncating.
    bool north = false, south = false, east = false, west = false;
    switch ((-direction / 2) & 3) {
    case 0: north = c, south = d, east = b, west = a; break;
    case 1: north = a, south = b, east = c, west = d; break;
    case 2: north = d, south = c, east = a, west = b; break;
    default: north = b, south = a, east = d, west = c; break;
    }
    return uint8_t((north << kGuobNorth) | (south << kGuobSouth) | (east << kGuobEast) | (west << kGuobWest));
}

uint8_t guobsOn(uint8_t present, bool visible, uint8_t wallSides, const GuobWorld& world) {
    if (!visible)
        return 0;
    uint8_t on = uint8_t(1u << kGuobGround);
    if (world.wallMode != 0)
        on |= uint8_t(1u << kGuobWall);
    on |= wallSides & uint8_t((1u << kGuobNorth) | (1u << kGuobSouth) | (1u << kGuobEast) | (1u << kGuobWest));
    return on & present;
}

} // namespace os2
