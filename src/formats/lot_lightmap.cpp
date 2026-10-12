#include "lot_lightmap.hpp"
#include "graphics_options.hpp"

#include "lot.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>

namespace os2 {

namespace {

using V3 = std::array<float, 3>;

V3 operator+(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 operator-(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 operator*(V3 a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
V3 operator*(V3 a, V3 b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }
float dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 vmax(V3 a, V3 b) { return {std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2])}; }
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
    g.sunShadows = graphicsOptions().heightMapShadows;
    return g;
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
    const V3 insideFloor = insideColor * state.diffParam("insidefloorambient", 0.001f);
    const V3 insideWall = insideColor * state.diffParam("insidewallambient", 0.001f);
    const float floorThreshold = state.diffParam("floorthreshold", 0.4f);
    const float wallThreshold = state.diffParam("wallthreshold", 0.4f);

    // The half-tile sample grid of each level.
    const uint32_t gw = layout.lotWidth * 2, gh = layout.lotHeight * 2;
    const size_t perLevel = size_t(gw) * gh;
    const int levels = std::max(layout.levels, 0);
    std::vector<V3> grid(perLevel * size_t(levels));
    std::vector<uint16_t> room(grid.size());
    std::vector<uint8_t> outside(grid.size());
    auto sampleAt = [&](int li, float x, float y) -> long {
        const long i = long(std::floor(x * 2)), j = long(std::floor(y * 2));
        if (li < 0 || li >= levels || i < 0 || j < 0 || i >= long(gw) || j >= long(gh))
            return -1;
        return long(size_t(li) * perLevel + size_t(j) * gw + size_t(i));
    };
    auto samplePos = [&](size_t at) {
        return std::array<float, 2>{(float(at % perLevel % gw) + 0.5f) / 2, (float(at % perLevel / gw) + 0.5f) / 2};
    };
    for (int li = 0; li < levels; ++li) {
        const int level = layout.minLevel + li;
        for (uint32_t j = 0; j < gh; ++j)
            for (uint32_t i = 0; i < gw; ++i) {
                const size_t at = size_t(li) * perLevel + size_t(j) * gw + i;
                room[at] = layout.roomAt(level, (float(i) + 0.5f) / 2, (float(j) + 0.5f) / 2);
                outside[at] = layout.outdoor(level, room[at]);
                grid[at] = outside[at] ? outsideAmbient : insideFloor;
            }
    }
    auto connected = [&](size_t a, size_t b) { return outside[a] ? outside[b] != 0 : room[a] == room[b]; };

    // Sources, each with the samples it seeds.
    struct Source {
        V3 position, color; // colour x the modifier's diffusion strength
        float falloff = 1;  // x the modifier's diffusion falloff
        float floorScale = 1, wallScale = 1;
        int level = 0, li = 0;
        std::vector<size_t> seeds;
        bool outdoor = false;
        bool portal = false; // directional (diffusionSourceIsDirectional): away from its wall
    };
    // The level whose floor a light is above (the objects' own level numbers
    // do not always agree with their height).
    auto levelOf = [&](const LightMapLight& l) {
        int li = std::clamp(l.level - layout.minLevel, 0, std::max(levels - 1, 0));
        for (int c = 0; c < levels; ++c)
            if (layout.elevationAt(layout.minLevel + c, l.position[0], l.position[1]) <= l.position[2] - 0.05f)
                li = c;
        return li;
    };
    // Outdoors, light above a level without floor falls to the floor or
    // terrain below (sources on upper levels seed the grids below them where
    // the floor lets light through; the inter-floor falloff is taken as 0).
    auto settle = [&](int& li, size_t& at) {
        const auto p = samplePos(at);
        while (li > 0 && outside[at] && !layout.hasFloorAt(layout.minLevel + li, p[0], p[1])) {
            const long below = sampleAt(li - 1, p[0], p[1]);
            if (below < 0 || !outside[size_t(below)])
                break;
            --li, at = size_t(below);
        }
    };
    std::vector<Source> sources;
    for (const auto& l : lights) {
        if (l.portal)
            continue;
        int li = levelOf(l);
        Source s;
        s.position = l.position;
        s.level = layout.minLevel + li, s.li = li;
        s.floorScale = l.floorScale, s.wallScale = l.wallScale;
        const long found = sampleAt(li, l.position[0], l.position[1]);
        if (found < 0)
            continue;
        size_t at = size_t(found);
        settle(li, at);
        s.level = layout.minLevel + li, s.li = li;
        s.seeds.push_back(at);
        s.outdoor = outside[at];
        const auto& mod = s.outdoor ? outsideObjects : insideObjects;
        s.color = V3{l.color[0], l.color[1], l.color[2]} * mod.diffusion;
        s.falloff = std::max(0.0f, l.falloff * mod.diffusionFalloff);
        if (luminance(s.color) > 1e-3f)
            sources.push_back(std::move(s));
    }

    // Seed and propagate, sources of equal falloff together.
    static const int kDx[8] = {0, -1, -1, -1, 0, 1, 1, 1}, kDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    auto diffuse = [&](std::vector<const Source*> batch) {
        std::stable_sort(batch.begin(), batch.end(), [](const Source* a, const Source* b) { return a->falloff > b->falloff; });
        for (int li = 0; li < levels; ++li) {
            for (size_t first = 0; first < batch.size();) {
                size_t last = first;
                while (last < batch.size() && std::abs(batch[last]->falloff - batch[first]->falloff) < 0.01f)
                    ++last;
                bool any = false;
                for (size_t si = first; si < last; ++si) {
                    const Source& s = *batch[si];
                    if (s.li != li)
                        continue;
                    for (size_t q : s.seeds) {
                        grid[q] = vmax(grid[q], s.color * s.floorScale);
                        any = true;
                    }
                }
                const float k = batch[first]->falloff * floorThreshold;
                for (int pass = 0; any && pass < 50; ++pass) {
                    bool changed = false;
                    const bool flipX = pass & 1, flipY = pass & 2;
                    for (uint32_t jj = 0; jj < gh; ++jj)
                        for (uint32_t ii = 0; ii < gw; ++ii) {
                            const uint32_t i = flipX ? gw - 1 - ii : ii, j = flipY ? gh - 1 - jj : jj;
                            const size_t at = size_t(li) * perLevel + size_t(j) * gw + i;
                            V3& c = grid[at];
                            for (int d = 0; d < 8; ++d) {
                                const long ni = long(i) + kDx[d], nj = long(j) + kDy[d];
                                if (ni < 0 || nj < 0 || ni >= long(gw) || nj >= long(gh))
                                    continue;
                                const size_t nb = size_t(li) * perLevel + size_t(nj) * gw + size_t(ni);
                                if (!connected(at, nb))
                                    continue;
                                const float kk = (d & 1) ? k * 1.4f : k;
                                for (size_t ch = 0; ch < 3; ++ch)
                                    if (grid[nb][ch] - c[ch] > kk * c[ch] + 1e-4f) {
                                        c[ch] = grid[nb][ch] / (1 + kk) + 0.01f;
                                        changed = true;
                                    }
                            }
                        }
                    if (!changed)
                        break;
                }
                first = last;
            }
        }
    };
    {
        std::vector<const Source*> batch;
        for (const auto& s : sources)
            batch.push_back(&s);
        diffuse(batch);
    }

    // What the light map shows at a sample: outdoors, the diffusion less the
    // outside ambient plus the state's direct light for `n`.
    auto shown = [&](size_t at, V3 n) {
        if (!outside[at])
            return grid[at];
        return vmax(grid[at] - outsideAmbient, {0, 0, 0}) + illum(n) * directScale;
    };

    // Portals (windows and doors): one per side, shining into the room across
    // (FindPortalLightValues, docs/lighting-mac.md 5.4), after the lamps.
    struct Portal {
        size_t from = 0, to = 0; // the samples it shines from and into
        uint16_t room = 0;    // and its room
        bool toOutside = false;
        V3 dir{};             // the way the light travels
        V3 color{};           // direct x f + spill x f_spill (room irradiance)
        int level = 0;
    };
    std::vector<Portal> portals;
    const auto insidePortals = modifier("insideportals"), insidePortalSpills = modifier("insideportalspills"),
               outsidePortals = modifier("outsideportals"), outsidePortalSpills = modifier("outsideportalspills");
    const size_t lampSources = sources.size();
    for (const auto& l : lights) {
        if (!l.portal)
            continue;
        const int li = levelOf(l);
        // The wall it sits in: the nearest pair of neighbouring samples
        // (along x or y, within a tile) that light does not pass between.
        long a = -1, b = -1;
        int axis = -1;
        float best = 1e9f;
        for (int ax = 0; ax < 2; ++ax)
            for (float t = -1.25f; t <= 0.76f; t += 0.5f) {
                const float x0 = l.position[0] + (ax == 0 ? t : 0), y0 = l.position[1] + (ax == 1 ? t : 0);
                const long sa = sampleAt(li, x0, y0), sb = sampleAt(li, x0 + (ax == 0 ? 0.5f : 0), y0 + (ax == 1 ? 0.5f : 0));
                if (sa < 0 || sb < 0 || sa == sb || connected(size_t(sa), size_t(sb)))
                    continue;
                if (std::abs(t + 0.25f) < best)
                    best = std::abs(t + 0.25f), a = sa, b = sb, axis = ax;
            }
        if (std::getenv("OS2_DEBUG_LIGHTMAP"))
            std::fprintf(stderr, "portal light at (%.2f %.2f %.2f) li %d axis %d rooms %d %d\n", l.position[0], l.position[1],
                         l.position[2], li, axis, a < 0 ? -1 : int(room[size_t(a)]), b < 0 ? -1 : int(room[size_t(b)]));
        if (axis < 0)
            continue;
        {
            for (int side = 0; side < 2; ++side) {
                const size_t from = size_t(side ? b : a), to = size_t(side ? a : b);
                // Each window and door has a light per side: one portal per direction.
                if (std::any_of(portals.begin(), portals.end(), [&](const Portal& p) { return p.from == from && p.to == to; }))
                    continue;
                const float sgn = side ? -1.0f : 1.0f;
                const V3 dir{axis == 0 ? sgn : 0, axis == 1 ? sgn : 0, 0};
                const V3 outward = dir * -1.0f; // the side the light comes from
                const V3 L = shown(from, outward);
                V3 dl{0, 0, 0}, spill{0, 0, 0};
                float f = 1, fs = 0;
                const bool hasSky = state.skyStrength > 0;
                if (outside[from] && !outside[to]) {
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
                const bool toOut = outside[to] != 0;
                const auto& mod = toOut ? outsidePortals : insidePortals;
                const auto& smod = toOut ? outsidePortalSpills : insidePortalSpills;
                Portal p;
                p.from = from, p.to = to, p.room = room[to], p.toOutside = toOut, p.dir = dir, p.level = layout.minLevel + li;
                p.color = dl * f + spill * fs;
                portals.push_back(p);
                const V3 lc{l.color[0], l.color[1], l.color[2]};
                int seedLevel = li;
                size_t seed = to;
                settle(seedLevel, seed);
                for (int part = 0; part < 2; ++part) {
                    Source s;
                    s.position = l.position;
                    s.level = layout.minLevel + seedLevel, s.li = seedLevel;
                    s.floorScale = l.floorScale, s.wallScale = l.wallScale;
                    s.seeds.push_back(seed);
                    s.outdoor = toOut;
                    s.portal = true;
                    const auto& m = part ? smod : mod;
                    s.color = lc * (part ? spill * fs : dl * f) * m.diffusion;
                    s.falloff = std::max(0.0f, l.falloff * m.diffusionFalloff);
                    if (luminance(s.color) > 1e-3f)
                        sources.push_back(std::move(s));
                }
            }
        }
    }
    {
        std::vector<const Source*> batch;
        for (size_t i = lampSources; i < sources.size(); ++i)
            batch.push_back(&sources[i]);
        diffuse(batch);
    }
    if (std::getenv("OS2_DEBUG_LIGHTMAP"))
        for (const auto& src : sources)
            std::fprintf(stderr, "lightmap source li %d room %d out %d colour %.2f %.2f %.2f falloff %.2f\n", src.li,
                         int(room[src.seeds[0]]), int(src.outdoor), src.color[0], src.color[1], src.color[2], src.falloff);

    // Dark rooms: + 0.5 x unlitRoomColor (the brightest whose threshold the room's mean is under).
    std::map<std::pair<int, uint16_t>, std::pair<V3, size_t>> sums;
    for (size_t at = 0; at < grid.size(); ++at)
        if (!outside[at]) {
            auto& sm = sums[{layout.minLevel + int(at / perLevel), room[at]}];
            sm.first = sm.first + grid[at];
            ++sm.second;
        }
    std::map<std::pair<int, uint16_t>, V3> brighten;
    for (const auto& [key, sm] : sums) {
        const float bright = luminance(sm.first * (1.0f / float(sm.second)));
        V3 add{0, 0, 0};
        for (const auto& u : state.unlitRooms)
            if (bright < u.threshold && luminance(V3{u.color[0], u.color[1], u.color[2]}) > luminance(add))
                add = V3{u.color[0], u.color[1], u.color[2]};
        if (luminance(add) > 0)
            brighten[key] = add * 0.5f;
    }
    for (size_t at = 0; at < grid.size(); ++at)
        if (!outside[at])
            if (auto it = brighten.find({layout.minLevel + int(at / perLevel), room[at]}); it != brighten.end())
                grid[at] = grid[at] + it->second;

    auto put = [&](uint32_t x, uint32_t y, V3 e) {
        if (x >= out.width || y >= out.height)
            return;
        uint8_t* px = &out.pixels[(size_t(y) * out.width + x) * 4];
        for (size_t c = 0; c < 3; ++c)
            px[c] = toByte(e[c]);
    };

    // Floor texels: the samples interpolated, those of other rooms replaced
    // by the texel's own; outdoors less the outside ambient (`outdoor`).
    const int k8 = LotLightMapLayout::kTexelsPerTile;
    const uint32_t pw = layout.lotWidth * uint32_t(k8), ph = layout.pageHeight();
    auto floorValue = [&](int li, float x, float y, bool* outdoor = nullptr) -> V3 {
        const long own = sampleAt(li, std::clamp(x, 0.0f, float(layout.lotWidth) - 1e-3f),
                                  std::clamp(y, 0.0f, float(layout.lotHeight) - 1e-3f));
        if (own < 0)
            return {0, 0, 0};
        const float sx = x * 2 - 0.5f, sy = y * 2 - 0.5f;
        const long i0 = long(std::floor(sx)), j0 = long(std::floor(sy));
        const float fx = sx - float(i0), fy = sy - float(j0);
        V3 e{0, 0, 0};
        for (int c = 0; c < 4; ++c) {
            const long i = std::clamp<long>(i0 + (c & 1), 0, long(gw) - 1);
            const long j = std::clamp<long>(j0 + (c >> 1), 0, long(gh) - 1);
            size_t at = size_t(li) * perLevel + size_t(j) * gw + size_t(i);
            if (!connected(size_t(own), at))
                at = size_t(own);
            const float w = ((c & 1) ? fx : 1 - fx) * ((c >> 1) ? fy : 1 - fy);
            e = e + grid[at] * w;
        }
        const bool out_ = outside[size_t(own)] != 0;
        if (outdoor)
            *outdoor = out_;
        return out_ ? vmax(e - outsideAmbient, {0, 0, 0}) : e;
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
    // Mac BV 0x705068, and its floor twin 0x702fd8): d = how far the
    // shadow's top is over the texel; a shadowed texel keeps
    // max(0, 1 - d x hmSunLightTransitionZone) of the sun's weight. The
    // shadow's top here: the highest of the occluders (walls, roofs, upper
    // floors) and the terrain met going towards the sun, less the sun's
    // drop over the way (FindHeightMapShadows is not decoded: inferred).
    const bool sunShadows = globals.sunShadows && sun && sun->d[2] > 0.05f && !layout.occluders.empty();
    float topOfAll = -1e9f;
    if (sunShadows) {
        for (float v : layout.occluders)
            topOfAll = std::max(topOfAll, v);
        for (float v : layout.elevations)
            topOfAll = std::max(topOfAll, v);
    }
    auto sunKeptAt = [&](int level, float x, float y, float z) {
        if (!sunShadows)
            return 1.0f;
        const float hx = sun->d[0], hy = sun->d[1];
        const float hl = std::sqrt(hx * hx + hy * hy);
        if (hl < 1e-4f)
            return 1.0f;
        const float step = 1.0f / float(k8);
        const float ux = hx / hl * step, uy = hy / hl * step, drop = sun->d[2] / hl * step; // per step: along, up
        float top = -1e9f;
        float px = x, py = y, rise = 0;
        for (int k = 1; k < 4096; ++k) {
            px += ux, py += uy, rise += drop;
            if (px < 0 || py < 0 || px >= float(layout.lotWidth) || py >= float(layout.lotHeight) || z + rise > topOfAll)
                break;
            float h = layout.occluderAt(px, py);
            h = std::max(h, layout.elevationAt(layout.minLevel, px, py));
            top = std::max(top, h - rise);
        }
        (void)level;
        const float d = top - z;
        return d <= 0 ? 1.0f : std::max(0.0f, 1.0f - d * globals.sunTransitionZone);
    };
    for (int li = 0; li < levels; ++li)
        for (uint32_t j = 0; j < ph; ++j)
            for (uint32_t i = 0; i < pw; ++i) {
                const float x = (float(i) + 0.5f) / float(k8), y = (float(j) + 0.5f) / float(k8);
                bool outdoor = false;
                V3 e = floorValue(li, x, y, &outdoor);
                if (outdoor) {
                    const int level = layout.minLevel + li;
                    const float kept = sunKeptAt(level, x, y, layout.elevationAt(level, x, y));
                    e = e + groundIllum(normalAt(level, x, y), kept) * directScale;
                }
                put(i, uint32_t(li) * ph + j, e);
            }

    // Room irradiance (GatherFloorLighting): per sample, I[4] += 0.25 c x
    // mix(white, floor colour, roomSaturation), I[5] += 0.25 c x roomCeilingStrength.
    auto albedo = [&](const std::array<float, 3>& c) {
        const float s = globals.roomSaturation;
        return V3{1 - s + c[0] * s, 1 - s + c[1] * s, 1 - s + c[2] * s};
    };
    std::map<std::pair<int, uint16_t>, std::pair<std::array<V3, 6>, std::array<float, 6>>> gather;
    for (size_t at = 0; at < grid.size(); ++at)
        if (!outside[at]) {
            const int level = layout.minLevel + int(at / perLevel);
            const auto p = samplePos(at);
            auto& g = gather[{level, room[at]}];
            g.first[4] = g.first[4] + grid[at] * albedo(layout.floorColorAt(level, p[0], p[1])) * 0.25f, g.second[4] += 1;
            g.first[5] = g.first[5] + grid[at] * (0.25f * globals.roomCeilingStrength), g.second[5] += 1;
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

    // Walls (inferred, see the header).
    for (const auto& f : layout.walls) {
        const int li = f.level - layout.minLevel;
        if (li < 0 || li >= levels)
            continue;
        const V3 n{f.normal[0], f.normal[1], 0};
        const bool out_ = layout.outdoor(f.level, f.room);
        const V3 ambient = out_ ? outsideWallAmbient : insideWall;
        std::vector<const Source*> src;
        for (const auto& s : sources)
            if (s.li == li && !s.portal && (out_ ? s.outdoor : !s.outdoor && std::any_of(s.seeds.begin(), s.seeds.end(), [&](size_t q) {
                                                       return room[q] == f.room;
                                                   })))
                src.push_back(&s);
        auto lightAt = [&](V3 p) {
            V3 e = ambient;
            for (const Source* s : src) {
                const V3 d = s->position - p;
                const float steps = 2 * std::sqrt(dot(d, d));
                const float k = s->falloff * wallThreshold;
                e = vmax(e, s->color * (s->wallScale / std::pow(1 + k, steps)));
            }
            return e;
        };
        // Lit directly: a light reaches the face's middle above the ambient.
        const V3 mid{f.origin[0] + f.along[0] * f.length / 2, f.origin[1] + f.along[1] * f.length / 2, (f.base + f.top) / 2};
        const bool lit = luminance(lightAt(mid)) > luminance(ambient) + 0.02f;
        const float contribution = lit ? globals.brightWallFloor : globals.darkWallFloor;
        V3 extra{0, 0, 0};
        if (!out_)
            if (auto it = brighten.find({f.level, f.room}); it != brighten.end())
                extra = it->second;
        const V3 sunlit = out_ ? illum(n) * directScale : V3{0, 0, 0};
        V3 sum{0, 0, 0};
        size_t count = 0;
        for (uint32_t j = 0; j < f.h; ++j)
            for (uint32_t i = 0; i < f.w; ++i) {
                const float s = std::clamp((float(i) - 0.5f) / float(k8), 0.0f, f.length);
                const float z = std::clamp(f.top - (float(j) - 0.5f) / float(k8), f.base, f.top);
                const float x = f.origin[0] + f.along[0] * s, y = f.origin[1] + f.along[1] * s;
                V3 e = lightAt(V3{x, y, z});
                if (out_) {
                    e = vmax(e - ambient, {0, 0, 0}) + sunlit;
                } else {
                    const V3 floor = floorValue(li, x + n[0] * 0.25f, y + n[1] * 0.25f) * contribution;
                    e = vmax(e, floor * (1 / std::pow(1 + wallThreshold, 2 * (z - f.base))));
                    e = e + extra;
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
    return result;
}

} // namespace os2
