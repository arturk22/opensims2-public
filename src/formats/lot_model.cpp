#include "lot_model.hpp"

#include "binreader.hpp"
#include "config_text.hpp"
#include "lot.hpp"
#include "lot_roof.hpp"
#include "property_set.hpp"
#include "rcol.hpp"
#include "resindex.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace os2 {

namespace {

constexpr uint32_t kTypeTxmt = 0x49596978, kTypeCatalogPattern = 0xCCA8E925, kTypeCatalogRoof = 0xACA8EA06,
                   kTypeCatalogFence = 0x2CB230B8;

// Wall slab half thickness, in tiles (inferred from screenshots of the game).
constexpr float kWallHalfThickness = 0.075f;
// Ground floors sit on the terrain; lift them so they win the depth test
// against the terrain paints drawn after them.
constexpr float kGroundFloorLift = 0.005f;
// Terrain textures repeat every this many tiles (not found in the data yet).
constexpr float kTerrainTextureTiles = 4.0f;
constexpr float kStoryHeight = 3.0f;
// Attic walls stop this far below the roof surface (OpenTS2: 2.5 x the wall's
// half thickness), half walls under a fence this far above their floor.
constexpr float kAtticRoofOffset = 0.1875f;
constexpr float kHalfWallHeight = 1.0f;

// `kind` "floorPattern" / "wallPattern" blocks of a catalog script, each
// followed by a `material "..."` line.
std::map<std::string, std::string> loadPatternScript(const std::filesystem::path& path, std::string_view kind) {
    std::map<std::string, std::string> out;
    if (!std::filesystem::exists(path))
        return out;
    std::string current;
    forEachConfigLine(readTextFile(path), [&](std::string_view line) {
        const auto t = tokenizeConfigLine(line);
        if (t.size() < 2 || t[0].kind != ConfigToken::Word || t[1].kind != ConfigToken::Word)
            return;
        if (t[0].word == kind)
            current = lowerAscii(t[1].word);
        else if (t[0].word == "material" && !current.empty())
            out[current] = t[1].word;
    });
    return out;
}

std::optional<uint32_t> parseGuid(const std::string& s) {
    if (s.empty() || s.size() > 8 || !std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(uint8_t(c)); }))
        return std::nullopt;
    return uint32_t(std::stoul(s, nullptr, 16));
}

MaterialDefinition terrainMaterial(const std::string& texture, bool blended) {
    MaterialDefinition m;
    m.materialType = "StandardMaterial";
    m.properties = {{"stdMatBaseTextureName", texture}, {"stdMatDiffCoef", "0.8,0.8,0.8,1"}};
    if (blended)
        m.properties.emplace_back("stdMatAlphaBlendMode", "blend");
    return m;
}

using Vec3f = std::array<float, 3>;

struct MeshBuilder {
    MeshGroup mesh;

    uint32_t vertex(const Vec3f& p, const Vec3f& n, float u, float v) {
        mesh.positions.push_back(p);
        mesh.normals.push_back(n);
        mesh.uvs.push_back({u, v});
        return uint32_t(mesh.positions.size() - 1);
    }
    void triangle(uint32_t a, uint32_t b, uint32_t c) { mesh.triangles.insert(mesh.triangles.end(), {a, b, c}); }
    void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        triangle(a, b, c);
        triangle(a, c, d);
    }
};

Vec3f normalOf(const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    const Vec3f u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    Vec3f n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0)
        for (auto& x : n)
            x /= len;
    return n;
}

// Terrain grid over level 0; `keep(x, y)` picks the tiles to emit.
template <typename Keep>
MeshGroup terrainMesh(const Lot& lot, Keep&& keep) {
    MeshBuilder b;
    const int w = int(lot.width), h = int(lot.height);
    for (int x = 0; x <= w; ++x)
        for (int y = 0; y <= h; ++y) {
            const float dx = lot.elevation(0, x + 1, y) - lot.elevation(0, x - 1, y);
            const float dy = lot.elevation(0, x, y + 1) - lot.elevation(0, x, y - 1);
            const float len = std::sqrt(dx * dx + dy * dy + 4.0f);
            b.vertex({float(x), float(y), lot.elevation(0, x, y)}, {-dx / len, -dy / len, 2.0f / len},
                     float(x) / kTerrainTextureTiles, float(y) / kTerrainTextureTiles);
        }
    auto at = [&](int x, int y) { return uint32_t(x * (h + 1) + y); };
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y)
            if (keep(x, y))
                b.quad(at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1));
    return std::move(b.mesh);
}

// Quarter-tile triangles (west, north, east, south) of each floor tile, with
// the tile's own texture coordinates, one mesh per material.
void addFloors(const Lot& lot, const PatternCatalog& patterns, std::map<std::string, MeshBuilder>& out,
               std::vector<std::string>& warnings) {
    std::map<uint16_t, std::string> materials;
    for (const auto& [ref, pattern] : lot.floorPatterns) {
        const std::string m = patterns.floorMaterial(pattern);
        if (m.empty())
            warnings.push_back("floor pattern " + pattern + " has no material");
        materials[ref] = m;
    }
    for (int level = lot.minLevel; level < lot.minLevel + lot.levelCount; ++level) {
        const float lift = level == 0 ? kGroundFloorLift : 0.0f;
        for (uint32_t x = 0; x < lot.width; ++x)
            for (uint32_t y = 0; y < lot.height; ++y) {
                const auto& quarters = lot.floor(level, x, y);
                if (quarters == std::array<uint16_t, 4>{})
                    continue;
                const int ix = int(x), iy = int(y);
                const float z00 = lot.elevation(level, ix, iy) + lift, z10 = lot.elevation(level, ix + 1, iy) + lift,
                            z11 = lot.elevation(level, ix + 1, iy + 1) + lift,
                            z01 = lot.elevation(level, ix, iy + 1) + lift;
                const float fx = float(x), fy = float(y);
                const Vec3f p00{fx, fy, z00}, p10{fx + 1, fy, z10}, p11{fx + 1, fy + 1, z11}, p01{fx, fy + 1, z01},
                    c{fx + 0.5f, fy + 0.5f, (z00 + z10 + z11 + z01) / 4};
                // Counter-clockwise from above: west, north, east, south.
                const std::array<std::array<Vec3f, 2>, 4> edges{{{p00, p01}, {p01, p11}, {p11, p10}, {p10, p00}}};
                for (size_t q = 0; q < 4; ++q) {
                    if (!quarters[q])
                        continue;
                    auto it = materials.find(quarters[q]);
                    if (it == materials.end() || it->second.empty())
                        continue;
                    MeshBuilder& b = out["floor:" + it->second];
                    const Vec3f& a = edges[q][1];
                    const Vec3f& e = edges[q][0];
                    const Vec3f n = normalOf(a, e, c);
                    // Texture top = west edge, right = north: the road tiles' edge
                    // lines (road_sidelines_01 at the west curb, _03 at the east
                    // one) only run along a north-south road this way. The
                    // coordinates run on across tiles, so that materials spanning
                    // several tiles (floorMaterialScaleU/V) continue.
                    auto v = [&](const Vec3f& p) { return b.vertex(p, n, p[1], p[0]); };
                    b.triangle(v(a), v(e), v(c));
                }
            }
    }
}

// Walls as slabs: one face per side with that side's covering, plus a top.
// Each slab is stretched by its half thickness at both ends to close corners;
// texture coordinates follow world position, so overlaps line up. Attic walls
// (gables) rise to the roof above them, half walls under a fence to 1 tile.
void addWalls(const Lot& lot, const PatternCatalog& patterns, const LotRoofs& roofs,
              std::map<std::string, MeshBuilder>& out, std::vector<std::string>& warnings) {
    std::map<uint16_t, std::string> materials;
    for (const auto& [ref, pattern] : lot.wallPatterns)
        materials[ref] = patterns.wallMaterial(pattern);
    size_t unknownFences = 0;
    for (const auto& w : lot.walls) {
        const bool fence = Lot::isFence(w.type);
        if (fence) {
            if (!patterns.fence(w.type))
                ++unknownFences;
            if (w.patterns == std::array<uint16_t, 2>{0xFFFF, 0xFFFF})
                continue; // rails only: see lotFencePieces
        }
        float dx = w.b.x - w.a.x, dy = w.b.y - w.a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len == 0)
            continue;
        dx /= len, dy /= len;
        const int level = w.a.level;
        const bool attic = w.type == Lot::Attic;
        // Bottom and top of the wall at a point along it (grid points or the middle).
        auto bottom = [&](float x, float y) { return lot.elevationAt(level, x, y); };
        auto top = [&](float x, float y) {
            const float z0 = bottom(x, y);
            float z1 = lot.hasLevel(level + 1) ? lot.elevationAt(level + 1, x, y) : z0 + kStoryHeight;
            if (attic)
                z1 = std::max(roofs.heightAt(x, y, level, z1, kAtticRoofOffset), z0);
            else if (fence)
                z1 = std::min(z1, z0 + kHalfWallHeight);
            return z1;
        };
        const float t = kWallHalfThickness;
        const float nx = dy, ny = -dx; // right of a -> b
        // Attic walls follow the roof through their middle too.
        std::vector<std::array<float, 2>> points{{w.a.x - dx * t, w.a.y - dy * t}};
        if (attic)
            points.push_back({(w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2});
        points.push_back({w.b.x + dx * t, w.b.y + dy * t});
        auto clampXY = [&](std::array<float, 2> p) {
            return std::array<float, 2>{std::clamp(p[0], 0.0f, float(lot.width)),
                                        std::clamp(p[1], 0.0f, float(lot.height))};
        };
        std::vector<float> zb, zt;
        for (size_t i = 0; i < points.size(); ++i) {
            // Heights at the wall's own ends, not at the stretched corners.
            const auto q = clampXY(i == 0 ? std::array<float, 2>{w.a.x, w.a.y}
                                   : i + 1 == points.size() ? std::array<float, 2>{w.b.x, w.b.y} : points[i]);
            zb.push_back(bottom(q[0], q[1]));
            zt.push_back(top(q[0], q[1]));
        }

        for (size_t seg = 0; seg + 1 < points.size(); ++seg) {
            const float ax = points[seg][0], ay = points[seg][1], bx = points[seg + 1][0], by = points[seg + 1][1];
            const float za0 = zb[seg], zb0 = zb[seg + 1], za1 = zt[seg], zb1 = zt[seg + 1];
            for (int side = 0; side < 2; ++side) {
                auto it = materials.find(w.patterns[size_t(side)]);
                if (it == materials.end() || it->second.empty())
                    continue;
                MeshBuilder& b = out["wall:" + it->second];
                const float s = side == 0 ? 1.0f : -1.0f;
                const Vec3f n{nx * s, ny * s, 0};
                const Vec3f pa0{ax + nx * t * s, ay + ny * t * s, za0}, pb0{bx + nx * t * s, by + ny * t * s, zb0};
                const Vec3f pa1{pa0[0], pa0[1], za1}, pb1{pb0[0], pb0[1], zb1};
                // u runs along the wall in world units (right to left as seen from the face).
                auto u = [&](const Vec3f& p) { return s * (p[0] * dx + p[1] * dy); };
                // v: 0 at the top of a full wall; attic and half walls keep the
                // texture's bottom at their base, as OpenTS2 does.
                auto vv = [&](float z, float base, float top) {
                    return attic || fence ? 1.0f - (z - base) / kStoryHeight : (top - z) / kStoryHeight;
                };
                const uint32_t i0 = b.vertex(pa0, n, u(pa0), vv(za0, za0, za1)),
                               i1 = b.vertex(pb0, n, u(pb0), vv(zb0, zb0, zb1)),
                               i2 = b.vertex(pb1, n, u(pb1), vv(zb1, zb0, zb1)),
                               i3 = b.vertex(pa1, n, u(pa1), vv(za1, za0, za1));
                if (side == 0)
                    b.quad(i0, i1, i2, i3);
                else
                    b.quad(i1, i0, i3, i2);
            }
            MeshBuilder& cap = out["walltop:wall_top"];
            const Vec3f r0{ax + nx * t, ay + ny * t, za1}, r1{bx + nx * t, by + ny * t, zb1},
                l1{bx - nx * t, by - ny * t, zb1}, l0{ax - nx * t, ay - ny * t, za1};
            const Vec3f up = normalOf(r0, r1, l1);
            auto vtx = [&](const Vec3f& p) { return cap.vertex(p, up, p[0], p[1]); };
            cap.quad(vtx(r0), vtx(r1), vtx(l1), vtx(l0));
        }
    }
    if (unknownFences)
        warnings.push_back(std::to_string(unknownFences) + " fence segments of unknown fences");
}

// Materials for the parts the game draws with its own shaders: roofs (no
// TXMT; OpenTS2 builds "Floor" materials around the catalog's textures) and water.
MaterialDefinition roofMaterial(const std::string& texture, const std::string& bump) {
    MaterialDefinition m;
    m.materialType = "Floor";
    m.properties = {{"stdMatBaseTextureName", texture}, {"stdMatDiffCoef", "0.8,0.8,0.8,1"}};
    if (!bump.empty())
        m.properties.insert(m.properties.end(), {{"stdMatNormalMapTextureEnabled", "true"},
                                                 {"stdMatNormalMapTextureName", bump}});
    return m;
}

// Pools: SimplifiedWaterSurface of PoolWaterSurfaceMaterial (water.matShad),
// colour (0.2, 0.3, 1.0) at 0.4 alpha, unlit, on the pool water layer (5).
MaterialDefinition poolWaterMaterial() {
    MaterialDefinition m;
    m.materialType = "PoolWaterSurfaceMaterial";
    m.properties = {{"stdMatDiffCoef", "0.2,0.3,1.0,0.4"},
                    {"stdMatUntexturedDiffAlpha", "0.4"},
                    {"stdMatLightingEnabled", "0"},
                    {"stdMatCullMode", "none"}};
    return m;
}

// Terrain water: TerrainWaterShader (waterOnTerrain) reflects
// "reflectionoutdoorwater-envcube", its alpha from the vertex colour (not in
// the data: colour and 0.8 here are guesses), with a moving bump map left out.
MaterialDefinition terrainWaterMaterial() {
    MaterialDefinition m;
    m.materialType = "TerrainWater";
    m.properties = {{"stdMatDiffCoef", "0.1,0.2,0.55,0.8"},
                    {"stdMatUntexturedDiffAlpha", "0.8"},
                    {"stdMatEnvCubeMode", "reflection"},
                    {"stdMatEnvCubeCoef", "0.35,0.35,0.35"},
                    {"stdMatEnvCubeTextureName", "reflectionoutdoorwater-envcube"},
                    {"stdMatCullMode", "none"}};
    return m;
}

// Terrain water over every tile whose water level is above the terrain at a
// corner (as OpenTS2 does), and pool water over each pool's floor tiles.
void addWater(const Lot& lot, std::map<std::string, MeshBuilder>& out) {
    MeshBuilder* terrain = nullptr;
    for (uint32_t x = 0; x < lot.width; ++x)
        for (uint32_t y = 0; y < lot.height; ++y) {
            const int ix = int(x), iy = int(y);
            const std::array<std::array<int, 2>, 4> c{{{ix, iy}, {ix + 1, iy}, {ix + 1, iy + 1}, {ix, iy + 1}}};
            bool wet = false;
            std::array<Vec3f, 4> p;
            for (size_t i = 0; i < 4; ++i) {
                const float wl = lot.waterLevel(c[i][0], c[i][1]);
                wet = wet || wl > lot.elevation(0, c[i][0], c[i][1]);
                p[i] = {float(c[i][0]), float(c[i][1]), wl};
            }
            if (!wet)
                continue;
            if (!terrain)
                terrain = &out["water:terrain"];
            const Vec3f up{0, 0, 1};
            auto v = [&](const Vec3f& q) { return terrain->vertex(q, up, q[0] / 4, q[1] / 4); };
            terrain->quad(v(p[0]), v(p[1]), v(p[2]), v(p[3]));
        }
    for (const auto& pool : lot.pools) {
        MeshBuilder& b = out["water:pool"];
        const int x0 = int(std::lround(pool.x)), y0 = int(std::lround(pool.y));
        for (int x = x0; x < x0 + int(pool.width); ++x)
            for (int y = y0; y < y0 + int(pool.height); ++y) {
                if (x < 0 || y < 0 || x >= int(lot.width) || y >= int(lot.height))
                    continue;
                // Pool tiles have a floor one level down (the pool's bottom) and none at the surface.
                const auto& bottomTile = lot.floor(pool.level - 1, uint32_t(x), uint32_t(y));
                if (bottomTile == std::array<uint16_t, 4>{} ||
                    lot.floor(pool.level, uint32_t(x), uint32_t(y)) != std::array<uint16_t, 4>{})
                    continue;
                const Vec3f up{0, 0, 1};
                auto v = [&](int px, int py) {
                    const Vec3f q{float(px), float(py), lot.elevation(pool.level, px, py) - pool.depth};
                    return b.vertex(q, up, q[0], q[1]);
                };
                b.quad(v(x, y), v(x + 1, y), v(x + 1, y + 1), v(x, y + 1));
            }
    }
}

} // namespace

PatternCatalog::PatternCatalog(ResourceIndex& index, const std::filesystem::path& resRoot) {
    const auto scripts = resRoot / "Catalog/Scripts";
    floors_ = loadPatternScript(scripts / "floorPatterns.txt", "floorPattern");
    walls_ = loadPatternScript(scripts / "wallPatterns.txt", "wallPattern");
    auto text = [&](const ResourceIndex::Location& loc) {
        const auto bytes = index.read(loc);
        return parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    };
    auto guidOf = [](const std::map<std::string, std::string>& props) -> std::optional<uint32_t> {
        auto it = props.find("guid");
        if (it == props.end())
            return std::nullopt;
        try {
            return uint32_t(std::stoul(it->second));
        } catch (const std::logic_error&) {
            return std::nullopt;
        }
    };
    auto get = [](const std::map<std::string, std::string>& props, const char* key) {
        auto it = props.find(key);
        return it == props.end() ? std::string() : it->second;
    };
    for (const auto& loc : index.ofType(kTypeCatalogRoof)) {
        const auto props = text(loc);
        if (auto guid = guidOf(props))
            roofs_[*guid] = {get(props, "texturetop"), get(props, "texturetopbump"), get(props, "textureedges"),
                             get(props, "texturetrim"), get(props, "textureunder")};
    }
    for (const auto& loc : index.ofType(kTypeCatalogFence)) {
        const auto props = text(loc);
        if (auto guid = guidOf(props))
            fences_[*guid] = {get(props, "rail"), get(props, "diagrail"), get(props, "post")};
    }
    for (const auto& loc : index.ofType(kTypeCatalogPattern)) {
        const auto bytes = index.read(loc);
        const auto props = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto guid = props.find("guid"), material = props.find("material");
        if (guid == props.end() || material == props.end())
            continue;
        try {
            byGuid_[uint32_t(std::stoul(guid->second))] = material->second;
        } catch (const std::logic_error&) {
        }
    }
}

const PatternCatalog::Roof* PatternCatalog::roof(uint32_t guid) const {
    auto it = roofs_.find(guid);
    return it == roofs_.end() ? nullptr : &it->second;
}

const PatternCatalog::Fence* PatternCatalog::fence(uint32_t guid) const {
    auto it = fences_.find(guid);
    return it == fences_.end() ? nullptr : &it->second;
}

std::string PatternCatalog::material(const std::map<std::string, std::string>& scripts,
                                     const std::string& pattern) const {
    if (auto it = scripts.find(lowerAscii(pattern)); it != scripts.end())
        return it->second;
    if (auto guid = parseGuid(pattern))
        if (auto it = byGuid_.find(*guid); it != byGuid_.end())
            return it->second;
    return pattern;
}

ObjectModel buildLotModel(const Lot& lot, ResourceIndex& index, const PatternCatalog& patterns) {
    ObjectModel model;
    model.name = "lot";
    model.warnings = lot.warnings;

    // Terrain: the base everywhere, then each paint over the tiles its mask
    // touches; none over pools (floors below the ground level), which would
    // hide them, as OpenTS2 leaves out terrain under floors.
    auto open = [&](int x, int y) {
        for (int level = lot.minLevel; level < 0; ++level)
            if (lot.floor(level, uint32_t(x), uint32_t(y)) != std::array<uint16_t, 4>{})
                return false;
        return true;
    };
    if (!lot.terrainBase.empty()) {
        model.materials["terrain:" + lot.terrainBase] = terrainMaterial(lot.terrainBase, false);
        ObjectModel::Part base;
        base.material = "terrain:" + lot.terrainBase;
        base.mesh = terrainMesh(lot, open);
        model.parts.push_back(std::move(base));
    }
    const uint32_t mw = lot.maskWidth, mh = lot.maskHeight;
    const float spacing = lot.width ? float(lot.width) / float(std::max(mw, 2u) - 1) : 1.0f;
    for (const auto& paint : lot.paints) {
        if (std::none_of(paint.mask.begin(), paint.mask.end(), [](uint8_t v) { return v != 0; }))
            continue;
        ObjectModel::AlphaMask mask;
        mask.width = mw, mask.height = mh, mask.spacing = spacing;
        mask.values = paint.mask;
        auto touched = [&](int tx, int ty) {
            const int x0 = int(std::floor(float(tx) / spacing)), x1 = int(std::ceil(float(tx + 1) / spacing));
            const int y0 = int(std::floor(float(ty) / spacing)), y1 = int(std::ceil(float(ty + 1) / spacing));
            for (int x = std::max(x0, 0); x <= std::min(x1, int(mw) - 1); ++x)
                for (int y = std::max(y0, 0); y <= std::min(y1, int(mh) - 1); ++y)
                    if (mask.values[size_t(y) * mw + size_t(x)])
                        return true;
            return false;
        };
        ObjectModel::Part part;
        part.material = "terrain:" + paint.texture;
        part.mesh = terrainMesh(lot, [&](int x, int y) { return open(x, y) && touched(x, y); });
        part.mask = int(model.masks.size());
        model.masks.push_back(std::move(mask));
        model.materials[part.material] = terrainMaterial(paint.texture, true);
        model.parts.push_back(std::move(part));
    }

    const LotRoofs roofs(lot);
    model.warnings.insert(model.warnings.end(), roofs.warnings.begin(), roofs.warnings.end());
    std::map<std::string, MeshBuilder> built;
    addFloors(lot, patterns, built, model.warnings);
    addWalls(lot, patterns, roofs, built, model.warnings);
    addWater(lot, built);
    model.materials["water:terrain"] = terrainWaterMaterial();
    model.materials["water:pool"] = poolWaterMaterial();
    for (auto& [key, b] : built)
        if (key.rfind("water:", 0) == 0) {
            ObjectModel::Part part;
            part.material = part.node = key;
            part.mesh = std::move(b.mesh);
            model.parts.push_back(std::move(part));
        }
    std::erase_if(built, [](const auto& kv) { return kv.first.rfind("water:", 0) == 0; });

    if (roofs.count()) {
        const PatternCatalog::Roof* pattern = patterns.roof(roofs.pattern());
        if (!pattern) {
            model.warnings.push_back("roof pattern " + hexString(roofs.pattern()) + " not in the catalog");
        } else {
            LotRoofs::Meshes meshes = roofs.build();
            const std::pair<MeshGroup*, std::pair<std::string, std::string>> parts[] = {
                {&meshes.top, {pattern->top, pattern->topBump}},
                {&meshes.edges, {pattern->edges, ""}},
                {&meshes.trim, {pattern->trim, ""}},
                {&meshes.under, {pattern->under, ""}}};
            for (const auto& [mesh, tex] : parts) {
                if (mesh->triangles.empty() || tex.first.empty())
                    continue;
                ObjectModel::Part part;
                part.material = part.node = "roof:" + tex.first;
                model.materials[part.material] = roofMaterial(tex.first, tex.second);
                part.mesh = std::move(*mesh);
                model.parts.push_back(std::move(part));
            }
        }
    }
    for (auto& [key, b] : built) {
        ObjectModel::Part part;
        part.material = key.substr(key.find(':') + 1);
        part.node = key;
        part.mesh = std::move(b.mesh);
        model.parts.push_back(std::move(part));
    }

    for (const auto& part : model.parts) {
        if (part.material.empty() || model.materials.count(part.material))
            continue;
        auto loc = index.findByName(part.material + "_txmt", kTypeTxmt);
        if (!loc) {
            model.warnings.push_back("TXMT not found: " + part.material);
            continue;
        }
        const auto bytes = index.read(*loc);
        Rcol rc(bytes);
        const auto* block = rc.find("cMaterialDefinition");
        if (!block) {
            model.warnings.push_back("TXMT without cMaterialDefinition: " + part.material);
            continue;
        }
        model.materials.emplace(part.material, MaterialDefinition::parse(block->body));
    }

    // Floor materials map their texture across floorMaterialScaleU x V tiles
    // (floor.matShad: texture matrix scale 1 / floorMaterialScale).
    for (auto& part : model.parts) {
        auto m = model.materials.find(part.material);
        if (part.node.rfind("floor:", 0) != 0 || m == model.materials.end())
            continue;
        auto scale = [&](const char* name) {
            const std::string v = m->second.property(name);
            try {
                const float f = v.empty() ? 1.0f : std::stof(v);
                return f > 0 ? f : 1.0f;
            } catch (const std::logic_error&) {
                return 1.0f;
            }
        };
        const float su = scale("floorMaterialScaleU"), sv = scale("floorMaterialScaleV");
        if (su != 1 || sv != 1)
            for (auto& uv : part.mesh.uvs)
                uv = {uv[0] / su, uv[1] / sv};
    }
    return model;
}

std::vector<LotFencePiece> lotFencePieces(const Lot& lot, const PatternCatalog& patterns,
                                          std::vector<std::string>* warnings) {
    std::vector<LotFencePiece> out;
    auto ground = [&](int level, float x, float y) {
        return lot.elevation(level, int(std::lround(x)), int(std::lround(y)));
    };
    // As OpenTS2's FenceCollection: a rail model runs one tile along its +X
    // (a diagonal rail one diagonal), placed at the start, turned towards the
    // end and sheared so its far end sits on the ground there.
    for (const auto& w : lot.walls) {
        if (!Lot::isFence(w.type))
            continue;
        const auto* fence = patterns.fence(w.type);
        if (!fence)
            continue;
        const float dx = w.b.x - w.a.x, dy = w.b.y - w.a.y, len = std::sqrt(dx * dx + dy * dy);
        if (len == 0)
            continue;
        const bool diagonal = len > 1.1f && !fence->diagonalRail.empty();
        const std::string& model = diagonal ? fence->diagonalRail : fence->rail;
        if (model.empty())
            continue;
        const float scale = diagonal ? len / std::sqrt(2.0f) : len;
        const float z0 = ground(w.a.level, w.a.x, w.a.y), z1 = ground(w.b.level, w.b.x, w.b.y);
        const float c = dx / len, s = dy / len, shear = (z1 - z0) / len;
        out.push_back({model, {c * scale, s * scale, shear * scale, 0, -s, c, 0, 0, 0, 0, 1, 0, w.a.x, w.a.y, z0, 1}});
    }
    size_t unknown = 0;
    for (const auto& p : lot.fencePosts) {
        const auto* fence = patterns.fence(p.fence);
        if (!fence || fence->post.empty()) {
            ++unknown;
            continue;
        }
        out.push_back({fence->post, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p.x, p.y, ground(p.level, p.x, p.y), 1}});
    }
    if (unknown && warnings)
        warnings->push_back(std::to_string(unknown) + " fence posts of unknown fences");
    return out;
}

} // namespace os2
