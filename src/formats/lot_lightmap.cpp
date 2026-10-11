#include "lot_lightmap.hpp"

#include "lot.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
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
    rooms = lot.rooms;
    elevations = lot.elevations;
    width = std::max<uint32_t>(256, lotWidth * kTexelsPerTile);
    height = uint32_t(levels) * pageHeight();
    shelfX_ = 0, shelfY_ = height, shelfH_ = 0;
    walls.clear();
    outdoorRooms_.clear();
    // A room is outdoors when most of its quarter tiles have no floor
    // (terrain): the lots keep fenced yards and the strip along the lot's
    // edge as rooms of their own (inferred; the game keeps an "outside" flag
    // per room that the lot data does not seem to hold).
    std::map<uint16_t, std::pair<size_t, size_t>> counts; // room -> (quarters, without floor)
    for (size_t i = 0; i < rooms.size() && i < lot.floors.size(); ++i)
        for (size_t q = 0; q < 4; ++q)
            if (const uint16_t r = rooms[i][q]) {
                auto& c = counts[r];
                ++c.first;
                c.second += lot.floors[i][q] == 0;
            }
    for (const auto& [r, c] : counts)
        if (c.second * 2 >= c.first)
            outdoorRooms_.push_back(r);
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

LotLightMap computeLotLightMap(const LotLightMapLayout& layout, const LightingState& state,
                               const std::vector<LightMapLight>& lights) {
    LotLightMap result;
    RgbaImage& out = result.image;
    out.width = layout.width, out.height = std::max<uint32_t>(layout.height, 1);
    out.pixels.assign(size_t(out.width) * out.height * 4, 0);
    for (size_t i = 3; i < out.pixels.size(); i += 4)
        out.pixels[i] = 255;

    // Outdoors: half the state's outdoor light (docs/lighting-mac.md 3), no simFill (inferred).
    const V3 sky = V3{state.skyColor[0], state.skyColor[1], state.skyColor[2]} * state.skyStrength;
    struct Dir {
        V3 l, color;
    };
    std::vector<Dir> sun;
    for (const auto& l : state.lights) {
        if (lower(l.name).starts_with("simfill"))
            continue;
        V3 d{l.direction[0], l.direction[1], l.direction[2]};
        const float len = std::sqrt(dot(d, d));
        if (len == 0)
            continue;
        sun.push_back({d * (1 / len), vmax(V3{l.color[0], l.color[1], l.color[2]} - sky, {0, 0, 0}) * l.intensity});
    }
    auto outdoorLight = [&](V3 n) {
        V3 e = sky;
        for (const auto& d : sun)
            e = e + d.color * std::max(dot(n, d.l), 0.0f);
        return e * 0.5f;
    };

    auto modifier = [&](const char* name) {
        auto it = state.modifiers.find(name);
        return it == state.modifiers.end() ? LightingState::Modifier{} : it->second;
    };
    const auto insideObjects = modifier("insideobjects"), outsideObjects = modifier("outsideobjects"),
               insidePortals = modifier("insideportals");
    const V3 portalTint = sky * 2.0f;
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
        if (i < 0 || j < 0 || i >= long(gw) || j >= long(gh))
            return -1;
        return long(size_t(li) * perLevel + size_t(j) * gw + size_t(i));
    };
    const V3 up{0, 0, 1};
    const V3 outdoorUp = outdoorLight(up);
    for (int li = 0; li < levels; ++li) {
        const int level = layout.minLevel + li;
        for (uint32_t j = 0; j < gh; ++j)
            for (uint32_t i = 0; i < gw; ++i) {
                const size_t at = size_t(li) * perLevel + size_t(j) * gw + i;
                room[at] = layout.roomAt(level, (float(i) + 0.5f) / 2, (float(j) + 0.5f) / 2);
                outside[at] = layout.outdoor(level, room[at]);
                grid[at] = outside[at] ? outdoorUp : insideFloor;
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
        bool portal = false;
    };
    std::vector<Source> sources;
    for (const auto& l : lights) {
        // The level whose floor the light is above (the objects' own level
        // numbers do not always agree with their height).
        int li = std::clamp(l.level - layout.minLevel, 0, std::max(levels - 1, 0));
        for (int c = 0; c < levels; ++c)
            if (layout.elevationAt(layout.minLevel + c, l.position[0], l.position[1]) <= l.position[2] - 0.05f)
                li = c;
        if (li < 0 || li >= levels)
            continue;
        Source s;
        s.position = l.position;
        s.level = layout.minLevel + li, s.li = li;
        s.floorScale = l.floorScale, s.wallScale = l.wallScale;
        s.portal = l.portal;
        if (l.portal) {
            for (const auto& o : std::array<std::array<float, 2>, 4>{{{0.35f, 0}, {-0.35f, 0}, {0, 0.35f}, {0, -0.35f}}}) {
                const long at = sampleAt(li, l.position[0] + o[0], l.position[1] + o[1]);
                if (at < 0 || outside[size_t(at)])
                    continue;
                if (std::none_of(s.seeds.begin(), s.seeds.end(), [&](size_t q) { return room[q] == room[size_t(at)]; }))
                    s.seeds.push_back(size_t(at));
            }
        } else {
            const long at = sampleAt(li, l.position[0], l.position[1]);
            if (at >= 0) {
                s.seeds.push_back(size_t(at));
                s.outdoor = outside[size_t(at)];
            }
        }
        const auto& mod = l.portal ? insidePortals : s.outdoor ? outsideObjects : insideObjects;
        s.color = V3{l.color[0], l.color[1], l.color[2]} * mod.diffusion;
        if (l.portal)
            s.color = s.color * portalTint;
        s.falloff = std::max(0.0f, l.falloff * mod.diffusionFalloff);
        if (s.seeds.empty() || luminance(s.color) <= 1e-3f)
            continue;
        sources.push_back(std::move(s));
    }
    std::stable_sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.falloff > b.falloff; });
    if (std::getenv("OS2_DEBUG_LIGHTMAP"))
        for (const auto& src : sources)
            std::fprintf(stderr, "lightmap source li %d seeds %zu room %d out %d colour %.2f %.2f %.2f falloff %.2f\n", src.li,
                         src.seeds.size(), int(room[src.seeds[0]]), int(src.outdoor), src.color[0], src.color[1], src.color[2], src.falloff);

    // Seed and propagate, sources of equal falloff together.
    static const int kDx[8] = {0, -1, -1, -1, 0, 1, 1, 1}, kDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    for (int li = 0; li < levels; ++li) {
        for (size_t first = 0; first < sources.size();) {
            size_t last = first;
            while (last < sources.size() && std::abs(sources[last].falloff - sources[first].falloff) < 0.01f)
                ++last;
            bool any = false;
            for (size_t si = first; si < last; ++si) {
                const Source& s = sources[si];
                if (s.li != li)
                    continue;
                for (size_t q : s.seeds) {
                    grid[q] = vmax(grid[q], s.color * s.floorScale);
                    any = true;
                }
            }
            const float k = sources[first].falloff * floorThreshold;
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

    // Floor texels: the samples interpolated, those of other rooms replaced by the texel's own.
    const int k8 = LotLightMapLayout::kTexelsPerTile;
    const uint32_t pw = layout.lotWidth * uint32_t(k8), ph = layout.pageHeight();
    auto floorValue = [&](int li, float x, float y) -> V3 {
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
        return e;
    };
    for (int li = 0; li < levels; ++li)
        for (uint32_t j = 0; j < ph; ++j)
            for (uint32_t i = 0; i < pw; ++i)
                put(i, uint32_t(li) * ph + j, floorValue(li, (float(i) + 0.5f) / float(k8), (float(j) + 0.5f) / float(k8)));

    // Room irradiance (GatherFloorLighting): I[4] += 0.25 c, I[5] += 0.25 c x roomCeilingStrength (1).
    std::map<std::pair<int, uint16_t>, std::pair<std::array<V3, 6>, std::array<float, 6>>> gather;
    for (size_t at = 0; at < grid.size(); ++at)
        if (!outside[at]) {
            auto& g = gather[{layout.minLevel + int(at / perLevel), room[at]}];
            g.first[4] = g.first[4] + grid[at] * 0.25f, g.second[4] += 1;
            g.first[5] = g.first[5] + grid[at] * 0.25f, g.second[5] += 1;
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
        const V3 ambient = out_ ? outdoorLight(n) : insideWall;
        std::vector<const Source*> src;
        for (const auto& s : sources)
            if (s.li == li && (out_ ? s.outdoor : !s.outdoor && std::any_of(s.seeds.begin(), s.seeds.end(), [&](size_t q) {
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
        const float contribution = lit ? 1.0f : 0.5f;
        V3 extra{0, 0, 0};
        if (!out_)
            if (auto it = brighten.find({f.level, f.room}); it != brighten.end())
                extra = it->second;
        V3 sum{0, 0, 0};
        size_t count = 0;
        for (uint32_t j = 0; j < f.h; ++j)
            for (uint32_t i = 0; i < f.w; ++i) {
                const float s = std::clamp((float(i) - 0.5f) / float(k8), 0.0f, f.length);
                const float z = std::clamp(f.top - (float(j) - 0.5f) / float(k8), f.base, f.top);
                const float x = f.origin[0] + f.along[0] * s, y = f.origin[1] + f.along[1] * s;
                V3 e = lightAt(V3{x, y, z});
                if (!out_) {
                    const V3 floor = floorValue(li, x + n[0] * 0.25f, y + n[1] * 0.25f) * contribution;
                    e = vmax(e, floor * (1 / std::pow(1 + wallThreshold, 2 * (z - f.base))));
                    e = e + extra;
                }
                put(f.x + i, f.y + j, e);
                sum = sum + e, ++count;
            }
        // GatherWallLighting: c = 0.5 x the wall's light, by its inward normal.
        if (!out_ && count)
            addSide(gather[{f.level, f.room}], n[0], n[1], sum * (0.5f / float(count)));
    }

    // Portals: colour x roomPortalStrength (0.75), by the direction into each room.
    for (const auto& s : sources)
        if (s.portal)
            for (size_t q : s.seeds) {
                const float qx = (float(q % perLevel % gw) + 0.5f) / 2, qy = (float(q % perLevel / gw) + 0.5f) / 2;
                float dx = qx - s.position[0], dy = qy - s.position[1];
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-4f)
                    continue;
                dx /= len, dy /= len;
                addSide(gather[{s.level, room[q]}], dx, dy, s.color * 0.75f);
            }
    for (const auto& [key, g] : gather) {
        auto& r = result.rooms[key];
        for (size_t d = 0; d < 6; ++d)
            r[d] = g.second[d] > 0 ? g.first[d] * (1 / g.second[d]) : V3{0, 0, 0};
    }
    return result;
}

} // namespace os2
