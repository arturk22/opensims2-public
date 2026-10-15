#include "lot_lightmap.hpp"
#include "graphics_options.hpp"

#include "lot.hpp"
#include "lot_diffusion.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <string>

namespace os2 {

namespace {

using V3 = std::array<float, 3>;

V3 operator+(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 operator*(V3 a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
V3 operator*(V3 a, V3 b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }
float dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float luminance(V3 c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint8_t toByte(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

} // namespace

void LotLightMapLayout::init(const Lot& lot) {
    lotWidth = lot.width, lotHeight = lot.height;
    minLevel = lot.minLevel, levels = lot.levelCount;
    // The light's rooms: 3ARY 0x0B (room definition 0, flooded from the
    // light-blocking walls; fences do not split it), where outdoors is room
    // 0 (docs/buy-build-mac.md §7.3, docs/lighting-mac.md).
    const auto light = lot.roomLayers.find(0x0B);
    const bool lightRooms = light != lot.roomLayers.end() && light->second.size() == lot.rooms.size();
    rooms = lightRooms ? light->second : lot.rooms;
    elevations = lot.elevations;
    floors = lot.floors;
    floorMaterials.assign(floors.size(), {});
    materialNames = {""};
    materialColors = {{1, 1, 1}};
    width = std::max<uint32_t>(256, lotWidth * kTexelsPerTile);
    height = uint32_t(levels) * pageHeight();
    shelfX_ = 0, shelfY_ = height, shelfH_ = 0;
    walls.clear();
    lightWalls.clear();
    for (const auto& w : lot.walls) {
        // walls.txt: blocksLight false for 4 (deck skirt), 16, 24 and 29
        // (pool walls, which the grid still blocks by their style, 29),
        // fences.txt: false.
        if (Lot::isFence(w.type) || w.type == 4 || w.type == 16 || w.type == 24)
            continue;
        lightWalls.push_back({w.a.level, int(std::lround(w.a.x)), int(std::lround(w.a.y)), int(std::lround(w.b.x)),
                              int(std::lround(w.b.y))});
    }
    outdoorRooms_.clear();
    // Without that map (lots that lack it), from 3ARY 0x09: a room is
    // outdoors when most of its quarters have no floor (inferred: fenced
    // yards are rooms of their own there).
    std::map<uint16_t, std::pair<size_t, size_t>> counts; // room -> (quarters, without floor)
    for (size_t i = 0; !lightRooms && i < rooms.size() && i < lot.floors.size(); ++i)
        for (size_t q = 0; q < 4; ++q)
            if (const uint16_t r = rooms[i][q]) {
                auto& c = counts[r];
                ++c.first;
                c.second += lot.floors[i][q] == 0;
            }
    for (const auto& [r, c] : counts)
        if (c.second * 2 >= c.first)
            outdoorRooms_.push_back(r);
    // The VM's rooms (3ARY 0x09) that are outdoors for the light: most of
    // their quarters outdoors in the light's map (a fenced yard).
    outdoorPlacementRooms_ = outdoorRooms_;
    if (lightRooms) {
        std::map<uint16_t, std::pair<size_t, size_t>> placement; // room -> (quarters, outdoors)
        for (size_t i = 0; i < lot.rooms.size(); ++i)
            for (size_t q = 0; q < 4; ++q)
                if (const uint16_t r = lot.rooms[i][q]) {
                    auto& c = placement[r];
                    ++c.first;
                    c.second += rooms[i][q] == 0;
                }
        for (const auto& [r, c] : placement)
            if (c.second * 2 > c.first)
                outdoorPlacementRooms_.push_back(r);
    }
}

bool LotLightMapLayout::outdoorPlacementRoom(uint16_t room) const {
    return !room ||
           std::find(outdoorPlacementRooms_.begin(), outdoorPlacementRooms_.end(), room) != outdoorPlacementRooms_.end();
}

std::array<float, 2> LotLightMapLayout::floorTexel(int level, float x, float y) const {
    const float page = float(std::clamp(level - minLevel, 0, std::max(levels - 1, 0)));
    return {x * kTexelsPerTile, page * float(pageHeight()) + y * kTexelsPerTile};
}

size_t LotLightMapLayout::addWall(WallFace face) {
    face.w = uint32_t(std::ceil(std::max(face.length, 0.0f) * kTexelsPerTile)) + 2;
    face.h = uint32_t(std::ceil(std::max(face.top - face.base, 0.0f) * kTexelsPerTile)) + 2;
    face.w = std::min(face.w, width);
    if (shelfX_ + face.w > width) {
        shelfY_ += shelfH_;
        shelfX_ = 0, shelfH_ = 0;
    }
    face.x = shelfX_, face.y = shelfY_;
    shelfX_ += face.w;
    shelfH_ = std::max(shelfH_, face.h);
    height = std::max(height, shelfY_ + shelfH_);
    walls.push_back(face);
    return walls.size() - 1;
}

std::array<float, 2> LotLightMapLayout::wallTexel(size_t i, float s, float z) const {
    const WallFace& f = walls[i];
    return {float(f.x) + 1 + s * kTexelsPerTile, float(f.y) + 1 + (f.top - z) * kTexelsPerTile};
}

uint16_t LotLightMapLayout::roomAt(int level, float x, float y) const {
    if (rooms.empty() || level < minLevel || level >= minLevel + levels || x < 0 || y < 0 ||
        x >= float(lotWidth) || y >= float(lotHeight))
        return 0;
    const uint32_t ix = uint32_t(x), iy = uint32_t(y);
    const size_t at = (size_t(level - minLevel) * lotWidth + ix) * lotHeight + iy;
    if (at >= rooms.size())
        return 0;
    // Quarters west, north (+y), east (+x), south meet at the tile's centre.
    const float dx = x - float(ix) - 0.5f, dy = y - float(iy) - 0.5f;
    const size_t q = std::abs(dx) > std::abs(dy) ? (dx < 0 ? 0 : 2) : (dy > 0 ? 1 : 3);
    return rooms[at][q];
}

bool LotLightMapLayout::outdoor(int, uint16_t room) const {
    return !room || std::find(outdoorRooms_.begin(), outdoorRooms_.end(), room) != outdoorRooms_.end();
}

float LotLightMapLayout::elevationAt(int level, float x, float y) const {
    const int w = int(lotWidth) + 1, h = int(lotHeight) + 1;
    auto at = [&](int gx, int gy) {
        if (level < minLevel || level >= minLevel + levels || elevations.empty())
            return 3.0f * float(level);
        gx = std::clamp(gx, 0, w - 1), gy = std::clamp(gy, 0, h - 1);
        const size_t i = (size_t(level - minLevel) * size_t(w) + size_t(gx)) * size_t(h) + size_t(gy);
        return i < elevations.size() ? elevations[i] : 3.0f * float(level);
    };
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - float(ix), fy = y - float(iy);
    const float a = at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx;
    const float b = at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx;
    return a * (1 - fy) + b * fy;
}

uint16_t LotLightMapLayout::materialIndex(const std::string& name) {
    if (name.empty())
        return 0;
    for (size_t i = 1; i < materialNames.size(); ++i)
        if (materialNames[i] == name)
            return uint16_t(i);
    materialNames.push_back(name);
    materialColors.push_back({1, 1, 1});
    return uint16_t(materialNames.size() - 1);
}

namespace {

// Index of the quarter tile under (x, y) on a level in the [level][x][y]
// arrays, and which quarter (west, north, east, south), or -1.
long quarterAt(const LotLightMapLayout& l, int level, float x, float y, size_t& q) {
    if (level < l.minLevel || level >= l.minLevel + l.levels || x < 0 || y < 0 || x >= float(l.lotWidth) ||
        y >= float(l.lotHeight))
        return -1;
    const uint32_t ix = uint32_t(x), iy = uint32_t(y);
    const float dx = x - float(ix) - 0.5f, dy = y - float(iy) - 0.5f;
    q = std::abs(dx) > std::abs(dy) ? (dx < 0 ? 0 : 2) : (dy > 0 ? 1 : 3);
    return long((size_t(level - l.minLevel) * l.lotWidth + ix) * l.lotHeight + iy);
}

} // namespace

std::array<float, 3> LotLightMapLayout::floorColorAt(int level, float x, float y) const {
    size_t q = 0;
    const long at = quarterAt(*this, level, x, y, q);
    if (at < 0 || size_t(at) >= floorMaterials.size())
        return {1, 1, 1};
    const uint16_t m = floorMaterials[size_t(at)][q];
    return m < materialColors.size() ? materialColors[m] : std::array<float, 3>{1, 1, 1};
}

bool LotLightMapLayout::hasFloorAt(int level, float x, float y) const {
    size_t q = 0;
    const long at = quarterAt(*this, level, x, y, q);
    return at >= 0 && size_t(at) < floors.size() && floors[size_t(at)][q] != 0;
}

float LotLightMapLayout::occluderAt(float x, float y) const {
    const long w = long(lotWidth) * kTexelsPerTile, h = long(lotHeight) * kTexelsPerTile;
    const long i = long(std::floor(x * kTexelsPerTile)), j = long(std::floor(y * kTexelsPerTile));
    if (occluders.empty() || i < 0 || j < 0 || i >= w || j >= h)
        return -1e9f;
    return occluders[size_t(j * w + i)];
}

LightMapGlobals LightMapGlobals::from(const LightingConfig& config) {
    LightMapGlobals g;
    auto numbers = [&](const char* name) {
        std::vector<float> out;
        auto it = config.globals.find(lower(name));
        if (it == config.globals.end())
            return out;
        const std::string& v = it->second;
        const char* p = v.c_str();
        while (*p) {
            char* end = nullptr;
            const float f = std::strtof(p, &end);
            if (end != p) {
                out.push_back(f);
                p = end;
            } else {
                ++p;
            }
        }
        return out;
    };
    auto one = [&](const char* name, float& v) {
        if (const auto n = numbers(name); !n.empty())
            v = n[0];
    };
    std::array<float, 3> color{1, 1, 1};
    if (const auto n = numbers("outsideDiffusionAmbientColor"); n.size() >= 3)
        color = {n[0], n[1], n[2]};
    float floor = 0.25f, wall = 0.25f;
    one("outsideFloorAmbient", floor);
    one("outsideWallAmbient", wall);
    for (size_t c = 0; c < 3; ++c)
        g.outsideAmbient[c] = color[c] * floor, g.outsideWallAmbient[c] = color[c] * wall;
    one("terrainStrength", g.terrainStrength);
    one("roomSaturation", g.roomSaturation);
    one("roomCeilingStrength", g.roomCeilingStrength);
    one("roomPortalStrength", g.roomPortalStrength);
    one("brightWallFloorContribution", g.brightWallFloor);
    one("darkWallFloorContribution", g.darkWallFloor);
    one("hmSunLightTransitionZone", g.sunTransitionZone);
    float steps = float(g.propagationSteps);
    one("wallPropagationSteps", steps);
    g.propagationSteps = std::max(0, int(steps));
    g.sunShadows = graphicsOptions().heightMapShadows;
    return g;
}

// The height map's shadow tops for a sun direction `L` (towards the sun),
// as cHeightMapShadow builds them (Mac BV: FillFromWalls 0xd76f40,
// FindHeightMapShadows 0xd76e68 with its column and row passes 0xd76254,
// 0xd76b74; docs/lighting-mac.md 5.3).
std::vector<float> sunShadowTops(const LotLightMapLayout& layout, std::array<float, 3> L) {
    constexpr int k = LotLightMapLayout::kTexelsPerTile;
    const long W = long(layout.lotWidth) * k, H = long(layout.lotHeight) * k;
    if (W <= 0 || H <= 0 || layout.occluders.size() != size_t(W * H))
        return {};
    // The grid: the highest floor over each texel (FillFromTerrain; the
    // terrain at the texel's centre) and the roofs.
    std::vector<float> g(layout.occluders);
    const int ground = layout.minLevel <= 0 && layout.minLevel + layout.levels > 0 ? 0 : layout.minLevel; // not a foundation's level -1
    for (long j = 0; j < H; ++j)
        for (long i = 0; i < W; ++i) {
            float& v = g[size_t(j * W + i)];
            v = std::max(v, layout.elevationAt(ground, (float(i) + 0.5f) / k, (float(j) + 0.5f) / k));
        }
    auto at = [&](long i, long j) -> float* {
        return i >= 0 && j >= 0 && i < W && j < H ? &g[size_t(j * W + i)] : nullptr;
    };
    // FillFromWalls: each wall's top written (not raised) along its texels,
    // from its top at one end to the other's: on the side away from the sun
    // for a wall along a grid line, for a diagonal one its texels and their
    // neighbours away from the sun.
    for (const auto& w : layout.sunWalls) {
        const long x0 = std::lround(w.a[0] * k), y0 = std::lround(w.a[1] * k);
        const long x1 = std::lround(w.b[0] * k), y1 = std::lround(w.b[1] * k);
        auto put = [&](long i, long j, float t) {
            if (float* p = at(i, j))
                *p = w.topA + (w.topB - w.topA) * t;
        };
        if (x0 == x1 || y0 == y1) {
            const bool vertical = x0 == x1; // along Y
            const long a = vertical ? y0 : x0, b = vertical ? y1 : x1;
            const long lo = std::min(a, b), hi = std::max(a, b);
            const float across = vertical ? L[0] : L[1];
            for (long q = lo; q < hi; ++q) {
                const float t = (float(q - a) + 0.5f) / float(b - a == 0 ? 1 : b - a);
                const float tt = std::clamp(a < b ? t : -t, 0.0f, 1.0f);
                if (across <= 0) // the +side's texels (column x0 or row y0)
                    vertical ? put(x0, q, tt) : put(q, y0, tt);
                if (across >= 0) // the -side's
                    vertical ? put(x0 - 1, q, tt) : put(q, y0 - 1, tt);
            }
        } else {
            const long n = std::abs(x1 - x0);
            const long sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1;
            // The texel of each step: the cell between (x, y) and (x + sx, y + sy).
            for (long q = 0; q < n; ++q) {
                const float t = (float(q) + 0.5f) / float(n);
                const long i = x0 + q * sx + (sx < 0 ? -1 : 0), j = y0 + q * sy + (sy < 0 ? -1 : 0);
                put(i, j, t);
                // The neighbours on the side away from the sun (inferred
                // from the binary's end texels: it fills the stair-step).
                const long ni = i - (L[0] > 0 ? 1 : L[0] < 0 ? -1 : 0);
                const long nj = j - (L[1] > 0 ? 1 : L[1] < 0 ? -1 : 0);
                if (ni != i)
                    put(ni, j, t);
                if (nj != j)
                    put(i, nj, t);
            }
        }
    }
    // FindHeightMapShadows: no sun, or the sun straight overhead, no shadow
    // (the tops at -1000); else a front of heights swept from the sun's
    // side, one column (or row) of texels at a time: each texel's top is
    // the front there after taking the grid's height, the front drops by
    // the sun's slope over a texel and moves sideways by the sun's
    // direction (a linear blend with the next texel).
    std::vector<float> top(size_t(W * H), -1000.0f);
    if (L[2] <= 0 || (std::fabs(L[0]) < 1e-4f && std::fabs(L[1]) < 1e-4f))
        return top;
    const float cell = 1.0f / float(k);
    const bool columns = std::fabs(L[0]) >= std::fabs(L[1]);
    const float la = columns ? L[0] : L[1], lb = columns ? L[1] : L[0];
    const long nA = columns ? W : H, nB = columns ? H : W;
    float dz = cell * L[2] / la, side = -lb / la;
    long a0 = 0, aEnd = nA, step = 1;
    if (dz >= 0) {
        dz = -dz, side = -side;
        a0 = nA - 1, aEnd = -1, step = -1;
    }
    std::vector<float> front(size_t(nB), -1000.0f);
    for (long a = a0; a != aEnd; a += step) {
        for (long b = 0; b < nB; ++b) {
            const size_t idx = columns ? size_t(b * W + a) : size_t(a * W + b);
            float& f = front[size_t(b)];
            f = std::max(f, g[idx]);
            top[idx] = f;
            f += dz;
        }
        if (side > 0) {
            for (long b = 0; b + 1 < nB; ++b)
                front[size_t(b)] += side * (front[size_t(b + 1)] - front[size_t(b)]);
        } else {
            for (long b = nB - 1; b > 0; --b)
                front[size_t(b)] += -side * (front[size_t(b - 1)] - front[size_t(b)]);
        }
    }
    return top;
}

LotLightMap computeLotLightMap(const LotLightMapLayout& layout, const LightingState& state,
                               const std::vector<LightMapLight>& lights, const LightMapGlobals& globals) {
    LotLightMap result;
    RgbaImage& out = result.image;
    out.width = layout.width, out.height = std::max<uint32_t>(layout.height, 1);
    out.pixels.assign(size_t(out.width) * out.height * 4, 0);
    for (size_t i = 3; i < out.pixels.size(); i += 4)
        out.pixels[i] = 255;

    // The state's direct light for a normal, FindIllumFromLightingState
    // (docs/lighting-mac.md 5.3): the first four lights, sun, sky, fill1, fill2.
    const V3 sky = V3{state.skyColor[0], state.skyColor[1], state.skyColor[2]} * state.skyStrength;
    struct Dir {
        V3 d, color;
        float intensity;
    };
    std::vector<Dir> direct;
    std::optional<Dir> sun;
    size_t sunIndex = 0;
    for (const auto& l : state.lights) {
        const std::string name = lower(l.name);
        if (name != "sun" && name != "sky" && name != "fill1" && name != "fill2")
            continue;
        V3 d{l.direction[0], l.direction[1], l.direction[2]};
        const float len = std::sqrt(dot(d, d));
        if (len == 0)
            continue;
        direct.push_back({d * (1 / len), V3{l.color[0], l.color[1], l.color[2]}, l.intensity});
        if (name == "sun")
            sun = direct.back(), sunIndex = direct.size() - 1;
    }
    // `sunKept`: the share of the sun's weight a height-map shadow leaves
    // (FUN_007047e4, docs/lighting-mac.md 5.3).
    auto illum = [&](V3 n, float sunKept = 1) {
        float w = 0;
        V3 e{0, 0, 0};
        for (size_t q = 0; q < direct.size(); ++q) {
            const Dir& d = direct[q];
            const float wi = std::max(dot(n, d.d), 0.0f) * d.intensity * (sun && q == sunIndex ? sunKept : 1.0f);
            e = e + d.color * wi;
            w += wi;
        }
        return sky * (1 - w) + e;
    };
    const float directScale = globals.terrainStrength * globals.floorLightMapScale;
    // The direct light on the ground (terrain and outside floors), with a
    // cool light's colour pulled towards grey by how much blue it has over
    // red: at night the state's moon (sun (.45, .45, 1) x 0.6, sky (.25,
    // .25, 1)) makes E about (0.32, 0.32, 0.8), and 2 x texture x light map
    // turned light paving bright lavender. A visual choice, not from the
    // binary (day and warm states are left as they are).
    auto groundIllum = [&](V3 n, float sunKept = 1) {
        V3 e = illum(n, sunKept);
        if (e[2] > e[0] && e[2] > 0) {
            const float k = (e[2] - e[0]) / e[2];
            const float l = luminance(e);
            e = e * (1 - k) + V3{l, l, l} * k;
        }
        return e;
    };
    const V3 outsideAmbient{globals.outsideAmbient[0], globals.outsideAmbient[1], globals.outsideAmbient[2]};
    const V3 outsideWallAmbient{globals.outsideWallAmbient[0], globals.outsideWallAmbient[1], globals.outsideWallAmbient[2]};

    auto modifier = [&](const char* name) {
        auto it = state.modifiers.find(name);
        return it == state.modifiers.end() ? LightingState::Modifier{} : it->second;
    };
    const auto insideObjects = modifier("insideobjects"), outsideObjects = modifier("outsideobjects");
    V3 insideColor{1, 1, 1};
    if (auto it = state.diffParams.find("insideambientcolor"); it != state.diffParams.end() && it->second.size() >= 3)
        insideColor = {it->second[0], it->second[1], it->second[2]};
    // The diffusion's parameters (cDiffusionManager::UpdateFromConfig and
    // SetDiffusionParams, docs/lighting-mac.md 10.5): the state's diffParams
    // inside, Lighting.txt's sets outside.
    DiffusionParams params;
    params.insideFloor = insideColor * state.diffParam("insidefloorambient", 0.001f);
    params.insideWall = insideColor * state.diffParam("insidewallambient", 0.001f);
    params.outsideFloor = outsideAmbient, params.outsideWall = outsideWallAmbient;
    params.floorThreshold = state.diffParam("floorthreshold", 0.4f);
    params.wallThreshold = state.diffParam("wallthreshold", 0.4f);
    params.steps = globals.propagationSteps;
    params.brightWallFloor = globals.brightWallFloor, params.darkWallFloor = globals.darkWallFloor;
    params.scale = globals.floorLightMapScale;

    const int levels = std::max(layout.levels, 0);
    const int k8 = LotLightMapLayout::kTexelsPerTile;
    const uint32_t pw = layout.lotWidth * uint32_t(k8), ph = layout.pageHeight();
    // The level whose floor a light is above (the objects' own level numbers
    // do not always agree with their height; inferred).
    auto levelOf = [&](const LightMapLight& l) {
        int li = std::clamp(l.level - layout.minLevel, 0, std::max(levels - 1, 0));
        for (int c = 0; c < levels; ++c)
            if (layout.elevationAt(layout.minLevel + c, l.position[0], l.position[1]) <= l.position[2] - 0.05f)
                li = c;
        return li;
    };
    auto facingOf = [](const std::array<float, 3>& d) { return std::atan2(d[1], d[0]); };
    auto anyLight = [](const std::array<float, 3>& c) { return c[0] > 1e-4f || c[1] > 1e-4f || c[2] > 1e-4f; };

    // The lamps' sources (BuildLightFromObjectLight): at the light, the
    // extension's rectangle (+-0.5), the light room there, colour x the
    // modifier's diffusion strength and falloff x its diffusion falloff
    // (insideObjects in a room, outsideObjects out).
    std::vector<DiffusionSource> sources;
    for (const auto& l : lights) {
        if (l.portal)
            continue;
        DiffusionSource s;
        s.level = layout.minLevel + levelOf(l);
        s.position = {l.position[0], l.position[1]};
        s.rect = {l.position[0] - 0.5f, l.position[1] - 0.5f, l.position[0] + 0.5f, l.position[1] + 0.5f};
        s.room = lightRoomAt(layout, s.level, l.position[0], l.position[1]);
        const auto& mod = s.room == 0 ? outsideObjects : insideObjects;
        s.rgb = V3{l.color[0], l.color[1], l.color[2]} * mod.diffusion;
        s.falloff = std::max(0.0f, l.falloff * mod.diffusionFalloff);
        s.floorScale = l.floorScale, s.wallScale = l.wallScale, s.interFloor = l.interFloor;
        s.directional = l.directional, s.directionalFalloff = l.directionalFalloff, s.facing = facingOf(l.direction);
        if (anyLight(s.rgb))
            sources.push_back(s);
    }
    std::vector<FloorColorGrid> floors = diffuseFloors(layout, sources, params);
    std::vector<std::vector<uint8_t>> pages(static_cast<size_t>(levels)), written(static_cast<size_t>(levels));
    auto copyPages = [&] {
        for (int li = 0; li < levels; ++li)
            floors[size_t(li)].copy(pages[size_t(li)], written[size_t(li)]);
    };
    copyPages();
    // The floor diffusion at a sample of a level (its page texel, x 2 x 0.5).
    auto sampleLight = [&](int li, int sx, int sy) -> V3 {
        const uint32_t i = uint32_t(sx * k8 / 2 + k8 / 4), j = uint32_t(sy * k8 / 2 + k8 / 4);
        if (li < 0 || li >= levels || i >= pw || j >= ph)
            return {0, 0, 0};
        const uint8_t* px = &pages[size_t(li)][(size_t(j) * pw + i) * 3];
        return {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
    };

    // Portals (windows and doors), shining into the side their light points
    // to (FindPortalLightValues, docs/lighting-mac.md 5.4), from the lamps'
    // light maps (the game reads the previous frame's).
    struct Portal {
        uint16_t room = 0; // the room it shines into
        bool toOutside = false;
        V3 dir{};          // the way the light travels
        V3 color{};        // direct x f + spill x f_spill (room irradiance)
        int level = 0;
    };
    std::vector<Portal> portals;
    const auto insidePortals = modifier("insideportals"), insidePortalSpills = modifier("insideportalspills"),
               outsidePortals = modifier("outsideportals"), outsidePortalSpills = modifier("outsideportalspills");
    std::vector<std::array<int, 5>> portalSides; // li, from x/y, to x/y
    const size_t lampSources = sources.size();
    for (const auto& l : lights) {
        if (!l.portal || levels == 0)
            continue;
        const int li = levelOf(l);
        const int level = layout.minLevel + li;
        const FloorColorGrid& g = floors[size_t(li)];
        // The wall it sits in: the nearest light-blocking wall along x or y
        // whose span the light is over, across the way the light points
        // when it leans (inferred: the game keeps the portals in its rooms).
        const float lx = l.position[0], ly = l.position[1];
        const float hx = l.direction[0], hy = l.direction[1], hl = std::sqrt(hx * hx + hy * hy);
        int ax = -1;
        float line = 0, best = 1.25f;
        for (const auto& w : layout.lightWalls) {
            if (w.level != level || (w.ax != w.bx && w.ay != w.by))
                continue;
            const int axis = w.ax == w.bx ? 0 : 1; // the axis across the wall
            const float across = axis == 0 ? lx - float(w.ax) : ly - float(w.ay);
            const float lo = float(axis == 0 ? std::min(w.ay, w.by) : std::min(w.ax, w.bx));
            const float hi = float(axis == 0 ? std::max(w.ay, w.by) : std::max(w.ax, w.bx));
            const float along = axis == 0 ? ly : lx;
            if (along < lo || along > hi || std::fabs(across) >= best)
                continue;
            if (hl > 0.1f && std::fabs((axis == 0 ? hx : hy) / hl) < 0.7f)
                continue;
            best = std::fabs(across), ax = axis, line = float(axis == 0 ? w.ax : w.ay);
        }
        if (std::getenv("OS2_DEBUG_LIGHTMAP"))
            std::fprintf(stderr, "portal light at (%.2f %.2f %.2f) li %d axis %d line %.1f\n", lx, ly, l.position[2], li, ax, line);
        if (ax < 0)
            continue;
        // The side its light points to (the light's -Z; one light per side).
        const float lean = l.direction[size_t(ax)];
        const bool toPlus = std::fabs(lean) > 1e-3f ? lean > 0 : (ax == 0 ? lx : ly) < line;
        auto sideSample = [&](float sgn) {
            const float x = ax == 0 ? line + 0.25f * sgn : lx, y = ax == 1 ? line + 0.25f * sgn : ly;
            return std::array<int, 2>{std::clamp(int(std::floor(x * 2)), 0, 2 * g.width - 1),
                                      std::clamp(int(std::floor(y * 2)), 0, 2 * g.height - 1)};
        };
        const auto fromS = sideSample(toPlus ? -1.0f : 1.0f), toS = sideSample(toPlus ? 1.0f : -1.0f);
        const int* from = fromS.data();
        const int* to = toS.data();
        if (std::any_of(portalSides.begin(), portalSides.end(), [&](const std::array<int, 5>& p) {
                return p[0] == li && p[1] == from[0] && p[2] == from[1] && p[3] == to[0] && p[4] == to[1];
            }))
            continue;
        portalSides.push_back({li, from[0], from[1], to[0], to[1]});
        const bool toB = toPlus;
        // The rooms on the two sides (the light rooms, with or without floor there).
        struct SideRoom {
            uint16_t room;
            bool outside;
        };
        auto sideRoom = [&](const int* smp) {
            const uint16_t r = lightRoomAt(layout, level, (float(smp[0]) + 0.5f) / 2, (float(smp[1]) + 0.5f) / 2);
            return SideRoom{r, r == 0};
        };
        const SideRoom fromHalf = sideRoom(from), toHalf = sideRoom(to);
        if (fromHalf.room == toHalf.room)
            continue;
        const float sgn = toB ? 1.0f : -1.0f;
        const V3 dir{ax == 0 ? sgn : 0, ax == 1 ? sgn : 0, 0};
        const V3 outward = dir * -1.0f; // the side the light comes from
        // L: the light on the side it comes from (inferred: the floor's, plus
        // the state's direct light outdoors, where the game reads the wall's).
        V3 L = sampleLight(li, from[0], from[1]) * 2.0f * globals.floorLightMapScale;
        if (fromHalf.outside)
            L = L + illum(outward) * directScale;
        V3 dl{0, 0, 0}, spill{0, 0, 0};
        float f = 1, fs = 0;
        const bool hasSky = state.skyStrength > 0;
        if (fromHalf.outside && !toHalf.outside) {
            if (!hasSky || !sun) {
                dl = illum(outward);
            } else {
                f = std::max(0.0f, dot(sun->d, outward));
                dl = sun->color * sun->intensity;
                spill = sky, fs = 1;
            }
            dl = dl + L, spill = spill + L * 0.5f;
            fs = 1;
        } else {
            dl = L * 0.5f;
            if (hasSky)
                spill = dl, fs = 1;
        }
        const bool toOut = toHalf.outside;
        const auto& mod = toOut ? outsidePortals : insidePortals;
        const auto& smod = toOut ? outsidePortalSpills : insidePortalSpills;
        Portal p;
        p.room = toHalf.room, p.toOutside = toOut, p.dir = dir, p.level = layout.minLevel + li;
        p.color = dl * f + spill * fs;
        portals.push_back(p);
        const V3 lc{l.color[0], l.color[1], l.color[2]};
        const float cx = (float(to[0]) + 0.5f) / 2, cy = (float(to[1]) + 0.5f) / 2;
        for (int part = 0; part < 2; ++part) {
            DiffusionSource s;
            s.level = layout.minLevel + li;
            s.position = {cx, cy};
            s.rect = {cx - 0.5f, cy - 0.5f, cx + 0.5f, cy + 0.5f};
            s.room = toHalf.room;
            const auto& m = part ? smod : mod;
            s.rgb = lc * (part ? spill * fs : dl * f) * m.diffusion;
            s.falloff = std::max(0.0f, l.falloff * m.diffusionFalloff);
            s.floorScale = l.floorScale, s.wallScale = l.wallScale, s.interFloor = l.interFloor;
            s.directional = l.directional, s.directionalFalloff = l.directionalFalloff, s.facing = facingOf(dir);
            if (anyLight(s.rgb))
                sources.push_back(s);
        }
    }
    if (sources.size() > lampSources) {
        floors = diffuseFloors(layout, sources, params);
        copyPages();
    }
    if (std::getenv("OS2_DEBUG_LIGHTMAP"))
        for (const auto& src : sources)
            std::fprintf(stderr, "lightmap source level %d room %d colour %.2f %.2f %.2f falloff %.2f%s\n", src.level, int(src.room),
                         src.rgb[0], src.rgb[1], src.rgb[2], src.falloff, src.directional ? " directional" : "");
    const auto wallTexels = diffuseWalls(layout, sources, params, floors, pages);

    auto put = [&](uint32_t x, uint32_t y, V3 e) {
        if (x >= out.width || y >= out.height)
            return;
        uint8_t* px = &out.pixels[(size_t(y) * out.width + x) * 4];
        for (size_t c = 0; c < 3; ++c)
            px[c] = toByte(e[c]);
    };

    // The terrain's normal (ground level without floor; up elsewhere).
    auto normalAt = [&](int level, float x, float y) -> V3 {
        if (level != 0 || layout.hasFloorAt(level, x, y))
            return {0, 0, 1};
        const float h = 0.25f;
        const float dx = (layout.elevationAt(level, x + h, y) - layout.elevationAt(level, x - h, y)) / (2 * h);
        const float dy = (layout.elevationAt(level, x, y + h) - layout.elevationAt(level, x, y - h)) / (2 * h);
        const V3 n{-dx, -dy, 1};
        return n * (1 / std::sqrt(dot(n, n)));
    };
    // The sun's height-map shadow (cHeightMapShadow; LightTerrainTileWithShadows,
    // Mac BV 0x705068, its floor twin 0x702fd8): d = how far the shadow's
    // top is over the texel; a shadowed texel keeps max(0, 1 - d x
    // hmSunLightTransitionZone) of the sun's weight.
    const bool sunShadows = globals.sunShadows && sun && sun->d[2] > 0 && !layout.occluders.empty() &&
                            size_t(pw) * ph == layout.occluders.size();
    std::vector<float> shadowTop;
    if (sunShadows)
        shadowTop = sunShadowTops(layout, {sun->d[0], sun->d[1], sun->d[2]});
    auto shadowDepth = [&](float x, float y, float z) {
        if (shadowTop.empty())
            return 0.0f;
        const long i = long(std::floor(x * float(k8))), j = long(std::floor(y * float(k8)));
        if (i < 0 || j < 0 || i >= long(pw) || j >= long(ph))
            return 0.0f;
        return shadowTop[size_t(j) * pw + size_t(i)] - z;
    };
    auto sunKept = [&](float d) {
        return d <= 0 ? 1.0f : std::max(0.0f, 1.0f - d * globals.sunTransitionZone);
    };

    // Floor texels: the diffusion's page, plus the state's direct light on
    // the outdoor tiles (AddDirectFloorLighting: the tiles of room 0).
    for (int li = 0; li < levels; ++li) {
        const int level = layout.minLevel + li;
        for (uint32_t j = 0; j < ph; ++j)
            for (uint32_t i = 0; i < pw; ++i) {
                const size_t at = size_t(j) * pw + i;
                if (!written[size_t(li)][at])
                    continue;
                const uint8_t* px = &pages[size_t(li)][at * 3];
                V3 e{px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
                const float x = (float(i) + 0.5f) / float(k8), y = (float(j) + 0.5f) / float(k8);
                if (lightRoomAt(layout, level, x, y) == 0) {
                    const float kept = sunKept(shadowDepth(x, y, layout.elevationAt(level, x, y)));
                    e = e + groundIllum(normalAt(level, x, y), kept) * directScale;
                }
                put(i, uint32_t(li) * ph + j, e);
            }
    }

    // Room irradiance (GatherFloorLighting): per sample half of a room,
    // I[4] += 0.25 c x mix(white, floor colour, roomSaturation), I[5] +=
    // 0.25 c x roomCeilingStrength.
    auto albedo = [&](const std::array<float, 3>& c) {
        const float s = globals.roomSaturation;
        return V3{1 - s + c[0] * s, 1 - s + c[1] * s, 1 - s + c[2] * s};
    };
    std::map<std::pair<int, uint16_t>, std::pair<std::array<V3, 6>, std::array<float, 6>>> gather;
    for (const auto& g : floors)
        for (int sy = 0; sy < 2 * g.height; ++sy)
            for (int sx = 0; sx < 2 * g.width; ++sx)
                for (const auto& h : g.at(sx, sy).h) {
                    if (!h.exists || h.outside)
                        continue;
                    const float x = (float(sx) + 0.5f) / 2, y = (float(sy) + 0.5f) / 2;
                    auto& gr = gather[{g.level, h.room}];
                    const V3 c{h.c[0], h.c[1], h.c[2]};
                    gr.first[4] = gr.first[4] + c * albedo(layout.floorColorAt(g.level, x, y)) * 0.25f, gr.second[4] += 1;
                    gr.first[5] = gr.first[5] + c * (0.25f * globals.roomCeilingStrength), gr.second[5] += 1;
                }
    auto addSide = [](std::pair<std::array<V3, 6>, std::array<float, 6>>& g, float nx, float ny, V3 c) {
        if (nx > 0)
            g.first[0] = g.first[0] + c * nx, g.second[0] += nx;
        if (nx < 0)
            g.first[1] = g.first[1] + c * -nx, g.second[1] += -nx;
        if (ny > 0)
            g.first[2] = g.first[2] + c * ny, g.second[2] += ny;
        if (ny < 0)
            g.first[3] = g.first[3] + c * -ny, g.second[3] += -ny;
    };

    // Wall texels: the wall diffusion's cells, plus the state's direct
    // light on the sides facing out (AddDirectWallLighting).
    for (size_t fi = 0; fi < layout.walls.size(); ++fi) {
        const auto& f = layout.walls[fi];
        const int li = f.level - layout.minLevel;
        if (li < 0 || li >= levels)
            continue;
        const V3 n{f.normal[0], f.normal[1], 0};
        const bool out_ = layout.outdoor(f.level, f.room);
        const V3 sunlit = out_ ? illum(n) * directScale : V3{0, 0, 0};
        // A face towards the sun takes the height map's shadow texel by
        // texel (AddDirectWallLighting -> LightWallTileWithShadows, Mac BV
        // 0x708590, 0x704d54: the point a little off the face, d = the
        // shadow's top there less its height).
        const bool wallShadows = out_ && !shadowTop.empty() && dot(n, sun->d) > 0;
        const auto& cell = wallTexels[fi];
        V3 sum{0, 0, 0};
        size_t count = 0;
        for (uint32_t j = 0; j < f.h; ++j)
            for (uint32_t i = 0; i < f.w; ++i) {
                const auto& b = cell[size_t(j) * f.w + i];
                V3 e{b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f};
                if (out_) {
                    const float s = std::clamp((float(i) - 0.5f) / float(k8), 0.0f, f.length);
                    const float z = std::clamp(f.top - (float(j) - 0.5f) / float(k8), f.base, f.top);
                    const float x = f.origin[0] + f.along[0] * s, y = f.origin[1] + f.along[1] * s;
                    const float off = 0.5f / float(k8);
                    e = e + (wallShadows ? illum(n, sunKept(shadowDepth(x + n[0] * off, y + n[1] * off, z))) * directScale : sunlit);
                }
                put(f.x + i, f.y + j, e);
                sum = sum + e, ++count;
            }
        // GatherWallLighting: c = 0.5 x the wall's light x mix(white,
        // wallpaper colour, roomSaturation), by its inward normal.
        if (!out_ && count) {
            const auto& wc = f.material < layout.materialColors.size() ? layout.materialColors[f.material]
                                                                       : std::array<float, 3>{1, 1, 1};
            addSide(gather[{f.level, f.room}], n[0], n[1], sum * (0.5f / float(count)) * albedo(wc));
        }
    }

    // Portals into a room: colour x roomPortalStrength, by the way the light travels.
    for (const auto& p : portals)
        if (!p.toOutside)
            addSide(gather[{p.level, p.room}], p.dir[0], p.dir[1], p.color * globals.roomPortalStrength);
    for (const auto& [key, g] : gather) {
        auto& r = result.rooms[key];
        for (size_t d = 0; d < 6; ++d)
            r[d] = g.second[d] > 0 ? g.first[d] * (1 / g.second[d]) : V3{0, 0, 0};
    }

    // Unlit rooms (cLightingManager::FindUnlitRooms, Mac BV 0x701a28, and
    // BrightenUnlitRooms 0x707820, after GatherRoomLighting): the state
    // keeps one unlitRoomColor and threshold t (the last command wins);
    // with a colour and t > 0, a room whose mean irradiance (the six
    // directions / 6) has its largest channel m under t gets f = 1 - m / t:
    // colour x f x the light map's scale (0.5) added to every floor texel
    // of the room and every texel of its walls' sides (AddToARGB32,
    // saturating), and colour x f added to the first five of its six
    // irradiance colours. (t <= 0 gives the game a denormal f: no effect.)
    if (!state.unlitRooms.empty()) {
        const auto& u = state.unlitRooms.back();
        const V3 unlit{u.color[0], u.color[1], u.color[2]};
        std::map<std::pair<int, uint16_t>, float> unlitShare;
        if (u.threshold > 0 && (unlit[0] != 0 || unlit[1] != 0 || unlit[2] != 0))
            for (auto& [key, r] : result.rooms) {
                V3 mean{0, 0, 0};
                for (const auto& v : r)
                    mean = mean + v;
                mean = mean * (1.0f / 6);
                const float m = std::max({std::fabs(mean[0]), std::fabs(mean[1]), std::fabs(mean[2])});
                if (m < u.threshold) {
                    const float f = 1 - m / u.threshold;
                    unlitShare[key] = f;
                    for (size_t d = 0; d < 5; ++d)
                        r[d] = r[d] + unlit * f;
                }
            }
        auto add = [&](uint32_t x, uint32_t y, V3 e) {
            if (x >= out.width || y >= out.height)
                return;
            uint8_t* px = &out.pixels[(size_t(y) * out.width + x) * 4];
            for (size_t c = 0; c < 3; ++c)
                px[c] = uint8_t(std::min(255L, long(px[c]) + std::lround(std::clamp(e[c], 0.0f, 1.0f) * 255.0f)));
        };
        if (!unlitShare.empty()) {
            for (int li = 0; li < levels; ++li)
                for (uint32_t j = 0; j < ph; ++j)
                    for (uint32_t i = 0; i < pw; ++i) {
                        const int level = layout.minLevel + li;
                        const float x = (float(i) + 0.5f) / float(k8), y = (float(j) + 0.5f) / float(k8);
                        if (!layout.hasFloorAt(level, x, y))
                            continue;
                        const uint16_t r = layout.roomAt(level, x, y);
                        if (layout.outdoor(level, r))
                            continue;
                        if (auto it = unlitShare.find({level, r}); it != unlitShare.end())
                            add(i, uint32_t(li) * ph + j, unlit * (it->second * globals.floorLightMapScale));
                    }
            for (const auto& f : layout.walls)
                if (!layout.outdoor(f.level, f.room))
                    if (auto it = unlitShare.find({f.level, f.room}); it != unlitShare.end())
                        for (uint32_t j = 0; j < f.h; ++j)
                            for (uint32_t i = 0; i < f.w; ++i)
                                add(f.x + i, f.y + j, unlit * (it->second * globals.wallLightMapScale));
        }
    }
    return result;
}

} // namespace os2
